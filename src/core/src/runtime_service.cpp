#include "floserver/core/runtime_service.h"

#include "floserver/core/cpu_features.h"
#include "floserver/core/paths.h"
#include "floserver/core/downloader.h"
#include "floserver/core/garnet.h"
#include "floserver/core/mailpit.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::size_t kKeptFinishedJobs = 32;
constexpr std::size_t kMaxWaitingJobs = 16;

bool is_runtime_name(std::string_view s) {
    return !s.empty() && s.size() <= 32 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

std::chrono::sys_seconds system_now() {
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

// Composer and other platform-free runtimes are listed as "any".
bool runs_on(const RuntimeFile& r, std::string_view platform) {
    return r.platform == platform || r.platform == "any";
}

// Whether FloServer installs this entry of the list here: PHP's usual (NTS)
// build, a runtime with no variant, never a Mailpit with a known security
// problem; Bun's regular build on a CPU with AVX2 and its "baseline" build on
// one without (D31).
bool installable(const RuntimeFile& r, bool avx2) {
    if (!runtime_version_allowed(r.name, r.version)) {
        return false;
    }
    if (r.name == "bun") {
        return r.variant == (avx2 ? "" : "baseline");
    }
    return r.variant.empty() || r.variant == "nts";
}

bool is_finished(std::string_view state) {
    return state == "done" || state == "failed" || state == "cancelled";
}

std::string utc_text(std::chrono::sys_seconds t) {
    const auto days = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day ymd{days};
    const std::chrono::hh_mm_ss hms{t - days};
    std::array<char, 32> text{};
    const int n = std::snprintf(text.data(), text.size(), "%04d-%02u-%02uT%02d:%02d:%02dZ",
                                static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                                static_cast<unsigned>(ymd.day()), static_cast<int>(hms.hours().count()),
                                static_cast<int>(hms.minutes().count()),
                                static_cast<int>(hms.seconds().count()));
    return {text.data(), n > 0 ? static_cast<std::size_t>(n) : 0};
}

}  // namespace

struct RuntimeService::Job {
    std::int64_t id = 0;
    std::string name;
    std::string requested;
    std::string variant;  // asked for (php-redis); empty: plain or NTS
    std::string version;
    std::string state = "queued";
    std::string message;
    std::uint64_t received = 0;
    std::uint64_t total = 0;
    std::atomic<bool> cancel{false};
};

RuntimeService::RuntimeService(RuntimeServiceConfig config, std::shared_ptr<HttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {
    if (!config_.now) {
        config_.now = system_now;
    }
    if (!config_.has_avx2) {
        config_.has_avx2 = cpu_has_avx2;
    }
    // No install can be running yet: clear what a crash may have left.
    remove_leftovers(runtimes_root());
    std::error_code ec;
    for (fs::directory_iterator it(downloads_dir(config_.data_dir), ec), end; !ec && it != end; it.increment(ec)) {
        // .part: cut off mid-download; .download: complete, but the install
        // after it didn't finish (the hash is checked again if reused).
        if (it->path().extension() == ".part" || it->path().extension() == ".download") {
            std::error_code ignored;
            fs::remove(it->path(), ignored);
        }
    }
}

RuntimeService::~RuntimeService() {
    stop();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void RuntimeService::stop() {
    stop_requested_ = true;
    {
        const std::lock_guard lock(jobs_mutex_);
        stopping_ = true;
        for (auto& [id, job] : jobs_) {
            job->cancel = true;
        }
    }
    jobs_changed_.notify_all();
}

fs::path RuntimeService::runtimes_root() const { return runtimes_dir(config_.data_dir); }

std::optional<RuntimeService::CachedList> RuntimeService::get_list(bool refresh, std::string& error) {
    std::unique_lock lock(list_mutex_);
    // One fetch at a time; the others wait for it rather than fetch again.
    list_changed_.wait(lock, [this] { return !fetching_; });
    const auto now = config_.now();
    if (list_ && !refresh && std::chrono::steady_clock::now() - list_->fetched < config_.list_max_age &&
        list_->manifest.expires > now) {
        return list_;
    }
    if (!transport_) {
        error = "downloads aren't available on this platform yet";
        return std::nullopt;
    }
    fetching_ = true;
    lock.unlock();

    // The network part runs without the lock, so a slow server can't block
    // callers that only need the cached list.
    std::optional<CachedList> fresh;
    try {
        const fs::path state = manifest_state_file(config_.data_dir);
        ManifestFetchOptions options;
        options.check.trusted_keys = config_.keys;
        options.check.now = now;
        options.check.last_sequences = load_last_sequences(state);
        options.retry_delay = config_.retry_delay;
        options.sleep = config_.sleep;
        options.cancel = &stop_requested_;
        ManifestFetch fetched = fetch_manifest(*transport_, config_.source, options);
        if (fetched.manifest) {
            std::string store_error;
            // Not being able to store the sequence only weakens rollback
            // protection on this computer; the list itself is good, so use it.
            (void)store_last_sequence(state, fetched.key_id, fetched.manifest->sequence, store_error);
            // Kept for offline use: Settings' list (D20) and the templates.
            // Every good list, with templates or not: one kept only when it
            // had templates left Settings with no list offline, so each
            // opening without the internet tried to fetch one. The pair is
            // checked again when read back, so a half-written pair is just
            // ignored. Under offline_mutex_, so an offline read never sees
            // half a pair; a later fallback reads this newest pair.
            {
                const std::lock_guard offline_lock(offline_mutex_);
                std::string keep_error;
                (void)(write_file_atomically(kept_manifest_file(config_.data_dir), fetched.manifest_bytes, keep_error) &&
                       write_file_atomically(kept_signature_file(config_.data_dir), fetched.signature_bytes,
                                             keep_error));
                offline_.reset();
            }
            fresh = CachedList{std::move(*fetched.manifest), std::move(fetched.key_id), std::move(fetched.warnings),
                               std::chrono::steady_clock::now()};
        } else {
            error = fetched.message;
        }
    } catch (...) {
        lock.lock();
        fetching_ = false;
        list_changed_.notify_all();
        throw;
    }

    lock.lock();
    fetching_ = false;
    const bool got = fresh.has_value();
    if (got) {
        list_ = std::move(fresh);
    }
    list_changed_.notify_all();
    return got ? list_ : std::nullopt;
}

json RuntimeService::list(bool refresh, bool installed_only, bool cached_only) {
    json out = {{"platform", config_.platform}, {"installed", json::array()}, {"available", json::array()}};
    for (const InstalledRuntime& r : list_installed(runtimes_root())) {
        out["installed"].push_back({{"name", r.name},
                                    {"version", r.version},
                                    {"variant", r.variant},
                                    {"platform", r.platform},
                                    {"license", r.license},
                                    {"folder", path_to_utf8(r.dir.filename())},
                                    {"path", path_to_utf8(r.dir)}});
    }
    // The window asks often and only needs what's installed: no download.
    if (installed_only) {
        return out;
    }
    std::string error;
    std::optional<CachedList> cached;
    if (cached_only && !refresh) {
        // Settings (D20) opens without going online: the list in hand, even
        // if old, else the last signed list kept on disk.
        {
            const std::lock_guard lock(list_mutex_);
            cached = list_;
        }
        if (!cached) {
            const std::lock_guard lock(offline_mutex_);
            if (auto kept = open_kept_list(read_small_file(kept_manifest_file(config_.data_dir), kMaxManifestBytes),
                                           read_small_file(kept_signature_file(config_.data_dir), kMaxSignatureBytes))) {
                cached = CachedList{std::move(*kept), {}, {}, std::chrono::steady_clock::now()};
            } else {
                error = "there's no runtime list on this computer yet";
            }
        }
    } else {
        cached = get_list(refresh, error);
    }
    if (!cached) {
        out["list"] = nullptr;
        out["list_error"] = error;
        return out;
    }
    out["list"] = {{"sequence", cached->manifest.sequence},
                   {"expires", utc_text(cached->manifest.expires)},
                   {"key", cached->key_id},
                   {"warnings", cached->warnings}};
    for (const RuntimeFile& r : cached->manifest.runtimes) {
        if (!runs_on(r, config_.platform) || !runtime_version_allowed(r.name, r.version) ||
            (r.name == "bun" && !installable(r, config_.has_avx2()))) {
            continue;  // Bun: the build this CPU runs, not both
        }
        out["available"].push_back({{"name", r.name},
                                    {"version", r.version},
                                    {"variant", r.variant},
                                    {"size", r.size},
                                    {"license", r.license},
                                    {"installed", is_installed(runtimes_root(), r)}});
    }
    return out;
}

std::optional<Manifest> RuntimeService::fresh_list(std::string& error) {
    auto cached = get_list(true, error);
    if (!cached) {
        return std::nullopt;
    }
    return std::move(cached->manifest);
}

std::optional<std::vector<ProjectTemplate>> RuntimeService::templates(std::string& error, bool fetch) {
    if (fetch) {
        std::string fetch_error;  // offline is not an error here
        if (const auto cached = get_list(false, fetch_error); cached && !cached->manifest.templates.empty()) {
            return cached->manifest.templates;
        }
    }
    // The list in hand, even if stale: the same one a project job reads, so
    // the wizard and the job always agree.
    {
        const std::lock_guard lock(list_mutex_);
        if (list_ && !list_->manifest.templates.empty()) {
            return list_->manifest.templates;
        }
    }
    return offline_templates(error);
}

std::optional<Manifest> RuntimeService::open_kept_list(const std::optional<std::string>& list,
                                                       const std::optional<std::string>& sig) const {
    if (!list || !sig) {
        return std::nullopt;
    }
    ManifestCheck check;
    check.trusted_keys = config_.keys;
    check.now = config_.now();
    check.last_sequences = load_last_sequences(manifest_state_file(config_.data_dir));
    // An expired copy still opens: what it lists is checked against a fresh
    // list before anything is downloaded.
    const json doc = json::parse(*list, nullptr, false);
    if (doc.is_object() && doc.contains("expires") && doc["expires"].is_string()) {
        if (const auto expires = parse_utc_time(doc["expires"].get<std::string>()); expires && check.now >= *expires) {
            check.now = *expires - std::chrono::seconds{1};
        }
    }
    auto opened = open_manifest(*list, *sig, check);
    return opened.manifest ? std::move(opened.manifest) : std::nullopt;
}

std::optional<std::vector<ProjectTemplate>> RuntimeService::offline_templates(std::string& error) {
    const std::lock_guard lock(offline_mutex_);
    if (offline_) {
        return offline_;
    }
    // 1. The last signed list this computer used, signature checked again.
    //    An expired copy still serves its templates: they only describe how
    //    to make a project, and every runtime they need is checked against a
    //    fresh list when it's downloaded.
    const auto list = read_small_file(kept_manifest_file(config_.data_dir), kMaxManifestBytes);
    const auto sig = read_small_file(kept_signature_file(config_.data_dir), kMaxSignatureBytes);
    if (auto kept = open_kept_list(list, sig); kept && !kept->templates.empty()) {
        offline_ = std::move(kept->templates);
        return offline_;
    }
    // The built-in ones aren't kept for later while a kept pair exists: it
    // may only have been mid-rewrite.
    const bool kept_pair = list.has_value() || sig.has_value();
    // 2. The templates built into FloServer.
    const json builtin = json::parse(config_.builtin_templates_json, nullptr, false);
    if (builtin.is_array()) {
        std::vector<std::string> warnings;
        auto parsed = parse_templates(builtin, warnings);
        if (!parsed.empty()) {
            if (kept_pair) {
                return parsed;
            }
            offline_ = std::move(parsed);
            return offline_;
        }
    }
    error = "the project templates couldn't be loaded; connect to the internet and open New project again";
    return std::nullopt;
}

std::optional<std::int64_t> RuntimeService::install(const std::string& name, const std::string& version,
                                                    ServiceRefusal& refusal, const std::string& variant) {
    if (!is_runtime_name(name)) {
        refusal = {"bad_params", "\"" + name + "\" isn't a runtime name such as php, mariadb or node"};
        return std::nullopt;
    }
    if (!version.empty() && !Version::parse(version)) {
        refusal = {"bad_params", "\"" + version + "\" isn't a version such as 8.5 or 8.5.12"};
        return std::nullopt;
    }
    if (!variant.empty() && !is_runtime_name(variant)) {
        refusal = {"bad_params", "\"" + variant + "\" isn't a variant such as php8-4"};
        return std::nullopt;
    }
    if (!transport_) {
        refusal = {"downloads_unavailable", "downloads aren't available on this platform yet"};
        return std::nullopt;
    }
    const std::lock_guard lock(jobs_mutex_);
    if (stopping_) {
        refusal = {"stopping", "FloServer is stopping"};
        return std::nullopt;
    }
    if (cleaning_) {
        refusal = {"busy", "FloServer is removing leftover downloads; try again in a moment"};
        return std::nullopt;
    }
    std::size_t waiting = 0;
    for (const auto& [id, job] : jobs_) {
        if (job->name == name && job->requested == version && job->variant == variant && !is_finished(job->state) &&
            !job->cancel) {
            return id;
        }
        if (!is_finished(job->state)) {
            ++waiting;
        }
    }
    if (waiting >= kMaxWaitingJobs) {
        refusal = {"busy", "too many installs are waiting; try again when they're done"};
        return std::nullopt;
    }
    auto job = std::make_shared<Job>();
    job->id = next_job_++;
    job->name = name;
    job->requested = version;
    job->variant = variant;
    jobs_[job->id] = job;
    queue_.push_back(job);

    // Forget old finished jobs.
    std::vector<std::int64_t> finished;
    for (const auto& [id, j] : jobs_) {
        if (is_finished(j->state)) {
            finished.push_back(id);
        }
    }
    for (std::size_t i = 0; i + kKeptFinishedJobs < finished.size(); ++i) {
        jobs_.erase(finished[i]);
    }

    if (!worker_started_) {
        worker_ = std::thread([this] { worker(); });
        worker_started_ = true;
    }
    jobs_changed_.notify_all();
    return job->id;
}

std::optional<json> RuntimeService::job(std::int64_t id) const {
    const std::lock_guard lock(jobs_mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return std::nullopt;
    }
    return describe(*it->second);
}

bool RuntimeService::busy() const {
    const std::lock_guard lock(jobs_mutex_);
    return removing_ > 0 ||
           std::any_of(jobs_.begin(), jobs_.end(), [](const auto& entry) { return !is_finished(entry.second->state); });
}

bool RuntimeService::begin_cleanup() {
    const std::lock_guard lock(jobs_mutex_);
    if (cleaning_ || removing_ > 0 ||
        std::any_of(jobs_.begin(), jobs_.end(), [](const auto& entry) { return !is_finished(entry.second->state); })) {
        return false;
    }
    cleaning_ = true;
    return true;
}

void RuntimeService::end_cleanup() {
    const std::lock_guard lock(jobs_mutex_);
    cleaning_ = false;
}

bool RuntimeService::cancel(std::int64_t id) {
    const std::lock_guard lock(jobs_mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end() || is_finished(it->second->state)) {
        return false;
    }
    it->second->cancel = true;
    return true;
}

bool RuntimeService::remove(const std::string& name, const std::string& version, ServiceRefusal& refusal) {
    if (!is_runtime_name(name) || version.empty()) {
        refusal = {"bad_params", "name a runtime and an installed version, such as php 8.5.12"};
        return false;
    }
    {
        const std::lock_guard lock(jobs_mutex_);
        if (cleaning_) {
            refusal = {"busy", "FloServer is removing leftover downloads; try again in a moment"};
            return false;
        }
        for (const auto& [id, job] : jobs_) {
            if (job->name == name && !is_finished(job->state)) {
                refusal = {"busy", "an install of " + name + " is still running"};
                return false;
            }
        }
        ++removing_;  // its .removing- folder is no leftover meanwhile
    }
    struct Done {
        RuntimeService& service;
        Done(const Done&) = delete;
        Done& operator=(const Done&) = delete;
        Done(Done&&) = delete;
        Done& operator=(Done&&) = delete;
        explicit Done(RuntimeService& s) : service(s) {}
        ~Done() {
            const std::lock_guard lock(service.jobs_mutex_);
            --service.removing_;
        }
    } const done(*this);
    // Bun's baseline build lives in "<version>-baseline" (D31): "1.3.14"
    // finds it too, this CPU's build first.
    std::string folder = version;
    if (name == "bun" && !version.ends_with("-baseline")) {
        const std::string baseline = version + "-baseline";
        const std::array<std::string, 2> order = config_.has_avx2() ? std::array<std::string, 2>{version, baseline}
                                                                    : std::array<std::string, 2>{baseline, version};
        std::error_code ec;
        for (const std::string& candidate : order) {
            if (fs::is_directory(runtimes_root() / "bun" / path_from_utf8(candidate), ec)) {
                folder = candidate;
                break;
            }
        }
    }
    std::string error;
    if (!remove_runtime(runtimes_root(), name, folder, error)) {
        refusal = {"not_removed", error};
        return false;
    }
    return true;
}

json RuntimeService::describe(const Job& job) {
    return {{"id", job.id},
            {"kind", "install"},
            {"name", job.name},
            {"requested", job.requested},
            {"variant", job.variant},
            {"version", job.version},
            {"state", job.state},
            {"received", job.received},
            {"total", job.total},
            {"message", job.message}};
}

void RuntimeService::set_state(Job& job, const char* state, std::string message) {
    const std::lock_guard lock(jobs_mutex_);
    job.state = state;
    if (!message.empty()) {
        job.message = std::move(message);
    }
}

void RuntimeService::worker() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(jobs_mutex_);
            jobs_changed_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                for (auto& waiting : queue_) {
                    waiting->state = "cancelled";
                }
                queue_.clear();
                return;
            }
            job = queue_.front();
            queue_.pop_front();
        }
        try {
            run(*job);
        } catch (const std::exception& e) {
            set_state(*job, "failed", std::string("unexpected error: ") + e.what());
        } catch (...) {
            set_state(*job, "failed", "unexpected error");
        }
    }
}

namespace {

// The newest entry of `name` for this platform (or "any") matching `wanted`
// and, when given, `variant` exactly; else what installable() takes (plain
// and NTS builds; Bun's baseline build on a CPU without AVX2).
// `offered` gets every version looked at.
std::optional<RuntimeFile> pick_entry(const Manifest& manifest, const std::string& name, const std::string& platform,
                                      const std::string& wanted_text, const std::string& variant, bool avx2,
                                      std::vector<std::string>* offered = nullptr) {
    const auto wanted = wanted_text.empty() ? std::optional<Version>() : Version::parse(wanted_text);
    std::vector<RuntimeFile> candidates = runtimes_for(manifest, name, platform);
    for (RuntimeFile& r : runtimes_for(manifest, name, "any")) {
        candidates.push_back(std::move(r));
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const RuntimeFile& a, const RuntimeFile& b) {
        return Version::parse(a.version).value_or(Version{}) > Version::parse(b.version).value_or(Version{});
    });
    std::optional<RuntimeFile> entry;
    for (const RuntimeFile& r : candidates) {
        const bool kind = variant.empty() ? installable(r, avx2) : r.variant == variant;
        if (!kind || !runtime_version_allowed(r.name, r.version)) {
            continue;  // a Mailpit with a known security problem is never installed
        }
        if (offered != nullptr) {
            offered->push_back(r.version);
        }
        const auto v = Version::parse(r.version);
        if (!entry && v && (!wanted || wanted->matches(*v))) {
            entry = r;
        }
    }
    return entry;
}

// The PHP series ("8.4") of the NTS PHP installs, oldest first, each once.
std::vector<std::string> installed_php_series(const std::vector<InstalledRuntime>& installed) {
    std::vector<std::string> out;
    for (const InstalledRuntime& r : installed) {
        if (r.name != "php" || (!r.variant.empty() && r.variant != "nts")) {
            continue;
        }
        const auto v = Version::parse(r.version);
        if (v && v->parts().size() >= 2) {
            const std::string s = std::to_string(v->parts()[0]) + "." + std::to_string(v->parts()[1]);
            if (std::find(out.begin(), out.end(), s) == out.end()) {
                out.push_back(s);
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return Version::parse(a).value_or(Version{}) < Version::parse(b).value_or(Version{});
    });
    return out;
}

}  // namespace

bool RuntimeService::fetch_install(Job& job, const RuntimeFile& entry, std::uint64_t base, std::string& error,
                                   fs::path* dir) {
    const std::string folder = runtime_folder(entry);
    if (folder.empty()) {
        error = "the runtime list names " + entry.name + " in a way FloServer can't install";
        return false;
    }
    set_state(job, "downloading");
    const fs::path archive =
        downloads_dir(config_.data_dir) / path_from_utf8(entry.name + "-" + folder + ".download");
    DownloadOptions options;
    options.progress = [&](const DownloadProgress& p) {
        {
            const std::lock_guard lock(jobs_mutex_);
            job.received = base + p.received;
        }
        return !job.cancel.load();
    };
    options.cancel = &job.cancel;
    const DownloadResult downloaded = download_file(*transport_, entry, archive, options);
    if (downloaded.cancelled || job.cancel) {
        std::error_code ignored;
        fs::remove(archive, ignored);
        error.clear();
        return false;
    }
    if (!downloaded.ok) {
        error = downloaded.message;
        return false;
    }
    set_state(job, "installing");
    const InstallResult installed = install_runtime(entry, archive, runtimes_root(), config_.vc_runtime_dir);
    std::error_code ignored;
    fs::remove(archive, ignored);
    if (!installed.ok) {
        error = installed.message;
        return false;
    }
    if (dir != nullptr) {
        *dir = installed.dir;
    }
    return true;
}

void RuntimeService::run(Job& job) {
    if (job.cancel) {
        set_state(job, "cancelled", "cancelled");
        return;
    }
    set_state(job, "checking");
    std::string error;
    const auto cached = get_list(false, error);
    if (!cached) {
        set_state(job, "failed", error);
        return;
    }

    // Newest first; PHP's thread-safe builds aren't what FloServer runs.
    std::vector<std::string> offered;
    const bool avx2 = config_.has_avx2();
    const std::optional<RuntimeFile> entry =
        pick_entry(cached->manifest, job.name, config_.platform, job.requested, job.variant, avx2, &offered);
    if (!entry) {
        std::string message = "the runtime list has no " + job.name +
                              (job.requested.empty() ? "" : " " + job.requested) +
                              (job.variant.empty() ? "" : " (" + job.variant + ")") + " for " + config_.platform;
        if (!offered.empty()) {
            message += " (it has";
            for (const auto& v : offered) {
                message += " " + v;
            }
            message += ")";
        }
        set_state(job, "failed", message);
        return;
    }
    const fs::path root = runtimes_root();
    const auto installed_now = list_installed(root);

    // What comes with it (D30): for Garnet, the .NET runtime first (it
    // can't run without it) and phpredis for each PHP series; for a PHP
    // while Garnet is installed, its series' phpredis.
    std::optional<RuntimeFile> dotnet;
    std::vector<RuntimeFile> extras;  // phpredis builds: a failure is only noted
    std::vector<std::string> notes;
    const auto add_php_redis = [&](const std::string& series) {
        if (const auto r = pick_entry(cached->manifest, std::string(kPhpRedisRuntime), config_.platform, "",
                                      php_redis_variant(series), avx2)) {
            if (!is_installed(root, *r)) {
                extras.push_back(*r);
            }
        } else {
            notes.push_back("there's no phpredis for PHP " + series + " in the runtime list, so its projects use predis");
        }
    };
    if (job.name == kGarnetRuntime) {
        dotnet = pick_entry(cached->manifest, std::string(kDotnetRuntime), config_.platform,
                            std::string(kGarnetDotnetSeries), "", avx2);
        if (!dotnet) {
            set_state(job, "failed",
                      "the runtime list has no .NET " + std::string(kGarnetDotnetSeries) + " runtime, which Garnet needs");
            return;
        }
        if (is_installed(root, *dotnet)) {
            dotnet.reset();
        }
        for (const std::string& series : installed_php_series(installed_now)) {
            add_php_redis(series);
        }
    } else if (job.name == "php" && newest_garnet(installed_now) != nullptr) {
        const auto v = Version::parse(entry->version);
        if (v && v->parts().size() >= 2) {
            add_php_redis(std::to_string(v->parts()[0]) + "." + std::to_string(v->parts()[1]));
        }
    }

    const bool main_installed = is_installed(root, *entry);
    {
        const std::lock_guard lock(jobs_mutex_);
        job.version = entry->version;
        job.total = (main_installed ? 0 : entry->size) + (dotnet ? dotnet->size : 0);
        for (const RuntimeFile& r : extras) {
            job.total += r.size;
        }
    }
    std::uint64_t base = 0;
    const auto cancelled = [&] {
        if (job.cancel) {
            set_state(job, "cancelled", "cancelled");
            return true;
        }
        return false;
    };
    if (dotnet) {
        if (!fetch_install(job, *dotnet, base, error)) {
            if (!cancelled()) {
                set_state(job, "failed", "the .NET runtime Garnet needs couldn't be installed: " + error);
            }
            return;
        }
        base += dotnet->size;
    }
    std::string done_message;
    if (main_installed) {
        done_message = job.name + " " + entry->version + " is already installed";
    } else {
        fs::path dir;
        if (!fetch_install(job, *entry, base, error, &dir)) {
            if (!cancelled()) {
                set_state(job, "failed", error);
            }
            return;
        }
        base += entry->size;
        done_message = job.name + " " + entry->version + " is installed in " + path_to_utf8(dir);
    }
    for (const RuntimeFile& r : extras) {
        if (job.cancel) {
            break;  // the main install is done: what's left comes with the next install
        }
        const auto series = php_redis_series_of_variant(r.variant).value_or(r.variant);
        if (!fetch_install(job, r, base, error) && !job.cancel) {
            std::string note = "phpredis for PHP " + series;
            note += " couldn't be installed (";
            note += error;
            note += "), so its projects use predis";
            notes.push_back(std::move(note));
        }
        base += r.size;
    }
    for (const std::string& n : notes) {
        done_message += "; " + n;
    }
    set_state(job, "done", done_message);
    if (config_.installed) {
        config_.installed(job.name, entry->version);  // an older one left by an update that stopped half-way, too
    }
}

}  // namespace floserver::core

#include "floserver/core/runtime_service.h"

#include "floserver/core/paths.h"
#include "floserver/core/downloader.h"
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
            // Kept for offline use (the templates). The pair is checked
            // again when read back, so a half-written pair is just ignored.
            if (!fetched.manifest->templates.empty()) {
                // Under offline_mutex_, so an offline read never sees half a
                // pair; a later fallback reads this newest pair.
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
        if (!runs_on(r, config_.platform)) {
            continue;
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
                                                    ServiceRefusal& refusal) {
    if (!is_runtime_name(name)) {
        refusal = {"bad_params", "\"" + name + "\" isn't a runtime name such as php, mariadb or node"};
        return std::nullopt;
    }
    if (!version.empty() && !Version::parse(version)) {
        refusal = {"bad_params", "\"" + version + "\" isn't a version such as 8.5 or 8.5.12"};
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
    std::size_t waiting = 0;
    for (const auto& [id, job] : jobs_) {
        if (job->name == name && job->requested == version && !is_finished(job->state) && !job->cancel) {
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
        for (const auto& [id, job] : jobs_) {
            if (job->name == name && !is_finished(job->state)) {
                refusal = {"busy", "an install of " + name + " is still running"};
                return false;
            }
        }
    }
    std::string error;
    if (!remove_runtime(runtimes_root(), name, version, error)) {
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
    const auto wanted = job.requested.empty() ? std::optional<Version>() : Version::parse(job.requested);
    std::optional<RuntimeFile> entry;
    std::vector<std::string> offered;
    std::vector<RuntimeFile> candidates = runtimes_for(cached->manifest, job.name, config_.platform);
    for (RuntimeFile& r : runtimes_for(cached->manifest, job.name, "any")) {
        candidates.push_back(std::move(r));
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const RuntimeFile& a, const RuntimeFile& b) {
        return Version::parse(a.version).value_or(Version{}) > Version::parse(b.version).value_or(Version{});
    });
    for (const RuntimeFile& r : candidates) {
        if (!r.variant.empty() && r.variant != "nts") {
            continue;
        }
        offered.push_back(r.version);
        const auto v = Version::parse(r.version);
        if (!entry && v && (!wanted || wanted->matches(*v))) {
            entry = r;
        }
    }
    if (!entry) {
        std::string message = "the runtime list has no " + job.name +
                              (job.requested.empty() ? "" : " " + job.requested) + " for " + config_.platform;
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
    {
        const std::lock_guard lock(jobs_mutex_);
        job.version = entry->version;
        job.total = entry->size;
    }
    const fs::path root = runtimes_root();
    if (is_installed(root, *entry)) {
        set_state(job, "done", job.name + " " + entry->version + " is already installed");
        return;
    }

    const std::string folder = runtime_folder(*entry);
    if (folder.empty()) {
        set_state(job, "failed", "the runtime list names " + job.name + " in a way FloServer can't install");
        return;
    }
    set_state(job, "downloading");
    const fs::path archive =
        downloads_dir(config_.data_dir) / path_from_utf8(entry->name + "-" + folder + ".download");
    DownloadOptions options;
    options.progress = [&](const DownloadProgress& p) {
        {
            const std::lock_guard lock(jobs_mutex_);
            job.received = p.received;
        }
        return !job.cancel.load();
    };
    options.cancel = &job.cancel;
    const DownloadResult downloaded = download_file(*transport_, *entry, archive, options);
    if (downloaded.cancelled || job.cancel) {
        std::error_code ignored;
        fs::remove(archive, ignored);
        set_state(job, "cancelled", "cancelled");
        return;
    }
    if (!downloaded.ok) {
        set_state(job, "failed", downloaded.message);
        return;
    }

    set_state(job, "installing");
    const InstallResult installed = install_runtime(*entry, archive, root, config_.vc_runtime_dir);
    std::error_code ignored;
    fs::remove(archive, ignored);
    if (!installed.ok) {
        set_state(job, "failed", installed.message);
        return;
    }
    set_state(job, "done", job.name + " " + entry->version + " is installed in " + path_to_utf8(installed.dir));
}

}  // namespace floserver::core

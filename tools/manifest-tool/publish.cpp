#include "publish.h"

#include "build.h"
#include "templates_json.h"
#include "floserver/core/encoding.h"
#include "floserver/core/sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>

namespace floserver::manifest_tool {

namespace {

using namespace floserver::core;
using nlohmann::json;

constexpr std::size_t kMaxListBytes = std::size_t{8} * 1024 * 1024;
constexpr std::size_t kMaxSigBytes = 4096;
constexpr std::size_t kMaxSumsBytes = std::size_t{4} * 1024 * 1024;
constexpr std::uint64_t kMaxFileBytes = std::uint64_t{2} * 1024 * 1024 * 1024;

std::string describe_failure(const HttpReply& r) {
    if (r.status == 0) {
        return r.error.empty() ? std::string("no answer") : r.error;
    }
    return "HTTP " + std::to_string(r.status);
}

// A small text file, whole. Nothing when it isn't a 200 or is too big.
std::optional<std::string> get_text(HttpTransport& http, const std::string& url, std::size_t max,
                                    const std::atomic<bool>* cancel, std::string& error,
                                    HttpReply* reply_out = nullptr) {
    std::string body;
    bool too_big = false;
    HttpLimits limits;
    limits.max_time = std::chrono::seconds{60};
    limits.cancel = cancel;
    const HttpReply reply = http.get(url, limits, [&](std::span<const std::uint8_t> chunk) {
        if (body.size() + chunk.size() > max) {
            too_big = true;
            return false;
        }
        body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        return true;
    });
    if (reply_out != nullptr) {
        *reply_out = reply;
    }
    if (too_big) {
        error = url + " is larger than expected";
        return std::nullopt;
    }
    if (reply.status != 200 || reply.stopped) {
        error = url + ": " + describe_failure(reply);
        return std::nullopt;
    }
    return body;
}

struct Download {
    HttpReply reply;
    std::uint64_t size = 0;
    std::string sha256;
    bool too_big = false;
};

Download download_and_hash(CheckContext& ctx, const std::string& url) {
    Download d;
    Sha256 hash;
    HttpLimits limits;
    limits.max_time = std::chrono::minutes{30};
    limits.cancel = ctx.cancel;
    d.reply = ctx.http.get(url, limits, [&](std::span<const std::uint8_t> chunk) {
        if (d.size + chunk.size() > kMaxFileBytes) {
            d.too_big = true;
            return false;
        }
        hash.update(chunk);
        d.size += chunk.size();
        if (ctx.progress) {
            ctx.progress(d.size);
        }
        return true;
    });
    if (d.reply.status == 200 && !d.reply.stopped) {
        const Sha256Digest digest = hash.finish();
        d.sha256 = to_hex(digest);
    }
    return d;
}

bool cancelled(const std::atomic<bool>* cancel) { return cancel != nullptr && cancel->load(); }

std::string host_of(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    if (url.starts_with(kScheme)) {
        url.remove_prefix(kScheme.size());
    }
    return std::string(url.substr(0, url.find('/')));
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    return s;
}

// A 64-hex-digit word on the line.
std::optional<std::string> hash_on_line(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size()) {
        if (!is_word_char(line[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < line.size() && is_word_char(line[j])) {
            ++j;
        }
        const std::string_view word = line.substr(i, j - i);
        if (word.size() == 64 && std::all_of(word.begin(), word.end(), is_hex)) {
            return lower(std::string(word));
        }
        i = j;
    }
    return std::nullopt;
}

bool names_file(std::string_view line, std::string_view name) {
    const auto edge = [](char c) { return !is_word_char(c) && c != '.' && c != '-'; };
    for (std::size_t at = line.find(name); at != std::string_view::npos; at = line.find(name, at + 1)) {
        const bool start_ok = at == 0 || edge(line[at - 1]);
        const std::size_t after = at + name.size();
        const bool end_ok = after == line.size() || edge(line[after]);
        if (start_ok && end_ok) {
            return true;
        }
    }
    return false;
}

}  // namespace

LiveResult fetch_live_list(HttpTransport& http, std::string_view cdn,
                           const std::vector<TrustedKey>& keys, std::chrono::sys_seconds now,
                           const std::atomic<bool>* cancel) {
    LiveResult result;
    const std::string base(cdn);
    std::string error;
    const auto list = get_text(http, base + "/manifest.json", kMaxListBytes, cancel, error);
    if (!list) {
        result.error = "Can't read the live list: " + error;
        return result;
    }
    const auto sig = get_text(http, base + "/manifest.json.sig", kMaxSigBytes, cancel, error);
    if (!sig) {
        result.error = "Can't read the live list's signature: " + error;
        return result;
    }

    // An expired list is still read, its signature checked, so it can be
    // replaced: open it as of a second before it expired.
    ManifestCheck check;
    check.trusted_keys = keys;
    check.now = now;
    bool expired = false;
    const json doc = json::parse(*list, nullptr, false);
    if (doc.is_object() && doc.contains("expires") && doc["expires"].is_string()) {
        if (const auto expires = parse_utc_time(doc["expires"].get<std::string>());
            expires && now >= *expires) {
            check.now = *expires - std::chrono::seconds{1};
            expired = true;
        }
    }
    const auto opened = open_manifest(*list, *sig, check);
    if (!opened.manifest) {
        result.error = "The live list doesn't open: " + opened.message;
        return result;
    }
    LiveList live;
    live.sequence = opened.manifest->sequence;
    live.expires = opened.manifest->expires;
    live.expired = expired;
    live.key_id = opened.key_id;
    live.runtimes = opened.manifest->runtimes;
    live.template_count = opened.manifest->templates.size();
    live.templates_json = doc.is_object() && doc.contains("templates") ? doc["templates"].dump() : "[]";
    result.list = std::move(live);
    return result;
}

std::string runtime_title(std::string_view runtime, std::string_view version) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kNames = {{
        {"php", "PHP"},
        {"mariadb", "MariaDB"},
        {"node", "Node.js"},
        {"composer", "Composer"},
        {"nginx", "nginx"},
        {"apache", "Apache"},
        {"wordpress", "WordPress"},
        {"python", "Python"},
        {"phpmyadmin", "phpMyAdmin"},
    }};
    std::string name(runtime);
    for (const auto& [id, title] : kNames) {
        if (id == runtime) {
            name = title;
        }
    }
    return name + " " + std::string(version);
}

std::vector<PublishFile> files_from_live(const LiveList& live, std::string_view cdn,
                                         std::vector<std::string>& notes) {
    std::vector<PublishFile> files;
    const std::string prefix = std::string(cdn) + "/";
    for (const RuntimeFile& r : live.runtimes) {
        const std::string_view url = r.urls.empty() ? std::string_view() : std::string_view(r.urls.front());
        if (!url.starts_with(prefix)) {
            notes.push_back(runtime_title(r.name, r.version) + " isn't on " + std::string(cdn) +
                            " and is left out");
            continue;
        }
        PublishFile f;
        f.path = std::string(url.substr(prefix.size()));
        f.title = runtime_title(r.name, r.version);
        f.published = true;
        f.size = r.size;
        f.sha256 = to_hex(r.sha256);
        files.push_back(std::move(f));
    }
    return files;
}

AddResult new_file(std::string_view text, std::string_view cdn,
                   const std::vector<PublishFile>& existing) {
    AddResult result;
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n')) {
        text.remove_suffix(1);
    }
    const std::string prefix = std::string(cdn) + "/";
    if (text.starts_with(prefix)) {
        text.remove_prefix(prefix.size());
    }
    while (text.starts_with('/')) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        result.error = "Type the file's path on the CDN, such as php/8.5.13/php-8.5.13-nts-Win32-vs17-x64.zip.";
        return result;
    }
    if (!std::all_of(text.begin(), text.end(), [](char c) {
            return is_word_char(c) || c == '.' || c == '-' || c == '/';
        })) {
        result.error = "A CDN path has only letters, digits, '.', '-', '_' and '/'. "
                       "A '+' in a file name must be '-' on the CDN.";
        return result;
    }
    const CdnPath p = describe_cdn_path(text);
    if (!p.listable) {
        result.error = p.problem;
        return result;
    }
    for (const PublishFile& f : existing) {
        if (f.path == text) {
            result.error = "That file is already in the list.";
            return result;
        }
        if (f.title == runtime_title(p.runtime, p.version)) {
            result.error = f.title + " is already in the list (" + f.path + "). Remove it first to replace it.";
            return result;
        }
    }
    PublishFile f;
    f.path = std::string(text);
    f.title = runtime_title(p.runtime, p.version);
    result.file = std::move(f);
    return result;
}

OfficialSource official_source(std::string_view path) {
    OfficialSource s;
    const CdnPath p = describe_cdn_path(path);
    if (!p.listable) {
        return s;
    }
    const std::string& v = p.version;
    s.name = p.file;
    using Kind = OfficialSource::Kind;
    if (p.runtime == "php") {
        s.kind = Kind::Sums;
        s.urls = {"https://downloads.php.net/~windows/releases/sha256sum.txt",
                  "https://downloads.php.net/~windows/releases/archives/sha256sum.txt"};
    } else if (p.runtime == "mariadb") {
        s.kind = Kind::Sums;
        s.urls = {"https://archive.mariadb.org/mariadb-" + v + "/winx64-packages/sha256sums.txt"};
    } else if (p.runtime == "node") {
        s.kind = Kind::Sums;
        s.urls = {"https://nodejs.org/dist/v" + v + "/SHASUMS256.txt"};
    } else if (p.runtime == "composer") {
        s.kind = Kind::Sums;
        s.urls = {"https://getcomposer.org/download/" + v + "/composer.phar.sha256sum"};
    } else if (p.runtime == "nginx") {
        s.kind = Kind::Original;
        s.urls = {"https://nginx.org/download/" + p.file};
    } else if (p.runtime == "wordpress") {
        s.kind = Kind::Original;
        s.urls = {"https://wordpress.org/" + p.file};
    } else if (p.runtime == "python") {
        // cpython-<v>-<date>-... on the CDN is cpython-<v>+<date>-... upstream.
        const std::string head = "cpython-" + v + "-";
        const std::string date = p.file.substr(head.size(), 8);
        s.kind = Kind::Sums;
        s.name = "cpython-" + v + "+" + p.file.substr(head.size());
        s.urls = {"https://github.com/astral-sh/python-build-standalone/releases/download/" + date +
                  "/SHA256SUMS"};
    } else if (p.runtime == "phpmyadmin") {
        // Each download has its own .sha256 next to it: "<hash>  <file>".
        s.kind = Kind::Sums;
        s.urls = {"https://files.phpmyadmin.net/phpMyAdmin/" + v + "/" + p.file + ".sha256"};
    } else if (p.runtime == "apache") {
        // Apache Lounge publishes no checksum list.
        s.kind = Kind::ByHand;
    }
    return s;
}

std::optional<std::string> find_official_hash(std::string_view sums, std::string_view name) {
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start <= sums.size();) {
        std::size_t end = sums.find('\n', start);
        if (end == std::string_view::npos) {
            end = sums.size();
        }
        std::string_view line = sums.substr(start, end - start);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        start = end + 1;
    }
    if (!name.empty()) {
        for (const std::string_view line : lines) {
            if (names_file(line, name)) {
                if (auto h = hash_on_line(line)) {
                    return h;
                }
            }
        }
    }
    // composer.phar.sha256sum and similar hold just the hash, on one line.
    std::vector<std::string_view> filled;
    for (const std::string_view line : lines) {
        if (line.find_first_not_of(" \t") != std::string_view::npos) {
            filled.push_back(line);
        }
    }
    if (filled.size() == 1) {
        std::string_view l = filled.front();
        while (l.starts_with(' ') || l.starts_with('\t')) {
            l.remove_prefix(1);
        }
        while (l.ends_with(' ') || l.ends_with('\t')) {
            l.remove_suffix(1);
        }
        if (l.size() == 64 && std::all_of(l.begin(), l.end(), is_hex)) {
            return lower(std::string(l));
        }
    }
    return std::nullopt;
}

void check_file(CheckContext& ctx, PublishFile& file) {
    file.state = CheckState::NotRun;
    file.detail.clear();
    const std::string url = ctx.cdn + "/" + file.path;

    if (file.published) {
        // Still served, at the size in the signed list (the app checks every
        // byte against the signed SHA-256 when it downloads it).
        HttpLimits limits;
        limits.max_time = std::chrono::seconds{60};
        limits.cancel = ctx.cancel;
        const HttpReply r = ctx.http.get(url, limits, [](std::span<const std::uint8_t>) { return false; });
        if (cancelled(ctx.cancel)) {
            file.detail = "Stopped";
            return;
        }
        if (r.status != 200) {
            file.state = CheckState::Failed;
            file.detail = "The CDN doesn't serve it any more (" + describe_failure(r) + ")";
        } else if (!r.content_length) {
            file.state = CheckState::Failed;
            file.detail = "The CDN didn't say the file's size";
        } else if (*r.content_length != file.size) {
            file.state = CheckState::Failed;
            file.detail = "The CDN serves a different size (" + std::to_string(*r.content_length) +
                          " bytes, the list says " + std::to_string(file.size) + ")";
        } else {
            file.state = CheckState::Ok;
            file.detail = "Still served";
        }
        return;
    }

    const Download got = download_and_hash(ctx, url);
    if (cancelled(ctx.cancel)) {
        file.detail = "Stopped";
        return;
    }
    if (got.too_big) {
        file.state = CheckState::Failed;
        file.detail = "Larger than 2 GB";
        return;
    }
    if (got.reply.status != 200 || got.reply.stopped || got.size == 0) {
        file.state = CheckState::Failed;
        file.detail = got.reply.status == 200
                          ? std::string("The CDN serves an empty file")
                          : "The CDN doesn't serve it (" + describe_failure(got.reply) +
                                "). Upload it to exactly this path.";
        return;
    }
    file.size = got.size;
    file.sha256 = got.sha256;

    const OfficialSource source = official_source(file.path);
    std::optional<std::string> official;
    std::string from;
    std::string why_not;
    if (source.kind == OfficialSource::Kind::Sums) {
        std::string fetch_error;
        bool listed_nowhere = true;
        for (const std::string& sums_url : source.urls) {
            auto cached = ctx.sums_cache.find(sums_url);
            if (cached == ctx.sums_cache.end()) {
                std::string error;
                auto text = get_text(ctx.http, sums_url, kMaxSumsBytes, ctx.cancel, error);
                if (cancelled(ctx.cancel)) {
                    file.detail = "Stopped";
                    return;
                }
                if (!text) {
                    // Not cached: the next run asks again.
                    fetch_error = error;
                    listed_nowhere = false;
                    continue;
                }
                cached = ctx.sums_cache.emplace(sums_url, std::move(text)).first;
            }
            if (const std::optional<std::string>& sums = cached->second; sums.has_value()) {
                official = find_official_hash(sums.value(), source.name);
                if (official) {
                    from = host_of(sums_url);
                    break;
                }
            }
        }
        if (!official) {
            why_not = listed_nowhere
                          ? "Its official checksum list (" + host_of(source.urls.front()) +
                                ") doesn't name " + source.name + ". Check the file name and version"
                          : "Can't read the official checksum list: " + fetch_error;
        }
    } else if (source.kind == OfficialSource::Kind::Original) {
        const Download orig = download_and_hash(ctx, source.urls.front());
        if (cancelled(ctx.cancel)) {
            file.detail = "Stopped";
            return;
        }
        if (orig.reply.status == 200 && !orig.reply.stopped && !orig.too_big) {
            official = orig.sha256;
            from = "the original from " + host_of(source.urls.front());
        } else {
            why_not = "Can't download the original to compare with (" + describe_failure(orig.reply) + ")";
        }
    }

    if (source.kind == OfficialSource::Kind::Unknown) {
        file.state = CheckState::Failed;
        file.detail = "FloServer Publisher doesn't know where this runtime's official checksums are; "
                      "add it to official_source() in tools/manifest-tool/publish.cpp.";
    } else if (source.kind == OfficialSource::Kind::ByHand) {
        // Only a project that publishes no checksum at all (Apache Lounge).
        file.state = CheckState::ByHand;
        file.detail = "Its project publishes no checksum list. Compare its SHA-256 with the one on its "
                      "download page: " + file.sha256;
    } else if (!official) {
        // A file with an official source is never signed without that source:
        // it fails, and the next Publish tries again.
        file.state = CheckState::Failed;
        file.detail = why_not + ".";
    } else if (*official == file.sha256) {
        file.state = CheckState::Ok;
        file.detail = "Matches " + from;
    } else {
        file.state = CheckState::Failed;
        file.detail = "DIFFERENT from the official file (" + from + "). Don't publish it.";
    }
}

S3Check check_direct_s3(HttpTransport& http, std::string_view s3_base, std::string_view path,
                        const std::atomic<bool>* cancel) {
    HttpLimits limits;
    limits.max_time = std::chrono::seconds{60};
    limits.cancel = cancel;
    const HttpReply r = http.get(std::string(s3_base) + "/" + std::string(path), limits,
                                 [](std::span<const std::uint8_t>) { return false; });
    S3Check check;
    check.server_date = r.server_date;
    if (r.status == 200) {
        check.problem = "The S3 bucket answers direct reads: make it private, so only CloudFront can read it.";
    } else if (r.status != 403) {
        check.problem = "A direct S3 read gave " + describe_failure(r) + " instead of 403; check the bucket by hand.";
    }
    return check;
}

std::string clock_problem(std::optional<std::chrono::sys_seconds> server_date, std::chrono::sys_seconds now) {
    if (!server_date) {
        return "Can't check this computer's clock (no time from the server). Try again.";
    }
    const auto off = std::chrono::abs(*server_date - now);
    if (off > std::chrono::minutes{10}) {
        return "This computer's clock is " +
               std::to_string(std::chrono::duration_cast<std::chrono::minutes>(off).count()) +
               " minutes off. Set the clock right, then publish.";
    }
    return {};
}

SuccessorCheck check_successor(const LiveList& live, std::string_view templates_json,
                               std::chrono::sys_seconds now) {
    SuccessorCheck check;
    if (live.expired) {
        if (now - live.expires > std::chrono::days{kMaxExpiryDays}) {
            check.stop.push_back("The live list (sequence " + std::to_string(live.sequence) + ") expired on " +
                                 format_utc_time(live.expires).substr(0, 10) +
                                 ", more than 90 days ago. It may be an old copy; check the bucket and "
                                 "publish with floserver-manifest instead.");
        } else {
            check.confirm.push_back("The live list (sequence " + std::to_string(live.sequence) + ") expired on " +
                                    format_utc_time(live.expires).substr(0, 10) +
                                    ". Is that the latest list you published?");
        }
    }
    const json live_templates = json::parse(live.templates_json, nullptr, false);
    const json next_templates = json::parse(templates_json, nullptr, false);
    if (live_templates != next_templates) {
        check.confirm.push_back("The project templates built into this Publisher differ from the live list's (" +
                                std::to_string(live.template_count) +
                                " there). The new list will carry this Publisher's. Is this Publisher the newest build?");
    }
    return check;
}

BuiltList build_next_list(std::string_view cdn, const std::vector<PublishFile>& files,
                          std::int64_t sequence, std::chrono::sys_seconds now, int days) {
    BuiltList built;
    BuildOptions options;
    options.sequence = sequence;
    options.days = days;
    options.now = now;
    options.templates_json = std::string(kBuiltInTemplates);

    nlohmann::ordered_json list = nlohmann::ordered_json::array();
    for (const PublishFile& f : files) {
        std::string result;
        if (f.state == CheckState::Ok) {
            result = "ok";
        } else if (f.state == CheckState::ByHand && f.accepted) {
            result = "unchecked";
            options.accepted.insert(f.path);
        } else {
            built.errors.push_back(f.title + ": " +
                                   (f.state == CheckState::NotRun   ? std::string("not checked yet")
                                    : f.state == CheckState::ByHand ? std::string("compare it by hand and tick it")
                                                                    : f.detail));
            continue;
        }
        nlohmann::ordered_json entry = nlohmann::ordered_json::object();
        entry["path"] = f.path;
        entry["size"] = f.size;
        entry["sha256"] = f.sha256;
        entry["result"] = result;
        list.push_back(std::move(entry));
    }
    if (!built.errors.empty()) {
        return built;
    }
    nlohmann::ordered_json report = nlohmann::ordered_json::object();
    report["cdn"] = std::string(cdn);
    report["only"] = "";
    report["files"] = std::move(list);

    BuildResult r = build_manifest(report.dump(), options);
    built.errors = std::move(r.errors);
    built.notes = std::move(r.notes);
    built.manifest_json = std::move(r.manifest_json);
    built.sequence = sequence;
    built.runtime_count = r.runtime_count;
    built.template_count = r.template_count;
    built.expires = now + std::chrono::days{days};
    return built;
}

std::string format_size(std::uint64_t bytes) {
    std::array<char, 32> buf{};
    int n = 0;
    if (bytes >= 1'000'000) {
        n = std::snprintf(buf.data(), buf.size(), "%.1f MB", static_cast<double>(bytes) / 1e6);
    } else {
        n = std::snprintf(buf.data(), buf.size(), "%.0f KB", static_cast<double>(bytes) / 1e3);
    }
    return n > 0 ? std::string(buf.data()) : std::string();
}

}  // namespace floserver::manifest_tool

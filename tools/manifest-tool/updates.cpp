#include "updates.h"

#include "publish.h"

#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <regex>
#include <set>

namespace floserver::manifest_tool {

namespace {

using nlohmann::json;
using core::Version;

constexpr std::size_t kMaxAnswer = std::size_t{8} * 1024 * 1024;  // python-build-standalone's release is ~2 MB

// The order the Publisher lists runtimes in (its catalog's), for the rows.
constexpr std::array<std::string_view, 17> kOrder = {
    "php",    "mariadb",    "mysql",   "postgresql", "nginx",  "apache",  "composer", "node",     "bun",
    "deno",   "python",     "wordpress", "phpmyadmin", "mailpit", "garnet", "dotnet",   "php-redis"};

std::size_t order_of(std::string_view runtime) {
    const auto it = std::find(kOrder.begin(), kOrder.end(), runtime);
    return static_cast<std::size_t>(it - kOrder.begin());
}

// "8.5.13" -> "8.5" (2 parts); "24.22.1" -> "24" (1 part).
std::string first_parts(std::string_view version, std::size_t parts) {
    std::size_t end = 0;
    for (std::size_t i = 0; i < parts; ++i) {
        end = version.find('.', i == 0 ? 0 : end + 1);
        if (end == std::string_view::npos) {
            return std::string(version);
        }
    }
    return std::string(version.substr(0, end));
}

bool newer(std::string_view a, std::string_view b) {
    const auto va = Version::parse(a);
    const auto vb = Version::parse(b);
    if (!va || !vb) {
        return a > b;
    }
    return *vb < *va;
}

// The newest version per series of `versions`.
std::vector<UpstreamVersion> newest_by_series(std::string_view runtime, const std::vector<std::string>& versions) {
    std::map<std::string, std::string> best;
    for (const std::string& v : versions) {
        if (!Version::parse(v)) {
            continue;
        }
        const std::string s = update_series(runtime, v);
        auto& slot = best[s];
        if (slot.empty() || newer(v, slot)) {
            slot = v;
        }
    }
    std::vector<UpstreamVersion> out;
    out.reserve(best.size());
    for (const auto& [s, v] : best) {
        out.push_back({s, v});
    }
    return out;
}

std::optional<json> parse_json(std::string_view text) {
    try {
        return json::parse(text);
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

std::string text_of(const json& j, const char* key) {
    return j.is_object() && j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string();
}

std::vector<std::string> regex_versions(std::string_view text, const std::regex& re, std::size_t group = 1) {
    std::vector<std::string> out;
    const std::string s(text);
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) {
        out.push_back((*it)[group].str());
    }
    return out;
}

// "PostgreSQL 17", "Bun".
std::string title_of(std::string_view runtime, std::string_view series) {
    std::string t = runtime_title(runtime, series, "");
    while (!t.empty() && t.back() == ' ') {
        t.pop_back();
    }
    return t;
}

}  // namespace

std::string update_series(std::string_view runtime, std::string_view version) {
    if (runtime == "node" || runtime == "postgresql") {
        return first_parts(version, 1);
    }
    if (runtime == "bun") {
        // A project names its Bun as "1.3" (D31), and 1.4 can change what it
        // runs: a new minor is a new series, for you to choose, not an update.
        return first_parts(version, 2);
    }
    if (runtime == "deno" || runtime == "mailpit" || runtime == "garnet" ||
        runtime == "phpmyadmin" || runtime == "php-redis") {
        return {};
    }
    return first_parts(version, 2);
}

std::vector<UpstreamVersion> parse_php_sums(std::string_view text) {
    // A rebuild has a number after the version ("php-8.5.12-1-nts-...").
    static const std::regex re(R"(php-(\d+\.\d+\.\d+)(?:-\d+)?-nts-Win32-vs1[67]-x64\.zip)");
    return newest_by_series("php", regex_versions(text, re));
}

std::vector<UpstreamVersion> parse_node_index(std::string_view text) {
    const auto doc = parse_json(text);
    std::vector<std::string> versions;
    if (doc && doc->is_array()) {
        for (const json& r : *doc) {
            // "lts": false, or the LTS line's name ("Krypton").
            const bool lts = r.is_object() && r.contains("lts") && r.at("lts").is_string();
            bool windows = false;
            if (r.is_object() && r.contains("files") && r.at("files").is_array()) {
                for (const json& f : r.at("files")) {
                    windows = windows || (f.is_string() && f.get<std::string>() == "win-x64-zip");
                }
            }
            const std::string v = text_of(r, "version");
            if (lts && windows && v.starts_with('v')) {
                versions.push_back(v.substr(1));
            }
        }
    }
    return newest_by_series("node", versions);
}

std::vector<std::string> parse_mariadb_majors(std::string_view text) {
    std::vector<std::string> out;
    const auto doc = parse_json(text);
    if (!doc || !doc->is_object() || !doc->contains("major_releases") || !doc->at("major_releases").is_array()) {
        return out;
    }
    for (const json& m : doc->at("major_releases")) {
        if (text_of(m, "release_status") == "Stable" &&
            text_of(m, "release_support_type").find("Long Term") != std::string::npos) {
            out.push_back(text_of(m, "release_id"));
        }
    }
    return out;
}

std::optional<std::string> parse_mariadb_latest(std::string_view text) {
    const auto doc = parse_json(text);
    if (!doc || !doc->is_object() || !doc->contains("releases") || !doc->at("releases").is_object()) {
        return std::nullopt;
    }
    std::optional<std::string> best;
    for (const auto& [id, release] : doc->at("releases").items()) {
        if (Version::parse(id) && (!best || newer(id, *best))) {
            best = id;
        }
    }
    return best;
}

std::vector<UpstreamVersion> parse_composer_versions(std::string_view text) {
    const auto doc = parse_json(text);
    std::vector<std::string> versions;
    if (doc && doc->is_object()) {
        // "stable": the newest; "2.2": the LTS line for old PHP. Both are offered.
        for (const auto& [key, list] : doc->items()) {
            if ((key != "stable" && key != "2.2") || !list.is_array()) {
                continue;
            }
            for (const json& r : list) {
                const std::string v = text_of(r, "version");
                if (v.starts_with("2.")) {
                    versions.push_back(v);
                }
            }
        }
    }
    return newest_by_series("composer", versions);
}

std::vector<UpstreamVersion> parse_nginx_page(std::string_view html) {
    static const std::regex re(R"(nginx-(\d+\.(\d+)\.\d+)\.zip)");
    std::vector<std::string> stable;
    const std::string s(html);
    for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) {
        // Even minor: a stable branch (1.30); odd: mainline (1.31).
        if (std::stoi((*it)[2].str()) % 2 == 0) {
            stable.push_back((*it)[1].str());
        }
    }
    return newest_by_series("nginx", stable);
}

std::vector<UpstreamVersion> parse_postgres_versions(std::string_view text) {
    std::vector<UpstreamVersion> out;
    const auto doc = parse_json(text);
    if (!doc || !doc->is_array()) {
        return out;
    }
    for (const json& r : *doc) {
        if (!r.is_object() || !r.contains("supported") || !r.at("supported").is_boolean() || !r.at("supported").get<bool>()) {
            continue;
        }
        const std::string major = text_of(r, "major");
        // "latestMinor" is a number in some answers and text in others.
        std::string minor;
        if (r.contains("latestMinor")) {
            const json& m = r.at("latestMinor");
            minor = m.is_string() ? m.get<std::string>() : m.is_number_integer() ? std::to_string(m.get<int>()) : "";
        }
        std::string version = major;
        version.append(".").append(minor);
        if (!major.empty() && !minor.empty() && Version::parse(version)) {
            out.push_back({major, std::move(version)});
        }
    }
    return out;
}

std::vector<UpstreamVersion> parse_wordpress_stable_check(std::string_view text) {
    const auto doc = parse_json(text);
    std::vector<std::string> versions;
    if (doc && doc->is_object()) {
        for (const auto& [v, state] : doc->items()) {
            if (state.is_string() && (state.get<std::string>() == "latest" || state.get<std::string>() == "outdated")) {
                versions.push_back(v);
            }
        }
    }
    return newest_by_series("wordpress", versions);
}

std::optional<std::string> parse_phpmyadmin_version(std::string_view text) {
    const auto doc = parse_json(text);
    const std::string v = doc ? text_of(*doc, "version") : std::string();
    return Version::parse(v) ? std::optional<std::string>(v) : std::nullopt;
}

std::optional<std::string> parse_github_latest(std::string_view text, std::string_view tag_prefix) {
    const auto doc = parse_json(text);
    if (!doc || !doc->is_object()) {
        return std::nullopt;
    }
    const auto flag = [&doc](const char* key) {
        return doc->contains(key) && doc->at(key).is_boolean() && doc->at(key).get<bool>();
    };
    std::string tag = text_of(*doc, "tag_name");
    if (flag("prerelease") || flag("draft") || !tag.starts_with(tag_prefix)) {
        return std::nullopt;
    }
    tag.erase(0, tag_prefix.size());
    return Version::parse(tag) ? std::optional<std::string>(tag) : std::nullopt;
}

std::vector<UpstreamVersion> parse_python_release(std::string_view text) {
    static const std::regex re(R"(^cpython-(\d+\.\d+\.\d+)\+\d{8}-x86_64-pc-windows-msvc-install_only_stripped\.tar\.gz$)");
    const auto doc = parse_json(text);
    std::vector<std::string> versions;
    if (doc && doc->is_object() && doc->contains("assets") && doc->at("assets").is_array()) {
        for (const json& a : doc->at("assets")) {
            const std::string name = text_of(a, "name");
            std::smatch m;
            if (std::regex_match(name, m, re)) {
                versions.push_back(m[1].str());
            }
        }
    }
    return newest_by_series("python", versions);
}

std::vector<UpstreamVersion> parse_apachelounge_page(std::string_view html) {
    // "Win64" has been "win64" on some releases (2.4.63).
    static const std::regex re(R"(httpd-(\d+\.\d+\.\d+)-\d+-[Ww]in64-VS\d+\.zip)");
    return newest_by_series("apache", regex_versions(html, re));
}

std::optional<std::string> parse_dotnet_releases(std::string_view text) {
    const auto doc = parse_json(text);
    const std::string v = doc ? text_of(*doc, "latest-runtime") : std::string();
    return Version::parse(v) ? std::optional<std::string>(v) : std::nullopt;
}

std::optional<std::string> parse_mysql_page(std::string_view html) {
    // The Windows zip's name: never a number elsewhere on the page.
    static const std::regex re(R"(mysql-(8\.4\.\d+)-winx64\.zip)");
    std::optional<std::string> best;
    for (const std::string& v : regex_versions(html, re)) {
        if (!best || newer(v, *best)) {
            best = v;
        }
    }
    return best;
}

std::optional<std::string> parse_pecl_stable(std::string_view text) {
    std::string v(text);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) {
        v.pop_back();
    }
    return Version::parse(v) ? std::optional<std::string>(v) : std::nullopt;
}

bool update_series_wanted(std::string_view runtime, std::string_view series,
                          const std::vector<std::string>& listed_series) {
    const auto at_least = [&series](std::string_view floor) {
        const auto s = Version::parse(series);
        const auto f = Version::parse(floor);
        return s && f && !(*s < *f);
    };
    if (runtime == "php") {
        return at_least("8.2");
    }
    if (runtime == "mariadb") {
        return at_least("11.4");  // its finder offers long-term series only
    }
    if (runtime == "mysql") {
        return series == "8.4";  // D28: the 8.4 LTS only
    }
    if (runtime == "postgresql") {
        return at_least("17");
    }
    if (runtime == "node") {
        return at_least("22");  // its finder offers LTS lines only
    }
    if (runtime == "python") {
        return at_least("3.12");
    }
    // Others: only a series newer than any listed (a new nginx stable branch,
    // a new Composer or WordPress minor).
    return std::all_of(listed_series.begin(), listed_series.end(),
                       [&series](const std::string& l) { return newer(series, l); });
}

std::vector<UpdateRow> compare_updates(const std::map<std::string, std::vector<std::string>>& listed,
                                       const std::map<std::string, FinderAnswer>& found) {
    std::vector<UpdateRow> out;
    for (const auto& [runtime, versions] : listed) {
        if (order_of(runtime) >= kOrder.size()) {
            continue;  // FloServer's own installer and anything else without a finder
        }
        // The newest listed version of each series.
        std::map<std::string, std::string> have;
        std::vector<std::string> listed_series;
        for (const std::string& v : versions) {
            const std::string s = update_series(runtime, v);
            auto& slot = have[s];
            if (slot.empty() || newer(v, slot)) {
                slot = v;
            }
        }
        listed_series.reserve(have.size());
        for (const auto& [s, v] : have) {
            listed_series.push_back(s);
        }
        const auto f = found.find(runtime);
        if (f == found.end() || !f->second.problem.empty()) {
            UpdateRow row;
            row.runtime = runtime;
            row.title = title_of(runtime, "");
            row.kind = UpdateKind::Failed;
            row.detail = f == found.end() ? "not checked" : f->second.problem;
            out.push_back(std::move(row));
            continue;
        }
        // A site that names none of the listed series: its answer has changed
        // (or isn't what was asked for), never "up to date".
        // (A new series FloServer would offer counts: Composer names only its
        // newest minor and 2.2.)
        const bool any_listed = std::any_of(f->second.versions.begin(), f->second.versions.end(),
                                            [&](const UpstreamVersion& u) {
                                                return have.contains(u.series) ||
                                                       update_series_wanted(runtime, u.series, listed_series);
                                            });
        if (!any_listed) {
            UpdateRow row;
            row.runtime = runtime;
            row.title = title_of(runtime, "");
            row.kind = UpdateKind::Failed;
            row.detail = f->second.source + " named none of the listed series (the site may have changed)";
            out.push_back(std::move(row));
            continue;
        }
        std::set<std::string> seen;
        for (const UpstreamVersion& u : f->second.versions) {
            const auto h = have.find(u.series);
            UpdateRow row;
            row.runtime = runtime;
            row.series = u.series;
            row.title = title_of(runtime, u.series);
            row.latest = u.version;
            if (h != have.end()) {
                seen.insert(u.series);
                row.listed = h->second;
                row.kind = newer(u.version, h->second) ? UpdateKind::Update : UpdateKind::UpToDate;
                row.ticked = row.kind == UpdateKind::Update;
                if (row.kind == UpdateKind::Update && runtime == "php-redis") {
                    // PECL names the source release; its Windows builds can come later.
                    row.ticked = false;
                    row.detail = "check that its Windows builds are out for each PHP series";
                }
            } else if (update_series_wanted(runtime, u.series, listed_series)) {
                row.kind = UpdateKind::NewSeries;
            } else {
                continue;
            }
            out.push_back(std::move(row));
        }
        // A listed series the site no longer names (ended): shown as it is.
        for (const auto& [s, v] : have) {
            if (!seen.contains(s)) {
                UpdateRow row;
                row.runtime = runtime;
                row.series = s;
                row.title = title_of(runtime, s);
                row.listed = v;
                row.latest = v;
                row.kind = UpdateKind::UpToDate;
                row.detail = "no longer named on its site";
                out.push_back(std::move(row));
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const UpdateRow& a, const UpdateRow& b) {
        if (a.runtime != b.runtime) {
            return order_of(a.runtime) < order_of(b.runtime);
        }
        return newer(a.series, b.series);
    });
    return out;
}

namespace {

std::optional<std::string> ask(core::HttpTransport& http, const std::string& url, const std::atomic<bool>* cancel,
                               std::string& error) {
    std::string body;
    bool too_big = false;
    core::HttpLimits limits;
    limits.max_time = std::chrono::seconds{60};
    limits.cancel = cancel;
    const core::HttpReply reply = http.get(url, limits, [&](std::span<const std::uint8_t> chunk) {
        if (body.size() + chunk.size() > kMaxAnswer) {
            too_big = true;
            return false;
        }
        body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        return true;
    });
    if (too_big) {
        error = "its answer is larger than expected";
        return std::nullopt;
    }
    if (reply.status != 200 || reply.stopped) {
        if ((reply.status == 403 || reply.status == 429) && url.starts_with("https://api.github.com/")) {
            error = "GitHub's hourly limit for checks without an account is used up; try again in an hour";
        } else {
            error = reply.status == 0 ? (reply.error.empty() ? "no answer" : reply.error)
                                      : "HTTP " + std::to_string(reply.status);
        }
        return std::nullopt;
    }
    return body;
}

// Whether a file is there: its first bytes are asked for and the rest left.
// The HTTP status, or 0 when nothing answered.
// `zip`: its first bytes are a zip's ("PK\3\4"), not a page it was sent to.
int exists(core::HttpTransport& http, const std::string& url, const std::atomic<bool>* cancel, bool& zip) {
    core::HttpLimits limits;
    limits.max_time = std::chrono::seconds{30};
    limits.cancel = cancel;
    zip = false;
    const core::HttpReply reply = http.get(url, limits, [&zip](std::span<const std::uint8_t> chunk) {
        zip = chunk.size() >= 4 && chunk[0] == 'P' && chunk[1] == 'K' && chunk[2] == 3 && chunk[3] == 4;
        return false;
    });
    return reply.status;
}

}  // namespace

std::size_t runtimes_to_ask(const std::map<std::string, std::vector<std::string>>& listed) {
    return static_cast<std::size_t>(std::count_if(listed.begin(), listed.end(), [](const auto& entry) {
        return order_of(entry.first) < kOrder.size();
    }));
}

std::map<std::string, FinderAnswer> find_updates(core::HttpTransport& http,
                                                 const std::map<std::string, std::vector<std::string>>& listed,
                                                 const std::atomic<bool>* cancel,
                                                 const std::function<void(std::string_view)>& progress,
                                                 const std::string& save_dir) {
    std::map<std::string, FinderAnswer> out;
    // One address per runtime, then how to read it. A runtime whose answer
    // is two steps (MariaDB) reads the second in its own step.
    const auto get = [&](FinderAnswer& a, const std::string& url) -> std::optional<std::string> {
        a.source = url;
        std::string error;
        auto text = ask(http, url, cancel, error);
        if (!text) {
            a.problem = url + ": " + error;
        } else if (!save_dir.empty()) {
            std::string name = url.substr(url.find("//") + 2);
            std::replace_if(name.begin(), name.end(), [](char c) { return c == '/' || c == '?' || c == ':'; }, '_');
            std::string ignored;
            (void)core::write_file_atomically(core::path_from_utf8(save_dir) / core::path_from_utf8(name), *text, ignored);
        }
        return text;
    };
    const auto one = [](std::string_view runtime, std::optional<std::string> v) {
        return v ? std::vector<UpstreamVersion>{{update_series(runtime, *v), *v}} : std::vector<UpstreamVersion>{};
    };
    for (const auto& [runtime, versions] : listed) {
        if (cancel != nullptr && cancel->load()) {
            break;
        }
        if (order_of(runtime) >= kOrder.size()) {
            continue;
        }
        if (progress) {
            progress(runtime);
        }
        FinderAnswer a;
        std::vector<UpstreamVersion> got;
        bool read = true;
        const auto via = [&](const std::string& url, const auto& parse) {
            if (const auto text = get(a, url)) {
                got = parse(*text);
            } else {
                read = false;
            }
        };
        if (runtime == "php") {
            via("https://downloads.php.net/~windows/releases/sha256sum.txt", parse_php_sums);
        } else if (runtime == "node") {
            via("https://nodejs.org/dist/index.json", parse_node_index);
        } else if (runtime == "composer") {
            via("https://getcomposer.org/versions", parse_composer_versions);
        } else if (runtime == "nginx") {
            via("https://nginx.org/en/download.html", parse_nginx_page);
        } else if (runtime == "postgresql") {
            via("https://www.postgresql.org/versions.json", parse_postgres_versions);
        } else if (runtime == "wordpress") {
            via("https://api.wordpress.org/core/stable-check/1.0/", parse_wordpress_stable_check);
        } else if (runtime == "apache") {
            via("https://www.apachelounge.com/download/", parse_apachelounge_page);
        } else if (runtime == "python") {
            via("https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest", parse_python_release);
        } else if (runtime == "phpmyadmin") {
            via("https://www.phpmyadmin.net/home_page/version.json",
                [&](std::string_view t) { return one(runtime, parse_phpmyadmin_version(t)); });
        } else if (runtime == "mysql") {
            // dev.mysql.com doesn't answer programs reliably: Oracle's download
            // server is asked for the next 8.4 patches' zips instead.
            std::optional<Version> newest;
            for (const std::string& v : versions) {
                const auto parsed = Version::parse(v);
                if (parsed && parsed->parts().size() == 3 && update_series("mysql", v) == "8.4" &&
                    (!newest || *newest < *parsed)) {
                    newest = parsed;
                }
            }
            a.source = "https://cdn.mysql.com/Downloads/MySQL-8.4/";
            if (!newest) {
                a.problem = "no MySQL 8.4 version in the list to look from";
                read = false;
            } else {
                // Every one of the next patches: an older one is moved to Oracle's
                // archive when a newer comes out, so a gap doesn't end the search.
                const auto patch = newest->parts().back();
                std::string latest = newest->to_string();
                for (auto next = patch + 1; next <= patch + 8 && read; ++next) {
                    const std::string v = "8.4." + std::to_string(next);
                    const std::string url = a.source + "mysql-" + v + "-winx64.zip";
                    bool zip = false;
                    const int status = exists(http, url, cancel, zip);
                    if (status == 200 && zip) {
                        latest = v;
                    } else if (status != 404) {
                        a.problem = url + (status == 0 ? ": no answer" : ": HTTP " + std::to_string(status) +
                                                                            (status == 200 ? " but not a zip" : ""));
                        read = false;
                    }
                }
                if (read) {
                    got.push_back({"8.4", latest});
                }
            }
        } else if (runtime == "php-redis") {
            via("https://pecl.php.net/rest/r/redis/stable.txt", [&](std::string_view t) { return one(runtime, parse_pecl_stable(t)); });
        } else if (runtime == "dotnet") {
            // Garnet's runtime: the channel of the listed one ("10.0").
            const std::string channel = versions.empty() ? std::string("10.0") : update_series("dotnet", versions.front());
            via("https://builds.dotnet.microsoft.com/dotnet/release-metadata/" + channel + "/releases.json",
                [&](std::string_view t) {
                    const auto v = parse_dotnet_releases(t);
                    return v ? std::vector<UpstreamVersion>{{channel, *v}} : std::vector<UpstreamVersion>{};
                });
        } else if (runtime == "mariadb") {
            if (const auto majors = get(a, "https://downloads.mariadb.org/rest-api/mariadb/")) {
                for (const std::string& series : parse_mariadb_majors(*majors)) {
                    if (!update_series_wanted("mariadb", series, {})) {
                        continue;
                    }
                    const auto latest = get(a, "https://downloads.mariadb.org/rest-api/mariadb/" + series + "/latest/");
                    if (!latest) {
                        read = false;
                        break;
                    }
                    if (const auto v = parse_mariadb_latest(*latest)) {
                        got.push_back({series, *v});
                    }
                }
            } else {
                read = false;
            }
        } else {
            // GitHub releases: Bun, Deno, Mailpit, Garnet.
            const std::map<std::string, std::pair<std::string, std::string>> github = {
                {"bun", {"oven-sh/bun", "bun-v"}},
                {"deno", {"denoland/deno", "v"}},
                {"mailpit", {"axllent/mailpit", "v"}},
                {"garnet", {"microsoft/garnet", "v"}},
            };
            const auto g = github.find(runtime);
            if (g == github.end()) {
                continue;
            }
            via("https://api.github.com/repos/" + g->second.first + "/releases/latest",
                [&](std::string_view t) { return one(runtime, parse_github_latest(t, g->second.second)); });
        }
        if (read && got.empty() && a.problem.empty()) {
            a.problem = a.source + ": no version could be read from its answer (the site may have changed)";
        } else if (read) {
            a.problem.clear();
        }
        a.versions = std::move(got);
        out[runtime] = std::move(a);
    }
    return out;
}

}  // namespace floserver::manifest_tool

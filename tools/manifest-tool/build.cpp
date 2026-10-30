#include "build.h"

#include "floserver/core/encoding.h"
#include "floserver/core/manifest.h"
#include "floserver/core/version.h"

#include <monocypher-ed25519.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <regex>

namespace floserver::manifest_tool {

namespace {

using nlohmann::json;
using namespace floserver::core;

struct CatalogEntry {
    std::string_view name;
    // Files of this runtime that are deliberately not listed (other builds of
    // the same release); anything else that doesn't match file_pattern is an error.
    std::string_view other_builds;
    std::string_view license;
    std::string_view platform;
    std::string_view variant;
    // File name pattern; {v} is replaced by the version from the folder name.
    std::string_view file_pattern;
    // Official download address for the fallback, or empty when there is no
    // stable direct link. {v} = version, {file} = file name.
    std::string_view upstream;
};

// The runtimes FloServer 1.0 knows (Windows x64). Add a row here before
// publishing a new kind of runtime.
constexpr std::array<CatalogEntry, 8> kCatalog = {{
    {"php", R"(php-{v}(-\d+)?-Win32-vs1[67]-x64\.zip)", "PHP-3.01", "windows-x64", "nts", R"(php-{v}(-\d+)?-nts-Win32-vs1[67]-x64\.zip)",
     "https://downloads.php.net/~windows/releases/archives/{file}"},
    {"mariadb", "", "GPL-2.0", "windows-x64", "", R"(mariadb-{v}-winx64\.zip)",
     "https://archive.mariadb.org/mariadb-{v}/winx64-packages/{file}"},
    {"node", "", "MIT", "windows-x64", "", R"(node-v{v}-win-x64\.zip)",
     "https://nodejs.org/dist/v{v}/{file}"},
    {"composer", "", "MIT", "any", "", R"(composer\.phar)", "https://getcomposer.org/download/{v}/{file}"},
    {"nginx", "", "BSD-2-Clause", "windows-x64", "", R"(nginx-{v}\.zip)",
     "https://nginx.org/download/{file}"},
    {"apache", "", "Apache-2.0", "windows-x64", "", R"(httpd-{v}-\d+-Win64-VS1[78]\.zip)", ""},
    // The WordPress template's files (D9), copied into each new project.
    {"wordpress", "", "GPL-2.0", "any", "", R"(wordpress-{v}\.zip)", "https://wordpress.org/{file}"},
    // python-build-standalone (D17). Its own names have "+" where the CDN's
    // have "-", so there's no fallback address built from the CDN name.
    {"python", "", "PSF-2.0", "windows-x64", "",
     R"(cpython-{v}-\d{8}-x86_64-pc-windows-msvc-install_only_stripped\.tar\.gz)", ""},
}};

std::string join(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (const std::string_view p : parts) {
        out += p;
    }
    return out;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    for (std::size_t pos = text.find(from); pos != std::string::npos;
         pos = text.find(from, pos + to.size())) {
        text.replace(pos, from.size(), to);
    }
    return text;
}

std::string regex_escape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (std::string_view(R"(\^$.|?*+()[]{})").find(c) != std::string_view::npos) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

// https://<lower-case host>[/<path>], no query, percent signs or dot segments:
// the base every listed URL is built on.
bool is_cdn_base(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    if (!url.starts_with(kScheme) || url.size() > 200) {
        return false;
    }
    const std::string_view rest = url.substr(kScheme.size());
    const std::string_view host = rest.substr(0, rest.find('/'));
    if (host.empty() || host.front() == '.' || host.back() == '.' || host.front() == '-' ||
        !std::all_of(host.begin(), host.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        })) {
        return false;
    }
    const std::string_view path = rest.substr(host.size());
    if (!std::all_of(path.begin(), path.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '/' || c == '-' || c == '_' || c == '.';
        })) {
        return false;
    }
    const std::string padded = std::string(path) + "/";
    return padded.find("/./") == std::string::npos && padded.find("/../") == std::string::npos &&
           padded.find("//") == std::string::npos;
}

std::string archive_of(std::string_view file) {
    if (file.ends_with(".zip")) {
        return "zip";
    }
    if (file.ends_with(".tar.gz")) {
        return "tar.gz";
    }
    if (file.ends_with(".tar.xz")) {
        return "tar.xz";
    }
    return "file";
}

// Runs the finished list through the app's own checks, signed with a
// throwaway key, so the tool never writes a list the app would refuse.
std::optional<std::string> self_check(const std::string& bytes, std::chrono::sys_seconds now,
                                      std::size_t expected, std::size_t expected_templates) {
    std::array<std::uint8_t, 32> seed{};
    seed.fill(0x5a);
    std::array<std::uint8_t, 64> secret{};
    Ed25519PublicKey pub{};
    crypto_ed25519_key_pair(secret.data(), pub.data(), seed.data());
    std::array<std::uint8_t, 64> sig{};
    crypto_ed25519_sign(sig.data(), secret.data(),
                        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    ManifestCheck check;
    check.trusted_keys = {{"self-check", pub}};
    check.now = now;
    const auto r = open_manifest(
        bytes, json{{"key", "self-check"}, {"signature", to_base64(sig)}}.dump(), check);
    if (!r.manifest) {
        return "the list fails the app's own checks: " + r.message;
    }
    if (!r.warnings.empty()) {
        return "the app would skip an entry: " + r.warnings.front();
    }
    if (r.manifest->runtimes.size() != expected) {
        return "the app reads a different number of runtimes than were written";
    }
    if (r.manifest->templates.size() != expected_templates) {
        return "the app reads a different number of templates than were written";
    }
    return std::nullopt;
}

struct Classified {
    CdnPath parts;
    const CatalogEntry* entry = nullptr;  // set when the list can carry the file
    bool note = false;                    // left out on purpose (not an error)
    std::string message;
};

Classified classify(const std::string& path) {
    Classified c;
    // <runtime>/<version>/<file>
    const auto first = path.find('/');
    const auto second = first == std::string::npos ? first : path.find('/', first + 1);
    if (second == std::string::npos || path.find('/', second + 1) != std::string::npos) {
        c.message = path + ": expected <runtime>/<version>/<file>";
        return c;
    }
    c.parts.runtime = path.substr(0, first);
    c.parts.version = path.substr(first + 1, second - first - 1);
    c.parts.file = path.substr(second + 1);
    const std::string& name = c.parts.runtime;
    const std::string& version = c.parts.version;
    const std::string& file = c.parts.file;

    const auto entry = std::find_if(kCatalog.begin(), kCatalog.end(),
                                     [&](const CatalogEntry& e) { return e.name == name; });
    if (entry == kCatalog.end()) {
        c.message = join({path, ": \"", name, "\" isn't a runtime FloServer knows; add it to the catalog"});
        return c;
    }
    if (!Version::parse(version)) {
        c.message = join({path, ": \"", version, "\" isn't a version folder such as 8.5.12"});
        return c;
    }
    const std::regex pattern(
        replace_all(std::string(entry->file_pattern), "{v}", regex_escape(version)));
    if (!std::regex_match(file, pattern)) {
        const bool source = file.ends_with(".tar.gz") || file.ends_with(".tar.xz");
        const bool other_build =
            !entry->other_builds.empty() &&
            std::regex_match(file, std::regex(replace_all(std::string(entry->other_builds), "{v}",
                                                          regex_escape(version))));
        if (source || other_build) {
            c.note = true;
            c.message = join({path, source ? ": source archive, not listed"
                                           : ": another build of this release, not listed"});
        } else {
            c.message = join({path, ": the file name doesn't match a ", name, " ", version,
                              " Windows download; check the name and the version folder"});
        }
        return c;
    }
    c.entry = &*entry;
    return c;
}

}  // namespace

CdnPath describe_cdn_path(std::string_view path) {
    Classified c = classify(std::string(path));
    c.parts.listable = c.entry != nullptr;
    c.parts.problem = std::move(c.message);
    return std::move(c.parts);
}

std::string format_utc_time(std::chrono::sys_seconds t) {
    using namespace std::chrono;
    const sys_days day = floor<days>(t);
    const year_month_day ymd{day};
    const hh_mm_ss<seconds> time{t - day};
    std::array<char, 32> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%04d-%02u-%02uT%02d:%02d:%02dZ", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                  static_cast<int>(time.hours().count()), static_cast<int>(time.minutes().count()),
                  static_cast<int>(time.seconds().count()));
    return n > 0 ? std::string(buf.data()) : std::string();
}

BuildResult build_manifest(std::string_view report_json, const BuildOptions& options) {
    BuildResult result;
    const auto fail = [&](std::string message) {
        result.errors.push_back(std::move(message));
    };

    if (options.sequence < 1) {
        fail("--sequence must be 1 or more, and higher than the last published list");
    }
    if (options.days < 1 || options.days > kMaxExpiryDays) {
        fail("--days must be from 1 to " + std::to_string(kMaxExpiryDays));
    }
    const json report = json::parse(report_json, nullptr, /*allow_exceptions=*/false);
    if (!report.is_object() || !report.contains("files") || !report["files"].is_array()) {
        fail("the report isn't a FloServer CDN check report");
        return result;
    }
    if (report.contains("only") && !(report["only"].is_string() && report["only"].get<std::string>().empty())) {
        fail("the report checked only some files (-Only); a published list must come from a full check");
        return result;
    }
    std::string cdn;
    if (report.contains("cdn") && report["cdn"].is_string()) {
        cdn = report["cdn"].get<std::string>();
    }
    while (cdn.ends_with('/')) {
        cdn.pop_back();
    }
    if (!is_cdn_base(cdn)) {
        fail("the report's \"cdn\" isn't a plain https address such as https://cdn.floserver.org");
        return result;
    }

    std::set<std::string> seen_paths;
    std::set<std::string> seen_runtimes;
    std::set<std::string> used_accepts;
    nlohmann::ordered_json runtimes = nlohmann::ordered_json::array();

    for (const json& f : report["files"]) {
        if (!f.is_object() || !f.contains("path") || !f["path"].is_string()) {
            fail("the report has an entry without a path");
            continue;
        }
        const std::string path = f["path"].get<std::string>();
        if (!seen_paths.insert(path).second) {
            fail(path + ": listed twice in the report");
            continue;
        }

        const Classified kind = classify(path);
        if (!kind.entry) {
            if (kind.note) {
                result.notes.push_back(kind.message);
            } else {
                fail(kind.message);
            }
            continue;
        }
        const CatalogEntry* const entry = kind.entry;
        const std::string& name = kind.parts.runtime;
        const std::string& version = kind.parts.version;
        const std::string& file = kind.parts.file;

        const std::string state =
            f.contains("result") && f["result"].is_string() ? f["result"].get<std::string>() : "";
        if (state == "unchecked") {
            if (options.accepted.contains(path)) {
                used_accepts.insert(path);
            } else {
                fail(join({path, ": not checked against the official file; compare it by hand, "
                                 "then pass --accept ", path}));
                continue;
            }
        } else if (state != "ok") {
            fail(path + ": the check report says it does NOT match the official file");
            continue;
        }

        if (!f.contains("size") || !f["size"].is_number_unsigned() ||
            f["size"].get<std::uint64_t>() == 0) {
            fail(path + ": the report has no size");
            continue;
        }
        const std::string sha =
            f.contains("sha256") && f["sha256"].is_string() ? f["sha256"].get<std::string>() : "";
        if (sha.size() != 64 || !from_hex(sha)) {
            fail(path + ": the report has no valid sha256");
            continue;
        }
        if (!seen_runtimes.insert(join({name, "/", version, "/", entry->platform})).second) {
            fail(path + ": a second file for the same runtime and version");
            continue;
        }

        nlohmann::ordered_json urls = nlohmann::ordered_json::array();
        urls.push_back(join({cdn, "/", path}));
        if (!entry->upstream.empty()) {
            urls.push_back(replace_all(replace_all(std::string(entry->upstream), "{v}", version),
                                       "{file}", file));
        }

        nlohmann::ordered_json r = nlohmann::ordered_json::object();
        r["name"] = name;
        r["version"] = version;
        r["platform"] = entry->platform;
        if (!entry->variant.empty()) {
            r["variant"] = entry->variant;
        }
        r["archive"] = archive_of(file);
        r["size"] = f["size"].get<std::uint64_t>();
        std::string lower_sha = sha;
        std::transform(lower_sha.begin(), lower_sha.end(), lower_sha.begin(),
                       [](char c) { return (c >= 'A' && c <= 'F') ? static_cast<char>(c + 32) : c; });
        r["sha256"] = lower_sha;
        r["license"] = entry->license;
        r["urls"] = urls;
        runtimes.push_back(std::move(r));
    }

    for (const std::string& a : options.accepted) {
        if (!used_accepts.contains(a)) {
            fail("--accept " + a + ": no unchecked file with that path in the report");
        }
    }
    if (runtimes.empty()) {
        fail("the report has no runtimes to list");
    }

    // Templates: read by the app's own rules, and every runtime they need listed.
    const json templates = json::parse(options.templates_json, nullptr, false);
    if (!templates.is_array()) {
        fail("the templates aren't a JSON array");
    } else {
        std::vector<std::string> warnings;
        const auto parsed = parse_templates(templates, warnings);
        for (const std::string& w : warnings) {
            fail(w);
        }
        const auto listed = [&runtimes](const std::string& name) {
            return std::any_of(runtimes.begin(), runtimes.end(),
                               [&](const nlohmann::ordered_json& r) { return r.value("name", std::string()) == name; });
        };
        for (const ProjectTemplate& t : parsed) {
            // The same list a project job installs from.
            for (const TemplateNeed& n : template_runtime_needs(t, t.parsed)) {
                if (!listed(n.name)) {
                    fail("template " + t.id + " needs " + n.name + ", which the list doesn't have");
                }
            }
        }
        result.template_count = parsed.size();
    }
    if (!result.errors.empty()) {
        return result;
    }

    nlohmann::ordered_json doc = nlohmann::ordered_json::object();
    doc["schema"] = kManifestSchema;
    doc["sequence"] = options.sequence;
    doc["expires"] = format_utc_time(options.now + std::chrono::days{options.days});
    doc["runtimes"] = runtimes;
    doc["templates"] = nlohmann::ordered_json::parse(options.templates_json);
    std::string bytes = doc.dump(2) + "\n";

    if (auto problem = self_check(bytes, options.now, runtimes.size(), result.template_count)) {
        fail(*problem);
        return result;
    }
    result.runtime_count = runtimes.size();
    result.manifest_json = std::move(bytes);
    return result;
}

}  // namespace floserver::manifest_tool

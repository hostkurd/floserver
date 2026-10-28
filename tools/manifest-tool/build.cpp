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
constexpr std::array<CatalogEntry, 6> kCatalog = {{
    {"php", "PHP-3.01", "windows-x64", "nts", R"(php-{v}(-\d+)?-nts-Win32-vs1[67]-x64\.zip)",
     "https://downloads.php.net/~windows/releases/archives/{file}"},
    {"mariadb", "GPL-2.0", "windows-x64", "", R"(mariadb-{v}-winx64\.zip)",
     "https://archive.mariadb.org/mariadb-{v}/winx64-packages/{file}"},
    {"node", "MIT", "windows-x64", "", R"(node-v{v}-win-x64\.zip)",
     "https://nodejs.org/dist/v{v}/{file}"},
    {"composer", "MIT", "any", "", R"(composer\.phar)", "https://getcomposer.org/download/{v}/{file}"},
    {"nginx", "BSD-2-Clause", "windows-x64", "", R"(nginx-{v}\.zip)",
     "https://nginx.org/download/{file}"},
    {"apache", "Apache-2.0", "windows-x64", "", R"(httpd-{v}-\d+-Win64-VS1[78]\.zip)", ""},
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
                                      std::size_t expected) {
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
    return std::nullopt;
}

}  // namespace

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

        // <runtime>/<version>/<file>
        const auto first = path.find('/');
        const auto second = first == std::string::npos ? first : path.find('/', first + 1);
        if (second == std::string::npos || path.find('/', second + 1) != std::string::npos) {
            fail(path + ": expected <runtime>/<version>/<file>");
            continue;
        }
        const std::string name = path.substr(0, first);
        const std::string version = path.substr(first + 1, second - first - 1);
        const std::string file = path.substr(second + 1);

        const auto* entry = std::find_if(kCatalog.begin(), kCatalog.end(),
                                         [&](const CatalogEntry& c) { return c.name == name; });
        if (entry == kCatalog.end()) {
            fail(join({path, ": \"", name, "\" isn't a runtime FloServer knows; add it to the catalog"}));
            continue;
        }
        if (!Version::parse(version)) {
            fail(join({path, ": \"", version, "\" isn't a version folder such as 8.5.12"}));
            continue;
        }
        const std::regex pattern(
            replace_all(std::string(entry->file_pattern), "{v}", regex_escape(version)));
        if (!std::regex_match(file, pattern)) {
            result.notes.push_back(path + ": not the Windows runtime file, left out" +
                                   (file.ends_with(".tar.gz") ? " (source archive)" : ""));
            continue;
        }

        const std::string state = f.value("result", "");
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
        const std::string sha = f.value("sha256", "");
        if (sha.size() != 64 || !from_hex(sha)) {
            fail(path + ": the report has no valid sha256");
            continue;
        }
        if (!seen_runtimes.insert(join({name, "/", version, "/", entry->platform})).second) {
            fail(path + ": a second file for the same runtime and version");
            continue;
        }

        nlohmann::ordered_json urls = nlohmann::ordered_json::array();
        urls.push_back(options.cdn + "/" + path);
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
    if (!result.errors.empty()) {
        return result;
    }

    nlohmann::ordered_json doc = nlohmann::ordered_json::object();
    doc["schema"] = kManifestSchema;
    doc["sequence"] = options.sequence;
    doc["expires"] = format_utc_time(options.now + std::chrono::days{options.days});
    doc["runtimes"] = runtimes;
    std::string bytes = doc.dump(2) + "\n";

    if (auto problem = self_check(bytes, options.now, runtimes.size())) {
        fail(*problem);
        return result;
    }
    result.runtime_count = runtimes.size();
    result.manifest_json = std::move(bytes);
    return result;
}

}  // namespace floserver::manifest_tool

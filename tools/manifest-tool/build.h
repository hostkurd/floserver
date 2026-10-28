#pragma once

// Makes manifest.json from a CDN check report (tools/cdn-check), so the list
// only names files that the CDN really serves and that matched the official
// releases.
//
// Report (floserver-cdn-report.json, written by check-floserver-cdn.ps1):
//   {"cdn": "https://cdn.floserver.org",       // where the files were checked
//    "only": "",                               // set when only some files were checked
//    "files": [{"path": "php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip",
//               "size": 36310917, "sha256": "<64 hex>",
//               "result": "ok" | "fail" | "unchecked"}, ...]}
//
// Paths are <runtime>/<version>/<file>. The runtime's licence, platform and
// upstream fallback URL come from the catalog in build.cpp.

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::manifest_tool {

// The CDN address in the list is the one the report was checked against, so
// every listed URL is one that was really fetched and compared.
struct BuildOptions {
    std::int64_t sequence = 0;       // must be higher than the last published list
    int days = 30;                   // expiry, 1-90 days from `now`
    std::chrono::sys_seconds now;
    std::set<std::string> accepted;  // "unchecked" paths compared by hand
};

struct BuildResult {
    std::optional<std::string> manifest_json;  // set when there are no errors
    std::vector<std::string> errors;
    std::vector<std::string> notes;  // files left out on purpose, e.g. source archives
    std::size_t runtime_count = 0;
};

[[nodiscard]] BuildResult build_manifest(std::string_view report_json, const BuildOptions& options);

// "YYYY-MM-DDTHH:MM:SSZ"
[[nodiscard]] std::string format_utc_time(std::chrono::sys_seconds t);

inline constexpr int kMaxExpiryDays = 90;

}  // namespace floserver::manifest_tool

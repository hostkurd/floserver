#pragma once

// Each project's request log, for the window's Log box (step 5b). nginx
// writes one line per request in the floserver_requests format:
//
//   $msec $status $request_time $request_method $request_uri
//   1793306687.123 200 0.038 GET /products?page=2
//
// nginx escapes '"', '\' and control characters in $request_uri as \xHH, so
// a line never holds a raw newline. The service keeps each log small by
// emptying it when it grows past kRequestLogLimit.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

struct RequestLogEntry {
    std::int64_t time_ms = 0;      // since 1970, UTC
    int status = 0;
    std::int64_t duration_ms = 0;
    std::string method;            // "GET"; "-" for a request nginx couldn't read
    std::string uri;               // at most kRequestLogUriLimit bytes; longer ones end in "..."
};

inline constexpr std::uintmax_t kRequestLogLimit = std::uintmax_t{1024} * 1024;
inline constexpr std::size_t kRequestLogUriLimit = 300;
// The most entries one read returns.
inline constexpr std::size_t kRequestLogMaxEntries = 200;

// The nginx log_format line for it (without the leading spaces).
inline constexpr std::string_view kRequestLogFormat =
    "log_format floserver_requests '$msec $status $request_time $request_method $request_uri';";

// <web>/logs/sites/<project>.log
[[nodiscard]] std::filesystem::path request_log_file(const std::filesystem::path& web_dir, std::string_view project);

// The last `max_entries` (at most kRequestLogMaxEntries) well-formed lines of
// `text`, oldest first. Lines that don't fit the format are skipped; a
// partial first line (the read started mid-line) is dropped.
[[nodiscard]] std::vector<RequestLogEntry> parse_request_log(std::string_view text, std::size_t max_entries,
                                                             bool starts_mid_line);

// The same from the end of `file` (its last 64 KiB). A missing file has no entries.
[[nodiscard]] std::vector<RequestLogEntry> read_request_log(const std::filesystem::path& file,
                                                            std::size_t max_entries);

}  // namespace floserver::core

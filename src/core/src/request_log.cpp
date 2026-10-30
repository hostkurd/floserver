#include "floserver/core/request_log.h"

#include "floserver/core/paths.h"

#include <algorithm>
#include <charconv>
#include <deque>
#include <fstream>
#include <optional>

namespace floserver::core {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kTailBytes = std::size_t{64} * 1024;
constexpr std::size_t kMaxMethod = 16;

std::optional<std::int64_t> whole(std::string_view s) {
    std::int64_t value = 0;
    if (s.empty()) {
        return std::nullopt;
    }
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || end != s.data() + s.size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

// "1793306687.123" (seconds with milliseconds, as nginx writes $msec and
// $request_time) -> 1793306687123. Up to three decimals.
std::optional<std::int64_t> milliseconds(std::string_view s) {
    const std::size_t dot = s.find('.');
    const auto seconds = whole(s.substr(0, dot));
    if (!seconds || *seconds > std::int64_t{1} << 40) {
        return std::nullopt;
    }
    std::int64_t ms = 0;
    if (dot != std::string_view::npos) {
        const std::string_view decimals = s.substr(dot + 1);
        if (decimals.empty() || decimals.size() > 3) {
            return std::nullopt;
        }
        const auto d = whole(decimals);
        if (!d) {
            return std::nullopt;
        }
        ms = *d;
        for (std::size_t i = decimals.size(); i < 3; ++i) {
            ms *= 10;
        }
    }
    return *seconds * 1000 + ms;
}

std::string_view next_field(std::string_view& line) {
    const std::size_t space = line.find(' ');
    const std::string_view field = line.substr(0, space);
    line = space == std::string_view::npos ? std::string_view() : line.substr(space + 1);
    return field;
}

std::optional<RequestLogEntry> parse_line(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    RequestLogEntry e;
    const auto time = milliseconds(next_field(line));
    const auto status = whole(next_field(line));
    const auto duration = milliseconds(next_field(line));
    const std::string_view method = next_field(line);
    if (!time || !status || *status < 100 || *status > 999 || !duration || method.empty() || method.size() > kMaxMethod ||
        !std::all_of(method.begin(), method.end(), [](char c) { return (c >= 'A' && c <= 'Z') || c == '_' || c == '-'; }) ||
        line.empty()) {
        return std::nullopt;
    }
    // Printable ASCII only: nginx escapes the rest, so anything else means
    // the line isn't one of ours.
    if (!std::all_of(line.begin(), line.end(), [](char c) { return c >= 0x20 && c < 0x7F; })) {
        return std::nullopt;
    }
    e.time_ms = *time;
    e.status = static_cast<int>(*status);
    e.duration_ms = *duration;
    e.method = std::string(method);
    if (line.size() > kRequestLogUriLimit) {
        e.uri = std::string(line.substr(0, kRequestLogUriLimit - 3)) + "...";
    } else {
        e.uri = std::string(line);
    }
    return e;
}

}  // namespace

fs::path request_log_file(const fs::path& web_dir, std::string_view project) {
    return web_dir / "logs" / "sites" / path_from_utf8(std::string(project) + ".log");
}

std::vector<RequestLogEntry> parse_request_log(std::string_view text, std::size_t max_entries, bool starts_mid_line) {
    max_entries = std::min(max_entries, kRequestLogMaxEntries);
    if (starts_mid_line) {
        const std::size_t nl = text.find('\n');
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
    }
    std::deque<RequestLogEntry> kept;
    while (!text.empty() && max_entries > 0) {
        const std::size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        // A last line without its newline may still be being written.
        if (nl == std::string_view::npos) {
            break;
        }
        text.remove_prefix(nl + 1);
        if (auto e = parse_line(line)) {
            kept.push_back(std::move(*e));
            if (kept.size() > max_entries) {
                kept.pop_front();
            }
        }
    }
    return {std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end())};
}

std::vector<RequestLogEntry> read_request_log(const fs::path& file, std::size_t max_entries) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        return {};
    }
    // One byte before the tail too: when it's a newline, the tail starts on a whole line.
    const auto start = static_cast<std::streamoff>(std::max<std::streamoff>(0, size - static_cast<std::streamoff>(kTailBytes) - 1));
    in.seekg(start);
    std::string text(static_cast<std::size_t>(size - start), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    // From the file's start, every line is whole; otherwise the first,
    // partial or just the newline before the tail, is dropped.
    return parse_request_log(text, max_entries, start > 0);
}

}  // namespace floserver::core

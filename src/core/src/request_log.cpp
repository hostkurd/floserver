#include "floserver/core/request_log.h"

#include "floserver/core/mailpit.h"
#include "floserver/core/paths.h"
#include "floserver/core/plain_file.h"

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

namespace {

// The end of `file`: its last kTailBytes, and one byte before them (when
// that's a newline, the tail starts on a whole line). `mid_line`: the read
// didn't start at the file's start. Nothing when there's no file or it's empty.
std::optional<std::string> read_tail(const fs::path& file, bool& mid_line) {
    // The log itself, checked on the open file: never through a link (a link
    // to a file elsewhere would show that file).
    PlainFileRefusal why = PlainFileRefusal::None;
    auto tail = read_plain_file(file, 0, kTailBytes, why);
    if (!tail || tail->text.empty()) {
        return std::nullopt;
    }
    mid_line = !tail->from_start;
    return std::move(tail->text);
}

}  // namespace

std::vector<RequestLogEntry> read_request_log(const fs::path& file, std::size_t max_entries) {
    bool mid_line = false;
    const auto text = read_tail(file, mid_line);
    // From the file's start, every line is whole; otherwise the first,
    // partial or just the newline before the tail, is dropped.
    return text ? parse_request_log(*text, max_entries, mid_line) : std::vector<RequestLogEntry>{};
}

std::vector<std::string> parse_text_log(std::string_view text, std::size_t max_lines, bool starts_mid_line) {
    max_lines = std::min(max_lines, kRequestLogMaxEntries);
    if (starts_mid_line) {
        const std::size_t nl = text.find('\n');
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
    }
    std::deque<std::string> kept;
    while (!text.empty() && max_lines > 0) {
        const std::size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        // Control characters (a terminal's escapes) as spaces; bidi, zero-width
        // and tag characters out; bad UTF-8 as U+FFFD (clean_error_text).
        std::string shown = clean_error_text(line.substr(0, kTextLogLineLimit * 2), kTextLogLineLimit);
        if (shown.size() > kTextLogLineLimit) {
            // At most the limit in bytes, cut before a character, never inside one.
            std::size_t cut = kTextLogLineLimit;
            while (cut > 0 && (static_cast<unsigned char>(shown[cut]) & 0xC0U) == 0x80U) {
                --cut;
            }
            shown.resize(cut);
        }
        while (!shown.empty() && shown.back() == ' ') {
            shown.pop_back();
        }
        if (shown.empty()) {
            continue;
        }
        kept.push_back(std::move(shown));
        if (kept.size() > max_lines) {
            kept.pop_front();
        }
    }
    return {std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end())};
}

std::vector<std::string> read_text_log(const fs::path& file, std::size_t max_lines) {
    bool mid_line = false;
    const auto text = read_tail(file, mid_line);
    return text ? parse_text_log(*text, max_lines, mid_line) : std::vector<std::string>{};
}

std::optional<nlohmann::json> pool_php_log(const nlohmann::json& web_status, const fs::path& web_dir,
                                           const std::string& project, std::size_t max_lines) {
    using nlohmann::json;
    const json& pools = web_status.is_object() && web_status.contains("pools") ? web_status.at("pools") : json();
    if (!pools.is_array()) {
        return std::nullopt;
    }
    for (const json& pool : pools) {
        if (!pool.is_object() || !pool.contains("sites") || !pool.at("sites").is_array()) {
            continue;
        }
        const json& sites = pool.at("sites");
        if (std::none_of(sites.begin(), sites.end(),
                         [&](const json& s) { return s.is_string() && s.get_ref<const std::string&>() == project; })) {
            continue;
        }
        const std::string key = pool.contains("key") && pool.at("key").is_string() ? pool.at("key").get<std::string>()
                                                                                   : std::string();
        // The web service's own key ("php-8.4.27+gd-1a2b"): a file name, never a path.
        if (key.empty() || key.find_first_of("/\\:") != std::string::npos || key.find("..") != std::string::npos) {
            return std::nullopt;
        }
        const std::string file = key + ".php-errors.log";
        json others = json::array();
        for (const json& s : sites) {
            if (s.is_string() && s.get_ref<const std::string&>() != project) {
                others.push_back(s);
            }
        }
        return json{{"lines", read_text_log(web_dir / "logs" / path_from_utf8(file), max_lines)},
                    {"file", file},
                    {"php", pool.contains("version") && pool.at("version").is_string() ? pool.at("version") : json("")},
                    {"shared_with", std::move(others)}};
    }
    return std::nullopt;
}

}  // namespace floserver::core

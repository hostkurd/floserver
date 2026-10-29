#include "model/view_model.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>

namespace floserver::ui {

namespace {

using nlohmann::json;

std::string text_of(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return {};
}

// A whole number >= 0 (nlohmann keeps small numbers as signed or unsigned).
std::optional<std::uint64_t> count_of(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) {
        return std::nullopt;
    }
    const json& v = j[key];
    if (v.is_number_unsigned()) {
        return v.get<std::uint64_t>();
    }
    if (v.is_number_integer() && v.get<std::int64_t>() >= 0) {
        return static_cast<std::uint64_t>(v.get<std::int64_t>());
    }
    return std::nullopt;
}

bool flag_of(const json& j, const char* key) {
    return j.is_object() && j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
}

std::string missing_list(const json& p) {
    std::string out;
    if (p.contains("missing") && p["missing"].is_array()) {
        for (const auto& m : p["missing"]) {
            if (m.is_string()) {
                out += (out.empty() ? "" : ", ") + m.get<std::string>();
            }
        }
    }
    return out;
}

// A code word on its own inside text of the window's language.
std::string code(std::string_view s, Lang lang) { return is_rtl(lang) ? isolate_ltr(s) : std::string(s); }

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), lower);
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

bool is_host_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

ProjectRow row_from(const json& p, Lang lang) {
    ProjectRow row;
    row.name = text_of(p, "name");
    row.on = flag_of(p, "on");
    row.keep_running = flag_of(p, "keep_running");
    const std::string state = text_of(p, "state");
    const std::string run = text_of(p, "run");
    const std::string missing = missing_list(p);
    const std::string path = text_of(p, "path");

    if (state == "ready") {
        if (run == "failed") {
            row.state = RowState::Problem;
            const bool own_server =
                !text_of(p, "node").empty() || !text_of(p, "python").empty() || text_of(p, "web") == "apache";
            row.detail = std::string(text(lang, own_server ? Str::ServerCantStart : Str::PhpCantStart));
        } else {
            row.state = run == "sleeping" ? RowState::Asleep : RowState::On;
        }
    } else if (state == "off") {
        row.state = RowState::Off;
        if (!missing.empty()) {
            row.detail = fill(lang, Str::Needs, {missing});
        }
    } else if (state == "needs_download") {
        row.state = RowState::NeedsDownload;
        row.detail = fill(lang, Str::Needs, {missing});
    } else {
        row.state = RowState::Problem;
        // The service's reason is in English for now.
        row.detail = code(text_of(p, "problem"), lang);
    }

    if (row.state == RowState::On || row.state == RowState::Asleep) {
        row.address = text_of(p, "address");
    }
    if (row.address.empty() && row.detail.empty() && !path.empty()) {
        row.detail = code(path, lang);
    }

    const std::string php = text_of(p, "php");
    if (!php.empty()) {
        row.chips.push_back("PHP " + php);
    }
    const std::string python = text_of(p, "python");
    if (!python.empty()) {
        row.chips.push_back("Python " + python);
    }
    const std::string node = text_of(p, "node");
    if (!node.empty()) {
        row.chips.push_back("Node " + node);
    }
    if (text_of(p, "web") == "apache") {
        row.chips.emplace_back("Apache");
    }
    const std::string database = text_of(p, "database");
    if (!database.empty()) {
        row.chips.push_back(database_chip(database));
    }
    return row;
}

}  // namespace

std::vector<ProjectRow> rows_from_list(const json& result, Lang lang) {
    std::vector<ProjectRow> rows;
    if (!result.is_object() || !result.contains("projects") || !result["projects"].is_array()) {
        return rows;
    }
    for (const auto& p : result["projects"]) {
        if (text_of(p, "name").empty()) {
            continue;
        }
        rows.push_back(row_from(p, lang));
    }
    return rows;
}

Str state_label(RowState state) {
    switch (state) {
        case RowState::On:
            return Str::StateOn;
        case RowState::Asleep:
            return Str::StateAsleep;
        case RowState::Off:
            return Str::StateOff;
        case RowState::NeedsDownload:
            return Str::StateNeedsDownload;
        case RowState::Problem:
            break;
    }
    return Str::StateProblem;
}

std::string database_chip(std::string_view database) {
    const std::size_t dash = database.find('-');
    const std::string family = lower_ascii(database.substr(0, dash));
    const std::string_view version = dash == std::string_view::npos ? std::string_view() : database.substr(dash + 1);
    std::string shown;
    if (family == "mariadb") {
        shown = "MariaDB";
    } else if (family == "mysql") {
        shown = "MySQL";
    } else if (family == "postgres" || family == "postgresql") {
        shown = "PostgreSQL";
    } else {
        shown = std::string(database.substr(0, dash));
    }
    if (!version.empty()) {
        shown += " ";
        shown += version;
    }
    return shown;
}

bool is_openable_address(std::string_view address) {
    std::string_view rest;
    if (address.starts_with("http://")) {
        rest = address.substr(7);
    } else if (address.starts_with("https://")) {
        rest = address.substr(8);
    } else {
        return false;
    }
    if (rest.ends_with("/")) {
        rest.remove_suffix(1);
    }
    std::string_view host = rest;
    std::string_view port;
    if (const std::size_t colon = rest.find(':'); colon != std::string_view::npos) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
        if (port.empty() || port.size() > 5) {
            return false;
        }
        unsigned value = 0;
        for (const char c : port) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        if (value == 0 || value > 65535) {
            return false;
        }
    }
    constexpr std::string_view kSuffix = ".localhost";
    if (host.size() <= kSuffix.size() || !lower_ascii(host).ends_with(kSuffix)) {
        return false;
    }
    if (!std::all_of(host.begin(), host.end(), is_host_char)) {
        return false;
    }
    // No empty labels ("..", a leading dot) and no label starting or ending with '-'.
    std::size_t start = 0;
    while (start <= host.size()) {
        std::size_t end = host.find('.', start);
        if (end == std::string_view::npos) {
            end = host.size();
        }
        const std::string_view label = host.substr(start, end - start);
        if (label.empty() || label.front() == '-' || label.back() == '-') {
            return false;
        }
        start = end + 1;
    }
    return true;
}

bool row_matches(const ProjectRow& row, std::string_view query) {
    const std::string q = lower_ascii(trim(query));
    if (q.empty()) {
        return true;
    }
    return lower_ascii(row.name).find(q) != std::string::npos ||
           lower_ascii(row.address).find(q) != std::string::npos;
}

std::string human_bytes(std::uint64_t bytes) {
    constexpr std::uint64_t kKb = 1024;
    constexpr std::uint64_t kMb = kKb * 1024;
    const bool mb = bytes >= kMb;
    const std::uint64_t unit = mb ? kMb : kKb;
    const std::uint64_t tenths = (bytes * 10 + unit / 2) / unit;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + (mb ? " MB" : " KB");
}

FooterText footer_text(const std::vector<ProjectRow>& rows, const json* status, const json* web, Lang lang) {
    FooterText out;
    std::size_t running = 0;
    std::size_t asleep = 0;
    for (const ProjectRow& r : rows) {
        running += r.state == RowState::On ? 1U : 0U;
        asleep += r.state == RowState::Asleep ? 1U : 0U;
    }
    out.any_running = running > 0;
    out.counts = fill_pattern(text(lang, Str::FooterCounts), false, {std::to_string(running), std::to_string(asleep)});

    if (web != nullptr && web->is_object() && web->contains("nginx") && (*web)["nginx"].is_object()) {
        const json& n = (*web)["nginx"];
        if (flag_of(n, "running")) {
            const auto http = count_of(n, "http_port");
            const auto https = count_of(n, "https_port");
            if (http && https) {
                out.nginx = fill_pattern(text(lang, Str::FooterNginx), false,
                                         {std::to_string(*http), std::to_string(*https)});
            }
        } else if (std::any_of(rows.begin(), rows.end(), [](const ProjectRow& r) { return r.on; })) {
            // Only worth saying when something should be served.
            out.nginx = std::string(text(lang, Str::FooterNginxOff));
        }
    }

    if (status != nullptr) {
        if (const auto memory = count_of(*status, "memory_bytes")) {
            out.memory = fill(lang, Str::FooterMemory, {human_bytes(*memory)});
        }
    }
    return out;
}

}  // namespace floserver::ui

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
        } else if (run == "starting") {
            row.state = RowState::Waking;
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

    if (row.state == RowState::On || row.state == RowState::Asleep || row.state == RowState::Waking) {
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
    const std::string database = text_of(p, "database");
    if (!database.empty()) {
        row.chips.push_back(database_chip(database));
    }
    if (p.contains("missing") && p["missing"].is_array()) {
        for (const auto& m : p["missing"]) {
            if (m.is_string() && !m.get_ref<const std::string&>().empty()) {
                row.missing.push_back(m.get<std::string>());
            }
        }
    }
    row.path = path;
    row.php = php;
    row.node = node;
    row.python = python;
    row.database = database;
    if (p.contains("wants") && p["wants"].is_object()) {
        const json& want = p["wants"];
        row.wants.known = true;
        row.wants.php = text_of(want, "php");
        row.wants.node = text_of(want, "node");
        row.wants.python = text_of(want, "python");
        row.wants.database = text_of(want, "database");
        row.wants.web = text_of(want, "web") == "apache" ? "apache" : "nginx";
    }
    // The web server last, as in the mockup. nginx only when floserver.json
    // could be read (it serves every project that isn't Apache's).
    if (text_of(p, "web") == "apache") {
        row.chips.emplace_back("Apache");
    } else if (row.wants.known && row.wants.web == "nginx") {
        row.chips.emplace_back("nginx");
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
        case RowState::Waking:
            return Str::StateWaking;
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
        running += r.state == RowState::On || r.state == RowState::Waking ? 1U : 0U;
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
    if (web != nullptr) {
        if (const auto idle = count_of(*web, "idle_after_s")) {
            out.idle_minutes = std::max<std::uint64_t>(1, (*idle + 59) / 60);
        }
    }

    if (status != nullptr) {
        if (const auto memory = count_of(*status, "memory_bytes")) {
            out.memory = fill(lang, Str::FooterMemory, {human_bytes(*memory)});
        }
    }
    return out;
}

// --- the project panel -------------------------------------------------------

namespace {

// Newest first by version, compared number by number ("8.10" after "8.9").
bool newer(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() || j < b.size()) {
        std::uint64_t x = 0;
        std::uint64_t y = 0;
        while (i < a.size() && a[i] >= '0' && a[i] <= '9') {
            x = x * 10 + static_cast<std::uint64_t>(a[i++] - '0');
        }
        while (j < b.size() && b[j] >= '0' && b[j] <= '9') {
            y = y * 10 + static_cast<std::uint64_t>(b[j++] - '0');
        }
        if (x != y) {
            return x > y;
        }
        // Past the separator ('.', or anything else), on to the next number.
        if (i < a.size()) {
            ++i;
        }
        if (j < b.size()) {
            ++j;
        }
    }
    return false;
}

// One choice per installed series, newest first; the series is the value.
std::vector<std::string> series_of(const Installed& installed, const std::string& runtime) {
    std::vector<std::string> out;
    const auto it = installed.find(runtime);
    if (it == installed.end()) {
        return out;
    }
    for (const std::string& v : it->second) {
        const std::string s = version_series(runtime, v);
        if (std::find(out.begin(), out.end(), s) == out.end()) {
            out.push_back(s);
        }
    }
    std::stable_sort(out.begin(), out.end(), newer);
    return out;
}

// Adds a choice per series and picks the one matching `wanted` (as written
// in floserver.json; "8.4" or "8.4.27"). A wanted series that isn't
// installed is still listed, so the picker shows the truth.
void add_series(VersionField& f, const Installed& installed, const std::string& runtime, const std::string& wanted,
                const std::string& value_prefix, const std::string& label_prefix, Lang lang) {
    const std::string want = wanted.empty() ? std::string() : version_series(runtime, wanted);
    bool found = false;
    for (const std::string& s : series_of(installed, runtime)) {
        f.choices.push_back({label_prefix + s, value_prefix + s});
        if (!want.empty() && s == want) {
            f.current = f.choices.size() - 1;
            found = true;
        }
    }
    if (!want.empty() && !found) {
        f.choices.push_back({fill(lang, Str::NotInstalled, {label_prefix + want}), value_prefix + want});
        f.current = f.choices.size() - 1;
    }
}

}  // namespace

Installed installed_from_list(const json& result) {
    Installed out;
    if (!result.is_object() || !result.contains("installed") || !result["installed"].is_array()) {
        return out;
    }
    for (const json& r : result["installed"]) {
        const std::string name = text_of(r, "name");
        const std::string version = text_of(r, "version");
        if (!name.empty() && !version.empty()) {
            out[name].push_back(version);
        }
    }
    for (auto& [name, versions] : out) {
        std::stable_sort(versions.begin(), versions.end(), newer);
    }
    return out;
}

std::string version_series(std::string_view runtime, std::string_view version) {
    const std::size_t first = version.find('.');
    if (runtime == "node" || first == std::string_view::npos) {
        return std::string(version.substr(0, first));
    }
    const std::size_t second = version.find('.', first + 1);
    return std::string(version.substr(0, second));
}

std::vector<VersionField> version_fields(const ProjectRow& row, const Installed& installed, Lang lang) {
    std::vector<VersionField> out;
    const Wants& w = row.wants;

    // PHP: without the key, a PHP project gets the newest installed; a Node
    // or Python project gets none.
    VersionField php{Field::Php, "php", std::string(text(lang, Str::FieldPhp)), {}, 0};
    const bool own_server = w.php.empty() && (!w.node.empty() || !w.python.empty());
    php.choices.push_back({std::string(text(lang, own_server ? Str::ChoiceNone : Str::ChoiceNewest)), std::nullopt});
    add_series(php, installed, "php", w.php, "", "", lang);
    out.push_back(std::move(php));

    VersionField db{Field::Database, "database", std::string(text(lang, Str::FieldDatabase)), {}, 0};
    db.choices.push_back({std::string(text(lang, Str::ChoiceNone)), std::nullopt});
    std::string wanted_db;
    if (w.database.starts_with("mariadb-")) {
        wanted_db = w.database.substr(8);
    }
    add_series(db, installed, "mariadb", wanted_db, "mariadb-", "MariaDB ", lang);
    if (!w.database.empty() && wanted_db.empty()) {
        // Another family (a newer FloServer's): shown, not offered.
        db.choices.push_back({database_chip(w.database), w.database});
        db.current = db.choices.size() - 1;
    }
    out.push_back(std::move(db));

    VersionField web{Field::Web, "web", std::string(text(lang, Str::FieldWeb)), {{"nginx", "nginx"}, {"Apache", "apache"}},
                     w.web == "apache" ? 1U : 0U};
    out.push_back(std::move(web));

    VersionField node{Field::Node, "node", std::string(text(lang, Str::FieldNode)), {}, 0};
    node.choices.push_back({std::string(text(lang, Str::ChoiceNone)), std::nullopt});
    add_series(node, installed, "node", w.node, "", "", lang);
    out.push_back(std::move(node));

    if (!w.python.empty() || installed.contains("python")) {
        VersionField python{Field::Python, "python", std::string(text(lang, Str::FieldPython)), {}, 0};
        python.choices.push_back({std::string(text(lang, Str::ChoiceNone)), std::nullopt});
        add_series(python, installed, "python", w.python, "", "", lang);
        out.push_back(std::move(python));
    }
    return out;
}

std::uint16_t database_port(const json& databases, std::string_view database) {
    const std::size_t dash = database.find('-');
    if (dash == std::string_view::npos || !databases.is_object() || !databases.contains("servers") ||
        !databases["servers"].is_array()) {
        return 0;
    }
    const std::string series = version_series(database.substr(0, dash), database.substr(dash + 1));
    for (const json& server : databases["servers"]) {
        if (version_series("mariadb", text_of(server, "series")) == series ||
            version_series("mariadb", text_of(server, "version")) == series) {
            const auto port = count_of(server, "port");
            if (port && *port > 0 && *port <= 65535) {
                return static_cast<std::uint16_t>(*port);
            }
        }
    }
    return 0;
}

std::vector<LogLine> log_lines(const json& result, const std::function<void(std::int64_t, int&, int&, int&)>& local) {
    std::vector<LogLine> out;
    if (!result.is_object() || !result.contains("entries") || !result["entries"].is_array()) {
        return out;
    }
    const auto two = [](int v) {
        const std::string s = std::to_string(std::clamp(v, 0, 99));
        return s.size() < 2 ? "0" + s : s;
    };
    for (const json& e : result["entries"]) {
        const auto t = count_of(e, "t");
        const auto status = count_of(e, "status");
        const auto ms = count_of(e, "ms");
        if (!t || !status || !ms || *status > 999) {
            continue;
        }
        LogLine line;
        int h = 0;
        int m = 0;
        int sec = 0;
        local(static_cast<std::int64_t>(*t), h, m, sec);
        line.time = two(h) + ":" + two(m) + ":" + two(sec);
        line.method = text_of(e, "method");
        line.uri = text_of(e, "uri");
        line.status = static_cast<int>(*status);
        if (*ms < 1000) {
            line.duration = std::to_string(*ms) + " ms";
        } else {
            const std::uint64_t tenths = (*ms + 50) / 100;
            line.duration = std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " s";
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::string log_text(const LogLine& line) {
    std::string method = line.method;
    if (method.size() < 4) {
        method.append(4 - method.size(), ' ');
    }
    return line.time + "  " + method + " " + line.uri + "  " + std::to_string(line.status) + "  " + line.duration;
}

std::string env_text(const json& login) {
    std::string out;
    if (!login.is_object() || !login.contains("env") || !login["env"].is_array()) {
        return out;
    }
    // One allocation: the text holds the password, and a grown string would
    // leave partial copies behind in freed memory.
    std::size_t size = 0;
    for (const json& pair : login["env"]) {
        if (pair.is_array() && pair.size() == 2 && pair[0].is_string() && pair[1].is_string()) {
            size += pair[0].get_ref<const std::string&>().size() + pair[1].get_ref<const std::string&>().size() + 3;
        }
    }
    out.reserve(size);
    for (const json& pair : login["env"]) {
        if (pair.is_array() && pair.size() == 2 && pair[0].is_string() && pair[1].is_string()) {
            // Appended piece by piece: no temporary copies of the password.
            out.append(pair[0].get_ref<const std::string&>())
                .append("=")
                .append(pair[1].get_ref<const std::string&>())
                .append("\r\n");
        }
    }
    return out;
}

// --- downloads -------------------------------------------------------------------

std::vector<Download> downloads_for(const ProjectRow& row, const Installed& installed) {
    std::vector<Download> out;
    const auto add = [&out](Download d) {
        if (std::find(out.begin(), out.end(), d) == out.end()) {
            out.push_back(std::move(d));
        }
    };
    for (const std::string& m : row.missing) {
        const std::size_t space = m.find(' ');
        Download d{m.substr(0, space), space == std::string::npos ? std::string() : m.substr(space + 1)};
        // Only names and versions the service could have meant; anything else isn't offered.
        const bool name_ok = !d.name.empty() && std::all_of(d.name.begin(), d.name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        });
        const bool version_ok = std::all_of(d.version.begin(), d.version.end(), [](char c) {
            return (c >= '0' && c <= '9') || c == '.';
        });
        if (name_ok && version_ok) {
            add(std::move(d));
        }
    }
    if (!out.empty() && !installed.contains("nginx")) {
        add({"nginx", {}});
    }
    return out;
}

std::string runtime_label(std::string_view name) {
    if (name == "php") {
        return "PHP";
    }
    if (name == "mariadb") {
        return "MariaDB";
    }
    if (name == "node") {
        return "Node.js";
    }
    if (name == "python") {
        return "Python";
    }
    if (name == "composer") {
        return "Composer";
    }
    if (name == "apache") {
        return "Apache";
    }
    return std::string(name);
}

std::string download_button_text(const std::vector<Download>& downloads, Lang lang) {
    if (downloads.size() == 1) {
        const Download& d = downloads.front();
        return fill(lang, Str::DownloadOne, {runtime_label(d.name) + (d.version.empty() ? "" : " " + d.version)});
    }
    return fill_pattern(text(lang, Str::DownloadMany), false, {std::to_string(downloads.size())});
}

std::string job_progress_text(const json& job, Lang lang) {
    const std::string state = text_of(job, "state");
    if (state.empty() || state == "done" || state == "failed" || state == "cancelled") {
        return {};
    }
    std::string what = runtime_label(text_of(job, "name"));
    if (const std::string version = text_of(job, "version"); !version.empty()) {
        what += " " + version;
    }
    if (state == "downloading") {
        const auto received = count_of(job, "received");
        const auto total = count_of(job, "total");
        if (received && total && *total > 0) {
            const std::uint64_t percent = std::min<std::uint64_t>(100, *received * 100 / *total);
            return fill(lang, Str::Downloading, {what, std::to_string(percent) + "%"});
        }
        return fill(lang, Str::Downloading, {what, ""});
    }
    return fill(lang, Str::Installing, {what});
}

}  // namespace floserver::ui

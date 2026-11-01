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
        row.wants.php_off = want.contains("php") && want.at("php").is_boolean() && !want.at("php").get<bool>();
        row.wants.node = text_of(want, "node");
        row.wants.python = text_of(want, "python");
        row.wants.database = text_of(want, "database");
        row.wants.web = text_of(want, "web") == "apache" ? "apache" : "nginx";
        if (want.contains("extensions") && want["extensions"].is_array()) {
            for (const auto& e : want["extensions"]) {
                if (e.is_string() && !e.get_ref<const std::string&>().empty()) {
                    row.wants.extensions.push_back(e.get<std::string>());
                }
            }
        }
        if (want.contains("php_ini") && want["php_ini"].is_object()) {
            for (const auto& [key, value] : want["php_ini"].items()) {
                if (value.is_string()) {
                    row.wants.php_ini[key] = value.get<std::string>();
                }
            }
        }
    }
    row.static_site = flag_of(p, "static");
    row.php_files = flag_of(p, "php_files");
    if (row.static_site) {
        row.chips.emplace_back("Static");  // the mockup's "Static · nginx"
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

std::string new_project_address(const std::string& name, const FooterText& footer) {
    if (footer.http_port == 0 || footer.https_port == 0) {
        return "https://" + name + ".localhost";
    }
    const std::uint64_t port = footer.tls ? footer.https_port : footer.http_port;
    const std::uint64_t usual = footer.tls ? 443 : 80;
    return (footer.tls ? "https://" : "http://") + name + ".localhost" +
           (port == usual ? std::string() : ":" + std::to_string(port));
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
                out.http_port = *http;
                out.https_port = *https;
                out.tls = flag_of(n, "tls");
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

bool is_newer_version(std::string_view a, std::string_view b) { return newer(std::string(a), std::string(b)); }

std::string version_series(std::string_view runtime, std::string_view version) {
    const std::size_t first = version.find('.');
    if (runtime == "node" || first == std::string_view::npos) {
        return std::string(version.substr(0, first));
    }
    const std::size_t second = version.find('.', first + 1);
    return std::string(version.substr(0, second));
}

namespace {

// What projects.use takes for PHP besides a version (core's kPhpNone and
// kPhpNewest; the model doesn't link the core).
constexpr std::string_view kPhpNone = "none";
constexpr std::string_view kPhpNewest = "newest";

// The pickers; `project` gives PHP its None and Newest installed choices
// (a project's panel), else the first PHP choice removes the key and means
// the newest (shared sites, the new project wizard).
std::vector<VersionField> fields_of(const ProjectRow& row, const Installed& installed, Lang lang, bool project) {
    std::vector<VersionField> out;
    const Wants& w = row.wants;

    VersionField php{Field::Php, "php", std::string(text(lang, Str::FieldPhp)), {}, 0};
    if (project) {
        // None writes false, Newest installed "newest", a series its number.
        // Without the key the folder decides, and what's in effect is the
        // one ticked: None for a static site or a Node or Python project,
        // Newest installed for a PHP one. Picking it still writes the key
        // (`implicit`), so the project keeps it from then on.
        php.choices.push_back({std::string(text(lang, Str::ChoiceNone)), std::string(kPhpNone)});
        php.choices.push_back({std::string(text(lang, Str::ChoiceNewest)), std::string(kPhpNewest)});
        const bool absent = w.php.empty() && !w.php_off;
        if (absent) {
            const bool none = !w.node.empty() || !w.python.empty() || row.static_site;
            php.current = none ? 0U : 1U;
            php.implicit = true;
        } else {
            php.current = w.php_off ? 0U : 1U;
        }
        const bool newest = w.php == kPhpNewest;
        add_series(php, installed, "php", newest ? std::string() : w.php, "", "", lang);
    } else {
        // Without the key, a PHP project gets the newest installed; a Node
        // or Python project gets none.
        const bool own_server = w.php.empty() && (!w.node.empty() || !w.python.empty() || row.static_site);
        php.choices.push_back({std::string(text(lang, own_server ? Str::ChoiceNone : Str::ChoiceNewest)), std::nullopt});
        add_series(php, installed, "php", w.php, "", "", lang);
    }
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

}  // namespace

std::vector<VersionField> version_fields(const ProjectRow& row, const Installed& installed, Lang lang) {
    return fields_of(row, installed, lang, true);
}

std::string php_off_note(const ProjectRow& row, Lang lang) {
    return row.wants.php_off && row.php_files ? std::string(text(lang, Str::PhpOffNote)) : std::string();
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

DatabaseServerState database_server(const json& databases, std::string_view database) {
    const std::size_t dash = database.find('-');
    if (dash == std::string_view::npos || !databases.is_object() || !databases.contains("servers") ||
        !databases["servers"].is_array()) {
        return {};
    }
    const std::string series = version_series(database.substr(0, dash), database.substr(dash + 1));
    for (const json& server : databases["servers"]) {
        if (version_series("mariadb", text_of(server, "series")) == series ||
            version_series("mariadb", text_of(server, "version")) == series) {
            return {text_of(server, "series"), text_of(server, "state")};
        }
    }
    return {};
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
        // Digits and single dots, starting and ending with a digit ("8", "8.2", "11.4.13").
        const bool version_ok =
            d.version.empty() ||
            (d.version.front() != '.' && d.version.back() != '.' && d.version.find("..") == std::string::npos &&
             std::all_of(d.version.begin(), d.version.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; }));
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
    if (name == "phpmyadmin") {
        return "phpMyAdmin";
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

// --- templates --------------------------------------------------------------------

namespace {

std::uint32_t colour_of(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') {
        return 0;
    }
    std::uint32_t v = 0;
    for (std::size_t i = 1; i < hex.size(); ++i) {
        const char c = hex[i];
        const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) {
            return 0;
        }
        v = v * 16 + static_cast<std::uint32_t>(d);
    }
    return v;
}

std::string text_in(const json& texts, Lang lang) {
    const char* code = lang == Lang::Ar ? "ar" : lang == Lang::Ku ? "ku" : "en";
    const std::string out = text_of(texts, code);
    return out.empty() ? text_of(texts, "en") : out;
}

}  // namespace

std::vector<TemplateCard> cards_from_list(const json& result, Lang lang) {
    std::vector<TemplateCard> out;
    if (!result.is_object() || !result.contains("templates") || !result["templates"].is_array()) {
        return out;
    }
    for (const json& t : result["templates"]) {
        TemplateCard c;
        c.id = text_of(t, "id");
        c.title = t.is_object() && t.contains("title") ? text_in(t["title"], lang) : std::string();
        if (c.id.empty() || c.title.empty()) {
            continue;
        }
        c.description = t.contains("description") ? text_in(t["description"], lang) : std::string();
        c.mark = text_of(t, "mark");
        c.mark_bg = colour_of(text_of(t, "mark_bg"));
        c.mark_fg = colour_of(text_of(t, "mark_fg"));
        if (t.contains("chips") && t["chips"].is_array()) {
            for (const json& chip : t["chips"]) {
                if (chip.is_string()) {
                    c.chips.push_back(chip.get<std::string>());
                }
            }
        }
        if (t.contains("config") && t["config"].is_object()) {
            const json& cfg = t["config"];
            c.config.known = true;
            c.config.php = text_of(cfg, "php");
            c.config.node = text_of(cfg, "node");
            c.config.python = text_of(cfg, "python");
            c.config.database = text_of(cfg, "database");
            c.config.web = text_of(cfg, "web") == "apache" ? "apache" : "nginx";
            c.uses_php = cfg.contains("php");
        }
        c.studio = flag_of(t, "studio");
        out.push_back(std::move(c));
    }
    return out;
}

namespace {

// A template's versions are the ones it was tried with, not a requirement:
// WordPress runs on any MariaDB from 10.6 and any PHP 8. When the named
// series isn't installed but the same or a newer one is, that one is picked
// (the newest), so a new project doesn't download a second MariaDB or PHP.
// The named one stays in the list, to pick by hand. An older installed
// series isn't picked: the template may need something it doesn't have.
void prefer_installed(VersionField& f, const Installed& installed) {
    const char* runtime = nullptr;
    std::string_view prefix;
    switch (f.field) {
        case Field::Php: runtime = "php"; break;
        case Field::Database: runtime = "mariadb"; prefix = "mariadb-"; break;
        case Field::Node: runtime = "node"; break;
        case Field::Python: runtime = "python"; break;
        default: return;
    }
    if (f.current >= f.choices.size()) {
        return;
    }
    const std::optional<std::string>& value = f.choices[f.current].value;
    if (!value || !value->starts_with(prefix)) {
        return;  // "none", or a database that isn't MariaDB
    }
    const std::string wanted = value->substr(prefix.size());
    const std::vector<std::string> have = series_of(installed, runtime);
    if (wanted.empty() || std::find(have.begin(), have.end(), wanted) != have.end()) {
        return;
    }
    for (std::size_t i = 0; i < f.choices.size(); ++i) {
        const std::optional<std::string>& v = f.choices[i].value;
        if (!v || !v->starts_with(prefix)) {
            continue;
        }
        const std::string s = v->substr(prefix.size());
        if (std::find(have.begin(), have.end(), s) != have.end() && !newer(wanted, s)) {
            f.current = i;  // choices list the installed series newest first
            return;
        }
    }
}

// Settings' default for the field's runtime (D20), when it's installed and
// not older than what the template names: a template tried with PHP 8.3 runs
// on the default 8.5, but isn't handed an older default 8.2.
void prefer_default(VersionField& f, const std::string& named, const Installed& installed, const Defaults& defaults) {
    const char* runtime = nullptr;
    std::string prefix;
    switch (f.field) {
        case Field::Php: runtime = "php"; break;
        case Field::Database: runtime = "mariadb"; prefix = "mariadb-"; break;
        case Field::Node: runtime = "node"; break;
        case Field::Python: runtime = "python"; break;
        default: return;
    }
    const auto it = defaults.find(runtime);
    if (it == defaults.end() || f.current >= f.choices.size()) {
        return;
    }
    // A template without a database (or with another kind) isn't given one;
    // PHP's "Newest installed" takes the default.
    const std::optional<std::string>& value = f.choices[f.current].value;
    if (f.field != Field::Php && (!value || !value->starts_with(prefix))) {
        return;
    }
    const std::vector<std::string> have = series_of(installed, runtime);
    if (std::find(have.begin(), have.end(), it->second) == have.end()) {
        return;
    }
    if (!named.empty() && newer(version_series(runtime, named), it->second)) {
        return;
    }
    for (std::size_t i = 0; i < f.choices.size(); ++i) {
        if (f.choices[i].value == prefix + it->second) {
            f.current = i;
            return;
        }
    }
}

}  // namespace

Defaults defaults_from_list(const json& result) {
    Defaults out;
    if (result.is_object() && result.contains("defaults") && result["defaults"].is_object()) {
        for (const auto& [name, series] : result["defaults"].items()) {
            if (series.is_string() && !series.get<std::string>().empty()) {
                out[name] = series.get<std::string>();
            }
        }
    }
    return out;
}

std::vector<VersionField> template_fields(const TemplateCard& card, const Installed& installed, Lang lang,
                                          const Defaults& defaults) {
    ProjectRow row;
    row.wants = card.config;
    std::vector<VersionField> all = fields_of(row, installed, lang, false);
    std::vector<VersionField> out;
    for (VersionField& f : all) {
        const bool keep = (f.field == Field::Php && card.uses_php) ||
                          (f.field == Field::Database && (card.uses_php || !card.config.database.empty())) ||
                          (f.field == Field::Web && card.uses_php) ||
                          (f.field == Field::Node && !card.config.node.empty()) ||
                          (f.field == Field::Python && !card.config.python.empty());
        if (keep) {
            prefer_installed(f, installed);
            const std::string& db = card.config.database;
            const std::string named = f.field == Field::Php        ? card.config.php
                                      : f.field == Field::Database ? (db.starts_with("mariadb-") ? db.substr(8) : std::string())
                                      : f.field == Field::Node     ? card.config.node
                                                                   : card.config.python;
            prefer_default(f, named, installed, defaults);
            out.push_back(std::move(f));
        }
    }
    return out;
}

bool is_project_name(std::string_view name) {
    return !name.empty() && name.size() <= 63 && name.front() != '-' && name.back() != '-' && !name.starts_with("xn--") &&
           std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::string free_project_name(const std::string& base, const std::vector<ProjectRow>& rows) {
    // "phpmyadmin" is kept for phpMyAdmin (phpmyadmin.localhost).
    const auto taken = [&rows](const std::string& n) {
        return n == "phpmyadmin" ||
               std::any_of(rows.begin(), rows.end(), [&](const ProjectRow& r) { return r.name == n; });
    };
    if (!taken(base)) {
        return base;
    }
    for (int i = 2; i < 1000; ++i) {
        std::string candidate = base + "-" + std::to_string(i);
        if (!taken(candidate)) {
            return candidate;
        }
    }
    return base;
}

// --- Shared sites (D19) ------------------------------------------------------------

SharedSites shared_from_reply(const json& result) {
    SharedSites out;
    out.known = true;
    out.on = flag_of(result, "on");
    out.dir = text_of(result, "dir");
    out.php = text_of(result, "php");
    out.web = text_of(result, "web") == "nginx" ? "nginx" : "apache";
    out.database = text_of(result, "database");
    if (result.is_object() && result.contains("status") && result["status"].is_object()) {
        const json& status = result["status"];
        out.address = text_of(status, "address");
        out.run = text_of(status, "run");
        out.state = text_of(status, "state");
        out.problem = text_of(status, "problem");
        if (status.contains("missing") && status["missing"].is_array()) {
            for (const json& m : status["missing"]) {
                if (m.is_string() && !m.get_ref<const std::string&>().empty()) {
                    out.missing.push_back(m.get<std::string>());
                }
            }
        }
    }
    return out;
}

bool is_shared_address(std::string_view address) {
    // https: Settings → Ports & HTTPS can serve https://localhost/.
    std::string_view rest;
    if (address.starts_with("http://localhost")) {
        rest = address.substr(std::string_view("http://localhost").size());
    } else if (address.starts_with("https://localhost")) {
        rest = address.substr(std::string_view("https://localhost").size());
    } else {
        return false;
    }
    if (rest.ends_with("/")) {
        rest.remove_suffix(1);
    }
    if (rest.empty()) {
        return true;
    }
    if (rest.front() != ':' || rest.size() < 2 || rest.size() > 6) {
        return false;
    }
    unsigned value = 0;
    for (const char c : rest.substr(1)) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value > 0 && value <= 65535;
}

SharedRow shared_row(const SharedSites& shared, const Installed& installed, Lang lang) {
    SharedRow row;
    // "localhost:8080", or "localhost" when there's no address yet (off).
    std::string host = "localhost";
    if (is_shared_address(shared.address)) {
        host = shared.address.substr(std::string_view("http://").size());
        if (host.ends_with("/")) {
            host.pop_back();
        }
    }
    if (!shared.on) {
        row.state = RowState::Off;
        row.detail = fill(lang, Str::SharedHint, {shared.dir, host + "/<folder>/"});
    } else if (shared.state == "needs_download") {
        row.state = RowState::NeedsDownload;
        std::string missing;
        for (const std::string& m : shared.missing) {
            missing += (missing.empty() ? "" : ", ") + m;
        }
        row.detail = fill(lang, Str::Needs, {missing});
    } else if (shared.state == "problem" || shared.run == "failed") {
        row.state = RowState::Problem;
        // The service's reason is in English for now.
        row.detail = shared.problem.empty() ? std::string(text(lang, Str::SharedCantStart)) : code(shared.problem, lang);
    } else {
        row.state = shared.run == "sleeping"   ? RowState::Asleep
                    : shared.run == "starting" ? RowState::Waking
                                               : RowState::On;
        row.detail = code(shared.dir, lang);
        if (is_shared_address(shared.address)) {
            row.address = host;
        }
    }
    // The PHP they get: the setting, else the newest installed series.
    std::string php = shared.php;
    if (php.empty()) {
        const std::vector<std::string> series = series_of(installed, "php");
        php = series.empty() ? std::string() : series.front();
    }
    if (!php.empty()) {
        row.chips.push_back("PHP " + php);
    }
    row.chips.emplace_back(shared.web == "nginx" ? "nginx" : "Apache");
    return row;
}

std::vector<Download> shared_downloads(const SharedSites& shared, const Installed& installed) {
    if (!shared.on || shared.state != "needs_download") {
        return {};
    }
    ProjectRow row;
    row.missing = shared.missing;
    return downloads_for(row, installed);
}

std::vector<VersionField> shared_fields(const SharedSites& shared, const Installed& installed, Lang lang) {
    // PHP and Database as a PHP project's; the Web server with what each one means here.
    ProjectRow row;
    row.wants.known = true;
    row.wants.php = shared.php;
    row.wants.database = shared.database;
    row.wants.web = shared.web;
    std::vector<VersionField> out;
    for (VersionField& f : fields_of(row, installed, lang, false)) {
        if (f.field == Field::Php || f.field == Field::Database) {
            out.push_back(std::move(f));
        }
    }
    out.push_back({Field::Web,
                   "web",
                   std::string(text(lang, Str::FieldWeb)),
                   {{std::string(text(lang, Str::WebApacheChoice)), "apache"},
                    {std::string(text(lang, Str::WebNginxChoice)), "nginx"}},
                   shared.web == "nginx" ? 1U : 0U});
    return out;
}

}  // namespace floserver::ui

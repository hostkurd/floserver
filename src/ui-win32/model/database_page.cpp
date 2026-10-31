#include "model/database_page.h"

#include "model/php_page.h"

#include <algorithm>
#include <cctype>

namespace floserver::ui {

namespace {

using nlohmann::json;

std::string text_of(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return {};
}

bool flag_of(const json& j, const char* key) {
    return j.is_object() && j.contains(key) && j[key].is_boolean() && j[key].get<bool>();
}

int int_of(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number_integer()) {
        const auto n = j[key].get<std::int64_t>();
        return n > 0 && n < 65536 ? static_cast<int>(n) : 0;
    }
    return 0;
}

std::map<std::string, std::string> strings_of(const json& j, const char* key) {
    std::map<std::string, std::string> out;
    if (j.is_object() && j.contains(key) && j[key].is_object()) {
        for (const auto& [k, v] : j[key].items()) {
            if (v.is_string()) {
                out[k] = v.get<std::string>();
            }
        }
    }
    return out;
}

std::vector<std::string> list_of(const json& j, const char* key) {
    std::vector<std::string> out;
    if (j.is_object() && j.contains(key) && j[key].is_array()) {
        for (const auto& v : j[key]) {
            if (v.is_string()) {
                out.push_back(v.get<std::string>());
            }
        }
    }
    return out;
}

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t");
    return std::string(s.substr(first, last - first + 1));
}

std::string series_value(const DbSeries& series, const std::string& key) {
    const auto it = series.ini.find(key);
    return it != series.ini.end() ? it->second : std::string();
}

constexpr std::string_view kShared = "_shared";

}  // namespace

DbServer db_server_from_json(const json& server) {
    DbServer out;
    if (!server.is_object()) {
        return out;
    }
    out.exists = true;
    out.state = text_of(server, "state");
    out.port = int_of(server, "port");
    out.port_wanted = int_of(server, "port_wanted");
    out.connections = int_of(server, "connections");
    out.error = text_of(server, "error");
    return out;
}

DbServer db_server_from_list(const json& databases, std::string_view series) {
    if (databases.is_object() && databases.contains("servers") && databases["servers"].is_array()) {
        for (const json& s : databases["servers"]) {
            if (text_of(s, "series") == series) {
                return db_server_from_json(s);
            }
        }
    }
    return {};
}

std::optional<DbSeries> db_series_from_reply(const json& reply) {
    DbSeries out;
    out.series = text_of(reply, "series");
    if (out.series.empty()) {
        return std::nullopt;
    }
    out.version = text_of(reply, "version");
    out.port = int_of(reply, "port");
    out.keep_running = flag_of(reply, "keep_running");
    out.kept_by = list_of(reply, "kept_by");
    out.ini = strings_of(reply, "ini");
    out.changed = strings_of(reply, "changed");
    out.custom_ini = text_of(reply, "custom_ini");
    out.custom_ini_exists = flag_of(reply, "custom_ini_exists");
    out.custom_problems = list_of(reply, "custom_problems");
    out.custom_keys = list_of(reply, "custom_keys");
    out.used_by = list_of(reply, "used_by");
    out.databases = list_of(reply, "databases");
    out.problems = list_of(reply, "problems");
    out.known_options = list_of(reply, "known_options");
    std::sort(out.known_options.begin(), out.known_options.end());
    out.server = db_server_from_json(reply.is_object() && reply.contains("server") ? reply["server"] : json());
    out.restarting = flag_of(reply, "restarting");
    return out;
}

std::vector<std::pair<std::string, std::string>> installed_db_series(const Installed& installed) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto it = installed.find("mariadb");
    if (it == installed.end()) {
        return out;
    }
    std::vector<std::string> versions = it->second;
    std::sort(versions.begin(), versions.end(),
              [](const std::string& a, const std::string& b) { return is_newer_version(a, b); });
    for (const std::string& v : versions) {
        const std::string series = version_series("mariadb", v);
        if (series.find('.') == std::string::npos) {
            continue;
        }
        if (std::none_of(out.begin(), out.end(), [&](const auto& s) { return s.first == series; })) {
            out.emplace_back(series, v);
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> settings_db_versions(bool has_installed, const Installed& installed,
                                                                     const json& runtimes_reply) {
    if (has_installed) {
        return installed_db_series(installed);
    }
    Installed from_reply;
    if (runtimes_reply.is_object() && runtimes_reply.contains("installed") && runtimes_reply["installed"].is_array()) {
        for (const json& r : runtimes_reply["installed"]) {
            const std::string name = text_of(r, "name");
            const std::string version = text_of(r, "version");
            if (!name.empty() && !version.empty()) {
                from_reply[name].push_back(version);
            }
        }
    }
    return installed_db_series(from_reply);
}

const std::vector<DbIniField>& settings_db_ini_fields() {
    static const std::vector<DbIniField> fields = {
        {"max_allowed_packet", "max_allowed_packet"},
        {"innodb_buffer_pool_size", "innodb_buffer_pool_size"},
        {"max_connections", "max_connections"},
        {"wait_timeout", "wait_timeout"},
        {"character_set_server", "character-set-server"},
        {"collation_server", "collation-server"},
        {"sql_mode", "sql_mode"},
        {"default_time_zone", "default-time-zone"},
    };
    return fields;
}

std::string db_option_base(std::string_view name) {
    std::string out = trim(name);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    while (out.starts_with("loose_")) {
        out.erase(0, 6);
    }
    return out;
}

std::optional<std::string> unknown_db_option(const std::vector<std::string>& known, std::string_view key) {
    const std::string base = db_option_base(key);
    if (known.empty() || base.empty()) {
        return std::nullopt;
    }
    // skip-, enable-, disable- and maximum- go with the name after them.
    for (const std::string_view prefix : {"skip_", "enable_", "disable_", "maximum_"}) {
        if (base.starts_with(prefix) && std::binary_search(known.begin(), known.end(), base.substr(prefix.size()))) {
            return std::nullopt;
        }
    }
    return unknown_ini_name(known, base);
}

std::string unknown_db_option_text(std::string_view series, std::string_view key, const std::string& suggestion,
                                   Lang lang) {
    return suggestion.empty() ? fill(lang, Str::DbUnknownOption, {series, key})
                              : fill(lang, Str::DbUnknownOptionMaybe, {series, key, suggestion});
}

// --- changes until Save --------------------------------------------------------

std::string db_ini_text(const DbSeries& series, const DbChanges& changes, const std::string& key) {
    if (const auto it = changes.ini.find(key); it != changes.ini.end()) {
        return it->second.value_or(std::string());
    }
    return series_value(series, key);
}

bool db_ini_marked(const DbSeries& series, const DbChanges& changes, const std::string& key) {
    if (const auto it = changes.ini.find(key); it != changes.ini.end()) {
        return it->second.has_value();
    }
    return series.changed.contains(key);
}

void db_set_ini_text(const DbSeries& series, DbChanges& changes, const std::string& key, const std::string& text) {
    const std::string value = trim(text);
    if (value == series_value(series, key)) {
        changes.ini.erase(key);
    } else if (value.empty()) {
        db_reset_ini(series, changes, key);
    } else {
        changes.ini[key] = value;
    }
}

void db_reset_ini(const DbSeries& series, DbChanges& changes, const std::string& key) {
    if (series.changed.contains(key)) {
        changes.ini[key] = std::nullopt;
    } else {
        changes.ini.erase(key);
    }
}

std::string db_port_text(const DbSeries& series, const DbChanges& changes) {
    if (changes.port) {
        return *changes.port;
    }
    return series.port > 0 ? std::to_string(series.port) : std::string();
}

void db_set_port_text(const DbSeries& series, DbChanges& changes, const std::string& text) {
    const std::string value = trim(text);
    const std::string saved = series.port > 0 ? std::to_string(series.port) : std::string();
    if (value == saved) {
        changes.port.reset();
    } else {
        changes.port = value;
    }
}

bool db_port_bad(const DbChanges& changes) {
    if (!changes.port || changes.port->empty()) {
        return false;
    }
    const std::string& t = *changes.port;
    if (t.size() > 5 || !std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return true;
    }
    const int n = std::stoi(t);
    return n < 1024 || n > 65535;
}

bool db_keep_running(const DbSeries& series, const DbChanges& changes) {
    return changes.keep_running.value_or(series.keep_running);
}

void db_toggle_keep_running(const DbSeries& series, DbChanges& changes) {
    const bool want = !db_keep_running(series, changes);
    if (want == series.keep_running) {
        changes.keep_running.reset();
    } else {
        changes.keep_running = want;
    }
}

json database_set_params(const std::string& series, const DbChanges& changes) {
    json params = {{"series", series}};
    if (!changes.ini.empty()) {
        json ini = json::object();
        for (const auto& [key, value] : changes.ini) {
            ini[key] = value ? json(*value) : json(nullptr);
        }
        params["ini"] = std::move(ini);
    }
    if (changes.port && !db_port_bad(changes)) {
        params["port"] = changes.port->empty() ? json(nullptr) : json(std::stoi(*changes.port));
    }
    if (changes.keep_running) {
        params["keep_running"] = *changes.keep_running;
    }
    return params;
}

std::string db_names_text(const std::vector<std::string>& names, Lang lang) {
    std::string projects;
    bool shared = false;
    for (const std::string& n : names) {
        if (n == kShared) {
            shared = true;
        } else {
            projects += (projects.empty() ? "" : ", ") + n;
        }
    }
    if (is_rtl(lang) && !projects.empty()) {
        projects = isolate_ltr(projects);
    }
    if (!shared) {
        return projects;
    }
    return projects.empty() ? std::string(text(lang, Str::PhpSharedOnly))
                            : fill_pattern(text(lang, Str::PhpAndShared), false, {projects});
}

std::string db_restart_text(const DbSeries& series, const DbServer& server, Lang lang) {
    if (series.used_by.empty() && !server.exists) {
        return fill(lang, Str::DbAppliesNobody, {series.series});
    }
    if (server.state != "running" && server.state != "starting") {
        return fill(lang, Str::DbAppliesSleeping, {series.series});
    }
    std::size_t projects = 0;
    bool shared = false;
    for (const std::string& user : series.used_by) {
        if (user == kShared) {
            shared = true;
        } else {
            ++projects;
        }
    }
    std::string who;
    if (projects == 1) {
        who = std::string(text(lang, Str::SettingsOneProject));
    } else if (projects > 1) {
        who = fill(lang, Str::SettingsProjects, {std::to_string(projects)});
    }
    if (shared) {
        who = who.empty() ? std::string(text(lang, Str::PhpSharedOnly))
                          : fill_pattern(text(lang, Str::PhpAndShared), false, {who});
    }
    if (who.empty()) {
        return fill(lang, Str::DbAppliesSleeping, {series.series});
    }
    // `who` is already in the reader's direction: only the version is isolated.
    return fill_pattern(text(lang, Str::DbApplies), false,
                        {is_rtl(lang) ? isolate_ltr(series.series) : series.series, who});
}

std::string db_state_text(const DbSeries& series, const DbServer& server, Lang lang) {
    if (!server.exists) {
        return std::string(text(lang, Str::DbNotInUse));
    }
    const std::string port = std::to_string(server.port);
    std::string line;
    if (server.state == "running") {
        line = fill(lang, Str::SettingsRunningOnPort, {port});
        if (!line.empty()) {
            // "running on port 3306" starts the line here.
            const auto c = static_cast<unsigned char>(line.front());
            if (c >= 'a' && c <= 'z') {
                line.front() = static_cast<char>(c - 'a' + 'A');
            }
        }
    } else if (server.state == "starting") {
        line = std::string(text(lang, Str::SettingsStarting));
    } else if (server.state == "stopping") {
        line = std::string(text(lang, Str::DbStopping));
    } else if (server.state == "failed") {
        line = fill(lang, Str::DbFailed, {server.error});
    } else {
        line = fill(lang, Str::DbSleeping, {port});
    }
    if (server.port_wanted > 0) {
        line += " " + fill(lang, Str::DbPortTaken, {std::to_string(server.port_wanted), port});
    }
    (void)series;
    return line;
}

DbActions db_actions(const DbServer& server) {
    DbActions out;
    if (!server.exists) {
        return out;
    }
    const bool running = server.state == "running";
    const bool busy = server.state == "starting" || server.state == "stopping";
    out.start = !running && !busy;
    out.stop = running;
    out.restart = running;
    return out;
}

std::optional<Str> db_password_problem(std::string_view password) {
    if (password.size() < 8) {
        return Str::DbPasswordShort;
    }
    if (password.size() > 100) {
        return Str::DbPasswordLong;
    }
    if (!std::all_of(password.begin(), password.end(), [](char c) { return c > 0x20 && c < 0x7f; })) {
        return Str::DbPasswordChars;
    }
    return std::nullopt;
}

}  // namespace floserver::ui

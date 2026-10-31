#include "model/php_page.h"

#include <algorithm>
#include <array>
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

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t");
    return std::string(s.substr(first, last - first + 1));
}

// The ones people turn on most, in that order, after those already on.
constexpr std::array kPopular{"exif", "bcmath", "gmp", "soap", "xsl", "pdo_pgsql", "pgsql", "ldap", "sockets",
                              "imap", "tidy", "ftp", "xdebug"};

const PhpExtension* find_extension(const PhpSeries& series, const std::string& name) {
    const auto it = std::find_if(series.extensions.begin(), series.extensions.end(),
                                 [&](const PhpExtension& e) { return e.name == name; });
    return it != series.extensions.end() ? &*it : nullptr;
}

std::string series_value(const PhpSeries& series, const std::string& key) {
    const auto it = series.ini.find(key);
    return it != series.ini.end() ? it->second : std::string();
}

}  // namespace

std::optional<PhpSeries> php_series_from_reply(const json& reply) {
    PhpSeries out;
    out.series = text_of(reply, "series");
    if (out.series.empty()) {
        return std::nullopt;
    }
    out.version = text_of(reply, "version");
    out.extensions_fixed = flag_of(reply, "extensions_fixed");
    if (reply.contains("extensions") && reply["extensions"].is_array()) {
        for (const auto& e : reply["extensions"]) {
            PhpExtension ext;
            ext.name = text_of(e, "name");
            if (ext.name.empty()) {
                continue;
            }
            ext.on = flag_of(e, "on");
            ext.base = flag_of(e, "base");
            ext.missing = flag_of(e, "missing");
            out.extensions.push_back(std::move(ext));
        }
    }
    out.ini = strings_of(reply, "ini");
    out.changed = strings_of(reply, "changed");
    out.custom_ini = text_of(reply, "custom_ini");
    out.custom_ini_exists = flag_of(reply, "custom_ini_exists");
    out.used_by = list_of(reply, "used_by");
    out.problems = list_of(reply, "problems");
    return out;
}

std::vector<std::pair<std::string, std::string>> installed_php_series(const Installed& installed) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto it = installed.find("php");
    if (it == installed.end()) {
        return out;
    }
    std::vector<std::string> versions = it->second;
    std::sort(versions.begin(), versions.end(),
              [](const std::string& a, const std::string& b) { return is_newer_version(a, b); });
    for (const std::string& v : versions) {
        const std::string series = version_series("php", v);
        if (series.find('.') == std::string::npos) {
            continue;
        }
        if (std::none_of(out.begin(), out.end(), [&](const auto& s) { return s.first == series; })) {
            out.emplace_back(series, v);
        }
    }
    return out;
}

const std::vector<PhpIniField>& settings_php_ini_fields() {
    static const std::vector<PhpIniField> fields = {
        {"memory_limit", {}},
        {"upload_max_filesize", {}},
        {"post_max_size", {}},
        {"max_execution_time", {}},
        {"max_input_vars", {}},
        {"date.timezone", {}},
        {"display_errors", {"On", "Off"}},
        {"error_reporting", {"E_ALL", "E_ALL & ~E_DEPRECATED", "E_ALL & ~E_NOTICE & ~E_DEPRECATED"}},
    };
    return fields;
}

const std::vector<PhpIniField>& project_php_ini_fields() {
    static const std::vector<PhpIniField> fields = {
        {"memory_limit", {}},
        {"upload_max_filesize", {}},
        {"post_max_size", {}},
        {"max_execution_time", {}},
        {"display_errors", {"On", "Off"}},
    };
    return fields;
}

std::vector<PhpExtension> ordered_extensions(const PhpSeries& series) {
    std::vector<PhpExtension> on;
    std::vector<PhpExtension> popular;
    std::vector<PhpExtension> rest;
    for (const PhpExtension& e : series.extensions) {
        if (e.on) {
            on.push_back(e);
        } else if (std::find(kPopular.begin(), kPopular.end(), e.name) != kPopular.end()) {
            popular.push_back(e);
        } else {
            rest.push_back(e);
        }
    }
    const auto rank = [](const PhpExtension& e) {
        return std::find(kPopular.begin(), kPopular.end(), e.name) - kPopular.begin();
    };
    std::stable_sort(popular.begin(), popular.end(),
                     [&](const PhpExtension& a, const PhpExtension& b) { return rank(a) < rank(b); });
    std::sort(rest.begin(), rest.end(), [](const PhpExtension& a, const PhpExtension& b) { return a.name < b.name; });
    on.insert(on.end(), popular.begin(), popular.end());
    on.insert(on.end(), rest.begin(), rest.end());
    return on;
}

bool extension_matches(std::string_view name, std::string_view query) {
    const std::string q = trim(query);
    if (q.empty()) {
        return true;
    }
    std::string n(name);
    std::string lq = q;
    std::transform(n.begin(), n.end(), n.begin(), lower);
    std::transform(lq.begin(), lq.end(), lq.begin(), lower);
    return n.find(lq) != std::string::npos;
}

bool is_slow_extension(std::string_view name) { return name == "xdebug"; }

// --- Settings > PHP ---------------------------------------------------------------

bool extension_on(const PhpSeries& series, const PhpChanges& changes, const std::string& name) {
    if (const auto it = changes.extensions.find(name); it != changes.extensions.end()) {
        return it->second;
    }
    const PhpExtension* e = find_extension(series, name);
    return e != nullptr && e->on;
}

void toggle_extension(const PhpSeries& series, PhpChanges& changes, const std::string& name) {
    const bool want = !extension_on(series, changes, name);
    const PhpExtension* e = find_extension(series, name);
    if (e != nullptr && e->on == want) {
        changes.extensions.erase(name);
    } else {
        changes.extensions[name] = want;
    }
}

std::size_t extensions_on(const PhpSeries& series, const PhpChanges& changes) {
    return static_cast<std::size_t>(std::count_if(series.extensions.begin(), series.extensions.end(),
                                                  [&](const PhpExtension& e) { return extension_on(series, changes, e.name); }));
}

std::string ini_text(const PhpSeries& series, const PhpChanges& changes, const std::string& key) {
    if (const auto it = changes.ini.find(key); it != changes.ini.end()) {
        return it->second.value_or(std::string());
    }
    return series_value(series, key);
}

bool ini_marked(const PhpSeries& series, const PhpChanges& changes, const std::string& key) {
    if (const auto it = changes.ini.find(key); it != changes.ini.end()) {
        return it->second.has_value();
    }
    return series.changed.contains(key);
}

void set_ini_text(const PhpSeries& series, PhpChanges& changes, const std::string& key, const std::string& text) {
    const std::string value = trim(text);
    if (value == series_value(series, key)) {
        changes.ini.erase(key);  // what it is already
    } else if (value.empty()) {
        reset_ini(series, changes, key);
    } else {
        changes.ini[key] = value;
    }
}

void reset_ini(const PhpSeries& series, PhpChanges& changes, const std::string& key) {
    if (series.changed.contains(key)) {
        changes.ini[key] = std::nullopt;
    } else {
        changes.ini.erase(key);  // it is the default already
    }
}

json php_set_params(const std::string& series, const PhpChanges& changes) {
    json params = {{"series", series}};
    if (!changes.extensions.empty()) {
        json ext = json::object();
        for (const auto& [name, on] : changes.extensions) {
            ext[name] = on;
        }
        params["extensions"] = std::move(ext);
    }
    if (!changes.ini.empty()) {
        json ini = json::object();
        for (const auto& [key, value] : changes.ini) {
            ini[key] = value ? json(*value) : json(nullptr);
        }
        params["ini"] = std::move(ini);
    }
    return params;
}

std::string php_restart_text(const PhpSeries& series, Lang lang) {
    std::size_t projects = 0;
    bool shared = false;
    for (const std::string& user : series.used_by) {
        if (user == "_shared") {
            shared = true;
        } else {
            ++projects;
        }
    }
    if (projects == 0 && !shared) {
        return fill(lang, Str::PhpAppliesNobody, {series.series});
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
    return fill_pattern(text(lang, Str::PhpApplies), false, {who});
}

bool php_answer_matches(std::string_view to, std::string_view series, std::string_view shown_to,
                        std::string_view shown_series) {
    return to == shown_to && (series.empty() || series == shown_series);
}

std::string ini_error_key(std::string_view message, const std::vector<std::string>& keys) {
    std::string best;
    for (const std::string& key : keys) {
        if (!key.empty() && key.size() > best.size() && message.starts_with(key) && message.size() > key.size() &&
            (message[key.size()] == ':' || message[key.size()] == ' ')) {
            best = key;
        }
    }
    return best;
}

// --- the project panel's PHP part -------------------------------------------------

bool project_uses_php(const ProjectRow& row) {
    if (!row.php.empty() || !row.wants.php.empty()) {
        return true;
    }
    if (row.static_site) {
        return false;
    }
    return row.wants.node.empty() && row.wants.python.empty();
}

std::optional<std::string> project_php_series(const ProjectRow& row, const Installed& installed) {
    for (const std::string& v : {row.php, row.wants.php}) {
        if (!v.empty()) {
            std::string series = version_series("php", v);
            if (series.find('.') != std::string::npos) {
                return series;
            }
        }
    }
    const auto all = installed_php_series(installed);
    if (!all.empty()) {
        return all.front().first;
    }
    return std::nullopt;
}

ProjectPhp project_php_of(const ProjectRow& row) {
    ProjectPhp out;
    for (const std::string& e : row.wants.extensions) {
        if (e.starts_with('-')) {
            if (e.size() > 1) {
                out.extensions[e.substr(1)] = false;
            }
        } else {
            out.extensions[e] = true;
        }
    }
    out.ini = row.wants.php_ini;
    return out;
}

bool project_extension_on(const PhpSeries* series, const ProjectPhp& php, const std::string& name) {
    if (const auto it = php.extensions.find(name); it != php.extensions.end()) {
        return it->second;
    }
    if (series != nullptr) {
        const PhpExtension* e = find_extension(*series, name);
        return e != nullptr && e->on;
    }
    return false;
}

void toggle_project_extension(const PhpSeries* series, ProjectPhp& php, const std::string& name) {
    const bool want = !project_extension_on(series, php, name);
    const bool series_on = series != nullptr && project_extension_on(series, ProjectPhp{}, name);
    if (want == series_on) {
        php.extensions.erase(name);
    } else {
        php.extensions[name] = want;
    }
}

void set_project_ini(ProjectPhp& php, const std::string& key, const std::string& text) {
    const std::string value = trim(text);
    if (value.empty()) {
        php.ini.erase(key);
    } else {
        php.ini[key] = value;
    }
}

std::optional<std::pair<std::string, std::string>> parse_ini_line(std::string_view line) {
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }
    std::string key = trim(line.substr(0, eq));
    if (key.empty()) {
        return std::nullopt;
    }
    return std::pair{std::move(key), trim(line.substr(eq + 1))};
}

json project_php_params(const std::string& project, const ProjectPhp& before, const ProjectPhp& after) {
    json params = {{"name", project}};
    json ext = json::object();
    for (const auto& [name, on] : after.extensions) {
        const auto it = before.extensions.find(name);
        if (it == before.extensions.end() || it->second != on) {
            ext[name] = on ? "on" : "off";
        }
    }
    for (const auto& [name, on] : before.extensions) {
        if (!after.extensions.contains(name)) {
            ext[name] = nullptr;
        }
    }
    json ini = json::object();
    for (const auto& [key, value] : after.ini) {
        const auto it = before.ini.find(key);
        if (it == before.ini.end() || it->second != value) {
            ini[key] = value;
        }
    }
    for (const auto& [key, value] : before.ini) {
        if (!after.ini.contains(key)) {
            ini[key] = nullptr;
        }
    }
    if (!ext.empty()) {
        params["extensions"] = std::move(ext);
    }
    if (!ini.empty()) {
        params["ini"] = std::move(ini);
    }
    return params;
}

}  // namespace floserver::ui

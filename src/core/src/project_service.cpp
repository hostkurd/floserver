#include "floserver/core/project_service.h"

#include "floserver/core/shared_sites.h"

#include "floserver/core/paths.h"
#include "floserver/core/php_pool.h"
#include "floserver/core/php_settings.h"
#include "floserver/core/site_plan.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string_view>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const char* state_name(SiteState s) {
    switch (s) {
        case SiteState::Off:
            return "off";
        case SiteState::Ready:
            return "ready";
        case SiteState::NeedsDownload:
            return "needs_download";
        case SiteState::Problem:
            return "problem";
    }
    return "problem";
}

json site_json(const Project& project, const PlannedSite& site, const WebPlan& plan) {
    json out = {{"name", project.name},
                {"path", path_to_utf8(project.dir)},
                {"on", project.on},
                {"keep_running", project.keep_running},
                {"state", state_name(site.state)},
                {"missing", site.missing}};
    if (site.pool) {
        out["php"] = plan.pools.at(*site.pool).version;
    }
    if (site.apache) {
        out["web"] = "apache";
    }
    if (site.static_site) {
        out["static"] = true;
    }
    if (site.php_files) {
        out["php_files"] = true;  // PHP is off and there are .php files: they're sent as they are
    }
    if (site.app) {
        const AppSpec& app = plan.apps.at(*site.app);
        out[app.kind == AppKind::Python ? "python" : "node"] = app.version;
        if (!app.node_version.empty()) {
            out["node"] = app.node_version;
        }
    }
    if (!site.problem.empty()) {
        out["problem"] = site.problem;
    }
    if (site.database) {
        out["database"] = site.database->family + "-" + site.database->version;
    }
    // As floserver.json says it: the window's version choices start here.
    if (site.config) {
        const ProjectConfig& c = *site.config;
        json wants = {{"web", c.web == WebServer::Apache ? "apache" : "nginx"}};
        if (c.php) {
            wants["php"] = *c.php;
        } else if (c.php_choice == PhpChoice::Off) {
            wants["php"] = false;
        } else if (c.php_choice == PhpChoice::Newest) {
            wants["php"] = std::string(kPhpNewest);
        }
        if (c.node) {
            wants["node"] = *c.node;
        }
        if (c.python) {
            wants["python"] = *c.python;
        }
        if (c.database) {
            wants["database"] = c.database->family + "-" + c.database->version;
        }
        // The project's PHP changes (D20), as the file lists them.
        if (!c.extensions.empty() || !c.extensions_off.empty()) {
            json list = c.extensions;
            for (const std::string& e : c.extensions_off) {
                list.push_back("-" + e);
            }
            wants["extensions"] = std::move(list);
        }
        if (!c.php_ini.empty()) {
            wants["php_ini"] = c.php_ini;
        }
        out["wants"] = std::move(wants);
    }
    return out;
}

WebPlan plan_for(const AppConfig& config, const fs::path& data_dir) {
    return plan_sites(config, read_project_config_file, list_installed(runtimes_dir(data_dir)), std::nullopt);
}

// A project that is Off is still described with what it would need, so the
// window can show "Needs download" before it's turned on.
json describe_all(const AppConfig& config, const fs::path& data_dir) {
    AppConfig all_on = config;
    for (Project& p : all_on.projects) {
        p.on = true;
    }
    const WebPlan plan = plan_for(all_on, data_dir);
    json list = json::array();
    for (std::size_t i = 0; i < config.projects.size(); ++i) {
        json item = site_json(config.projects[i], plan.sites.at(i), plan);
        if (!config.projects[i].on) {
            item["state"] = "off";
        }
        list.push_back(std::move(item));
    }
    return list;
}

}  // namespace

ProjectService::ProjectService(fs::path data_dir) : data_dir_(std::move(data_dir)) {}

std::optional<AppConfig> ProjectService::load(ServiceRefusal& refusal) const {
    std::string error;
    auto config = load_app_config(app_config_file(data_dir_), error);
    if (!config) {
        refusal = {"config_unreadable", "FloServer's settings file can't be used: " + error};
    }
    return config;
}

bool ProjectService::save(const AppConfig& config, ServiceRefusal& refusal) const {
    std::string error;
    if (!save_app_config(app_config_file(data_dir_), config, error)) {
        refusal = {"config_unwritable", "FloServer couldn't save its settings: " + error};
        return false;
    }
    return true;
}

json ProjectService::describe(const AppConfig& config, const std::string& name) const {
    const json all = describe_all(config, data_dir_);
    for (const json& item : all) {
        if (item["name"] == name) {
            return item;
        }
    }
    return json::object();
}

std::optional<json> ProjectService::list(ServiceRefusal& refusal) const {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    const auto dir = config->projects_dir ? config->projects_dir : default_projects_dir();
    return json{{"projects", describe_all(*config, data_dir_)},
                {"projects_dir", dir ? json(path_to_utf8(*dir)) : json(nullptr)}};
}

std::optional<json> ProjectService::add(const fs::path& dir, const std::string& name, ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    std::string error;
    const auto added = add_project(*config, dir, name, error);
    if (!added) {
        refusal = {"bad_params", error};
        return std::nullopt;
    }
    if (!save(*config, refusal)) {
        return std::nullopt;
    }
    return describe(*config, added->name);
}

bool ProjectService::remove(const std::string& name, ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return false;
    }
    if (!remove_project(*config, name)) {
        refusal = {"unknown_project", "there's no project called " + name};
        return false;
    }
    return save(*config, refusal);
}

std::optional<json> ProjectService::set(const std::string& name, std::optional<bool> on,
                                        std::optional<bool> keep_running, ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    Project* project = find_project(*config, name);
    if (project == nullptr) {
        refusal = {"unknown_project", "there's no project called " + name};
        return std::nullopt;
    }
    if (on) {
        project->on = *on;
    }
    if (keep_running) {
        project->keep_running = *keep_running;
    }
    if (!save(*config, refusal)) {
        return std::nullopt;
    }
    return describe(*config, name);
}

std::optional<json> ProjectService::use(const std::string& name, const std::string& key,
                                        const std::optional<std::string>& value, ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    const auto parsed_key = parse_config_key(key);
    if (!parsed_key || key == "mariadb") {
        refusal = {"bad_params", "the key is php, node, python, database or web"};
        return std::nullopt;
    }
    const Project* project = find_project(*config, name);
    if (project == nullptr) {
        refusal = {"unknown_project", "there's no project called " + name};
        return std::nullopt;
    }
    std::string error;
    if (!set_project_config_file_value(project->dir, *parsed_key, key, value, error)) {
        refusal = {"config_refused", error};
        return std::nullopt;
    }
    return describe(*config, name);
}

std::optional<fs::path> ProjectService::folder(const std::string& name, ServiceRefusal& refusal) const {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    for (const Project& p : config->projects) {
        if (p.name == name) {
            return p.dir;
        }
    }
    refusal = {"unknown_project", "there's no project called " + name};
    return std::nullopt;
}

std::optional<json> ProjectService::describe_shared(const AppConfig& config, ServiceRefusal& refusal,
                                                    bool folders) const {
    std::string error;
    const auto shared = read_shared_config(config, error);
    if (!shared) {
        refusal = {"config_unreadable", "FloServer's settings for shared sites can't be used: " + error};
        return std::nullopt;
    }
    json out = {{"on", shared->on}, {"web", shared->web == WebServer::Apache ? "apache" : "nginx"}};
    const auto dir = shared_dir(*shared);
    out["dir"] = dir ? json(path_to_utf8(*dir)) : json(nullptr);
    if (const auto d = default_shared_dir()) {
        out["default_dir"] = path_to_utf8(*d);
    }
    if (shared->php) {
        out["php"] = *shared->php;
    }
    if (shared->database) {
        out["database"] = shared->database->family + "-" + shared->database->version;
    }
    if (!folders) {
        return out;  // the www folder isn't read
    }
    json list = json::array();
    if (dir) {
        for (const SharedFolder& f : scan_shared_folders(*dir, config)) {
            json j = {{"name", f.name}, {"kind", shared_kind_name(f.kind)}, {"host", f.host_name}};
            if (f.clash) {
                j["clash"] = *f.clash;
            }
            list.push_back(std::move(j));
        }
    }
    out["folders"] = std::move(list);
    return out;
}

namespace {

constexpr std::array<std::string_view, 4> kDefaultRuntimes = {"php", "mariadb", "node", "python"};

// config.json's "defaults", keeping only what this version understands.
json read_defaults(const AppConfig& config) {
    json out = json::object();
    // A copy (it's a few keys): GCC's -O3 null-dereference check trips on
    // iterators into nlohmann::json.
    const json stored = config.other.is_object() && config.other.contains("defaults") ? config.other.at("defaults")
                                                                                      : json();
    if (!stored.is_object()) {
        return out;
    }
    for (const std::string_view name : kDefaultRuntimes) {
        const std::string key(name);
        if (stored.contains(key) && stored.at(key).is_string() && Version::parse(stored.at(key).get<std::string>())) {
            out[key] = stored.at(key);
        }
    }
    return out;
}

}  // namespace

std::optional<json> ProjectService::runtime_users(const std::vector<InstalledRuntime>& installed,
                                                  ServiceRefusal& refusal) const {
    AppConfig config;
    {
        const std::lock_guard lock(mutex_);
        auto loaded = load(refusal);
        if (!loaded) {
            return std::nullopt;
        }
        config = std::move(*loaded);
    }
    // The floserver.json files are read without the lock: a slow disk
    // doesn't hold up other requests.
    json out = json::object();
    for (auto& [key, who] : floserver::core::runtime_users(config, read_project_config_file, installed, std::nullopt)) {
        out[key] = who;
    }
    return out;
}

std::optional<json> ProjectService::defaults(ServiceRefusal& refusal) const {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    return read_defaults(*config);
}

std::optional<json> ProjectService::set_default(const std::string& runtime, const std::optional<std::string>& series,
                                                ServiceRefusal& refusal) {
    if (std::find(kDefaultRuntimes.begin(), kDefaultRuntimes.end(), runtime) == kDefaultRuntimes.end()) {
        refusal = {"bad_params", "a default can be set for php, mariadb, node or python, not \"" + runtime + "\""};
        return std::nullopt;
    }
    if (series && !Version::parse(*series)) {
        refusal = {"bad_params", "\"" + *series + "\" isn't a version such as 8.5"};
        return std::nullopt;
    }
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    json& stored = config->other["defaults"];
    if (!stored.is_object()) {
        stored = json::object();  // unusable before: replaced
    }
    if (series) {
        stored[runtime] = *series;
    } else {
        stored.erase(runtime);
    }
    if (stored.empty()) {
        config->other.erase("defaults");
    }
    if (!save(*config, refusal)) {
        return std::nullopt;
    }
    return read_defaults(*config);
}

std::optional<json> ProjectService::shared(ServiceRefusal& refusal, bool folders) const {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    return describe_shared(*config, refusal, folders);
}

std::optional<json> ProjectService::set_shared(const json& changes, ServiceRefusal& refusal, bool folders) {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    std::string error;
    auto shared = read_shared_config(*config, error);
    if (!shared) {
        refusal = {"config_unreadable", "FloServer's settings for shared sites can't be used: " + error};
        return std::nullopt;
    }
    if (!changes.is_object()) {
        refusal = {"bad_params", "the changes must be an object"};
        return std::nullopt;
    }
    for (const auto& [key, value] : changes.items()) {
        if (key == "on") {
            if (!value.is_boolean()) {
                refusal = {"bad_params", "on must be true or false"};
                return std::nullopt;
            }
            shared->on = value.get<bool>();
        } else if (key == "dir") {
            if (value.is_null()) {
                shared->dir.reset();
                continue;
            }
            if (!value.is_string()) {
                refusal = {"bad_params", "dir must be a folder"};
                return std::nullopt;
            }
            const auto real = real_folder(path_from_utf8(value.get<std::string>()), error);
            if (!real) {
                refusal = {"bad_params", error};
                return std::nullopt;
            }
            shared->dir = *real;
        } else if (key == "php" || key == "web" || key == "database") {
            if (value.is_null() && key != "web") {
                if (key == "php") {
                    shared->php.reset();
                } else {
                    shared->database.reset();
                }
                continue;
            }
            // Checked by floserver.json's own rules.
            const ConfigResult parsed = parse_project_config(json{{key, value}}.dump());
            if (!parsed.config) {
                const std::string why = parsed.errors.empty() ? std::string("isn't valid") : parsed.errors.front().message;
                std::string message = key;
                message.append(" ").append(why);
                refusal = {"bad_params", std::move(message)};
                return std::nullopt;
            }
            if (key == "php" && php_off(*parsed.config)) {
                refusal = {"bad_params", "php can't be false: shared sites always have PHP"};
                return std::nullopt;
            }
            if (key == "php") {
                shared->php = parsed.config->php;  // "newest": none named, the newest
            } else if (key == "web") {
                shared->web = parsed.config->web;
            } else {
                shared->database = parsed.config->database;
            }
        } else {
            refusal = {"bad_params", "unknown setting " + key + " (on, dir, php, web, database)"};
            return std::nullopt;
        }
    }
    // The default www folder is made when shared sites are turned on.
    if (shared->on && !shared->dir) {
        if (const auto d = default_shared_dir()) {
            std::error_code ec;
            fs::create_directories(*d, ec);
            if (ec) {
                refusal = {"folder_unwritable", "can't make " + path_to_utf8(*d) + ": " + error_text(ec)};
                return std::nullopt;
            }
        }
    }
    write_shared_config(*config, *shared);
    if (!save(*config, refusal)) {
        return std::nullopt;
    }
    return describe_shared(*config, refusal, folders);
}

namespace {

// "8.5" from a request: two numbers, nothing else.
std::optional<std::string> request_series(const std::string& series, ServiceRefusal& refusal) {
    const auto v = Version::parse(series);
    if (!v || v->parts().size() != 2) {
        refusal = {"bad_params", "\"" + series + "\" isn't a PHP series such as 8.5"};
        return std::nullopt;
    }
    return v->to_string();
}

}  // namespace

std::optional<json> ProjectService::php_get(const std::string& series_text, ServiceRefusal& refusal) const {
    const auto series = request_series(series_text, refusal);
    if (!series) {
        return std::nullopt;
    }
    AppConfig config;
    {
        const std::lock_guard lock(mutex_);
        auto loaded = load(refusal);
        if (!loaded) {
            return std::nullopt;
        }
        config = std::move(*loaded);
    }
    std::vector<std::string> problems;
    const PhpSettings all = read_php_settings(config, &problems);
    const auto found = all.find(*series);
    const PhpSeriesSettings settings = found != all.end() ? found->second : PhpSeriesSettings{};
    const auto installed = list_installed(runtimes_dir(data_dir_));
    const InstalledRuntime* php = best_installed(installed, "php", *series);

    json out = {{"series", *series}, {"version", php != nullptr ? json(php->version) : json(nullptr)}};

    // Extensions: on Windows the ones the install has (ext/php_*.dll); with
    // builds that come with their extensions, the base list as it is.
    const auto& base = base_php_extensions();
    const auto available = php != nullptr ? available_php_extensions(php->dir) : std::nullopt;
    out["extensions_fixed"] = !available.has_value();
    json list = json::array();
    const auto is_base = [&base](const std::string& e) { return std::find(base.begin(), base.end(), e) != base.end(); };
    const auto on = [&](const std::string& e) {
        const auto it = settings.extensions.find(e);
        return it != settings.extensions.end() ? it->second : is_base(e);
    };
    const auto has = [&available](const std::string& e) {
        return !available || std::find(available->begin(), available->end(), e) != available->end();
    };
    for (const std::string& e : base) {
        json item = {{"name", e}, {"on", available ? on(e) : true}, {"base", true}};
        if (!has(e)) {
            item["missing"] = true;
        }
        list.push_back(std::move(item));
    }
    if (available) {
        std::set<std::string> others(available->begin(), available->end());
        for (const auto& [e, want] : settings.extensions) {
            if (want) {
                others.insert(e);  // turned on, even if this install lacks it: shown as missing
            }
        }
        for (const std::string& e : others) {
            if (is_base(e) || e == "opcache") {
                continue;
            }
            json item = {{"name", e}, {"on", on(e)}, {"base", false}};
            if (!has(e)) {
                item["missing"] = true;
            }
            list.push_back(std::move(item));
        }
    }
    out["extensions"] = std::move(list);

    json ini = json::object();
    for (const auto& [k, v] : php_ini_defaults()) {
        ini[k] = v;
    }
    for (const auto& [k, v] : settings.ini) {
        ini[k] = v;
    }
    out["ini"] = std::move(ini);
    out["changed"] = settings.ini;

    const fs::path custom = php_custom_ini_file(data_dir_, *series);
    std::error_code ec;
    out["custom_ini"] = path_to_utf8(custom);
    out["custom_ini_exists"] = fs::is_regular_file(custom, ec);

    // Who's on this series: the users of every install of it.
    std::set<std::string> users;
    const auto by_runtime = floserver::core::runtime_users(config, read_project_config_file, installed, std::nullopt);
    for (const InstalledRuntime& r : installed) {
        if (r.name == "php" && php_series_of(r.version) == *series) {
            const auto it = by_runtime.find("php/" + path_to_utf8(r.dir.filename()));
            if (it != by_runtime.end()) {
                users.insert(it->second.begin(), it->second.end());
            }
        }
    }
    out["used_by"] = users;
    if (!problems.empty()) {
        out["problems"] = problems;
    }
    // The names PHP knows, so the window can warn about one it would ignore.
    if (php != nullptr) {
        const std::string dir = path_to_utf8(php->dir);
        bool failed_before = false;
        {
            const std::lock_guard lock(known_mutex_);
            failed_before = known_failed_.contains(dir);
        }
        if (!failed_before) {
            const fs::path cache = php_custom_ini_file(data_dir_, *series).parent_path() /
                                   path_from_utf8("known-ini-" + path_to_utf8(php->dir.filename()) + ".json");
            std::string error;
            bool timed_out = false;
            if (const auto known = known_php_ini_names(php->dir, cache, error, timed_out)) {
                out["known_ini"] = *known;
            } else if (!timed_out) {
                // PHP itself can't list them: not asked again while the service runs.
                // A run that was stopped for taking too long is tried next time.
                const std::lock_guard lock(known_mutex_);
                known_failed_.insert(dir);
            }
        }
    }
    return out;
}

std::optional<json> ProjectService::php_set(const std::string& series_text, const json& extensions, const json& ini,
                                            ServiceRefusal& refusal) {
    const auto series = request_series(series_text, refusal);
    if (!series) {
        return std::nullopt;
    }
    if (!extensions.is_null() && !extensions.is_object()) {
        refusal = {"bad_params", R"(extensions is an object such as {"bcmath": true})"};
        return std::nullopt;
    }
    if (!ini.is_null() && !ini.is_object()) {
        refusal = {"bad_params", R"(ini is an object such as {"memory_limit": "1G"})"};
        return std::nullopt;
    }
    {
        const std::lock_guard lock(mutex_);
        auto config = load(refusal);
        if (!config) {
            return std::nullopt;
        }
        const PhpSettings all = read_php_settings(*config);
        const auto found = all.find(*series);
        PhpSeriesSettings settings = found != all.end() ? found->second : PhpSeriesSettings{};
        const auto& base = base_php_extensions();
        if (extensions.is_object()) {
            for (const auto& [name, value] : extensions.items()) {
                if (!is_php_extension_name(name)) {
                    refusal = {"bad_params", "\"" + name + "\" isn't an extension name (a-z, 0-9, _)"};
                    return std::nullopt;
                }
                if (name == "opcache") {
                    refusal = {"bad_params", "opcache is always on"};
                    return std::nullopt;
                }
                if (!value.is_null() && !value.is_boolean()) {
                    refusal = {"bad_params", "extension " + name + " is true, false or null"};
                    return std::nullopt;
                }
                const bool is_base = std::find(base.begin(), base.end(), name) != base.end();
                // Only changes from the base list are kept.
                if (value.is_null() || value.get<bool>() == is_base) {
                    settings.extensions.erase(name);
                } else {
                    settings.extensions[name] = value.get<bool>();
                }
            }
        }
        if (ini.is_object()) {
            for (const auto& [key, value] : ini.items()) {
                if (const auto problem = php_ini_key_problem(key)) {
                    refusal = {"bad_params", *problem};
                    return std::nullopt;
                }
                if (value.is_null()) {
                    settings.ini.erase(key);
                    continue;
                }
                if (!value.is_string()) {
                    refusal = {"bad_params", key + " is text, or null for the default"};
                    return std::nullopt;
                }
                if (const auto problem = php_ini_value_problem(value.get<std::string>())) {
                    refusal = {"bad_params", key + ": " + *problem};
                    return std::nullopt;
                }
                settings.ini[key] = value.get<std::string>();
            }
        }
        write_php_series(*config, *series, settings);
        if (!save(*config, refusal)) {
            return std::nullopt;
        }
    }
    return php_get(*series, refusal);
}

std::optional<fs::path> ProjectService::php_custom_ini(const std::string& series_text, ServiceRefusal& refusal) const {
    const auto series = request_series(series_text, refusal);
    if (!series) {
        return std::nullopt;
    }
    std::string error;
    if (!ensure_php_custom_ini(data_dir_, *series, error)) {
        refusal = {"folder_unwritable", error};
        return std::nullopt;
    }
    return php_custom_ini_file(data_dir_, *series);
}

std::optional<fs::path> ProjectService::php_final_ini(const std::string& series_text, ServiceRefusal& refusal) const {
    const auto series = request_series(series_text, refusal);
    if (!series) {
        return std::nullopt;
    }
    AppConfig config;
    {
        const std::lock_guard lock(mutex_);
        auto loaded = load(refusal);
        if (!loaded) {
            return std::nullopt;
        }
        config = std::move(*loaded);
    }
    const auto installed = list_installed(runtimes_dir(data_dir_));
    const InstalledRuntime* php = best_installed(installed, "php", *series);
    if (php == nullptr) {
        refusal = {"not_installed", "PHP " + *series + " isn't installed"};
        return std::nullopt;
    }
    const PhpSettings all = read_php_settings(config);
    const auto found = all.find(*series);
    const PhpIniSettings settings = effective_php_settings(
        *series, found != all.end() ? &found->second : nullptr, read_php_custom_ini(data_dir_, *series), nullptr);
    // Each pool logs to its own file; the real path is in the pool's copy.
    const PhpIniResult made = make_php_ini(php->dir, settings, fs::path("(each project's own log)"));
    const std::string text = "; What PHP " + php->version +
                             " uses for shared sites and for projects without PHP changes of their own.\n"
                             "; Written by FloServer when you ask to see it: edits here are lost and do nothing.\n"
                             "; Change values in Settings > PHP, or add your own lines to custom.ini beside this file.\n"
                             "; A project's own changes (its floserver.json) come after all of this.\n\n" +
                             made.text;
    fs::path file = php_custom_ini_file(data_dir_, *series).parent_path() / "final-php.ini";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    // Read-only, so Notepad says so on Save; made writable again to replace it.
    fs::permissions(file, fs::perms::owner_write, fs::perm_options::add, ec);
    std::string error;
    if (!write_file_atomically(file, text, error)) {
        refusal = {"folder_unwritable", error};
        return std::nullopt;
    }
    fs::permissions(file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove, ec);
    return file;
}

std::optional<json> ProjectService::project_php(const std::string& name, const json& extensions, const json& ini,
                                                ServiceRefusal& refusal) {
    std::map<std::string, std::optional<std::string>> ext_changes;
    std::map<std::string, std::optional<std::string>> ini_changes;
    const auto read = [&refusal](const json& in, const char* what,
                                 std::map<std::string, std::optional<std::string>>& out) {
        if (in.is_null()) {
            return true;
        }
        if (!in.is_object()) {
            refusal = {"bad_params", std::string(what) + " must be an object"};
            return false;
        }
        for (const auto& [key, value] : in.items()) {
            if (value.is_null()) {
                out[key] = std::nullopt;
            } else if (value.is_string()) {
                out[key] = value.get<std::string>();
            } else {
                refusal = {"bad_params", std::string(what) + " " + key + " is text, or null for the series' setting"};
                return false;
            }
        }
        return true;
    };
    if (!read(extensions, "extensions", ext_changes) || !read(ini, "ini", ini_changes)) {
        return std::nullopt;
    }
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    const Project* project = find_project(*config, name);
    if (project == nullptr) {
        refusal = {"unknown_project", "there's no project called " + name};
        return std::nullopt;
    }
    std::string error;
    if (!set_project_php_settings_file(project->dir, ext_changes, ini_changes, error)) {
        refusal = {"config_refused", error};
        return std::nullopt;
    }
    return describe(*config, name);
}

}  // namespace floserver::core

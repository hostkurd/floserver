#include "floserver/core/project_service.h"

#include "floserver/core/shared_sites.h"

#include "floserver/core/paths.h"
#include "floserver/core/site_plan.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <array>
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
            if (key == "php") {
                shared->php = parsed.config->php;
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

}  // namespace floserver::core

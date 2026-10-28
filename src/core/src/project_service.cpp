#include "floserver/core/project_service.h"

#include "floserver/core/paths.h"
#include "floserver/core/site_plan.h"

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
    if (!site.problem.empty()) {
        out["problem"] = site.problem;
    }
    if (site.database) {
        out["database"] = site.database->family + "-" + site.database->version;
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

}  // namespace floserver::core

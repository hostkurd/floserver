#pragma once

// The service's project work (D8, D10, D11): the project list in
// config.json and what each project needs. Running nginx and the PHP pools
// builds on this in the next step.
//
// config.json is read again on every request, so a change made by hand (or
// by a newer window) is seen at once; changes are written back atomically.
// A config.json that can't be read is never overwritten: requests that would
// change it are refused until it's fixed.

#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_service.h"  // ServiceRefusal

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace floserver::core {

class ProjectService {
public:
    explicit ProjectService(std::filesystem::path data_dir);

    // Every project with what it needs:
    //   {"projects": [{"name", "path", "on", "keep_running",
    //                  "state": "off" | "ready" | "needs_download" | "problem",
    //                  "php"?: "8.4.27", "missing": ["php 8.2"], "problem"?: "...",
    //                  "database"?: "mariadb-11.4",
    //                  "wants"?: {"web": "nginx" | "apache", "php"?, "node"?, "python"?, "database"?}}],
    //    "projects_dir": "..."}
    [[nodiscard]] std::optional<nlohmann::json> list(ServiceRefusal& refusal) const;

    // Adds an existing folder ({"path", "name"?}); the reply is that project.
    [[nodiscard]] std::optional<nlohmann::json> add(const std::filesystem::path& dir, const std::string& name,
                                                    ServiceRefusal& refusal);

    // Forgets a project; its folder is never touched.
    [[nodiscard]] bool remove(const std::string& name, ServiceRefusal& refusal);

    // Turns a project On or Off, or Keep running on or off.
    [[nodiscard]] std::optional<nlohmann::json> set(const std::string& name, std::optional<bool> on,
                                                    std::optional<bool> keep_running, ServiceRefusal& refusal);

    // Sets one key of the project's floserver.json ("php", "node",
    // "python", "database", "web"; nothing for `value` removes it), keeping
    // the file's other keys. The reply is that project.
    [[nodiscard]] std::optional<nlohmann::json> use(const std::string& name, const std::string& key,
                                                    const std::optional<std::string>& value, ServiceRefusal& refusal);

    // Shared sites (D19): the settings and the www folder's folders:
    //   {"on", "dir", "default_dir"?, "php"?, "web": "apache"|"nginx", "database"?,
    //    "folders": [{"name", "kind": "php"|"static"|"node"|"python"|"empty", "host", "clash"?}]}
    // Without `folders` the www folder isn't read and the reply has no
    // "folders" key (the window asks every 2 seconds and shows no folders).
    [[nodiscard]] std::optional<nlohmann::json> shared(ServiceRefusal& refusal, bool folders = true) const;

    // Changes some of {"on", "dir", "php", "web", "database"} (null resets
    // dir, php and database to their defaults). Turning them on makes the
    // default www folder when it isn't there; another folder must exist.
    // The reply is shared()'s, with or without the folders.
    [[nodiscard]] std::optional<nlohmann::json> set_shared(const nlohmann::json& changes, ServiceRefusal& refusal,
                                                           bool folders = true);

    // The registered folder of a project, or nothing.
    [[nodiscard]] std::optional<std::filesystem::path> folder(const std::string& name, ServiceRefusal& refusal) const;

private:
    [[nodiscard]] std::optional<AppConfig> load(ServiceRefusal& refusal) const;
    [[nodiscard]] bool save(const AppConfig& config, ServiceRefusal& refusal) const;
    [[nodiscard]] nlohmann::json describe(const AppConfig& config, const std::string& name) const;
    [[nodiscard]] std::optional<nlohmann::json> describe_shared(const AppConfig& config, ServiceRefusal& refusal,
                                                                bool folders) const;

    std::filesystem::path data_dir_;
    mutable std::mutex mutex_;  // one change at a time: load, change, save
};

}  // namespace floserver::core

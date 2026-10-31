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
#include "floserver/core/runtime_service.h"
#include "floserver/core/runtime_store.h"  // ServiceRefusal

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
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
    //                  "static"?: true, "php_files"?: true (PHP is off and the folder has .php files),
    //                  "wants"?: {"web": "nginx" | "apache", "php"?: "8.4" | "newest" | false,
    //                             "node"?, "python"?, "database"?,
    //                             "extensions"?: ["bcmath", "-exif"], "php_ini"?: {"memory_limit": "1024M"}}}],
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
    // the file's other keys; "php" also takes "none" (written as false) and
    // "newest". The reply is that project.
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

    // Settings (D20): who uses each installed runtime, as runtime_users()
    // says it: {"php/8.5.12": ["blog", "shop"], ...}. Reads every project's
    // floserver.json: for a request, never a timer.
    [[nodiscard]] std::optional<nlohmann::json> runtime_users(const std::vector<InstalledRuntime>& installed,
                                                              ServiceRefusal& refusal) const;

    // The series new projects start with (D20), from config.json's
    // "defaults": {"php": "8.5", "mariadb": "11.8", "node": "24", "python": "3.13"}.
    // A runtime that isn't there has no default (the newest installed).
    [[nodiscard]] std::optional<nlohmann::json> defaults(ServiceRefusal& refusal) const;
    // Sets one ("php", "mariadb", "node" or "python") to a series such as
    // "8.5", or clears it with nothing. Returns all of them.
    [[nodiscard]] std::optional<nlohmann::json> set_default(const std::string& runtime,
                                                            const std::optional<std::string>& series,
                                                            ServiceRefusal& refusal);

    // Settings → PHP (D20) for one series ("8.5"):
    //   {"series": "8.5", "version": "8.5.12" | null (the install used: the newest of the series),
    //    "extensions_fixed": bool (true: they come with the build and can't be changed here),
    //    "extensions": [{"name", "on", "base", "missing"?}],
    //    "ini": {every value the series gives: FloServer's defaults and its own},
    //    "changed": {the series' own values}, "custom_ini": "<data>/php/8.5/custom.ini",
    //    "custom_ini_exists": bool, "used_by": ["blog", "_shared"], "problems"?: ["..."],
    //    "known_ini"?: [every php.ini name the install knows, sorted]}
    // Reads the install's ext folder and every project's floserver.json:
    // for a request, never a timer. "known_ini" is asked of the install's
    // php CLI the first time (known_php_ini_names), then read from
    // <data>/php/<series>/known-ini-<version>.json; it's left out when PHP
    // can't tell (and not asked again of that install while the service runs).
    [[nodiscard]] std::optional<nlohmann::json> php_get(const std::string& series, ServiceRefusal& refusal) const;
    // Changes the series' settings: `extensions` {"bcmath": true|false|null},
    // `ini` {"memory_limit": "1G"|null}; null goes back to the default. Either
    // may be null. The reply is php_get()'s.
    [[nodiscard]] std::optional<nlohmann::json> php_set(const std::string& series, const nlohmann::json& extensions,
                                                        const nlohmann::json& ini, ServiceRefusal& refusal);
    // The series' custom.ini, made (with a comment saying what it's for) when
    // it isn't there.
    [[nodiscard]] std::optional<std::filesystem::path> php_custom_ini(const std::string& series,
                                                                      ServiceRefusal& refusal) const;
    // The php.ini the series' newest install uses for shared sites and for
    // projects without changes of their own, written to
    // <data>/php/<series>/final-php.ini (read-only) for the user to look at.
    // Written fresh on each call; nothing reads it back.
    [[nodiscard]] std::optional<std::filesystem::path> php_final_ini(const std::string& series,
                                                                     ServiceRefusal& refusal) const;
    // A project's own PHP changes, in its floserver.json: `extensions`
    // {"bcmath": "on"|"off"|null}, `ini` {"memory_limit": "1024M"|null}; null
    // goes back to the series' setting. The reply is that project.
    [[nodiscard]] std::optional<nlohmann::json> project_php(const std::string& name, const nlohmann::json& extensions,
                                                            const nlohmann::json& ini, ServiceRefusal& refusal);

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
    mutable std::mutex known_mutex_;
    mutable std::set<std::string> known_failed_;  // installs whose php couldn't list its settings
};

}  // namespace floserver::core

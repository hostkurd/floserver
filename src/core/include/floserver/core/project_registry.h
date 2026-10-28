#pragma once

// The list of projects the service knows (D8, D11), kept in <data>/config.json:
//
//   {
//     "projects_dir": "C:/Users/me/FloServer/projects",
//     "projects": [
//       {"name": "my-shop", "path": "C:/Users/me/FloServer/projects/my-shop",
//        "on": true, "keep_running": false}
//     ]
//   }
//
// Each project is a folder anywhere on the disk; its versions live in its own
// floserver.json (D10), not here. The name gives the address
// https://<name>.localhost (D7), so it is a DNS label: a-z, 0-9 and '-'.
//
// Other keys in config.json (the window's settings) are kept as they are, so
// an older service never drops settings a newer window wrote.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace floserver::core {

struct Project {
    std::string name;
    std::filesystem::path dir;  // absolute
    bool on = false;            // D11: the address answers (processes may still sleep)
    bool keep_running = false;  // D11: never put to sleep
    nlohmann::json other = nlohmann::json::object();  // keys of this entry this version doesn't use

    bool operator==(const Project&) const = default;
};

struct AppConfig {
    std::optional<std::filesystem::path> projects_dir;  // absent: the default (D8)
    std::vector<Project> projects;
    nlohmann::json other = nlohmann::json::object();  // keys this version doesn't use
};

inline constexpr std::size_t kMaxProjects = 500;
inline constexpr std::size_t kMaxProjectNameLength = 63;  // one DNS label

[[nodiscard]] std::filesystem::path app_config_file(const std::filesystem::path& data);

// D8's default projects folder: %USERPROFILE%\FloServer\projects, or
// ~/FloServer/projects. Nothing when there's no home folder.
[[nodiscard]] std::optional<std::filesystem::path> default_projects_dir();

// Why a name can't be used, in plain English, or nothing when it can.
[[nodiscard]] std::optional<std::string> project_name_problem(std::string_view name);

// A usable name made from a folder name: "My Shop_2" -> "my-shop-2".
// "project" when nothing usable is left (an Arabic folder name, say).
[[nodiscard]] std::string project_name_from(std::string_view folder_name);

// Reads config.json. A missing file is an empty config. Nothing (with
// `error`) when the file exists but can't be read or isn't a config; the
// service then refuses to change it rather than overwrite the user's list.
[[nodiscard]] std::optional<AppConfig> load_app_config(const std::filesystem::path& file, std::string& error);

[[nodiscard]] std::string write_app_config(const AppConfig& config);
[[nodiscard]] bool save_app_config(const std::filesystem::path& file, const AppConfig& config,
                                   std::string& error);

[[nodiscard]] Project* find_project(AppConfig& config, std::string_view name);
[[nodiscard]] const Project* find_project(const AppConfig& config, std::string_view name);

// The project whose folder is `dir`, or nothing.
[[nodiscard]] const Project* project_for_folder(const AppConfig& config, const std::filesystem::path& dir);

// The folder as the file system names it: full long names (no 8.3 short
// names, which nginx for Windows refuses), the real case, no "\\?\" prefix.
// Nothing (with `error`) when it doesn't exist or, on Windows, isn't on a
// drive letter (network shares aren't supported).
[[nodiscard]] std::optional<std::filesystem::path> real_folder(const std::filesystem::path& dir, std::string& error);

// Adds an existing folder. With an empty `name` one is made from the folder
// name, with "-2", "-3"... when it's taken. The folder is stored as
// real_folder() gives it. The new project starts Off.
// Nothing (with `error`) when the folder is missing, already a project, or
// the name can't be used or is taken.
[[nodiscard]] std::optional<Project> add_project(AppConfig& config, const std::filesystem::path& dir,
                                                 std::string_view name, std::string& error);

// Forgets a project. Its folder and files are never touched.
bool remove_project(AppConfig& config, std::string_view name);

}  // namespace floserver::core

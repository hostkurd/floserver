#pragma once

// floserver.json: the per-project versions file (design note D10).
//
//   {
//     "php": "8.2",
//     "node": "22",
//     "python": "3.13",
//     "database": "mariadb-11.4",
//     "web": "nginx",
//     "root": "public",
//     "extensions": ["intl", "gd"],
//     "command": "npm run serve -- --port {port}"
//   }
//
// Every key is optional. Unknown keys are kept as warnings, not errors, so a
// file written by a newer FloServer still opens in an older one.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

enum class WebServer : std::uint8_t { Nginx, Apache };

struct DatabaseSpec {
    std::string family;   // "mariadb" (1.0); "mysql" may follow (Q7)
    std::string version;  // "11.4"
    bool operator==(const DatabaseSpec&) const = default;
};

struct ProjectConfig {
    std::optional<std::string> php;        // "8.2", "8.2.27"
    std::optional<std::string> node;       // "22", "22.11.0"
    std::optional<std::string> python;     // "3.13" (D17)
    std::optional<DatabaseSpec> database;  // absent: the project needs no database
    WebServer web = WebServer::Nginx;
    std::string root = ".";                // document root, relative to the project
    std::vector<std::string> extensions;   // PHP extensions to enable
    // How a Node or Python project starts its own server (D11, D17),
    // instead of what FloServer would pick; {port} is replaced by the
    // port to listen on, which is also in the PORT variable.
    std::optional<std::string> command;

    bool operator==(const ProjectConfig&) const = default;
};

struct ConfigProblem {
    std::string key;      // "php", "extensions[1]", "" for the whole file
    std::string message;  // plain English, shown to the user
};

struct ConfigResult {
    std::optional<ProjectConfig> config;  // set when there are no errors
    std::vector<ConfigProblem> errors;
    std::vector<ConfigProblem> warnings;
};

// Parses the text of a floserver.json.
[[nodiscard]] ConfigResult parse_project_config(std::string_view json_text);

// Writes a config back as pretty JSON with keys in the documented order.
// Default values (web nginx, root ".") are left out.
[[nodiscard]] std::string write_project_config(const ProjectConfig& config);

// Finds the nearest floserver.json at or above `start` (used by the terminal
// shims). Returns the file's path, or nothing when there is none.
[[nodiscard]] std::optional<std::filesystem::path>
find_project_config(const std::filesystem::path& start);

inline constexpr std::string_view kProjectConfigFileName = "floserver.json";

}  // namespace floserver::core

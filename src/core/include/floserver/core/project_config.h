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
//     "extensions": ["intl", "gd", "-exif"],
//     "php_ini": {"memory_limit": "1024M"},
//     "command": "npm run serve -- --port {port}"
//   }
//
// Every key is optional. Unknown keys are kept as warnings, not errors, so a
// file written by a newer FloServer still opens in an older one.

#include <cstdint>
#include <filesystem>
#include <map>
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
    std::vector<std::string> extensions;   // PHP extensions to turn on
    // PHP extensions to turn off ("-exif" in the file): base ones, or ones
    // the series' settings turn on (D20).
    std::vector<std::string> extensions_off;
    // php.ini values for this project, after the series' (D20).
    std::map<std::string, std::string> php_ini;
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

// The keys the window and `flo use` change one at a time.
enum class ConfigKey : std::uint8_t { Php, Node, Python, Database, Web };

// "php", "node", "python", "database" (also "mariadb", as in `flo use
// mariadb 11.4`), "web". Nothing for anything else.
[[nodiscard]] std::optional<ConfigKey> parse_config_key(std::string_view name);

// Sets one key in the text of a floserver.json, keeping every other key and
// its order; nothing for `value` removes the key. Versions are checked
// (Version::parse); "database" takes "mariadb-11.4", and "mariadb" takes the
// version alone; "web" takes "nginx" (the default, so the key is removed) or
// "apache". Empty text is an empty file. Returns the new text, or nothing
// with `error` (plain English) when the old text or the new value is wrong.
[[nodiscard]] std::optional<std::string> set_project_config_value(std::string_view text, ConfigKey key,
                                                                  std::string_view key_name,
                                                                  const std::optional<std::string>& value,
                                                                  std::string& error);

// The project's PHP changes (D20) in the text of a floserver.json, keeping
// every other key: `extensions` maps a name to "on" (listed), "off" (listed
// as "-name") or nothing (back to the series' setting); `ini` maps a php.ini
// key to a value or nothing (back to the series' value). Empty lists and
// objects are removed. Nothing, with `error`, when a name, key or value
// can't be used or the old text is wrong.
[[nodiscard]] std::optional<std::string> set_project_php_settings(
    std::string_view text, const std::map<std::string, std::optional<std::string>>& extensions,
    const std::map<std::string, std::optional<std::string>>& ini, std::string& error);

// The same for `dir`/floserver.json, written atomically.
[[nodiscard]] bool set_project_php_settings_file(const std::filesystem::path& dir,
                                                 const std::map<std::string, std::optional<std::string>>& extensions,
                                                 const std::map<std::string, std::optional<std::string>>& ini,
                                                 std::string& error);

// The same for `dir`/floserver.json, written atomically. A missing file is
// made; one that isn't a plain file, or is over 64 KiB, is refused.
[[nodiscard]] bool set_project_config_file_value(const std::filesystem::path& dir, ConfigKey key,
                                                 std::string_view key_name,
                                                 const std::optional<std::string>& value, std::string& error);

}  // namespace floserver::core

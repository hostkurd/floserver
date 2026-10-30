#pragma once

// Shared sites (D19): a www folder like XAMPP's htdocs or Laragon's www.
// Every folder in it opens at http://localhost:<port>/<folder>/ and at
// http://<folder>.localhost:<port>, with no project to register. One PHP
// version, one web server and one database setting serve them all.
//
// The settings live in config.json beside the projects:
//
//   "shared": {"on": true, "dir": "C:/Users/me/FloServer/www",
//              "php": "8.5", "web": "apache", "database": "mariadb-11.4"}
//
// "dir" absent: %USERPROFILE%\FloServer\www. "php" absent: the newest PHP
// installed. "web": "apache" (the default, so .htaccess works) or "nginx".
//
// Projects win their addresses: <name>.localhost is the project's when one
// has that name; the folder still opens at localhost/<name>/. Node.js and
// Python apps need a server of their own, so a folder that is one (and has
// no index file) is shown as such, to be made a project instead.

#include "floserver/core/ports.h"
#include "floserver/core/project_config.h"
#include "floserver/core/project_registry.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

// Shared sites' name among sites and pools; project names can't have '_'.
inline constexpr std::string_view kSharedSiteName = "_shared";

struct SharedConfig {
    bool on = false;
    std::optional<std::filesystem::path> dir;  // absent: default_shared_dir()
    std::optional<std::string> php;            // "8.5"; absent: the newest installed
    WebServer web = WebServer::Apache;
    std::optional<DatabaseSpec> database;      // absent: none

    bool operator==(const SharedConfig&) const = default;
};

// %USERPROFILE%\FloServer\www, or ~/FloServer/www; nothing without a home folder.
[[nodiscard]] std::optional<std::filesystem::path> default_shared_dir();

// The folder in use: `dir`, else the default; nothing when neither exists.
[[nodiscard]] std::optional<std::filesystem::path> shared_dir(const SharedConfig& config);

// Reads config.json's "shared" (kept in AppConfig::other). A missing key is
// the defaults, off. Nothing (with `error`) when it's there but unusable.
[[nodiscard]] std::optional<SharedConfig> read_shared_config(const AppConfig& app, std::string& error);

// Writes `shared` into AppConfig::other, in the form read_shared_config() reads.
void write_shared_config(AppConfig& app, const SharedConfig& shared);

// The settings as a floserver.json would say them, for the site plan.
[[nodiscard]] ProjectConfig shared_project_config(const SharedConfig& shared, const std::string& php_version);

enum class SharedKind : std::uint8_t {
    Php,     // has .php files or an index.php
    Static,  // an index.html, or files with no PHP
    Node,    // package.json and no index file: needs its own server
    Python,  // requirements.txt, pyproject.toml or manage.py and no index file
    Empty,   // nothing in it yet
};

[[nodiscard]] std::string_view shared_kind_name(SharedKind kind);

struct SharedFolder {
    std::string name;                     // the folder's name as on disk (UTF-8)
    SharedKind kind = SharedKind::Empty;
    bool host_name = false;               // usable as <name>.localhost (a lower-case DNS label)
    std::optional<std::string> clash;     // the project that has <name>.localhost
};

inline constexpr std::size_t kMaxSharedFolders = 1000;

// The folders in `dir`, sorted by name; hidden ones (".git") and files left
// out. At most kMaxSharedFolders. Empty when the folder can't be read.
[[nodiscard]] std::vector<SharedFolder> scan_shared_folders(const std::filesystem::path& dir, const AppConfig& app);

// "http://localhost:8080", or "http://localhost" on port 80; shared sites are http only.
[[nodiscard]] std::string shared_address(const WebPorts& ports);

// The page localhost/ shows when the www folder has no index file of its
// own: a small PHP script, run by the shared sites' PHP pool, that reads the
// www folder only when the page is opened (nothing reads it in the
// background) and lists its folders with their addresses, Node.js/Python
// apps and project names. The project names come from config.json.
[[nodiscard]] std::string shared_list_script(const std::filesystem::path& dir, const std::filesystem::path& config_json,
                                             WebServer web);

}  // namespace floserver::core

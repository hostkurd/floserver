#pragma once

// The one shared Apache's config (D6), for projects that ask for it
// ("web": "apache") because they rely on .htaccess, such as WordPress or apps
// deployed to cPanel. Users never edit it; it's rewritten on every change.
//
// nginx keeps the web ports: it forwards each Apache project to Apache, which
// listens on one loopback port of its own and the service holds and relays
// (it sleeps when idle like a project's own server, app_runner.h). One
// virtual host per <name>.localhost; .htaccess is read (AllowOverride All).
// PHP goes to the same php-cgi pools nginx uses, through mod_proxy_fcgi
// ("GENERIC" backend, the script's real path, which php-cgi needs), spread
// over the pool's workers. HTTPS is nginx's: requests it received over https
// get HTTPS=on, as WordPress expects. Dot files and folders aren't served.

#include "floserver/core/ports.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace floserver::core {

struct ApacheSite {
    std::string name;                 // checked: project_name_problem()
    std::filesystem::path doc_root;
    std::vector<Port> php_ports;      // the pool's workers; empty: static files only
    bool https = false;               // nginx serves it over https: Apache's own redirects say so
};

// Shared sites (D19) when Apache serves them: the www folder at
// localhost/<folder>/ and each folder at <folder>.localhost (mod_vhost_alias).
// nginx answers localhost/ itself when the www folder has no index file.
struct ApacheShared {
    std::filesystem::path dir;
    std::vector<Port> php_ports;
    bool https = false;  // nginx serves https://localhost/: Apache's own redirects there say so
};

struct ApacheSpec {
    std::filesystem::path server_root;  // the installed Apache (Apache24): conf/mime.types
    std::filesystem::path modules_dir;  // where mod_*.so are (server_root/modules on Windows)
    std::filesystem::path work_dir;     // our folder: the pid file, the error log
    // Linux and macOS builds need an MPM and unixd loaded; Windows has its own built in.
    bool posix = false;
    // What Apache logs to its error log: "warn", or with
    // "proxy_fcgi:trace8" the values it sends PHP (FLOSERVER_APACHE_TRACE).
    std::string log_level = "warn";
    std::vector<ApacheSite> sites;
    std::optional<ApacheShared> shared;
    // The modules to load by short name, in order (apache_settings.h's
    // plan_apache_modules()); empty: the ones FloServer has always loaded.
    std::vector<std::string> modules;
    // FloServer's own lines after the sites (User and Group when it runs as root).
    std::string extra;
    // The user's custom.conf, already filtered (filter_apache_custom_conf()),
    // last, and where it came from for the comment above it.
    std::string custom_conf;
    std::string custom_conf_path;
};

// The port Apache listens on is filled in when it starts.
inline constexpr std::string_view kApachePortPlaceholder = "{port}";

// The config text with kApachePortPlaceholder where the port goes, or
// nothing (with `error`) when a name or path can't be written safely.
[[nodiscard]] std::optional<std::string> make_apache_config(const ApacheSpec& spec, std::string& error);

// Runs `httpd -t` on `text`, written to `file` (its output beside it, as
// <file>.log). Nothing when Apache accepts it; else what Apache said.
[[nodiscard]] std::optional<std::string> apache_config_problem(const std::filesystem::path& program,
                                                               const std::filesystem::path& server_root,
                                                               const std::filesystem::path& file,
                                                               const std::string& text);

// The httpd program in an Apache install: Apache24/bin/httpd.exe, bin/httpd,
// bin/apache2 (Debian's name). Nothing when it has none.
[[nodiscard]] std::optional<std::filesystem::path> apache_program(const std::filesystem::path& apache_dir);

// The ServerRoot of an Apache install: the Apache24 folder of an Apache
// Lounge zip, else the install itself.
[[nodiscard]] std::filesystem::path apache_server_root(const std::filesystem::path& apache_dir);

}  // namespace floserver::core

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
};

struct ApacheSpec {
    std::filesystem::path server_root;  // the installed Apache (Apache24): conf/mime.types
    std::filesystem::path modules_dir;  // where mod_*.so are (server_root/modules on Windows)
    std::filesystem::path work_dir;     // our folder: the pid file, the error log
    // Linux and macOS builds need an MPM and unixd loaded; Windows has its own built in.
    bool posix = false;
    std::vector<ApacheSite> sites;
};

// The port Apache listens on is filled in when it starts.
inline constexpr std::string_view kApachePortPlaceholder = "{port}";

// The config text with kApachePortPlaceholder where the port goes, or
// nothing (with `error`) when a name or path can't be written safely.
[[nodiscard]] std::optional<std::string> make_apache_config(const ApacheSpec& spec, std::string& error);

// The httpd program in an Apache install: Apache24/bin/httpd.exe, bin/httpd,
// bin/apache2 (Debian's name). Nothing when it has none.
[[nodiscard]] std::optional<std::filesystem::path> apache_program(const std::filesystem::path& apache_dir);

// The ServerRoot of an Apache install: the Apache24 folder of an Apache
// Lounge zip, else the install itself.
[[nodiscard]] std::filesystem::path apache_server_root(const std::filesystem::path& apache_dir);

}  // namespace floserver::core

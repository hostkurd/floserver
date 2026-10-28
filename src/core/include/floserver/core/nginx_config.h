#pragma once

// The one shared nginx's config, written by the service from the project list
// (D6). Users never edit it; it's rewritten on every change and nginx reloads.
//
// Each project that is On and ready gets <name>.localhost (D7) on the web
// ports, loopback only (D12): 127.0.0.1, and [::1] when the machine has IPv6.
// With a certificate the http port redirects to https; without one (no local
// certificate authority yet) the site is served on http.
//
// PHP requests go to the project's pool (site_plan.h) and are logged to that
// pool's access log, one line per request, which is how the service measures
// idle time (D11). Static files aren't logged.

#include "floserver/core/ports.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace floserver::core {

struct NginxTls {
    std::filesystem::path certificate;  // PEM, the site's certificate then the CA's
    std::filesystem::path key;          // PEM
};

struct NginxSite {
    std::string name;                   // checked: project_name_problem()
    std::filesystem::path doc_root;
    Port php_port = 0;                  // 0: no PHP, static files only
    std::filesystem::path php_access_log;  // the pool's idle log; empty for none
    std::optional<NginxTls> tls;
};

struct NginxSpec {
    std::filesystem::path nginx_dir;  // the installed nginx (its conf/mime.types, conf/fastcgi_params)
    std::filesystem::path work_dir;   // our folder: logs/, temp/, the pid file
    WebPorts ports{};
    bool ipv6 = true;                 // also listen on [::1]
    std::vector<NginxSite> sites;
};

// The config text, or nothing (with `error`) when a name or path can't be
// written safely into it.
[[nodiscard]] std::optional<std::string> make_nginx_config(const NginxSpec& spec, std::string& error);

// A path as a quoted nginx string with forward slashes, or nothing when it
// holds a character nginx can't take inside quotes: '"', '$' (nginx reads it
// as a variable and has no escape for it), '\' after conversion, or a control
// character.
[[nodiscard]] std::optional<std::string> nginx_quoted_path(const std::filesystem::path& path);

// The address users open, with the port when it isn't the usual one:
// "https://shop.localhost", "http://shop.localhost:8080".
[[nodiscard]] std::string site_address(const std::string& name, const WebPorts& ports, bool https);

}  // namespace floserver::core

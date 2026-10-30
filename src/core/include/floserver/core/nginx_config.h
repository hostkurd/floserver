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
//
// A project that runs its own server (a Node dev server) is forwarded as a
// whole to its port, which the service holds (app_runner.h); WebSocket
// upgrades pass through, so hot reload works.

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
    std::vector<Port> php_ports;        // the pool's workers; empty: static files only
    std::filesystem::path php_access_log;  // the pool's idle log; empty for none
    std::optional<NginxTls> tls;
    std::optional<Port> app_port;       // set: everything goes to this server instead
    std::filesystem::path request_log;  // every request, for the window's Log box; empty for none
};

// Shared sites (D19), on the http port: the www folder at localhost/<folder>/
// and each folder at <folder>.localhost (a project's name wins: exact names
// come before nginx's regex). With Apache everything goes to it; without,
// nginx serves the files and sends PHP to the pool. localhost/ runs
// `list_script` on the pool when the www folder has no index file of its own.
struct NginxShared {
    std::filesystem::path dir;
    std::filesystem::path list_script;      // shared_list_script()
    std::vector<Port> php_ports;            // the pool; nginx sends PHP to it unless Apache serves
    std::filesystem::path php_access_log;   // the pool's idle log
    std::optional<Port> app_port;           // the shared Apache serves (web apache)
    std::filesystem::path request_log;      // every request; empty for none
    // Projects that aren't served now (off, waiting for a download, a
    // problem): their <name>.localhost stays theirs, a 404, never a www folder.
    std::vector<std::string> reserved;
};

struct NginxSpec {
    std::filesystem::path nginx_dir;  // the installed nginx (its conf/mime.types, conf/fastcgi_params)
    std::filesystem::path work_dir;   // our folder: logs/, temp/, the pid file
    WebPorts ports{};
    bool ipv6 = false;                // also listen on [::1]: set from loopback_ipv6_available()
    // nginx before 1.25.1 has no "http2 on;" and takes "listen ... http2".
    bool http2_directive = true;
    // Only when the service itself runs as root (containers): nginx then
    // needs to be told to keep that user for its workers.
    std::string user;
    // Answered at /.floserver/config-id by the default http server, so the
    // service can tell when nginx serves this config after a reload.
    std::string config_id;
    std::vector<NginxSite> sites;
    std::optional<NginxShared> shared;
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

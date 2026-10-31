#pragma once

// Settings → Ports & HTTPS (D20): which ports the shared nginx listens on,
// HTTPS for projects, and HTTPS for shared sites' https://localhost/<folder>/.
// Kept in config.json beside the projects:
//
//   "web": {"ports": "auto" | "standard" | "alternate" | "custom",
//           "http_port": 8000, "https_port": 8443,
//           "https": true, "shared_https": false}
//
// "ports" absent or "auto": 80/443 when both are free, else 8080/8443 or the
// next free pair (Q4), as before. "standard" asks for 80/443 (addresses
// without a port), "alternate" for 8080/8443, "custom" for http_port and
// https_port (kept when another choice is picked, so they come back). A port
// asked for that another program holds is reported, and the automatic
// choice is used until it's free (the next start of nginx, or a change).
//
// "https" (default true): projects get certificates from FloServer's CA once
// it's trusted, and http:// redirects to https://. "shared_https" (default
// false): https://localhost/<folder>/ too, with one certificate for
// "localhost"; <folder>.localhost stays http. Keys this version doesn't
// know are kept.

#include "floserver/core/ports.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_service.h"  // ServiceRefusal

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

class ProjectService;

enum class PortChoice : std::uint8_t { Auto, Standard, Alternate, Custom };

struct WebSettings {
    PortChoice ports = PortChoice::Auto;
    std::optional<Port> http_port;   // "custom"'s
    std::optional<Port> https_port;
    bool https = true;
    bool shared_https = false;

    bool operator==(const WebSettings&) const = default;
};

[[nodiscard]] std::string_view port_choice_name(PortChoice choice);

// config.json's "web". A missing key is the defaults. Nothing (with `error`)
// when it's there but unusable.
[[nodiscard]] std::optional<WebSettings> read_web_settings(const AppConfig& app, std::string& error);

// Writes `settings` into AppConfig::other, keeping keys this version doesn't know.
void write_web_settings(AppConfig& app, const WebSettings& settings);

// Applies web.settings.set's changes: some of {"ports", "http_port",
// "https_port", "https", "shared_https"}. False (with `error`, in plain
// English) when one can't be used; `settings` is then unchanged.
[[nodiscard]] bool apply_web_settings_changes(WebSettings& settings, const nlohmann::json& changes, std::string& error);

// The pair the choice asks for: 80/443, 8080/8443 or the custom ports;
// nothing for "auto".
[[nodiscard]] std::optional<WebPorts> wanted_web_ports(const WebSettings& settings);

struct ChosenWebPorts {
    std::optional<WebPorts> ports;  // nothing: every pair is taken
    std::vector<Port> taken;        // asked for but held by another program
};

// The asked-for pair when both are free, else the automatic choice (with the
// ports that were taken).
[[nodiscard]] ChosenWebPorts choose_web_ports(const WebSettings& settings, const PortProbe& probe);

struct PortOwner {
    std::uint32_t pid = 0;
    std::string program;  // "httpd.exe"; empty when it can't be read (another user's program)
};
using PortOwnerLookup = std::function<std::optional<PortOwner>(Port)>;

// Windows: the program listening on `port` on any address, from the TCP
// table. Elsewhere nothing (not looked up).
[[nodiscard]] std::optional<PortOwner> find_port_owner(Port port);

// The TCP ports process `pid` listens on beyond loopback ("0.0.0.0:80"),
// from Windows' TCP table or Linux's /proc. Empty when there are none or
// they can't be read. The shared Apache is checked once it starts.
[[nodiscard]] std::vector<std::string> listening_beyond_loopback(std::uint32_t pid);

// Settings → Ports & HTTPS, for the daemon's web.settings.get and
// web.settings.set. Nothing here runs on a timer: each port check and owner
// lookup is for one request.
class WebSettingsService {
public:
    struct Hooks {
        std::function<std::optional<WebPorts>()> in_use;  // the ports nginx listens on now
        std::function<void()> reload;                     // bring the web server in line now
        std::function<nlohmann::json()> ca;               // the CA as certs.status says it, or null
        PortProbe probe;                                  // empty: can_listen_on_loopback
        PortOwnerLookup owner;                            // empty: find_port_owner
    };
    WebSettingsService(ProjectService& projects, Hooks hooks);

    //   {"ports": "auto"|"standard"|"alternate"|"custom", "http_port": n|null, "https_port": n|null,
    //    "https": bool, "shared_https": bool,
    //    "in_use": {"http", "https"} | null (nginx isn't running),
    //    "taken": [{"port", "pid"?, "program"?}] (ports the choice asks for that another program holds),
    //    "ca": {certs.status's "ca"} | null}
    [[nodiscard]] std::optional<nlohmann::json> get(ServiceRefusal& refusal) const;
    // Saves the changes, reloads the web server, and replies as get() does.
    // Ports are checked again only when the change was about ports.
    [[nodiscard]] std::optional<nlohmann::json> set(const nlohmann::json& changes, ServiceRefusal& refusal);

private:
    [[nodiscard]] nlohmann::json describe(const WebSettings& settings, bool check_ports) const;

    ProjectService& projects_;
    Hooks hooks_;
};

}  // namespace floserver::core

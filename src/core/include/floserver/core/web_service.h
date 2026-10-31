#pragma once

// Running the projects (D6, D7, D10, D11): the one shared nginx, the PHP
// pools and the projects' own servers (Node), kept in line with the project
// list.
//
// reload() reads config.json, every project's floserver.json and the
// installed runtimes, plans the sites (site_plan.h), opens or closes pools,
// writes nginx's config and starts, reloads or stops nginx. A new config is
// checked with `nginx -t` first; one that fails is never loaded, and the
// running nginx keeps the last good one. reload() runs on every project
// change and every few seconds, so edits to floserver.json by hand are
// picked up too.
//
// A supervisor thread wakes a sleeping pool when a connection waits on its
// socket, takes every connection to an app's port (app_runner.h), puts idle
// pools and apps to sleep, and restarts an nginx that ended.
//
// HTTPS: sites get certificates when the local CA exists and isn't known to
// be untrusted (on Windows, after `flo trust`); before that they're served
// on plain http, so the browser shows no warnings in the meantime.
// Settings > Ports & HTTPS (web_settings.h, config.json's "web") can turn
// it off for projects, turn it on for https://localhost/<folder>/, and ask
// for other ports.

#include "floserver/core/apache_config.h"
#include "floserver/core/app_runner.h"
#include "floserver/core/certificates.h"
#include "floserver/core/child_process.h"
#include "floserver/core/php_pool.h"
#include "floserver/core/ports.h"
#include "floserver/core/site_plan.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace floserver::core {

// The shared Apache's name among the apps; project names can't have '_'.
inline constexpr std::string_view kApacheAppName = "_apache";

struct WebServiceConfig {
    std::filesystem::path data_dir;
    // Fixed web ports (tests and dev copies, FLOSERVER_WEB_PORTS); else Q4.
    std::optional<WebPorts> fixed_ports;
    std::chrono::seconds idle_after{600};
    std::size_t php_workers = kPhpWorkers;  // php-cgi processes per pool, one port each
    std::chrono::seconds reload_every{5};
    PortProbe probe;  // empty: can_listen_on_loopback
    // phpMyAdmin (phpmyadmin.h): writes its config.inc.php when what it
    // says changed. Called on each reload while phpMyAdmin is served, after
    // the service has written the signon script and the install's stub.
    std::function<void()> phpmyadmin_config;
};

[[nodiscard]] std::filesystem::path web_dir(const std::filesystem::path& data);

// The script localhost/ runs when the www folder has no index file (D19).
[[nodiscard]] std::filesystem::path shared_list_script_file(const std::filesystem::path& web);

// The nginx program in an nginx install, or nothing when it has none.
[[nodiscard]] std::optional<std::filesystem::path> nginx_program(const std::filesystem::path& nginx_dir);

class WebService {
public:
    explicit WebService(WebServiceConfig config, CertStore& certs);
    WebService(const WebService&) = delete;
    WebService& operator=(const WebService&) = delete;
    WebService(WebService&&) = delete;
    WebService& operator=(WebService&&) = delete;
    // Stops nginx and every pool.
    ~WebService();

    // First reload, then the supervisor thread.
    void start();
    // Stops nginx and every pool and the thread. Safe to call twice.
    void stop();

    // Brings everything in line with the project list now; the status after.
    nlohmann::json reload();

    //   {"nginx": {"running", "pid", "version", "http_port", "https_port", "tls", "https_off"?,
    //              "ports_taken"? (asked for in Settings, held by another program), "error"?},
    //    "pools": [{"key", "version", "port", "state": "sleeping"|"running"|"failed",
    //   {"key", "version", "ports", "state", "pid"?, "pids"? (every worker), "sites",
    //    "keep_running", "idle_s"?, "error"?}],
    //    "apps": {"<name>": {"state", "port", "internal_port"?, "pid"?, "connections", "command"?,
    //             "log", "error"?}},
    //    "sites": {"<name>": {"address", "pool"? | "app"?, "run": "sleeping"|"starting"|"running"|"failed",
    //              "state"?, "missing"?, "problem"?, "error"?}},
    //    "shared": the same for shared sites, or null,
    //    "phpmyadmin": the same for phpMyAdmin with its "version" (and "name_taken": true when a
    //                  project has its address), or null when it isn't installed,
    //    "error"?: "..."}
    [[nodiscard]] nlohmann::json status() const;

    // The httpd.conf the shared Apache runs, or would with no project
    // (Settings > Apache's "Show the final httpd.conf"), with {port} for its
    // port; nothing (with `error`) when no Apache is installed.
    [[nodiscard]] std::optional<std::string> apache_final_config(std::string& error) const;

    // The ports nginx listens on now; nothing while it isn't running.
    [[nodiscard]] std::optional<WebPorts> ports_in_use() const;

private:
    struct Site {
        std::string address;
        std::optional<std::string> pool_key;
        bool app = false;                // its own server, in apps_ by the same name
        std::string app_kind;            // "node", "python" or "apache"
        std::string app_name;            // its runner in apps_: the project's name, or the shared Apache's
        bool static_site = false;        // nginx alone serves its files
        std::string state_override;      // "needs_download" / "problem" set here
        std::vector<std::string> missing;
        std::string problem;
        std::string version;             // phpMyAdmin's
        bool name_taken = false;         // phpMyAdmin: a project has its address
    };

    // What the user chose in Settings > Apache: modules and custom.conf's lines.
    struct ApachePart {
        std::vector<std::string> modules;
        std::string custom;
        bool operator==(const ApachePart&) const = default;
    };
    void reload_locked();
    // The shared Apache's config with Settings > Apache's modules and
    // custom.conf. Those are checked with `httpd -t` first (once per new
    // text); when Apache refuses them, the last ones it accepted are used
    // and apache_problem_ says why.
    [[nodiscard]] std::optional<std::string> apache_config_locked(const AppConfig& app, const ApacheSpec& spec,
                                                                  const std::filesystem::path& apache_dir,
                                                                  const std::string& version, std::string& error);
    // Before each reload: a shared Apache that failed to start with the
    // user's choices (or listens beyond loopback) goes back to the last
    // choices that ran; one that runs makes its choices the ones that ran.
    void watch_apache_locked();
    // The apps a reload settles on; those it drops are stopped.
    void replace_apps_locked(std::map<std::string, std::shared_ptr<AppRunner>> apps);
    void stop_locked();
    void supervise();
    [[nodiscard]] bool start_nginx_locked(std::string& error);
    void stop_nginx_locked();
    [[nodiscard]] std::optional<int> run_nginx(std::vector<std::string> args, const std::filesystem::path& log,
                                               std::string& error) const;
    [[nodiscard]] std::optional<std::string> test_nginx_config(const std::string& conf_name, std::string& error) const;
    void stop_leftover_nginx() const;
    [[nodiscard]] nlohmann::json status_locked() const;
    void trim_request_logs_locked() const;

    WebServiceConfig config_;
    CertStore& certs_;
    std::filesystem::path work_;

    mutable std::mutex mutex_;
    // By key. Shared with the supervisor while it waits on their sockets, so
    // a pool dropped by a reload ends once nothing uses it any more.
    std::map<std::string, std::shared_ptr<PhpPoolRunner>> pools_;
    std::map<std::string, std::shared_ptr<AppRunner>> apps_;        // by project name
    std::shared_ptr<PortSet> app_internal_ports_ = std::make_shared<PortSet>();
    std::map<std::string, Site> sites_;                             // served or held back, by name
    std::unique_ptr<ChildProcess> nginx_;
    std::filesystem::path nginx_dir_;
    std::string nginx_version_;
    std::optional<WebPorts> ports_;
    std::optional<WebPorts> wanted_ports_;  // what Settings asked for when ports_ was chosen; nothing: auto
    std::vector<Port> ports_taken_;          // of those, held by another program
    std::string config_text_;  // what nginx runs now
    bool tls_ = false;
    bool https_off_ = false;  // Settings > Ports & HTTPS turned it off for projects
    std::string nginx_error_;
    std::string error_;
    std::chrono::steady_clock::time_point nginx_retry_after_;
    std::chrono::steady_clock::time_point nginx_started_;
    int nginx_failures_ = 0;
    std::uint32_t apache_checked_pid_ = 0;  // the Apache whose listeners were checked (Settings > Apache)
    std::string rejected_text_;   // the last config nginx -t refused
    std::string rejected_error_;
    bool stopped_ = false;
    bool apache_used_own_ = false;   // apache_used_ is the user's choices (not a fallback)
    bool apache_ran_known_ = false;  // apache_ran_ is set
    std::string shared_script_text_;  // the list page's script as last written (D19)
    // phpMyAdmin's signon script and the install's stub as last written:
    // the install folder, then both texts.
    std::string phpmyadmin_files_;
    // Writes them when they changed; false (with `error`) when they can't be.
    [[nodiscard]] bool write_phpmyadmin_files_locked(const std::filesystem::path& install, std::string& error);
    // Settings > Apache: the config the shared Apache gets now, the last
    // text `httpd -t` accepted and refused, why, and the last choices it accepted.
    std::string apache_text_;
    std::string apache_tested_;
    std::string apache_rejected_;
    std::string apache_problem_;
    ApachePart apache_good_;
    ApachePart apache_used_;          // the choices in the config Apache was given last
    ApachePart apache_ran_;           // the choices of the last config Apache ran with
    std::string apache_version_;
    std::vector<std::string> apache_modules_;

    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace floserver::core

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
};

[[nodiscard]] std::filesystem::path web_dir(const std::filesystem::path& data);

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

    //   {"nginx": {"running", "pid", "version", "http_port", "https_port", "tls", "error"?},
    //    "pools": [{"key", "version", "port", "state": "sleeping"|"running"|"failed",
    //   {"key", "version", "ports", "state", "pid"?, "pids"? (every worker), "sites",
    //    "keep_running", "idle_s"?, "error"?}],
    //    "apps": {"<name>": {"state", "port", "internal_port"?, "pid"?, "connections", "command"?,
    //             "log", "error"?}},
    //    "sites": {"<name>": {"address", "pool"? | "app"?, "run": "sleeping"|"starting"|"running"|"failed",
    //              "state"?, "missing"?, "problem"?, "error"?}},
    //    "error"?: "..."}
    [[nodiscard]] nlohmann::json status() const;

private:
    struct Site {
        std::string address;
        std::optional<std::string> pool_key;
        bool app = false;                // its own server, in apps_ by the same name
        std::string app_kind;            // "node", "python" or "apache"
        std::string app_name;            // its runner in apps_: the project's name, or the shared Apache's
        std::string state_override;      // "needs_download" / "problem" set here
        std::vector<std::string> missing;
        std::string problem;
    };

    void reload_locked();
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
    std::string config_text_;  // what nginx runs now
    bool tls_ = false;
    std::string nginx_error_;
    std::string error_;
    std::chrono::steady_clock::time_point nginx_retry_after_;
    std::chrono::steady_clock::time_point nginx_started_;
    int nginx_failures_ = 0;
    std::string rejected_text_;   // the last config nginx -t refused
    std::string rejected_error_;
    bool stopped_ = false;

    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace floserver::core

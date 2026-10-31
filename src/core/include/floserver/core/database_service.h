#pragma once

// The MariaDB servers (D11, D12): one per series in use, shared by the
// projects that ask for it, each project with its own database and user.
//
// Sleep when idle: MariaDB can't be handed a listening socket the way
// php-cgi is (Q1), so the service holds the public port itself (3306, or the
// next free one; never another stack's) and relays each connection to
// mariadbd on an internal port. The first connection starts mariadbd and
// waits for it; when no connection has been open for the idle time it is
// shut down cleanly (mariadb-admin shutdown), and the port stays.
//
// Everything listens on 127.0.0.1 only. root's password and each project's
// are kept in the secure store (secret_store.h), never written to logs,
// arguments or files of FloServer's, except the .env lines a user asks for.

#include "floserver/core/child_process.h"
#include "floserver/core/mariadb.h"
#include "floserver/core/ports.h"
#include "floserver/core/runtime_service.h"  // ServiceRefusal
#include "floserver/core/secret_store.h"

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

struct DatabaseServiceConfig {
    std::filesystem::path data_dir;
    std::chrono::seconds idle_after{600};
    std::chrono::seconds reload_every{5};
    std::chrono::seconds start_limit{90};  // first start includes setting up the data folder
    PortProbe probe;                       // empty: can_listen_on_loopback
};

inline constexpr Port kFirstInternalDatabasePort = 33100;

class DatabaseService {
public:
    // `secrets` is copied: servers may outlive a reference.
    DatabaseService(DatabaseServiceConfig config, SecretStore secrets);
    DatabaseService(const DatabaseService&) = delete;
    DatabaseService& operator=(const DatabaseService&) = delete;
    DatabaseService(DatabaseService&&) = delete;
    DatabaseService& operator=(DatabaseService&&) = delete;
    ~DatabaseService();

    void start();
    // Shuts every server down cleanly and stops listening. Safe to call twice.
    void stop();

    // Brings the servers in line with the project list; the status after.
    // Servers no longer needed are shut down in the background.
    nlohmann::json reload();

    //   {"servers": [{"series", "version", "port", "internal_port",
    //                 "state": "sleeping"|"starting"|"running"|"stopping"|"failed",
    //                 "pid"?, "connections", "projects", "keep_running", "error"?}]}
    [[nodiscard]] nlohmann::json status() const;

    // The login for a project's database, made the first time (the server
    // starts if it sleeps). The project must be On and ask for a database.
    [[nodiscard]] std::optional<DatabaseLogin> project_login(const std::string& project, ServiceRefusal& refusal);

    // Shared sites' login (D19, user "www"), made the first time. Shared
    // sites must be on and have a database.
    [[nodiscard]] std::optional<DatabaseLogin> shared_login(ServiceRefusal& refusal);

    // Settings → Database for one series ("11.4"):
    //   {"server": that server as status() lists it, or null when it has none
    //              (nothing uses the series and it isn't kept running),
    //    "databases": ["blog", "_shared"]: the projects (and shared sites)
    //                 whose own database has been made on it}
    [[nodiscard]] nlohmann::json series_status(const std::string& series) const;
    // "start", "stop" or "restart" the series' server, on a thread of its
    // own (the reply doesn't wait). Stopping is a clean shutdown; a stopped
    // server starts again when a project connects. False (with `refusal`)
    // when the series has no server.
    bool server_action(const std::string& series, const std::string& action, ServiceRefusal& refusal);
    // After its my.ini settings changed: restarts the series' server in the
    // background when it runs (a sleeping one reads them when it wakes).
    // True when a restart was started.
    bool restart_if_running(const std::string& series);
    // root's password, from the secure store.
    [[nodiscard]] std::optional<std::string> root_password(const std::string& series, ServiceRefusal& refusal) const;
    // A new password for root (root_password_problem() says which are
    // refused): set with ALTER USER on the running server (started if it
    // sleeps), then kept in the secure store.
    bool set_root_password(const std::string& series, const std::string& password, ServiceRefusal& refusal);
    // The my.ini the series' server uses, written to
    // <data>/mariadb/<series>/final-my.ini (read-only) for the user to look at.
    [[nodiscard]] std::optional<std::filesystem::path> final_ini(const std::string& series,
                                                                 ServiceRefusal& refusal) const;

    class Server;
    struct InternalPorts;

private:
    void supervise();
    void reload_locked();
    [[nodiscard]] std::shared_ptr<Server> server_of(const std::string& series);
    // Runs `work` on a thread of its own, joined at stop().
    void run_later(std::function<void()> work);
    // Shuts `server` down for good on a thread of its own.
    void retire_later(std::shared_ptr<Server> server);
    void stop_later(std::shared_ptr<Server> server, bool retire);
    void reap_stoppers(bool all);

    DatabaseServiceConfig config_;
    SecretStore secrets_;
    std::shared_ptr<InternalPorts> internal_ports_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Server>> servers_;  // by series
    std::vector<std::weak_ptr<Server>> retired_;              // may still hold their port
    std::atomic<bool> stopping_{false};
    bool stopped_ = false;
    std::thread thread_;

    struct Stopper {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex stoppers_mutex_;
    std::vector<Stopper> stoppers_;
};

}  // namespace floserver::core

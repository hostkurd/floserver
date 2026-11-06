#pragma once

// The MariaDB servers (D11, D12): one per installed series, shared by the
// projects that ask for it, each project with its own database and user. A
// series nothing uses sleeps until something connects (phpMyAdmin, say).
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
#include "floserver/core/project_registry.h"
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
#include <string_view>
#include <thread>
#include <utility>
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

// Backups (D22): what the mariadb program gets when it loads a backup's SQL
// (`force`: routines and events, going on past errors), and mariadb-dump.
[[nodiscard]] std::vector<std::string> mariadb_load_args(bool force);
[[nodiscard]] std::vector<std::string> mariadb_dump_args(const std::string& database, const std::string& file);
// How long a dump or a load may run: 30 minutes, or FLOSERVER_DB_TOOL_SECONDS.
[[nodiscard]] std::chrono::seconds database_tool_limit();

// Another registered project whose database has the same name as
// `project`'s (project_database_name()), or nothing. Names can't have '_',
// so it shouldn't happen; a drop checks anyway.
[[nodiscard]] std::optional<std::string> project_sharing_database(const AppConfig& app, const std::string& project);

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
    // starts if it sleeps). The project must ask for a database; it may be off.
    // `retry_failed`: asked for by the user (phpMyAdmin), so a start that
    // failed is tried again now rather than its error repeated for 30 s.
    [[nodiscard]] std::optional<DatabaseLogin> project_login(const std::string& project, ServiceRefusal& refusal,
                                                             bool retry_failed = false);

    // projects.remove's "drop_database" (approved 2 Nov), before the project
    // leaves config.json: its own database and user (project_database_name(),
    // never a name from the client) are dropped on each installed series that
    // has them (its password is kept there) or that its floserver.json names,
    // the server started if it sleeps; then the password is deleted from the
    // secure store. A series that isn't installed has nothing to drop. False
    // (with `refusal`) when the name is reserved, another registered project's
    // database has the same name, or a drop failed: the project then stays
    // (the refusal says which series were already done; trying again is
    // safe). `dropped`: a database FloServer made (its password kept) was dropped.
    // Call it while the project is held (hold_project): no login can make
    // the database again until the project is gone from config.json.
    bool drop_project_database(const std::string& project, ServiceRefusal& refusal, bool& dropped);

    // Backups (D22). mariadb-dump of the project's own database with its
    // own login, --single-transaction (the project keeps running), written
    // to kDumpFileName in `work`; the server starts if it sleeps. Stopped
    // when `cancel` is set (refusal "cancelled").
    static constexpr std::string_view kDumpFileName = "database.sql";
    bool dump_project_database(const std::string& project, const std::filesystem::path& work,
                               const std::atomic<bool>& cancel, ServiceRefusal& refusal);
    // The mariadb program runs `sql` (backup.h's prepare_sql_for_load(): it
    // empties the database first) with the project's own login, so it can
    // only change that database.
    // `extras` (routines and events, when there are any) runs after it, with
    // --force: what the login may not make is a `warning`, not a failure.
    // Then the project's own password is set again (a dump may change it).
    // Both stop when `cancel` is set, and after database_tool_limit().
    bool load_project_database(const std::string& project, const std::filesystem::path& sql,
                               const std::atomic<bool>& cancel, ServiceRefusal& refusal,
                               const std::filesystem::path& extras = {}, std::string* warning = nullptr);
    // How much the project's database takes on disk; nothing when it has
    // none yet (or no database at all). Read from its folder: nothing starts.
    [[nodiscard]] std::optional<std::uint64_t> project_database_bytes(const std::string& project) const;

    // While the returned guard lives, project_login() refuses `project`
    // ("removing"): projects.remove holds it across the drop and the removal.
    class ProjectHold {
    public:
        ProjectHold() = default;
        ProjectHold(DatabaseService* service, std::string project)
            : service_(service), project_(std::move(project)) {}
        ProjectHold(const ProjectHold&) = delete;
        ProjectHold& operator=(const ProjectHold&) = delete;
        ProjectHold(ProjectHold&& other) noexcept
            : service_(std::exchange(other.service_, nullptr)), project_(std::move(other.project_)) {}
        ProjectHold& operator=(ProjectHold&&) = delete;
        ~ProjectHold() {
            if (service_ != nullptr) {
                service_->release_project(project_);
            }
        }

    private:
        DatabaseService* service_ = nullptr;
        std::string project_;
    };
    [[nodiscard]] ProjectHold hold_project(const std::string& project);

    // Shared sites' login (D19, user "www"), made the first time. Shared
    // sites must be on and have a database.
    [[nodiscard]] std::optional<DatabaseLogin> shared_login(ServiceRefusal& refusal);

    // Settings → Database for one series ("11.4"):
    //   {"server": that server as status() lists it, or null when it has none
    //              (the series isn't installed),
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
    // root's login on the series' server, started (and waited for) when it
    // sleeps or never ran: its first start makes root's password. It then
    // sleeps when idle like any other, unless it's kept running.
    [[nodiscard]] std::optional<DatabaseLogin> root_login(const std::string& series, ServiceRefusal& refusal);
    // Before the MariaDB install `version` ("11.4.2") is removed: when a
    // server of its series runs from it (or is still shutting down from
    // it), or it's the series' last install, that server shuts down (this
    // waits, so no file is in use) and none is made for the series until
    // the hold ends. An older patch release nothing runs from holds nothing:
    // the series' server stays up. The hold ends with the returned guard.
    class RemovalHold {
    public:
        RemovalHold() = default;
        RemovalHold(DatabaseService* service, std::string series)
            : service_(service), series_(std::move(series)) {}
        RemovalHold(const RemovalHold&) = delete;
        RemovalHold& operator=(const RemovalHold&) = delete;
        RemovalHold(RemovalHold&& other) noexcept
            : service_(std::exchange(other.service_, nullptr)), series_(std::move(other.series_)) {}
        RemovalHold& operator=(RemovalHold&&) = delete;
        ~RemovalHold() {
            if (service_ != nullptr) {
                service_->release_series(series_);
            }
        }
        [[nodiscard]] bool held() const { return service_ != nullptr; }

    private:
        DatabaseService* service_ = nullptr;
        std::string series_;
    };
    [[nodiscard]] RemovalHold hold_for_removal(const std::string& version);
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
    void release_series(const std::string& series) noexcept;
    void release_project(const std::string& project) noexcept;
    [[nodiscard]] bool project_held(const std::string& project) const;
    void reload_locked();
    [[nodiscard]] std::shared_ptr<Server> server_of(const std::string& series);
    // The server a project's database is on (its floserver.json's series).
    [[nodiscard]] std::shared_ptr<Server> project_server(const std::string& project, ServiceRefusal& refusal);
    bool run_project_tool(const std::string& project, bool dump, const std::filesystem::path& work,
                          const std::filesystem::path& input, const std::atomic<bool>& cancel, ServiceRefusal& refusal);
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
    std::map<std::string, int> held_;                         // series being removed: no server
    std::map<std::string, int> held_projects_;                // projects being removed: no login
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

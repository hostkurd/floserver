#pragma once

// A project that runs its own server (D11): a Node dev server, which nginx
// forwards the project's address to.
//
// A dev server binds its own port and can't be handed a listening socket the
// way php-cgi is (Q1), so it sleeps the way MariaDB does: the service holds
// the port nginx forwards to and relays each connection to the app's server
// on an internal port. The first connection starts it (after `npm install`
// when package.json lists packages and node_modules is missing) and waits until it listens; when no
// connection has been open for the idle time it is stopped, and the port
// stays. A server that can't start answers with a page saying why.
//
// How it starts, in order: "command" from floserver.json, run by the
// system's shell; else package.json's "dev" script, else its "start" script,
// through the npm that comes with the project's Node. The port to listen on
// is in PORT (HOST is 127.0.0.1); for dev servers known to ignore PORT
// (Vite, Next.js, Nuxt, Astro, Angular) it's also passed as --port.

#include "floserver/core/child_process.h"
#include "floserver/core/ports.h"
#include "floserver/core/site_plan.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace floserver::core {

inline constexpr Port kFirstAppInternalPort = 9600;

// The node program and npm's script in a Node install.
[[nodiscard]] std::optional<std::filesystem::path> node_program(const std::filesystem::path& node_dir);
[[nodiscard]] std::optional<std::filesystem::path> npm_script(const std::filesystem::path& node_dir);

struct AppLaunch {
    ChildOptions options;  // program, args, env; working_dir and log are set by the runner
    std::string shown;     // the command as a person would type it, for the log and the status
};

// How `app` starts, listening on `port`. Reads package.json when there's no
// command. Nothing (with `error`, in plain English) when it can't.
[[nodiscard]] std::optional<AppLaunch> app_launch(const AppSpec& app, Port port, std::string& error);

// The extra arguments for a package.json script whose dev server ignores
// PORT, such as "vite" → --port 9601 --strictPort --host 127.0.0.1; none for
// the rest or when the script names a port itself.
[[nodiscard]] std::vector<std::string> port_arguments(const std::string& script, Port port);

// Internal ports given out across apps.
class PortSet {
public:
    // A port from `first` up that `probe` says is free and isn't given out.
    [[nodiscard]] std::optional<Port> take(Port first, const PortProbe& probe);
    void give_back(Port port);

private:
    std::mutex mutex_;
    std::set<Port> taken_;
};

enum class AppState : std::uint8_t { Sleeping, Starting, Running, Failed };

struct AppRunnerConfig {
    std::filesystem::path work_dir;  // the web folder; logs go to apps/<name>.log
    std::shared_ptr<PortSet> internal_ports;
    PortProbe probe;
    std::chrono::seconds start_limit{120};    // from starting until it listens
    std::chrono::seconds install_limit{600};  // npm install
};

class AppRunner : public std::enable_shared_from_this<AppRunner> {
public:
    using Clock = std::chrono::steady_clock;

    // Opens the app's port. Nothing (with `error`) when it's taken.
    [[nodiscard]] static std::shared_ptr<AppRunner> open(const AppSpec& app, AppRunnerConfig config, std::string& error);
    AppRunner(const AppRunner&) = delete;
    AppRunner& operator=(const AppRunner&) = delete;
    AppRunner(AppRunner&&) = delete;
    AppRunner& operator=(AppRunner&&) = delete;
    ~AppRunner();

    [[nodiscard]] AppSpec app() const;
    // Same name and port. A different runtime or command stops the running
    // server; the next connection starts the new one.
    void set_app(const AppSpec& app);
    [[nodiscard]] const LoopbackListener& listener() const { return *listener_; }
    [[nodiscard]] const std::filesystem::path& log_file() const { return log_; }

    // Takes a waiting connection and serves it on a thread of its own,
    // starting the server first when it isn't running.
    void accept_waiting();
    // Starts the server in the background unless it runs (Keep running).
    void wake();
    // Housekeeping, about once a second: a server that ended, idle time.
    void tick(Clock::time_point now, std::chrono::seconds idle_after);
    // Stops the server and every connection, and starts nothing again.
    void close();

    //   {"state", "port", "internal_port"?, "pid"?, "connections", "command"?, "log", "error"?}
    [[nodiscard]] nlohmann::json describe() const;
    [[nodiscard]] AppState state() const;

private:
    AppRunner(const AppSpec& app, AppRunnerConfig config, std::unique_ptr<LoopbackListener> listener);
    // Starts it unless it runs, and waits. False (with `error`) when it can't.
    bool ensure_running(std::string& error);
    std::unique_ptr<ChildProcess> start_server(const AppSpec& app, Port port, std::string& shown, std::string& error);
    void serve(std::uintptr_t client);
    void stop_process(std::unique_lock<std::mutex>& lock);
    [[nodiscard]] std::string failure_page(const std::string& error) const;

    AppRunnerConfig config_;
    std::unique_ptr<LoopbackListener> listener_;
    std::filesystem::path log_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    AppSpec app_;
    AppState state_ = AppState::Sleeping;
    std::unique_ptr<ChildProcess> process_;
    std::optional<Port> internal_;
    std::string shown_;
    std::string error_;
    Clock::time_point started_;
    Clock::time_point retry_after_;
    bool closed_ = false;
    std::set<std::uintptr_t> clients_;  // open relays, ended by close()

    std::atomic<int> connections_{0};
    std::atomic<Clock::rep> last_activity_{0};
    std::atomic<int> threads_{0};
};

}  // namespace floserver::core

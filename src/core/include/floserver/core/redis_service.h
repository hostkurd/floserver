#pragma once

// Garnet, run by the service (D30; garnet.h has what's written where).
//
// Sleep when idle (D11), as Mailpit and MariaDB do: the service holds the
// public port (6379, or the one config.json keeps; 127.0.0.1 only) and
// relays each connection to Garnet's Unix-domain socket in the owner-only
// <data>/garnet (never TCP: no other program can take it). Before anything
// is forwarded the relay reads the connection's first bytes and closes it
// when they look like HTTP (classify_first_bytes()): a web page can send to
// a local port, and nothing it sends reaches Garnet.
//
// The first connection starts Garnet and waits until its socket answers
// (AUTH, then PING). When no relayed connection has been open for the idle
// time it's stopped: SAVE first (a checkpoint), so the data comes back
// with --recover on the next start ("saving" until it's gone). "keep_running"
// keeps it up, and Garnet checkpoints by itself every 5 minutes meanwhile.
// Every other stop (FloServer quits, a new memory limit, an update or a
// removal) saves too. A failed SAVE stays in the status ("save_error") until
// one works.

#include "floserver/core/child_process.h"
#include "floserver/core/garnet.h"
#include "floserver/core/ports.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_service.h"  // ServiceRefusal
#include "floserver/core/secret_store.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace floserver::core {

struct RedisServiceConfig {
    std::filesystem::path data_dir;
    std::chrono::seconds idle_after{600};
    std::chrono::seconds reload_every{5};
    std::chrono::seconds start_limit{60};  // from starting until its socket answers (.NET starts slowly)
    std::chrono::milliseconds first_bytes_limit{10000};  // a client says something first, or it's closed
    PortProbe probe;                       // empty: can_listen_on_loopback
    // Changes config.json under the lock every other change takes
    // (ProjectService::change_config); empty: loaded and saved directly.
    std::function<bool(const std::function<void(AppConfig&)>& change, std::string& error)> change_config;
};

class RedisService {
public:
    RedisService(RedisServiceConfig config, SecretStore secrets);
    RedisService(const RedisService&) = delete;
    RedisService& operator=(const RedisService&) = delete;
    RedisService(RedisService&&) = delete;
    RedisService& operator=(RedisService&&) = delete;
    ~RedisService();

    void start();
    // Saves and stops Garnet, and stops listening. Safe to call twice.
    void stop();
    // Brings it in line with the installed runtimes and config.json now.
    void reload();

    // redis.status as protocol.h has it (cheap; "projects" comes from the daemon).
    [[nodiscard]] nlohmann::json status() const;
    // The public port while Garnet is installed (.env lines), else nothing.
    [[nodiscard]] std::optional<Port> port() const;
    // The password (made the first time), or nothing (with `error`).
    [[nodiscard]] std::optional<std::string> password(std::string& error);

    // redis.set {keep_running?, memory_mb?}: saved in config.json.
    bool set(const nlohmann::json& changes, ServiceRefusal& refusal);
    // redis.flush: every key deleted (FLUSHALL, then SAVE); it wakes Garnet.
    bool flush(ServiceRefusal& refusal);

    // While the returned guard lives, Garnet is saved and stopped (this
    // waits) and doesn't start: its install can be removed or replaced.
    class RemovalHold {
    public:
        RemovalHold() = default;
        explicit RemovalHold(RedisService* service) : service_(service) {}
        RemovalHold(const RemovalHold&) = delete;
        RemovalHold& operator=(const RemovalHold&) = delete;
        RemovalHold(RemovalHold&& other) noexcept : service_(std::exchange(other.service_, nullptr)) {}
        RemovalHold& operator=(RemovalHold&&) = delete;
        ~RemovalHold() { release(); }
        void release() {
            if (service_ != nullptr) {
                std::exchange(service_, nullptr)->release_hold();
            }
        }

    private:
        RedisService* service_ = nullptr;
    };
    [[nodiscard]] RemovalHold hold_for_removal();

    // Saving: taken out of service and saving before it's stopped (asleep after).
    enum class State : std::uint8_t { Asleep, Starting, Running, Saving, Failed };

private:
    void supervise();
    void serve(std::uintptr_t client);
    bool ensure_running(std::string& error);
    void stop_process();
    // Saves (when it runs and `save`), then ends a process taken out of
    // process_ (killing_ counted), then lets starts go on.
    void kill_and_note(std::unique_ptr<ChildProcess> process, bool save = true);
    void release_hold();
    // Keeps (or clears, once a SAVE works) why the last SAVE failed.
    void note_save(const std::optional<RespReply>& saved);
    void run_detached(std::function<void()> work);
    void tick(std::chrono::steady_clock::time_point now);
    void refresh_keys() noexcept;
    bool remove_stale_socket(std::string& error) const;
    bool ensure_secret_files(std::string& error);
    // One command on Garnet's socket after AUTH; never through the relay.
    [[nodiscard]] std::optional<RespReply> command(const std::vector<std::string_view>& args,
                                                   std::chrono::milliseconds limit);
    void save_status_locked() const;
    void load_status();
    void touch();

    // Ordered for size (clang-tidy's padding check); what guards what is noted.
    std::unique_ptr<ChildProcess> process_;              // mutex_
    std::chrono::steady_clock::time_point retry_after_;  // mutex_
    std::int64_t keys_ = 0;                              // mutex_: DBSIZE when last asked
    std::atomic<std::int64_t> refused_{0};               // connections closed as HTTP
    std::atomic<std::chrono::steady_clock::rep> last_activity_{0};
    std::thread thread_;
    std::shared_ptr<LoopbackListener> listener_;  // mutex_
    std::string version_;                         // mutex_
    std::string dotnet_version_;                  // mutex_
    std::string install_error_;                   // mutex_: installed but can't run
    std::string error_;                           // mutex_
    std::string save_error_;                      // mutex_: the last SAVE failed, and why
    std::string password_;                        // mutex_
    SecretStore secrets_;
    std::mutex reload_mutex_;
    std::mutex secret_mutex_;
    mutable std::mutex mutex_;
    std::mutex refresh_mutex_;
    std::mutex threads_mutex_;
    std::condition_variable changed_;
    std::condition_variable threads_done_;
    std::filesystem::path entry_;       // mutex_
    std::filesystem::path dotnet_dir_;  // mutex_
    RedisServiceConfig config_;
    GarnetPaths paths_;
    int held_ = 0;     // mutex_
    int killing_ = 0;  // mutex_
    std::atomic<int> active_{0};
    int threads_ = 0;         // threads_mutex_
    RedisSettings settings_;  // mutex_
    bool installed_ = false;  // mutex_
    State state_ = State::Asleep;  // mutex_
    bool stopped_ = false;         // mutex_
    std::atomic<bool> keys_dirty_{false};
    std::atomic<bool> refreshing_{false};
    std::atomic<bool> stopping_{false};
};

}  // namespace floserver::core

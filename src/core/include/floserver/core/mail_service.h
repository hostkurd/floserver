#pragma once

// Mailpit, run by the service (D27; mailpit.h has what's written where).
//
// Sleep when idle (D11), as MariaDB and the dev servers do: the service
// holds the public SMTP port (1025, or the one config.json keeps; on
// 127.0.0.1 and [::1]) and the port nginx forwards mail.localhost to, and
// relays each connection to Mailpit on its two Unix-domain sockets in the
// owner-only <data>/mailpit (never TCP: no other program can take them). The first connection to
// either starts Mailpit and waits until it listens; when no relayed
// connection has been open for the idle time it is stopped, and the ports
// stay. "keep_running" keeps it up.
//
// New-mail info for the window: when a relayed SMTP connection ends, the
// service asks Mailpit's API (straight to its internal port, with the
// password) how many emails it took and which is the newest; when an inbox
// connection ends, the counts again (at most once a second). The service's
// own API calls never count as activity, so they don't keep Mailpit awake;
// while it sleeps the last values stand. Mailpit is never polled.

#include "floserver/core/child_process.h"
#include "floserver/core/mailpit.h"
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

struct MailServiceConfig {
    std::filesystem::path data_dir;
    std::chrono::seconds idle_after{600};
    std::chrono::seconds reload_every{5};
    std::chrono::seconds start_limit{30};  // from starting until both ports listen
    PortProbe probe;                       // empty: can_listen_on_loopback
    bool ipv6 = true;                      // also hold [::1]:<SMTP port> when the machine has it
    // Changes config.json under the lock every other change takes
    // (ProjectService::change_config); empty: loaded and saved directly.
    std::function<bool(const std::function<void(AppConfig&)>& change, std::string& error)> change_config;
    // Called (never under the service's own lock) when what PHP's php.ini
    // gets changed: the SMTP port, or Settings → Mail's "php".
    std::function<void()> php_changed;
};

// Where nginx forwards mail.localhost, and the file with the header it adds.
struct MailInbox {
    Port port = 0;
    std::filesystem::path auth_include;
};

class MailService {
public:
    MailService(MailServiceConfig config, SecretStore secrets);
    MailService(const MailService&) = delete;
    MailService& operator=(const MailService&) = delete;
    MailService(MailService&&) = delete;
    MailService& operator=(MailService&&) = delete;
    ~MailService();

    void start();
    // Stops Mailpit and stops listening. Safe to call twice.
    void stop();
    // Brings it in line with the installed runtimes and config.json now.
    void reload();

    // mail.status as protocol.h has it, from what the service knows (cheap):
    // everything but "inbox_url" and "inbox_served", which come from the web
    // service (the daemon adds them).
    [[nodiscard]] nlohmann::json status() const;
    // The inbox for nginx: when Mailpit is installed and can run. Its port
    // is held whether a project has the name "mail" or not.
    [[nodiscard]] std::optional<MailInbox> inbox() const;
    // The SMTP port while Mailpit is installed (.env lines), else nothing.
    [[nodiscard]] std::optional<Port> smtp_port() const;

    // mail.set {php?, notices?, keep_running?, max?}: saved in config.json.
    bool set(const nlohmann::json& changes, ServiceRefusal& refusal);
    // mail.test {to?}: one email through the public SMTP port (it wakes Mailpit).
    bool send_test(const std::string& to, ServiceRefusal& refusal);
    // mail.clear: every email deleted through Mailpit's API (it wakes Mailpit).
    bool clear(ServiceRefusal& refusal);

    // While the returned guard lives, Mailpit is stopped (this waits) and
    // doesn't start: its install can be removed or replaced.
    class RemovalHold {
    public:
        RemovalHold() = default;
        explicit RemovalHold(MailService* service) : service_(service) {}
        RemovalHold(const RemovalHold&) = delete;
        RemovalHold& operator=(const RemovalHold&) = delete;
        RemovalHold(RemovalHold&& other) noexcept : service_(std::exchange(other.service_, nullptr)) {}
        RemovalHold& operator=(RemovalHold&&) = delete;
        ~RemovalHold() {
            if (service_ != nullptr) {
                service_->release_hold();
            }
        }

    private:
        MailService* service_ = nullptr;
    };
    [[nodiscard]] RemovalHold hold_for_removal();
    // The emails (mailpit.db) and the counts deleted; only while held.
    bool delete_emails(std::string& error);

    enum class State : std::uint8_t { Asleep, Starting, Running, Failed };

private:
    void supervise();
    void serve(std::uintptr_t client, bool smtp);
    bool ensure_running(std::string& error);
    void stop_process();
    // Ends a process taken out of process_ (killing_ counted), then lets starts go on.
    void kill_and_note(std::unique_ptr<ChildProcess> process);
    void release_hold();
    // Runs `work` on a thread of its own, waited for at stop().
    void run_detached(std::function<void()> work);
    void tick(std::chrono::steady_clock::time_point now);
    void refresh(bool after_smtp) noexcept;
    void refresh_now(bool after_smtp);
    // The old smtp.sock and http.sock, removed before a start.
    bool remove_stale_sockets(std::string& error) const;
    struct SmtpListeners {
        std::shared_ptr<LoopbackListener> v4;
        std::shared_ptr<LoopbackListener> v6;
    };
    // 127.0.0.1:port and, where there's IPv6 loopback, [::1]:port: both or none.
    [[nodiscard]] SmtpListeners open_smtp(Port port) const;
    void save_counts_locked() const;
    void load_counts();
    // The password (made the first time) and the two files from it.
    bool ensure_secret_files(std::string& error);
    // GET/DELETE on Mailpit's API at its internal port, never through the relay.
    [[nodiscard]] std::optional<std::pair<int, std::string>> api(const std::string& method, const std::string& path);
    void touch();

    // Ordered for size (clang-tidy's padding check); what guards what is noted.
    std::unique_ptr<ChildProcess> process_;               // mutex_
    std::chrono::steady_clock::time_point retry_after_;   // mutex_
    std::int64_t accepted_seen_ = 0;  // mutex_: Mailpit's SMTPAccepted when last asked, this run
    std::int64_t unread_ = 0;         // mutex_
    std::int64_t total_ = 0;          // mutex_
    std::int64_t notice_seq_ = 0;     // mutex_
    std::atomic<std::chrono::steady_clock::rep> last_activity_{0};
    std::thread thread_;
    std::shared_ptr<LoopbackListener> smtp_listener_;   // mutex_
    std::shared_ptr<LoopbackListener> inbox_listener_;  // mutex_
    std::shared_ptr<LoopbackListener> smtp6_listener_;  // mutex_: [::1], when the machine has it
    nlohmann::json latest_;           // mutex_: null or {"subject", "to", "new_in_batch"}
    std::string version_;             // mutex_
    std::string install_error_;       // mutex_: installed but can't run (too old, no program, no port)
    std::string blocked_by_;          // mutex_: a project named "mail"
    std::string error_;               // mutex_
    std::string password_;            // mutex_
    std::string php_problem_;         // mutex_: why PHP's mail() can't be set up
    SecretStore secrets_;
    std::mutex reload_mutex_;  // one reload at a time
    std::mutex secret_mutex_;  // ensure_secret_files(), one at a time
    mutable std::mutex mutex_;
    std::filesystem::path program_;   // mutex_
    std::mutex refresh_mutex_;
    std::mutex threads_mutex_;
    std::condition_variable changed_;
    std::condition_variable threads_done_;
    MailServiceConfig config_;
    MailpitPaths paths_;
    int held_ = 0;                    // mutex_
    int killing_ = 0;                 // mutex_: processes taken out and still being ended
    std::atomic<int> active_{0};
    int threads_ = 0;                 // threads_mutex_
    MailSettings settings_;           // mutex_
    bool installed_ = false;          // mutex_
    State state_ = State::Asleep;     // mutex_
    bool stopped_ = false;            // mutex_
    std::atomic<bool> inbox_dirty_{false};
    std::atomic<bool> refreshing_{false};
    std::atomic<bool> stopping_{false};
};

}  // namespace floserver::core

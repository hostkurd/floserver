// floserverd: the background service in the user's session (D2).
//
// This stage: one service per user, the private channel, and the first two
// requests (status, quit). Runtimes, projects and idle timers come next.

#include "floserver/core/build_info.h"
#include "floserver/core/ipc.h"
#include "floserver/core/process.h"
#include "floserver/core/protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

namespace ipc = floserver::core::ipc;
namespace protocol = floserver::core::protocol;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

// Same-user programs only can connect (the channel sees to that), but cap
// them anyway so a stuck one can't pile up threads without end.
constexpr int kMaxClients = 64;

// Counts client threads so shutdown can wait for replies to be sent.
class ActiveClients {
public:
    // False when kMaxClients are already being served.
    bool try_add() {
        const std::lock_guard lock(mutex_);
        if (count_ >= kMaxClients) {
            return false;
        }
        ++count_;
        return true;
    }
    void remove() {
        {
            const std::lock_guard lock(mutex_);
            --count_;
        }
        changed_.notify_all();
    }
    void wait_until_idle(std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, limit, [this] { return count_ == 0; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int count_ = 0;
};

struct Service {
    std::shared_ptr<ipc::Server> server;
    Clock::time_point started = Clock::now();
    ActiveClients clients;
    std::atomic<bool> quitting{false};

    [[nodiscard]] json status() const {
        const auto memory = floserver::core::memory_in_use();
        return {{"version", floserver::core::kVersion},
                {"pid", floserver::core::current_pid()},
                {"uptime_s", std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count()},
                {"memory_bytes", memory ? json(*memory) : json(nullptr)}};
    }

    // One client: requests in, replies out, until it hangs up. Nothing a
    // client sends may take the service down.
    void serve(ipc::Connection connection) noexcept {
        try {
            serve_requests(connection);
        } catch (...) {  // NOLINT(bugprone-empty-catch): drop this client, keep serving others
        }
    }

    void serve_requests(ipc::Connection& connection) {
        while (auto text = connection.receive()) {
            std::string error_code;
            const auto request = protocol::parse_request(*text, error_code);
            std::string reply;
            bool quit = false;
            if (!request) {
                reply = protocol::make_error(0, error_code, "the request couldn't be read");
            } else if (request->method == "status") {
                reply = protocol::make_result(request->id, status());
            } else if (request->method == "quit") {
                reply = protocol::make_result(request->id, json::object());
                quit = true;
            } else {
                reply = protocol::make_error(request->id, "unknown_method",
                                             "this FloServer doesn't know \"" + request->method + "\"");
            }
            if (!connection.send(reply)) {
                break;
            }
            if (quit && !quitting.exchange(true)) {
                server->stop();
            }
        }
    }
};

int run() {
    const std::string instance = ipc::default_instance();
    std::unique_ptr<ipc::Server> server;
    std::string error;
    switch (ipc::Server::listen(instance, server, error)) {
        case ipc::ListenStatus::AlreadyRunning:
            std::cout << "FloServer is already running.\n";
            return 0;
        case ipc::ListenStatus::Failed:
            std::cerr << "floserverd: " << error << "\n";
            return 1;
        case ipc::ListenStatus::Listening:
            break;
    }

    auto service = std::make_shared<Service>();
    service->server = std::move(server);

    while (auto connection = service->server->accept()) {
        if (!service->clients.try_add()) {
            continue;  // too many clients: this one is closed
        }
        try {
            std::thread([service, c = std::move(*connection)]() mutable {
                service->serve(std::move(c));
                service->clients.remove();
            }).detach();
        } catch (const std::system_error&) {
            service->clients.remove();  // no thread for this client; keep serving
        }
    }
    // Give the client that asked us to quit time to read the reply.
    service->clients.wait_until_idle(std::chrono::seconds(2));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--version") {
        std::cout << "floserverd " << floserver::core::kVersion << "\n";
        return 0;
    }
    try {
        return run();
    } catch (const std::exception& e) {
        std::cerr << "floserverd: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "floserverd: unexpected error\n";
    }
    return 1;
}

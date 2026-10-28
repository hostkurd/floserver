#pragma once

// Talking to floserverd from the CLI and the window.

#include "floserver/core/ipc.h"
#include "floserver/core/protocol.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace floserver::core {

class ServiceClient {
public:
    // Nothing when the service isn't running (or `error` says what's wrong).
    [[nodiscard]] static std::optional<ServiceClient> connect(const std::string& instance,
                                                              std::string& error);

    // Sends one request and waits for its reply. Nothing if the connection broke.
    [[nodiscard]] std::optional<protocol::Reply> call(const std::string& method,
                                                      const nlohmann::json& params = nlohmann::json::object());

private:
    explicit ServiceClient(ipc::Connection connection) : connection_(std::move(connection)) {}
    ipc::Connection connection_;
    std::int64_t next_id_ = 1;
};

enum class StartResult : std::uint8_t { Started, AlreadyRunning, Failed };

// Starts floserverd from this program's folder and waits until it answers.
[[nodiscard]] StartResult start_service(const std::string& instance, std::string& error,
                                        std::chrono::milliseconds wait = std::chrono::seconds(5));

// Asks the service to quit and waits until it's gone. True when it isn't
// running afterwards (including when it wasn't running at all).
[[nodiscard]] bool stop_service(const std::string& instance, std::string& error,
                                std::chrono::milliseconds wait = std::chrono::seconds(5));

}  // namespace floserver::core

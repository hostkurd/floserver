#include "floserver/core/service_client.h"

#include "floserver/core/process.h"

#include <thread>

namespace floserver::core {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kPoll = std::chrono::milliseconds(100);

bool is_running(const std::string& instance) {
    std::string ignored;
    return ipc::connect(instance, ignored).has_value();
}

}  // namespace

std::optional<ServiceClient> ServiceClient::connect(const std::string& instance, std::string& error,
                                                    std::chrono::milliseconds timeout) {
    auto connection = ipc::connect(instance, error);
    if (!connection) {
        return std::nullopt;
    }
    (void)connection->set_timeout(timeout);
    return ServiceClient(std::move(*connection));
}

std::optional<protocol::Reply> ServiceClient::call(const std::string& method, const nlohmann::json& params) {
    const std::int64_t id = next_id_++;
    if (!connection_.send(protocol::make_request(id, method, params))) {
        return std::nullopt;
    }
    const auto text = connection_.receive();
    if (!text) {
        return std::nullopt;
    }
    auto reply = protocol::parse_reply(*text);
    if (!reply || reply->id != id) {
        return std::nullopt;
    }
    return reply;
}

StartResult start_service(const std::string& instance, std::string& error, std::chrono::milliseconds wait) {
    error.clear();
    if (auto existing = ipc::connect(instance, error)) {
        return StartResult::AlreadyRunning;
    }
    if (!error.empty()) {
        return StartResult::Failed;  // e.g. someone else holds the channel
    }
    const auto program = sibling_program("floserverd");
    std::error_code ec;
    if (!program || !std::filesystem::is_regular_file(*program, ec)) {
        error = "floserverd wasn't found next to this program";
        return StartResult::Failed;
    }
    if (!spawn_detached(*program, {}, error)) {
        return StartResult::Failed;
    }
    const auto deadline = Clock::now() + wait;
    while (Clock::now() < deadline) {
        if (is_running(instance)) {
            return StartResult::Started;
        }
        std::this_thread::sleep_for(kPoll);
    }
    error = "floserverd was started but didn't answer";
    return StartResult::Failed;
}

bool stop_service(const std::string& instance, std::string& error, std::chrono::milliseconds wait) {
    error.clear();
    {
        // Hang up straight after the reply: the service waits for its clients
        // to leave before it exits.
        auto client = ServiceClient::connect(instance, error);
        if (!client) {
            return error.empty();
        }
        const auto reply = client->call("quit");
        if (!reply || !reply->ok) {
            error = "the service didn't accept the request to quit";
            return false;
        }
    }
    const auto deadline = Clock::now() + wait;
    while (Clock::now() < deadline) {
        if (!is_running(instance)) {
            return true;
        }
        std::this_thread::sleep_for(kPoll);
    }
    error = "the service is still running";
    return false;
}

}  // namespace floserver::core

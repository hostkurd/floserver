#pragma once

// Messages on the private channel (ipc.h): one JSON object each way.
//
//   request:  {"v": 1, "id": 7, "method": "status", "params": {...}}
//   reply:    {"v": 1, "id": 7, "result": {...}}
//          or {"v": 1, "id": 7, "error": {"code": "unknown_method", "message": "..."}}
//
// Methods so far:
//   status  -> {"version", "pid", "uptime_s", "memory_bytes" (or null)}
//   quit    -> {}   the service stops after replying

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace floserver::core::protocol {

inline constexpr int kVersion = 1;

struct Request {
    std::int64_t id = 0;
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

struct Reply {
    std::int64_t id = 0;
    bool ok = false;
    nlohmann::json result;     // when ok
    std::string error_code;    // when not ok
    std::string error_message;
};

[[nodiscard]] std::string make_request(std::int64_t id, std::string_view method,
                                       const nlohmann::json& params = nlohmann::json::object());
[[nodiscard]] std::string make_result(std::int64_t id, const nlohmann::json& result);
[[nodiscard]] std::string make_error(std::int64_t id, std::string_view code, std::string_view message);

// Nothing when the text isn't a well-formed request or reply of this version.
[[nodiscard]] std::optional<Request> parse_request(std::string_view text, std::string& error_code);
[[nodiscard]] std::optional<Reply> parse_reply(std::string_view text);

}  // namespace floserver::core::protocol

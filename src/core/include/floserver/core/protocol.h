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
//   runtimes.list    {"refresh"?: bool}
//                    -> {"platform", "installed": [...], "available": [...],
//                        "list": {"sequence", "expires", "key", "warnings"} or null,
//                        "list_error"?: "..."}
//   runtimes.install {"name": "php", "version"?: "8.5"} -> {"job": 3}
//   runtimes.remove  {"name": "php", "version": "8.5.12"} -> {}
//   jobs.get         {"id": 3} -> runtime_service.h, RuntimeService::job()
//   jobs.cancel      {"id": 3} -> {"cancelled": bool}

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

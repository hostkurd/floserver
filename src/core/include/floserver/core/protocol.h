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
//   projects.list    {} -> project_service.h, ProjectService::list()
//   projects.add     {"path": "C:/sites/shop", "name"?: "shop"} -> the project
//   projects.remove  {"name": "shop", "drop_database"?: bool} -> {} or, with drop_database,
//                    {"database_dropped": bool} (false: there was none to drop)
//                    (the folder is never touched)
//                    drop_database: first the project's own MariaDB database and
//                    user (named by the service: "my-shop" -> my_shop, never by
//                    the request) are dropped and their password deleted from the
//                    secure store; reserved or shared names are refused, and a
//                    failed drop keeps the project. Nothing to drop when that
//                    MariaDB isn't installed. No login makes it again meanwhile.
//   projects.set     {"name": "shop", "on"?: bool, "keep_running"?: bool} -> the project
//   projects.rename  {"name": "shop", "new_name": "store", "rename_folder"?: bool}
//                    -> the project, with "old_name" and "old_path" (D22)
//                    Every other setting stays; its database and user keep
//                    their names ("database_name"), so its .env keeps working.
//                    Its certificate is made for the new name (the old one is
//                    deleted) and its logs move. rename_folder: its folder is
//                    renamed to the new name in the same parent folder,
//                    refused when one of that name is there or it's in use.
//   certs.status     {} -> {"ca": null or {"common_name", "fingerprint", "not_after",
//                                          "path", "der" (base64), "trusted": "yes"|"no"|"unsupported",
//                                          "expires_soon": bool}}
//   certs.ensure_ca  {} -> {"ca": {...}}   makes the CA when there's none
//   certs.remove     {} -> {}              deletes the CA and project certificates
//   web.status       {} -> web_service.h, WebService::status()
//   web.reload       {} -> the same, after bringing everything in line now
// projects.list and the projects.* changes also carry each project's
// "address" and "run" ("sleeping" | "running" | "failed") once it's served.

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
// A request's id even when the rest of it can't be read (bad params, say),
// so a refusal answers the request it refuses; 0 when there's none.
[[nodiscard]] std::int64_t request_id(std::string_view text);

}  // namespace floserver::core::protocol

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
//                    Mailpit: {"name": "mailpit", "version", "keep_emails"?: bool (default true)}
//                    -> {} or, with keep_emails false, {"emails_deleted": bool, "emails_error"?,
//                    "emails_kept"? (why not: another Mailpit version is still installed)}
//   mail.status      {} -> {"installed", "version"?, "state": "not_installed"|"asleep"|"starting"|
//                    "running"|"failed", "error"?, "smtp_port", "inbox_url", "inbox_served",
//                    "inbox_blocked_by"? (the project named "mail"), "unread", "total",
//                    "size_bytes" (mailpit.db on disk), "notice_seq" (+1 each time an SMTP
//                    connection that delivered email ends), "latest"?: {"subject", "to",
//                    "new_in_batch"} (cleaned, at most 200 characters each), "settings": {"php",
//                    "notices", "keep_running", "max"}, "php_error"? (why PHP's mail() isn't set up)}
//                    From the service's cache: cheap enough for a 2 s poll (D27).
//   mail.set         {"php"?, "notices"?, "keep_running"?, "max"?} -> {"ok": true}
//   mail.test        {"to"?} -> {"ok": true}   one email through the SMTP port (wakes Mailpit)
//   mail.clear       {} -> {"ok": true}        every email deleted (wakes Mailpit)
//   mail.env         {"name"} -> {"env": [[key, value]...], "env_file"}   the project's mail
//                    lines alone (databases.login's "env" has them too), for flo env
//   redis.status     {"projects"?: bool} -> {"installed", "version"?, "dotnet_version"?, "state": "not_installed"|
//                    "asleep"|"starting"|"running"|"failed", "error"?, "host": "127.0.0.1", "port",
//                    "keys" (DBSIZE when last asked), "refused" (connections closed as HTTP since
//                    the service started), "settings": {"keep_running", "memory_mb"},
//                    "projects"? (asked for: names with "redis": true), "phpredis": ["8.4", ...]} (D30)
//   redis.set        {"keep_running"?, "memory_mb"?: 16..65536} -> {"ok": true}
//   redis.flush      {} -> {"ok": true}   every key deleted, then saved (wakes Garnet)
//   redis.password   {} -> {"password"}   for Settings → Redis's Copy (never shown)
//                    A project's REDIS_* lines are in databases.login's and mail.env's "env"
//                    when it has "redis": true; mail.env answers for Redis alone too.
//   runtimes.install also takes "variant" (php-redis's "php8-4"); garnet brings dotnet and
//                    php-redis with it (runtime_service.h).
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
//                    -> the project, with "old_name", "old_path" and "old_database_name" (D22)
//                    Every other setting stays; its database and user keep
//                    their names ("database_name"), so its .env keeps working
//                    (only MariaDB's own names, never made, follow the new name).
//                    Its certificate is made for the new name (the old one is
//                    deleted) and its logs move. rename_folder: its folder is
//                    renamed to the new name in the same parent folder,
//                    refused when one of that name is there, it's in use, or
//                    Remove would never bin it (a link, the user's or Windows' folders).
//                    Refused with "busy" while the project's database is being dropped.
//   certs.status     {} -> {"ca": null or {"common_name", "fingerprint", "not_after",
//                                          "path", "der" (base64), "trusted": "yes"|"no"|"unsupported",
//                                          "expires_soon": bool}}
//   certs.ensure_ca  {} -> {"ca": {...}}   makes the CA when there's none
//   certs.remove     {} -> {}              deletes the CA and project certificates
//   web.status       {} -> web_service.h, WebService::status()
//   web.reload       {} -> the same, after bringing everything in line now
//   updates.status   {} -> update_service.h, UpdateService::status()
//   updates.check    {} -> the same; a check of the signed list starts now
//   updates.cancel   {} -> {"cancelled": bool}   stops the app's download
//   backup.plan      {"name"} -> what the Back up dialog offers (sizes, the default file)
//   backup.start     {"name", "to"?, "database"?, "leave_out"?} -> {"job", "file"}
//   backup.inspect   {"file"} -> its floserver-backup.json, and a new project's name and folder
//   backup.restore   {"name", "file", "files"?, "database"?, "install"?} -> {"job"}
//   projects.from_backup {"file", "name"?, "install"?} -> {"job", "name", "folder"}
//   backup.get       {"id"} -> backup_service.h, BackupService::job()
//   backup.cancel    {"id"} -> {"cancelled": bool}
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

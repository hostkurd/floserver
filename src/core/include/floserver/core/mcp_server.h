#pragma once

// `flo mcp`: a read-only MCP server for AI coding tools (D32, approved 11 Nov).
//
// Claude Code, Cursor, VS Code and others start `flo mcp` as a subprocess and
// talk to it over stdio: one JSON-RPC 2.0 message per line each way, UTF-8.
// It asks floserverd through the same per-user channel as `flo` (ipc.h), so
// it sees exactly what `flo` sees, and nothing more: it can't start, stop,
// install, change or delete anything, and never sees a password.
//
// Two eras of the Model Context Protocol are answered:
//   - 2025-11-25 (and 2025-06-18, 2025-03-26): `initialize` -> serverInfo,
//     capabilities {tools: {}}, the version negotiated; then
//     notifications/initialized, tools/list, tools/call, ping.
//   - 2026-07-28, stateless: no initialize; every request carries
//     _meta["io.modelcontextprotocol/protocolVersion"] and
//     _meta["io.modelcontextprotocol/clientCapabilities"]; server/discover
//     says the versions, capabilities and identity; a version this server
//     doesn't have is answered with UnsupportedProtocolVersionError (-32022).
// Batches (a JSON array) are refused: neither era has them.
//
// The tools: list_projects, project_info, list_runtimes, service_status,
// read_log, recent_mail. Text from logs and emails is written by others:
// the tools say it's data, not instructions. Passwords, .env files and other
// secrets are never asked for, and any key named like a password, secret or
// token is hidden from every answer as well (redact_secrets), whatever the
// service said.
//
// This is a pure function of the request and what the service answers
// (Backend), so it's tested without a service; the CLI adds the stdio loop.

#include "floserver/core/protocol.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace floserver::core::mcp {

// The stateless revision, and the handshake ones, newest first.
inline constexpr std::string_view kModernVersion = "2026-07-28";
inline constexpr std::array<std::string_view, 4> kLegacyVersions = {"2025-11-25", "2025-06-18", "2025-03-26",
                                                                   "2024-11-05"};

// JSON-RPC's codes, and MCP's own.
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kUnsupportedProtocolVersion = -32022;

// The most lines read_log gives, and emails recent_mail gives.
inline constexpr int kMaxLogLines = 200;
inline constexpr int kMaxMails = 20;
// The deepest nesting a message (or a floserver.json) may have.
inline constexpr int kMaxDepth = 64;
// The longest line read from stdin; a longer one is a parse error.
inline constexpr std::size_t kMaxLine = std::size_t{4} * 1024 * 1024;

// What the server asks of FloServer.
class Backend {
public:
    Backend() = default;
    virtual ~Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    // One request to floserverd (only read_only_method() ones are ever
    // asked). Nothing when the service can't be reached; `error` says why.
    [[nodiscard]] virtual std::optional<protocol::Reply> call(const std::string& method, const nlohmann::json& params,
                                                              std::string& error) = 0;
    // The text of <folder>/floserver.json, the folder being one the service
    // named (read_project_json). Nothing when it's missing (`why` empty) or
    // refused (`why` says why: a link, over 256 KiB...).
    [[nodiscard]] virtual std::optional<std::string> project_file(const std::filesystem::path& folder,
                                                                  std::string& why) = 0;
};

// The service's methods `flo mcp` may use: none of them changes anything or
// returns a password. Every other one is refused before it's sent.
[[nodiscard]] bool read_only_method(std::string_view method);

// True for a key named like a secret, percent-decoded and compared without
// case: containing pass, pwd, secret, token, cookie, credential, dsn, apikey,
// privatekey, passphrase, authorization, authentication, bearer, signature,
// salt, hmac, jwt, session, sessid or otp; with auth, auths, key, keys, pw,
// sig, code, sid, pat, sk, pin or mfa as a part of its own (APP_KEY,
// BASIC_AUTH, default_pw, x-amz-signature); or ending in "key" (licensekey).
[[nodiscard]] bool secret_key(std::string_view key);

// `text` with the values of secrets replaced by "[hidden]": after a
// secret_key() name in the shapes logs and configs use (name=v, "name":"v",
// 'name' => 'v', [name] => v, name: v, --name v), mysql's -pPASSWORD, the
// token after Bearer/Basic (and "Authorization: Bearer ..."), a URL's
// password (mysql://root:pw@host), and known tokens anywhere (ghp_,
// github_pat_, sk_live_, xoxb-, AKIA..., JWTs).
[[nodiscard]] std::string blank_secrets(std::string_view text);
// True when blank_secrets() would hide something.
[[nodiscard]] bool secret_value(std::string_view value);

// The second line of defence, after each tool picked what it shows: every
// value under a secret_key() becomes "[hidden]", and every string has its
// secrets blanked (blank_secrets), at any depth.
void redact_secrets(nlohmann::json& value);

// <folder>/floserver.json's text for project_info: only the folder's own
// plain file (plain_file.h: no symbolic or hard link, checked on the open
// file), at most 256 KiB. Nothing otherwise, with `why` (empty when there's
// none): a floserver.json that leads elsewhere (to ~/.docker/config.json,
// say) is never read.
[[nodiscard]] std::optional<std::string> read_project_json(const std::filesystem::path& folder, std::string& why);

// The tools as tools/list gives them, in a fixed order. `version` picks
// what that revision has (2025-03-26 has no title or outputSchema).
[[nodiscard]] nlohmann::json tool_list(std::string_view version);

class Server {
public:
    explicit Server(Backend& backend) : backend_(backend) {}

    // One line read from stdin -> the line to write to stdout (no newline),
    // or nothing (a notification, or a blank line).
    [[nodiscard]] std::optional<std::string> handle_line(std::string_view line);
    // The same for a parsed message.
    [[nodiscard]] std::optional<nlohmann::json> handle(const nlohmann::json& message);

    // The version `initialize` agreed on; empty until one did.
    [[nodiscard]] const std::string& legacy_version() const { return legacy_version_; }

private:
    [[nodiscard]] nlohmann::json call_tool(const std::string& name, const nlohmann::json& args, std::string_view version);

    Backend& backend_;
    std::string legacy_version_;
};

}  // namespace floserver::core::mcp

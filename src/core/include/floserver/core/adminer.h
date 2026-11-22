#pragma once

// Adminer (D35), an optional tool for PostgreSQL (runtime kind "adminer",
// platform "any"): one PHP file, adminer-<version>.php.
//
// When it's installed, FloServer's own nginx serves it at adminer.localhost
// (https per Settings → Ports & HTTPS), PHP from a pool of its own on the
// newest installed PHP with pgsql and pdo_pgsql on, sleeping when idle like
// any other. The name "adminer" is kept for it, like "phpmyadmin".
//
// nginx serves only FloServer's own <data>/adminer/index.php, which
// includes the installed file. That folder holds:
//
//   index.php     written by FloServer when its inputs change
//   servers.json  ["127.0.0.1:5432", ...]: FloServer's PostgreSQL servers,
//                 the only ones Adminer may sign in to
//   tokens/       single-use login tokens, owner-only
//   sessions/     Adminer's PHP sessions
//
// In FloServer, Adminer opens PostgreSQL only (MariaDB and MySQL keep
// phpMyAdmin): index.php refuses any other driver, and any server that
// isn't in servers.json, before Adminer runs.
//
// One-click login, as phpMyAdmin's (phpmyadmin.h): the service makes a token
// (32 random bytes, hex) holding the login for at most kSignonTokenLife and
// returns https://adminer.localhost/?floserver_token=…&lang=…; index.php
// claims it by renaming its file (so it works once) and hands Adminer its own
// login form, filled in. Passwords never go into a URL, a log or a command line.

#include "floserver/core/ports.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

// Adminer's name among sites and pools; project names can't have '_'.
inline constexpr std::string_view kAdminerSiteName = "_adminer";
// Its address is <this>.localhost; no new project may have this name.
inline constexpr std::string_view kAdminerHostName = "adminer";
// The runtime kind in the signed list.
inline constexpr std::string_view kAdminerRuntime = "adminer";

struct AdminerPaths {
    std::filesystem::path dir;       // <data>/adminer
    std::filesystem::path index;     // index.php
    std::filesystem::path servers;   // servers.json
    std::filesystem::path tokens;    // tokens/
    std::filesystem::path sessions;  // sessions/
};

[[nodiscard]] AdminerPaths adminer_paths(const std::filesystem::path& data);

// Makes the folders (tokens/ and sessions/ owner-only outside Windows).
[[nodiscard]] bool ensure_adminer_dirs(const AdminerPaths& paths, std::string& error);

// The installed file: <install>/adminer-<version>.php.
[[nodiscard]] std::filesystem::path adminer_file(const std::filesystem::path& install, std::string_view version);

// index.php's text, or nothing when a path can't be written into PHP.
[[nodiscard]] std::optional<std::string> adminer_index_script(const AdminerPaths& paths,
                                                              const std::filesystem::path& adminer_php);

// servers.json's text for these PostgreSQL ports.
[[nodiscard]] std::string adminer_servers_json(std::vector<Port> ports);

// Adminer's language for FloServer's: "ar", or "en" (Adminer has no Kurdish).
[[nodiscard]] std::string adminer_lang(std::string_view code);

// What one token signs in with.
struct AdminerLogin {
    std::string user;
    std::string password;
    Port port = 0;                        // 127.0.0.1's
    std::optional<std::string> database;  // a project: opens on its database
};

// Writes a token file (owner-only) and returns the token; first removes
// tokens older than kSignonTokenLife. Nothing (with `error`) when it can't.
[[nodiscard]] std::optional<std::string> create_adminer_token(const std::filesystem::path& tokens_dir,
                                                              const AdminerLogin& login,
                                                              std::chrono::system_clock::time_point now,
                                                              std::string& error);

// "https://adminer.localhost:8443/?floserver_token=…&lang=ar".
[[nodiscard]] std::string adminer_url(std::string_view address, std::string_view token, std::string_view lang);

// Why a MariaDB or MySQL project has no Adminer, and what to use instead.
[[nodiscard]] std::string adminer_not_postgresql_message();

class DatabaseService;
struct ServiceRefusal;

// adminer.login: what was asked. `series` ("18", as postgres) or `project`
// (its own login, opening on its database); `lang` is the window's.
struct AdminerLoginRequest {
    std::string series;
    std::string project;
    std::string lang;
};

// One click: the server is started when it sleeps, servers.json brought up
// to date with FloServer's PostgreSQL servers, and a token made. The link
// for Adminer at `address`, or nothing (with `refusal`).
[[nodiscard]] std::optional<std::string> adminer_login(DatabaseService& databases, const std::filesystem::path& data,
                                                       std::string_view address, const AdminerLoginRequest& request,
                                                       ServiceRefusal& refusal);

}  // namespace floserver::core

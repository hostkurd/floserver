#pragma once

// phpMyAdmin, an optional tool (runtime kind "phpmyadmin", platform "any").
//
// When it's installed, FloServer's own nginx serves it at
// phpmyadmin.localhost (https per Settings → Ports & HTTPS), PHP from a pool
// of its own on the newest installed PHP that sleeps when idle like any
// other. The name "phpmyadmin" is kept for it: new projects can't take it;
// a project that already has it keeps its address and phpMyAdmin isn't
// served (the window says why).
//
// Its files live in <data>/phpmyadmin, never in the install (which is
// replaced by an update) and never under any web root:
//
//   config.inc.php       written by FloServer when its inputs change, owner-only
//   floserver-signon.php the one-click login, served only at
//                        phpmyadmin.localhost/floserver-signon.php
//   tokens/              single-use login tokens, owner-only
//   sessions/            PHP sessions of phpMyAdmin and the signon
//   tmp/                 phpMyAdmin's TempDir
//
// The install gets a two-line config.inc.php that only includes ours (the
// way phpMyAdmin 5.2 finds its config: libraries/vendor_config.php names
// ROOT_PATH/config.inc.php), written again whenever it's missing.
//
// One-click login: the service makes a token (32 random bytes, hex) holding
// the login for at most kSignonTokenLife and returns
// https://phpmyadmin.localhost/floserver-signon.php?token=…&lang=…. The
// script claims the token by renaming its file (so it works once), deletes
// it, refuses a missing, malformed, used or expired one, sets phpMyAdmin's
// signon session and redirects to phpMyAdmin. Passwords never go into a URL,
// a log or a command line.

#include "floserver/core/ports.h"
#include "floserver/core/secret_store.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

// phpMyAdmin's name among sites and pools; project names can't have '_'.
inline constexpr std::string_view kPhpMyAdminSiteName = "_phpmyadmin";
// Its address is <this>.localhost; no new project may have this name.
inline constexpr std::string_view kPhpMyAdminHostName = "phpmyadmin";
// The runtime kind in the signed list.
inline constexpr std::string_view kPhpMyAdminRuntime = "phpmyadmin";
inline constexpr std::string_view kSignonScriptName = "floserver-signon.php";
inline constexpr std::chrono::seconds kSignonTokenLife{60};
inline constexpr std::size_t kSignonTokenBytes = 32;

// True for names no new project may take: "phpmyadmin".
[[nodiscard]] bool reserved_project_name(std::string_view name);

struct PhpMyAdminPaths {
    std::filesystem::path dir;            // <data>/phpmyadmin
    std::filesystem::path config;         // config.inc.php
    std::filesystem::path signon_script;  // floserver-signon.php
    std::filesystem::path tokens;         // tokens/
    std::filesystem::path sessions;       // sessions/
    std::filesystem::path temp;           // tmp/
};

[[nodiscard]] PhpMyAdminPaths phpmyadmin_paths(const std::filesystem::path& data);

// Makes the folders (tokens/ and sessions/ owner-only outside Windows).
[[nodiscard]] bool ensure_phpmyadmin_dirs(const PhpMyAdminPaths& paths, std::string& error);

// phpMyAdmin's language for FloServer's: "en", "ar", or "ckb" (Kurdish
// Sorani, also from "ku"); "en" for anything else.
[[nodiscard]] std::string phpmyadmin_lang(std::string_view code);

// One MariaDB server phpMyAdmin offers.
struct PhpMyAdminServer {
    std::string series;  // "11.4"
    Port port = 0;
};

// The signon session's cookie name for a series: "FloServerSignon_11_4".
[[nodiscard]] std::string signon_session_name(std::string_view series);

struct PhpMyAdminConfigSpec {
    std::string blowfish_hex;               // 64 hex digits (32 bytes)
    PhpMyAdminPaths paths;
    std::vector<PhpMyAdminServer> servers;  // in order: server 1, 2...
    std::string lang = "en";                // phpmyadmin_lang()
};

// The servers in the order config.inc.php lists them (by series, oldest
// first, one per series); phpMyAdmin's server N is element N-1.
[[nodiscard]] std::vector<PhpMyAdminServer> phpmyadmin_servers(std::vector<PhpMyAdminServer> servers);

// config.inc.php's text, or nothing (with `error`) when an input can't be
// written into it safely.
[[nodiscard]] std::optional<std::string> phpmyadmin_config(const PhpMyAdminConfigSpec& spec, std::string& error);

// The DefaultLang a config written by phpmyadmin_config() holds, or nothing.
[[nodiscard]] std::optional<std::string> phpmyadmin_config_lang(std::string_view config_text);

// The install's own config.inc.php: it only includes `config`.
[[nodiscard]] std::optional<std::string> phpmyadmin_config_stub(const std::filesystem::path& config);

// The signon script's text.
[[nodiscard]] std::optional<std::string> phpmyadmin_signon_script(const PhpMyAdminPaths& paths);

// A 32-byte random secret as 64 hex digits, or nothing when the system's
// random source fails.
[[nodiscard]] std::optional<std::string> make_blowfish_secret(std::string& error);

// What one token signs in with.
struct SignonLogin {
    std::size_t server = 1;              // phpMyAdmin's server number
    std::string series;                  // its series: the signon session's name
    std::string user;
    std::string password;
    std::string host = "127.0.0.1";
    Port port = 0;
    std::optional<std::string> only_db;  // a project: its database only
};

// 64 lower-case hex digits.
[[nodiscard]] bool is_signon_token(std::string_view token);

// Writes a token file (owner-only) and returns the token; first removes
// tokens older than kSignonTokenLife. Nothing (with `error`) when it can't.
[[nodiscard]] std::optional<std::string> create_signon_token(const std::filesystem::path& tokens_dir,
                                                             const SignonLogin& login,
                                                             std::chrono::system_clock::time_point now,
                                                             std::string& error);

// Deletes token files older than kSignonTokenLife (and leftovers of claimed ones).
void remove_expired_signon_tokens(const std::filesystem::path& tokens_dir);

// Keeps <data>/phpmyadmin/config.inc.php in line with its inputs: the secret
// (made once, kept in the secure store as "phpmyadmin/blowfish"), the
// MariaDB servers and FloServer's language. The file is written only when
// its text changes. Safe from any thread.
class PhpMyAdminConfigWriter {
public:
    using ServerSource = std::function<std::vector<PhpMyAdminServer>()>;
    PhpMyAdminConfigWriter(std::filesystem::path data, SecretStore secrets, ServerSource servers);

    // Writes the file when it changed; false (with `error`) when it can't.
    // `servers_out`: the servers as written (phpMyAdmin's server N is N-1).
    bool update(std::string& error, std::vector<PhpMyAdminServer>* servers_out = nullptr);
    // FloServer's language from now on ("en", "ar", "ku"); written on the next update().
    void set_lang(std::string_view code);

private:
    std::filesystem::path data_;
    SecretStore secrets_;
    ServerSource servers_;
    std::mutex mutex_;
    std::optional<std::string> lang_;  // unknown until the file's or the window's is read
    std::string secret_;
    std::string written_;
};

// "https://phpmyadmin.localhost:8443/floserver-signon.php?token=…&lang=ar":
// `address` is phpMyAdmin's address ("https://phpmyadmin.localhost:8443").
[[nodiscard]] std::string signon_url(std::string_view address, std::string_view token, std::string_view lang);

}  // namespace floserver::core

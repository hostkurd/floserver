#pragma once

// Mailpit (D27): a mail catcher for every project. One Go program
// (github.com/axllent/mailpit, MIT) that takes email over SMTP and shows it
// in a web inbox; nothing is ever sent on to a real address.
//
// Runtime kind "mailpit", installed like phpMyAdmin: one version at a time,
// updated in place (an install of a newer one removes the older ones).
// Versions older than kMinMailpitVersion are never offered (CVE-2026-45709)
// and an older install isn't run.
//
// Addresses:
//   SMTP   127.0.0.1:1025, or 1026, 1027... when it's taken, chosen once and
//          kept in config.json ("mail": {"port"}) so .env files stay right;
//          never a port another program holds.
//   inbox  https://mail.localhost (http when Ports & HTTPS says so). New
//          projects can't take the name "mail"; one that already has it
//          keeps its address and the inbox isn't served (the status says why).
//
// Mailpit itself listens only on those two sockets, never on a TCP port, so
// no other program (another user's, or this user's on Windows, where Go's
// listeners can be shared) can take its place; the folder is owner-only
// (make_owner_only_folder(): 0700, or a protected ACL on Windows).
//
// Its files are in <data>/mailpit, never in the install (an update
// replaces it) and never cleaned up by Settings → Folders or backed up:
//
//   mailpit.db         the emails (kept when the runtime is removed, unless asked)
//   smtp.sock          Mailpit's SMTP, a Unix-domain socket (0600)
//   http.sock          Mailpit's web inbox and API, a Unix-domain socket (0600)
//   ui-auth            the inbox password's hash, owner-only (never the password)
//   nginx-auth.conf    nginx's Authorization header for mail.localhost, owner-only
//   status.json        the last counts, shown while Mailpit sleeps
//
// The inbox password is 32 random bytes (hex), made once and kept in the
// secure store as "mailpit/ui". Mailpit asks for it on its web port; nginx
// adds it to every request for mail.localhost, so the user never types it.
// It's never in an argument, a log or shown text.

#include "floserver/core/ports.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace floserver::core {

// The runtime kind in the signed list.
inline constexpr std::string_view kMailpitRuntime = "mailpit";
// The inbox is at <this>.localhost; no new project may have this name.
inline constexpr std::string_view kMailHostName = "mail";
// The oldest Mailpit FloServer offers or runs.
inline constexpr std::string_view kMinMailpitVersion = "1.30.0";
inline constexpr Port kFirstMailPort = 1025;
// The port the service holds for the inbox (nginx forwards mail.localhost there).
inline constexpr Port kFirstMailInboxPort = 9500;
// The user name in the inbox's Basic authentication.
inline constexpr std::string_view kMailUiUser = "floserver";
inline constexpr std::string_view kMailSecretName = "mailpit/ui";
inline constexpr std::string_view kMailFrom = "floserver@localhost";

// False for a mailpit older than kMinMailpitVersion (or no version at all);
// true for every other runtime.
[[nodiscard]] bool runtime_version_allowed(std::string_view name, std::string_view version);

// Settings → Mail, config.json's "mail" (keys this version doesn't know are kept):
//   {"php": true, "notices": true, "keep_running": false, "max": 500, "port": 1025}
struct MailSettings {
    bool php = true;           // PHP's mail() goes to Mailpit
    bool notices = true;       // the window tells about new email
    bool keep_running = false; // never sleeps
    int max = 500;             // emails kept; older ones go (0: all kept)
    std::optional<Port> port;  // the SMTP port, once chosen

    bool operator==(const MailSettings&) const = default;
};

inline constexpr int kMaxMailMax = 100000;

// A value that can't be used is left at its default (`problems` says which).
[[nodiscard]] MailSettings read_mail_settings(const AppConfig& app, std::vector<std::string>* problems = nullptr);
// Writes them into AppConfig::other["mail"], keeping its other keys.
void write_mail_settings(AppConfig& app, const MailSettings& settings);

struct MailpitPaths {
    std::filesystem::path dir;          // <data>/mailpit
    std::filesystem::path database;     // mailpit.db
    std::filesystem::path ui_auth;      // ui-auth
    std::filesystem::path nginx_auth;   // nginx-auth.conf
    std::filesystem::path status;       // status.json
    std::filesystem::path log;          // <data>/web/logs/mailpit.log (Clear old logs empties it)
    std::filesystem::path smtp_socket;  // smtp.sock
    std::filesystem::path http_socket;  // http.sock
};

[[nodiscard]] MailpitPaths mailpit_paths(const std::filesystem::path& data);

// The mailpit program in an install (mailpit.exe; mailpit elsewhere), at
// its top or in its one folder.
[[nodiscard]] std::optional<std::filesystem::path> mailpit_program(const std::filesystem::path& install_dir);

// The newest installed Mailpit FloServer runs (not older than
// kMinMailpitVersion), or nothing.
[[nodiscard]] const InstalledRuntime* newest_mailpit(const std::vector<InstalledRuntime>& installed);
// The newest install of any version, allowed or not (to say why it doesn't run).
[[nodiscard]] const InstalledRuntime* newest_mailpit_any(const std::vector<InstalledRuntime>& installed);

// A socket's path as Mailpit and connect_local() both take it: UTF-8, '/'.
[[nodiscard]] std::string mailpit_socket_text(const std::filesystem::path& socket);
// Why the sockets can't be made in `paths.dir` (a path longer than
// sun_path takes, or with a character Mailpit's "unix:<path>:<mode>" can't
// hold), or nothing when they can.
[[nodiscard]] std::optional<std::string> mailpit_socket_problem(const MailpitPaths& paths);

// Mailpit's arguments: SMTP and web on the two sockets (0600), the
// database, the most emails kept, any SMTP login accepted (PHP and
// frameworks often send one), no version check, the inbox behind the
// password file. No TCP port, relay, POP3 or webhook. Nothing secret.
[[nodiscard]] std::vector<std::string> mailpit_args(const MailpitPaths& paths, int max);

// Mailpit's MP_* environment variables, removed from its environment so
// nothing set there (a relay, a webhook, another password) changes it.
[[nodiscard]] const std::vector<std::string>& mailpit_env_names();

// SHA-256-crypt ("$5$", Drepper's specification, as glibc's crypt() and
// `openssl passwd -5` make it). `salt`: at most 16 characters of
// ./0-9A-Za-z (more are cut); `rounds` 1000..999999999, 5000 when not
// written in the result.
[[nodiscard]] std::string sha256_crypt(std::string_view password, std::string_view salt, unsigned rounds = 5000);

// The ui-auth line for a password: "floserver:$5$rounds=1000$<salt>$<hash>\n",
// with a random salt. Nothing (with `error`) when the random source fails.
[[nodiscard]] std::optional<std::string> mailpit_auth_line(std::string_view password, std::string& error);
// The Authorization header's value: "Basic " + base64("floserver:<password>").
[[nodiscard]] std::string mailpit_basic_auth(std::string_view password);
// nginx-auth.conf's text, included where nginx forwards mail.localhost.
[[nodiscard]] std::string mailpit_nginx_auth(std::string_view password);

// PHP's mail() to Mailpit (D27), written into every pool's and the
// terminal's php.ini before the user's own lines when Mailpit is installed
// (newest_mailpit()), Settings → Mail's "php" is on and its port is chosen.
// Windows: SMTP, smtp_port, sendmail_from; elsewhere sendmail_path with
// Mailpit's own sendmail. Empty otherwise.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> mail_php_ini(
    const AppConfig& app, const std::vector<InstalledRuntime>& installed);
// Why `mailpit` can't go into sendmail_path, which PHP runs with /bin/sh -c
// (it's quoted there with '...'): a ', $, ", `, \ or control character.
// Nothing when it can, and always on Windows (no sendmail_path there).
[[nodiscard]] std::optional<std::string> mail_sendmail_problem(const std::filesystem::path& mailpit);
// The same as mail_php_ini() says it: why PHP's mail() isn't set up although
// it should be, or nothing.
[[nodiscard]] std::optional<std::string> mail_php_problem(const AppConfig& app,
                                                          const std::vector<InstalledRuntime>& installed);
// The same from its parts (the sendmail program is only used outside
// Windows; empty when mail_sendmail_problem() refuses it).
[[nodiscard]] std::vector<std::pair<std::string, std::string>> mail_php_ini_lines(Port port,
                                                                                  const std::filesystem::path& mailpit);

// Text from an email (a subject, a recipient) made safe to show: UTF-8
// (bad bytes become U+FFFD), control characters (C0, DEL, C1) as spaces,
// bidi, zero-width and other invisible characters (soft hyphen, tags,
// variation selectors) and line/paragraph separators removed, cut to
// `max_chars` code points on a character boundary ("…" added when cut).
[[nodiscard]] std::string clean_mail_text(std::string_view text, std::size_t max_chars = 200);
// Error text (from Mailpit's log, say) made safe for a terminal or a
// window: every control character (escapes, line breaks) a space, bidi and
// zero-width characters removed, at most `max_chars` code points.
[[nodiscard]] std::string clean_error_text(std::string_view text, std::size_t max_chars = 600);

// Which mail lines a project's .env gets.
enum class MailEnvKind : std::uint8_t {
    None,     // FloCMS: no mail settings (PHP's mail() is set up in php.ini)
    Laravel,  // MAIL_MAILER, MAIL_HOST, MAIL_PORT
    Python,   // EMAIL_HOST, EMAIL_PORT (Django's), SMTP_URL
    Other,    // MAIL_HOST, MAIL_PORT, SMTP_URL (Node and the rest)
};

// From the project's folder: a FloCMS project (integration.h), a Laravel
// one (env_style), a Python one (floserver.json asks for Python), else Other.
[[nodiscard]] MailEnvKind mail_env_kind(const std::filesystem::path& project_dir, bool python);
[[nodiscard]] std::vector<std::pair<std::string, std::string>> mail_env_values(MailEnvKind kind, Port port);

}  // namespace floserver::core

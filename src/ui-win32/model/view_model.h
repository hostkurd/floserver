#pragma once

// What the main window shows, made from the service's replies. Plain C++ so
// it's tested on every platform; the window only draws it (D1: the rules
// stay in the service, this only reads its answers).

#include "model/ui_strings.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// The row's state label (mockup: On / Sleeping / Waking… / Off / Needs
// download / Problem). Waking: the project's own server is starting.
enum class RowState : std::uint8_t { On, Asleep, Waking, Off, NeedsDownload, Problem };

// What floserver.json asks for, as written there ("8.4", not "8.4.27").
struct Wants {
    bool known = false;  // the file could be read
    std::string php;       // "8.4", "newest", or empty (no key, or PHP off)
    bool php_off = false;  // "php": false
    std::string node;
    std::string python;
    std::string database;  // "mariadb-11.4"
    std::string web = "nginx";
    // The project's own PHP changes (D20): "bcmath", "-exif"; "memory_limit" -> "1024M".
    std::vector<std::string> extensions;
    std::map<std::string, std::string> php_ini;
};

struct ProjectRow {
    std::string name;
    bool on = false;            // the switch
    bool keep_running = false;
    RowState state = RowState::Off;
    // The address while it's served, else empty.
    std::string address;
    // Shown under the name when there's no address: the folder, what's
    // missing or the problem. Already in the window's language, with code
    // words isolated in right-to-left languages.
    std::string detail;
    // "PHP 8.5.12", "MariaDB 11.4": only what the service reports.
    std::vector<std::string> chips;
    // For the project panel (step 5b): the folder as the service gave it,
    // what the versions resolved to, and what floserver.json asks for.
    std::string path;
    std::string php;       // "8.4.27"
    std::string node;
    std::string python;
    std::string database;  // "mariadb-11.4"
    Wants wants;
    // What the service says is missing, as it says it: "php", "php 8.2", "nginx".
    std::vector<std::string> missing;
    bool static_site = false;  // HTML files served as they are, no PHP
    bool php_files = false;    // PHP is off and the folder has .php files (sent as they are)
};

// projects.list's result ({"projects": [...]}). Entries without a name are skipped.
[[nodiscard]] std::vector<ProjectRow> rows_from_list(const nlohmann::json& result, Lang lang);

[[nodiscard]] Str state_label(RowState state);

// "mariadb-11.4" -> "MariaDB 11.4"; unknown families keep their name.
[[nodiscard]] std::string database_chip(std::string_view database);

// Only http:// or https:// to a host ending in ".localhost" (letters, digits,
// '-' and '.'), with an optional port 1-65535 and nothing after it, or a
// single "/". Anything else is never handed to the shell.
[[nodiscard]] bool is_openable_address(std::string_view address);

// Case-insensitive (ASCII) match of the search text on the name or address.
// Empty or blank text matches every row.
[[nodiscard]] bool row_matches(const ProjectRow& row, std::string_view query);

// "23.4 MB" / "812.0 KB", with Western digits and a dot in every language.
[[nodiscard]] std::string human_bytes(std::uint64_t bytes);

// The footer, from the rows, `status` and `web.status`. Each piece is empty
// when the service didn't say.
struct FooterText {
    std::string counts;  // "2 running · 1 asleep"
    std::string nginx;   // "nginx on ports 80 and 443"
    std::string memory;  // "Service memory: 23.4 MB"
    bool any_running = false;
    // web.status's idle time in whole minutes (at least 1), for the panel's
    // Keep running line; 0 when the service didn't say.
    std::uint64_t idle_minutes = 0;
    // nginx's ports and whether projects get https, while it runs (0: not known).
    std::uint64_t http_port = 0;
    std::uint64_t https_port = 0;
    bool tls = false;
};

// The address a new project called `name` will have, with the ports and
// scheme nginx uses now; "https://<name>.localhost" while that isn't known.
[[nodiscard]] std::string new_project_address(const std::string& name, const FooterText& footer);

[[nodiscard]] FooterText footer_text(const std::vector<ProjectRow>& rows, const nlohmann::json* status,
                                     const nlohmann::json* web, Lang lang);

// --- the project panel (step 5b) -------------------------------------------

// The installed versions per runtime ("php" -> {"8.5.12", "8.4.27"}), newest
// first, from runtimes.list's "installed".
using Installed = std::map<std::string, std::vector<std::string>>;
[[nodiscard]] Installed installed_from_list(const nlohmann::json& result);

// "8.4.27" -> "8.4" (PHP, MariaDB, Python); Node keeps only its major: "24".
[[nodiscard]] std::string version_series(std::string_view runtime, std::string_view version);

// Whether `a` is a newer version than `b`, number by number ("8.10" after "8.9").
[[nodiscard]] bool is_newer_version(std::string_view a, std::string_view b);

enum class Field : std::uint8_t { Php, Database, Web, Node, Python };

struct Choice {
    std::string label;                 // in the window's language
    std::optional<std::string> value;  // what projects.use is sent; nothing removes the key
};

struct VersionField {
    Field field;
    std::string key;      // projects.use's key: "php", "database", "web", "node", "python"
    std::string label;    // "PHP", "Web server"
    std::vector<Choice> choices;
    std::size_t current = 0;  // index into choices
    // `current` is what's in effect without the key in the file: picking it
    // still writes it (PHP only).
    bool implicit = false;
};

// The panel's version pickers for a project: PHP, Database, Web server,
// Node.js, and Python when one is installed or asked for. Each lists what's
// installed (one entry per series), plus what floserver.json asks for when
// that isn't installed ("8.1 (not installed)"). PHP starts with None (sends
// "none": false in the file) and Newest installed ("newest"). Without the
// key, the one in effect is ticked (None for a static, Node or Python
// project, Newest installed for a PHP one) and marked `implicit`.
[[nodiscard]] std::vector<VersionField> version_fields(const ProjectRow& row, const Installed& installed, Lang lang);

// The note under the panel's pickers when PHP is off and the folder has
// .php files (they're sent as they are, not run); empty otherwise.
[[nodiscard]] std::string php_off_note(const ProjectRow& row, Lang lang);

// The port the project's database server listens on, from databases.list
// (the server whose series matches `database`, "mariadb-11.4"), or 0.
[[nodiscard]] std::uint16_t database_port(const nlohmann::json& databases, std::string_view database);

struct LogLine {
    std::string time;    // "19:41:02"
    std::string method;
    std::string uri;
    int status = 0;
    std::string duration;  // "38 ms", "1.2 s"
};

// projects.log's entries. `local` turns a time (ms since 1970, UTC) into
// hours, minutes and seconds of local time.
[[nodiscard]] std::vector<LogLine> log_lines(const nlohmann::json& result,
                                             const std::function<void(std::int64_t ms, int& h, int& m, int& s)>& local);

// --- downloads from the window (step 5c) ----------------------------------------

struct Download {
    std::string name;     // "php"
    std::string version;  // "8.2", or empty for the newest
    bool operator==(const Download&) const = default;
};

// What to install so a project can run: each missing runtime, and nginx
// when none is installed (every project is served through it).
[[nodiscard]] std::vector<Download> downloads_for(const ProjectRow& row, const Installed& installed);

// The row's button: "Download PHP" or "Download 2 runtimes".
[[nodiscard]] std::string download_button_text(const std::vector<Download>& downloads, Lang lang);

// "php" -> "PHP", "mariadb" -> "MariaDB", "node" -> "Node.js"...
[[nodiscard]] std::string runtime_label(std::string_view name);

// A running install from jobs.get: "Downloading PHP 8.5.12… 42%",
// "Installing PHP 8.5.12…", or empty when it's finished.
[[nodiscard]] std::string job_progress_text(const nlohmann::json& job, Lang lang);

// --- New project from a template (D9) ---------------------------------------------

struct TemplateCard {
    std::string id;
    std::string title;        // in the window's language, English when the list has none
    std::string description;
    std::string mark;
    std::uint32_t mark_bg = 0;  // 0xRRGGBB
    std::uint32_t mark_fg = 0;
    std::vector<std::string> chips;
    Wants config;             // what its floserver.json starts with
    bool uses_php = false;    // its config names PHP: PHP, Database and Web server are offered
    bool studio = false;
};

// templates.list's result. Cards without an id or title are skipped.
[[nodiscard]] std::vector<TemplateCard> cards_from_list(const nlohmann::json& result, Lang lang);

// The series new projects start with (D20, set in Settings): "php" -> "8.5".
using Defaults = std::map<std::string, std::string>;
// runtimes.list's "defaults" (php, mariadb, node, python).
[[nodiscard]] Defaults defaults_from_list(const nlohmann::json& result);

// The wizard's version pickers for a template: the fields its config uses,
// starting at the template's versions; versions not installed yet are
// offered as "(not installed)" and downloaded when the project is made.
// A default from Settings is picked instead when it's installed and not
// older than the series the template names.
[[nodiscard]] std::vector<VersionField> template_fields(const TemplateCard& card, const Installed& installed, Lang lang,
                                                        const Defaults& defaults = {});

// A free name for a new project: `base`, else base-2, base-3... ("phpmyadmin"
// is never free: phpMyAdmin has that address).
[[nodiscard]] std::string free_project_name(const std::string& base, const std::vector<ProjectRow>& rows);

// The rules for a project name (as the service checks them): a-z 0-9 and
// '-', at most 63, not starting or ending with '-', not "xn--".
[[nodiscard]] bool is_project_name(std::string_view name);

// One log line as the Log box shows it: "19:41:02  GET  /cart  302  44 ms".
[[nodiscard]] std::string log_text(const LogLine& line);

// The .env lines from databases.login's "env" ([[key, value], ...]), one
// "KEY=value" per line ending in "\r\n", written the way `flo env --write`
// writes them. Empty when there are none.
[[nodiscard]] std::string env_text(const nlohmann::json& login);

// --- Shared sites (D19) ------------------------------------------------------------

// shared.get's answer, as the window keeps it (asked without the folders).
struct SharedSites {
    bool known = false;        // shared.get has answered at least once
    bool on = false;
    std::string dir;           // the www folder, or empty
    std::string php;           // the setting ("8.4"); empty: the newest installed
    std::string web = "apache";
    std::string database;      // "mariadb-11.4", or empty
    // From "status" (null while they're off):
    std::string address;       // "http://localhost:8080" while served
    std::string run;           // "running", "sleeping", "starting", "failed"
    std::string state;         // "needs_download", "problem", or empty
    std::vector<std::string> missing;
    std::string problem;
};

// shared.get's or shared.set's result; `known` is set.
[[nodiscard]] SharedSites shared_from_reply(const nlohmann::json& result);

// Only "http://localhost" or "https://localhost", with an optional port and a single "/" after it:
// shared sites' own address. Anything else is never handed to the shell.
[[nodiscard]] bool is_shared_address(std::string_view address);

// What the Shared sites row shows.
struct SharedRow {
    RowState state = RowState::Off;
    // Under the title: the hint when off, what's missing, the problem, or
    // the www folder. In the window's language, code words isolated.
    std::string detail;
    std::vector<std::string> chips;  // "PHP 8.5", "Apache"
    // The address button's text ("localhost:8080"); empty when there's
    // nothing to open.
    std::string address;
};

[[nodiscard]] SharedRow shared_row(const SharedSites& shared, const Installed& installed, Lang lang);

// What to install so shared sites can run (as downloads_for() for a project).
[[nodiscard]] std::vector<Download> shared_downloads(const SharedSites& shared, const Installed& installed);

// The settings page's pickers: PHP ("Newest installed" and each installed
// series), Database ("None" and each installed MariaDB series) and Web
// server (Apache, nginx). Each key is shared.set's.
[[nodiscard]] std::vector<VersionField> shared_fields(const SharedSites& shared, const Installed& installed, Lang lang);

}  // namespace floserver::ui

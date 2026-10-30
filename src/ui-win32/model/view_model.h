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
    std::string php;
    std::string node;
    std::string python;
    std::string database;  // "mariadb-11.4"
    std::string web = "nginx";
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
};

[[nodiscard]] FooterText footer_text(const std::vector<ProjectRow>& rows, const nlohmann::json* status,
                                     const nlohmann::json* web, Lang lang);

// --- the project panel (step 5b) -------------------------------------------

// The installed versions per runtime ("php" -> {"8.5.12", "8.4.27"}), newest
// first, from runtimes.list's "installed".
using Installed = std::map<std::string, std::vector<std::string>>;
[[nodiscard]] Installed installed_from_list(const nlohmann::json& result);

// "8.4.27" -> "8.4" (PHP, MariaDB, Python); Node keeps only its major: "24".
[[nodiscard]] std::string version_series(std::string_view runtime, std::string_view version);

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
};

// The panel's version pickers for a project: PHP, Database, Web server,
// Node.js, and Python when one is installed or asked for. Each lists what's
// installed (one entry per series), plus what floserver.json asks for when
// that isn't installed ("8.1 (not installed)").
[[nodiscard]] std::vector<VersionField> version_fields(const ProjectRow& row, const Installed& installed, Lang lang);

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

// One log line as the Log box shows it: "19:41:02  GET  /cart  302  44 ms".
[[nodiscard]] std::string log_text(const LogLine& line);

// The .env lines from databases.login's "env" ([[key, value], ...]), one
// "KEY=value" per line ending in "\r\n", written the way `flo env --write`
// writes them. Empty when there are none.
[[nodiscard]] std::string env_text(const nlohmann::json& login);

}  // namespace floserver::ui

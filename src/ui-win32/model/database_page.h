#pragma once

// Settings > Database (MariaDB): database.get's answer for one series, the
// server's state as databases.list says it, the changes not saved yet and
// what Save sends. Plain C++ so it's tested on every platform; the window
// only draws it.

#include "model/ui_strings.h"
#include "model/view_model.h"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace floserver::ui {

// One server, as databases.list (or database.get's "server") has it.
struct DbServer {
    bool exists = false;     // false: nothing uses the series and it isn't kept running
    std::string state;       // sleeping, starting, running, stopping, failed
    int port = 0;
    int port_wanted = 0;     // the port chosen in Settings, when another program has it
    int connections = 0;
    std::string error;
};

// database.get's (and database.set's) answer.
struct DbSeries {
    std::string series;   // "11.4"
    std::string version;  // the newest install of it; empty when none is
    int port = 0;         // the one chosen in Settings; 0: automatic
    bool keep_running = false;
    std::vector<std::string> kept_by;  // projects set to keep running that use it
    std::map<std::string, std::string> ini;      // FloServer's defaults and the series' values
    std::map<std::string, std::string> changed;  // the series' own values
    std::string custom_ini;
    bool custom_ini_exists = false;
    std::vector<std::string> custom_problems;
    std::vector<std::string> custom_keys;
    std::vector<std::string> used_by;     // project names, and "_shared"
    std::vector<std::string> databases;   // whose own database was made on it
    std::vector<std::string> problems;
    std::vector<std::string> known_options;  // canonical, sorted; empty: unknown
    DbServer server;
    bool restarting = false;  // database.set's: the server restarts for the new values
};

[[nodiscard]] std::optional<DbSeries> db_series_from_reply(const nlohmann::json& reply);
[[nodiscard]] DbServer db_server_from_json(const nlohmann::json& server);
// The series' server in databases.list's answer (the main window's 2 s
// snapshot), so Settings shows its state without asking anything itself.
[[nodiscard]] DbServer db_server_from_list(const nlohmann::json& databases, std::string_view series);

// The installed MariaDB series, newest first, each with its newest version.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> installed_db_series(const Installed& installed);
// As settings_php_versions(): the poll's list, else Settings' own runtimes.list answer.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> settings_db_versions(bool has_installed,
                                                                                    const Installed& installed,
                                                                                    const nlohmann::json& runtimes_reply);

// The my.ini values shown as fields: `key` as config.json keeps it
// (canonical: lower case, '_'), `label` as people write it.
struct DbIniField {
    std::string key;
    std::string label;
};
[[nodiscard]] const std::vector<DbIniField>& settings_db_ini_fields();

// "max-connections", "loose-Max_Connections" -> "max_connections": what
// MariaDB reads it as, for comparing with the names it knows.
[[nodiscard]] std::string db_option_base(std::string_view name);
// A name MariaDB doesn't know stops it from starting. Nothing when `known`
// is empty or knows it; else the closest known name ("" when none is close).
[[nodiscard]] std::optional<std::string> unknown_db_option(const std::vector<std::string>& known, std::string_view key);
[[nodiscard]] std::string unknown_db_option_text(std::string_view series, std::string_view key,
                                                 const std::string& suggestion, Lang lang);

// --- changes until Save -------------------------------------------------------

struct DbChanges {
    std::map<std::string, std::optional<std::string>> ini;  // nothing: back to the default
    std::optional<std::string> port;                         // as typed; empty: automatic
    std::optional<bool> keep_running;
    [[nodiscard]] bool empty() const { return ini.empty() && !port && !keep_running; }
};

[[nodiscard]] std::string db_ini_text(const DbSeries& series, const DbChanges& changes, const std::string& key);
[[nodiscard]] bool db_ini_marked(const DbSeries& series, const DbChanges& changes, const std::string& key);
void db_set_ini_text(const DbSeries& series, DbChanges& changes, const std::string& key, const std::string& text);
void db_reset_ini(const DbSeries& series, DbChanges& changes, const std::string& key);

[[nodiscard]] std::string db_port_text(const DbSeries& series, const DbChanges& changes);
void db_set_port_text(const DbSeries& series, DbChanges& changes, const std::string& text);
// The typed port can't be sent: not a number from 1024 to 65535.
[[nodiscard]] bool db_port_bad(const DbChanges& changes);

[[nodiscard]] bool db_keep_running(const DbSeries& series, const DbChanges& changes);
void db_toggle_keep_running(const DbSeries& series, DbChanges& changes);

// database.set's parameters: only what changed.
[[nodiscard]] nlohmann::json database_set_params(const std::string& series, const DbChanges& changes);

// The footer: what Save restarts.
[[nodiscard]] std::string db_restart_text(const DbSeries& series, const DbServer& server, Lang lang);
// The server's state in a line ("Running on port 3306", "Sleeping on port 3306: ...").
[[nodiscard]] std::string db_state_text(const DbSeries& series, const DbServer& server, Lang lang);
// Which of Start, Stop and Restart can be used now.
struct DbActions {
    bool start = false;
    bool stop = false;
    bool restart = false;
};
[[nodiscard]] DbActions db_actions(const DbServer& server);

// Project names with "_shared" as "shared sites", joined: "blog, shop and shared sites".
[[nodiscard]] std::string db_names_text(const std::vector<std::string>& names, Lang lang);

// Why a new root password can't be used (the service checks again): 8 to
// 100 characters, printable ASCII without spaces.
[[nodiscard]] std::optional<Str> db_password_problem(std::string_view password);

}  // namespace floserver::ui

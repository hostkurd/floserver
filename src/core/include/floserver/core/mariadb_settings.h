#pragma once

// Settings → Database (MariaDB): per series, in config.json, beside PHP's
// "php" key:
//
//   "mariadb": {"11.4": {"port": 3307, "keep_running": true,
//                        "ini": {"max_connections": "300"}}}
//
// "port" is the public port projects connect to (absent: 3306 or the next
// free one, kept in the series' folder as before). "keep_running": the
// server never sleeps, and it runs even when no project uses the series.
// "ini" holds my.ini values for the [mysqld] section; names are kept as
// lower case with '_' (MariaDB reads "max-connections" and
// "max_connections" as the same option).
//
// Each series may also have a user-owned <data>/mariadb/<series>/custom.ini
// whose lines go after the values, so they win over them. FloServer's own
// lines (where the data is, the port, 127.0.0.1 only, ...) are written last,
// and lines in custom.ini that would change them, move the server's files,
// open it beyond this computer or skip the passwords are left out with a
// comment saying why (mariadb_option_problem()), so the server stays local
// and needs a password whatever the file says.

#include "floserver/core/project_registry.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

struct MariaDbSeriesSettings {
    std::optional<std::uint16_t> port;
    bool keep_running = false;
    std::map<std::string, std::string> ini;  // canonical name -> value

    bool operator==(const MariaDbSeriesSettings&) const = default;
};

// By series ("11.4").
using MariaDbSettings = std::map<std::string, MariaDbSeriesSettings>;

// "max-connections", "Max_Connections" -> "max_connections".
[[nodiscard]] std::string canonical_mariadb_option(std::string_view name);

// Why `name` can't be set in a FloServer MariaDB, in plain words, or nothing
// when it can. Refused: what FloServer sets itself (datadir, basedir, port,
// socket, named pipe, shared memory, bind-address, pid and log files,
// plugin-dir and plugin loading, init-file, secure-file-priv, user,
// tmp/data directories), anything that would open the server beyond this
// computer (bind-address, extra-port, skip-networking, proxy protocol,
// Galera's wsrep_*) or skip the passwords (skip-grant-tables). The prefixes
// MariaDB reads (loose-, skip-, enable-, disable-, maximum-) and the
// shortened names it accepts ("skip-grant" for skip-grant-tables) are seen
// through. `name` may be written with '-' or '_'.
[[nodiscard]] std::optional<std::string> mariadb_option_problem(std::string_view name);
// One line, no control characters, at most 1000 bytes.
[[nodiscard]] std::optional<std::string> mariadb_value_problem(std::string_view value);

// FloServer's own my.ini values a user may change, in the order written.
[[nodiscard]] const std::vector<std::pair<std::string, std::string>>& mariadb_ini_defaults();

// config.json's "mariadb". Entries that can't be used are left out and
// described in `problems`.
[[nodiscard]] MariaDbSettings read_mariadb_settings(const AppConfig& app, std::vector<std::string>* problems = nullptr);
// Writes one series' settings into config.json's "mariadb" (AppConfig::other),
// keeping keys this version doesn't know; empty settings remove the series.
void write_mariadb_series(AppConfig& app, const std::string& series, const MariaDbSeriesSettings& settings);

// "11.4" from a request: two numbers, nothing else.
[[nodiscard]] std::optional<std::string> mariadb_request_series(std::string_view series);

// <data>/mariadb/<series>/custom.ini.
[[nodiscard]] std::filesystem::path mariadb_custom_ini_file(const std::filesystem::path& data, const std::string& series);
[[nodiscard]] std::string mariadb_custom_ini_header(const std::string& series);
[[nodiscard]] bool ensure_mariadb_custom_ini(const std::filesystem::path& data, const std::string& series,
                                             std::string& error);
// The file's text: empty when it's missing, too big (over 256 KiB), or only the header.
[[nodiscard]] std::string read_mariadb_custom_ini(const std::filesystem::path& data, const std::string& series);

// custom.ini as it goes into my.ini: lines naming a refused option, include
// directives (!include, !includedir) and anything that isn't a comment, a
// [group] or a name[=value] line are turned into a comment saying they were
// left out, and described in `problems` ("line 3: ..."). `keys`: the option
// names it sets (canonical), for "did you mean".
struct FilteredCustomIni {
    std::string text;
    std::vector<std::string> problems;
    std::vector<std::string> keys;
};
[[nodiscard]] FilteredCustomIni filter_mariadb_custom_ini(std::string_view text);

// What a server's my.ini holds beyond FloServer's own lines.
struct MariaDbExtras {
    std::map<std::string, std::string> ini;  // the series' values
    std::string custom_ini;                  // custom.ini, already filtered
    std::string custom_ini_path;             // for the comment above it
};
[[nodiscard]] MariaDbExtras mariadb_extras(const std::filesystem::path& data, const std::string& series,
                                           const MariaDbSeriesSettings* settings);

// Why a root password can't be used: 8 to 100 characters, printable ASCII
// without spaces (it goes through MYSQL_PWD and Windows' credential store).
[[nodiscard]] std::optional<std::string> root_password_problem(std::string_view password);

// The option names a MariaDB install knows, from `mariadbd --verbose --help`
// (canonical, sorted), so the window can warn about one MariaDB would refuse
// to start with. Asked once per install, then read from `cache_file`. The
// program is stopped after `limit`; `timed_out` then says so.
[[nodiscard]] std::optional<std::vector<std::string>> known_mariadb_options(
    const std::filesystem::path& server_program, const std::filesystem::path& cache_file, std::string& error,
    bool& timed_out, std::chrono::milliseconds limit = std::chrono::seconds{10});
// The names in `mariadbd --verbose --help`'s text.
[[nodiscard]] std::vector<std::string> parse_mariadb_help(std::string_view text);

}  // namespace floserver::core

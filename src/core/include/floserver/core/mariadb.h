#pragma once

// MariaDB pieces for the database service (D12), ported from FloCMS
// Studio's dbserver (the team's own code, D4) without Qt: where a series'
// files are, its config, the SQL FloServer runs, and the .env lines a
// project gets. MySQL (D28) is served the same way: where the two differ
// (program names, the first set-up, root's accounts, a few options) the
// family in MariaDbFiles says which (database_engine.h).
//
// One data folder per series ("11.4"): MariaDB can't go back a version on
// the same data, and patch releases of a series share it.
//   <data>/data/mariadb-<series>/   my.ini (my.cnf), installed (once set up), port,
//                                   data/ with mariadbd.err, mariadbd.pid,
//                                   mysqld.sock (not Windows), init.sql (briefly)
//   <data>/data/mysql-<series>/     the same for MySQL, with mysqld.err and mysqld.pid
//
// Passwords are only ever given to the servers' programs in their environment
// (MYSQL_PWD) or on their input, never as an argument. root's first
// password is set by an init file on the first start, before the server
// takes any connection.

#include "floserver/core/database_engine.h"
#include "floserver/core/ports.h"
#include "floserver/core/project_registry.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

// A server's files: a MariaDB's, or a MySQL's (`family` "mysql": mysqld,
// mysqladmin, mysql, mysqldump; mysqld itself sets the data folder up).
struct MariaDbFiles {
    std::string family{kMariaDbFamily};  // "mariadb" or "mysql"
    std::string series;                  // "11.4"
    std::filesystem::path base_dir;      // the install (holds bin/)
    std::filesystem::path server;        // bin/mariadbd(.exe)
    std::filesystem::path install_db;    // mariadb-install-db(.exe): bin/ or scripts/; MySQL: mysqld
    std::filesystem::path admin;         // mariadb-admin(.exe)
    std::filesystem::path client;        // mariadb(.exe)
    std::filesystem::path dump;          // mariadb-dump(.exe): backups (D22)
    std::filesystem::path folder;        // <data>/data/mariadb-<series> (mysql-<series>)
    std::filesystem::path data_dir;      // folder/data
    std::filesystem::path config;        // folder/my.ini (Windows) or my.cnf
    std::filesystem::path error_log;     // data_dir/mariadbd.err (mysqld.err)
    std::filesystem::path pid_file;      // data_dir/mariadbd.pid (mysqld.pid)
    std::filesystem::path init_file;     // data_dir/init.sql: root's password, first start only
    std::filesystem::path installed_marker;  // folder/installed: install-db finished
    std::filesystem::path socket;        // data_dir/mysqld.sock; empty on Windows
};

// "11.4.13" -> "11.4" (MySQL's "8.4.11" -> "8.4"); empty when it isn't a version.
[[nodiscard]] std::string mariadb_series(std::string_view version);

// The files for an install of `version` of `family` and FloServer's data
// folder. Programs are looked for in bin/, sbin/ (a system's mariadbd) and
// scripts/ (the Linux bintar's mariadb-install-db).
[[nodiscard]] MariaDbFiles mariadb_files(const std::filesystem::path& install_dir, const std::string& version,
                                         const std::filesystem::path& data,
                                         std::string_view family = kMariaDbFamily);

// MySQL on Windows (D28): Oracle's mysqld.exe and its tools read option-file
// and command-line paths in the ANSI code page (MariaDB 10.6+ reads UTF-8),
// so a folder with non-English letters breaks them. Such a path is given
// as its short (8.3) form, which is ASCII; when Windows keeps none (8.3
// names turned off), MySQL can't run there and says so.
//   `shorten`: GetShortPathNameW (windows_short_path()), or a test's.
using ShortPathFunction = std::function<std::filesystem::path(const std::filesystem::path&)>;
// GetShortPathNameW's answer for an existing path; empty when there's none
// (and always outside Windows).
[[nodiscard]] std::filesystem::path windows_short_path(const std::filesystem::path& path);
// `path` when it's ASCII, else its short form when that is; else nothing.
[[nodiscard]] std::optional<std::filesystem::path> ascii_path(const std::filesystem::path& path,
                                                              const ShortPathFunction& shorten);
// The paths MySQL's programs are given (its programs, the install, its
// folder and data folder, and the files in them), each as ascii_path()
// says. False, with `bad` the first path that has no ASCII form.
[[nodiscard]] bool ascii_mysql_files(MariaDbFiles& files, const ShortPathFunction& shorten,
                                     std::filesystem::path& bad);
// What a MySQL server says when ascii_mysql_files() can't: its error_code.
inline constexpr std::string_view kPathNotAsciiCode = "path_not_ascii";

// The config: 127.0.0.1 only on `port` (the internal port the service
// relays to), utf8mb4, a small buffer pool (a development server). Paths
// are relative (mariadbd runs in the series' folder), except basedir
// outside Windows. `as_root`: the service runs as root (containers):
// user=root. `with_init_file`: the first start, which sets root's password.
//
// In order: FloServer's defaults a user may change, Settings > Database's
// values (`extras`), the series' custom.ini (already filtered), then
// FloServer's own lines (files, port, 127.0.0.1) in a [mysqld] group of
// their own, so they win over anything before them. MySQL's also turn its
// X Protocol listener off (it would listen on every address, on 33060) and
// the binary log (a development server's disk would fill with it).
// MySQL's get secure-file-priv=NULL: no LOAD DATA INFILE or SELECT ... INTO
// OUTFILE reaches this computer's files (MariaDB has no such value). On Windows MySQL's basedir is
// written too (as ascii_path() gives it).
struct MariaDbExtras;  // mariadb_settings.h
[[nodiscard]] std::string mariadb_config(const MariaDbFiles& files, Port port, bool as_root, bool with_init_file,
                                         const MariaDbExtras* extras = nullptr);

// What mariadbd gets on its command line after --defaults-file, so no
// option file can change it: the port, 127.0.0.1 only, passwords needed,
// TCP on (named pipe and shared memory off on Windows), the socket, and for
// MySQL no X Protocol. The data
// folder stays in my.ini only (an argument would be read in the ANSI code
// page on Windows).
[[nodiscard]] std::vector<std::string> mariadb_command_line_guards(const MariaDbFiles& files, Port port);

// install-db finished: the system database exists, and the marker written
// only after install-db succeeded. A half-set-up folder isn't.
[[nodiscard]] bool mariadb_initialized(const MariaDbFiles& files);

// SQL text: an identifier in backticks, a string in single quotes.
[[nodiscard]] std::string sql_identifier(std::string_view name);
[[nodiscard]] std::string sql_string(std::string_view text);
// A database name for GRANT ... ON <it>.*, where '_' and '%' are wildcards:
// escaped, so the grant is for that one database.
[[nodiscard]] std::string grant_pattern(std::string_view database);

// root's password on every root account, and no anonymous logins. MySQL's
// set-up makes only root@localhost, and with skip-name-resolve a TCP login
// from 127.0.0.1 doesn't match it: root@127.0.0.1 and root@::1 are made
// too, with every right (as MariaDB's install-db makes them).
// The ALTER USER lines come first: a failing statement in MySQL's init file
// skips the ones after it, and root must never keep the empty password the
// set-up gave it.
[[nodiscard]] std::string root_password_sql(const std::string& password, std::string_view family = kMariaDbFamily);
// MySQL: how many accounts have no password (0 after the first start), as
// one number. FloServer checks it before it trusts the first start.
[[nodiscard]] std::string empty_password_count_sql();

// An option file for the servers' programs that holds only `password`
// in [client] (FloServer's tools get it this way: not in their arguments
// or environment). The value is quoted, '\' and '"' escaped.
[[nodiscard]] std::string client_password_option_file(const std::string& password);

// A project's database and its own user (@localhost and @127.0.0.1), all
// rights on that database only. Safe to run again: it sets the password
// the secure store holds.
// MySQL's keep its default login method (caching_sha2_password), and its
// collation (utf8mb4_0900_ai_ci).
[[nodiscard]] std::string project_database_sql(const std::string& database, const std::string& user,
                                               const std::string& password, std::string_view family = kMariaDbFamily);

// projects.remove's "drop_database" (approved 2 Nov): the project's
// database and its user, gone for good. The user is dropped on the hosts
// project_database_sql() makes it on (localhost, 127.0.0.1), and on ::1
// too in case one was made by hand there. Safe to run again.
[[nodiscard]] std::string drop_project_database_sql(const std::string& database, const std::string& user,
                                                    std::string_view family = kMariaDbFamily);

// Shared sites' login (D19): user "www", its database "www", and it may make
// and use any database whose name starts with "www_" (shared_database_sql).
// No project's database can have those names (reserved_database_name).
inline constexpr std::string_view kSharedDatabaseUser = "www";
inline constexpr std::string_view kSharedDatabasePrefix = "www_";

// The shared sites' user (@localhost and @127.0.0.1) and database, all
// rights on "www" and on every "www_..." database, which it may create.
// Safe to run again: it sets the password the secure store holds.
[[nodiscard]] std::string shared_database_sql(const std::string& password, std::string_view family = kMariaDbFamily);

// The database and user name for a project: its name with '-' as '_'
// ("my-shop" -> "my_shop"), so tools that don't quote names cope.
[[nodiscard]] std::string project_database_name(std::string_view project);
// The same for a registered project: the name it kept through a rename
// (D22), else the one its name gives.
[[nodiscard]] std::string project_database_name(const Project& project);

// A project's own user on a `family` server: its database's name, which
// MariaDB takes up to 80 characters. MySQL takes user names up to 32 (ERROR
// 1470), and a project's name may have 63: a longer one becomes its first 23
// characters, '_' and 8 hex digits of the SHA-256 of the whole name. It's
// made from the database's name, which is kept with the project through a
// rename (project_database_name()), so it never changes. Database names may
// have 64 characters on both.
inline constexpr std::size_t kMySqlMaxUserName = 32;
[[nodiscard]] std::string project_database_user(std::string_view database, std::string_view family);
// The name a project's database has on a `family` server. MySQL counts the
// '\' that escape a GRANT's '_' toward its 64-character limit (ERROR 1059),
// so there a name whose GRANT pattern (grant_pattern()) would be longer is
// cut the same way (its start, '_', 8 hex digits of its SHA-256) to fit:
// the same name every time. Elsewhere, and for shorter names, `database`.
// The secure store keeps its password under `database`, the name FloServer
// knows it by.
inline constexpr std::size_t kMySqlMaxIdentifier = 64;
[[nodiscard]] std::string server_database_name(std::string_view database, std::string_view family);

// Names a project's database and user can't have: MariaDB's and MySQL's own
// accounts and databases ("root" would take over the root account; mysql,
// sys, performance_schema, information_schema), and the shared
// sites' ("www", "www_..."), which their login may use.
[[nodiscard]] bool reserved_database_name(std::string_view name);

// Whether a project's database called `database` takes the project's new
// name when it's renamed (D22) instead of keeping its own: only MariaDB's
// own names (root, mysql...), which FloServer never makes for a project.
// Shared sites' (www, www_...) are reserved now but were made for projects
// from before shared sites, so they're kept like any other.
[[nodiscard]] bool database_follows_rename(std::string_view database);

// Which .env keys a project uses: FloCMS's (DB_TYPE, DB_NAME) or
// Laravel's (DB_CONNECTION, DB_DATABASE), from the project's .env or
// .env.example; FloCMS's when neither says.
enum class EnvStyle : std::uint8_t { FloCms, Laravel };
[[nodiscard]] EnvStyle env_style(const std::filesystem::path& project_dir);

struct DatabaseLogin {
    Port port = 0;
    std::string database;
    std::string user;
    std::string password;
};

// The .env lines for a project's database, in order.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> env_values(EnvStyle style, const DatabaseLogin& login);

// The database as one URL, for Python projects (D17): SQLAlchemy and most
// frameworks read DATABASE_URL. "mysql+pymysql://user:password@127.0.0.1:3306/db"
// (SQLAlchemy's form), or "mysql://..." for Django, whose dj-database-url
// knows only that one; the user and password percent-encoded.
[[nodiscard]] std::string database_url(const DatabaseLogin& login, bool django = false);

// A .env text with `values` set: a key's first line is replaced, later
// lines with the same key are dropped (FloCMS's two readers disagree on
// which wins), missing keys are added at the end. Every other line, and the
// file's line endings, stay as they were.
[[nodiscard]] std::string update_env_text(const std::string& text,
                                          const std::vector<std::pair<std::string, std::string>>& values);

}  // namespace floserver::core

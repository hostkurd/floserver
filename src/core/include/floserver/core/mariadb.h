#pragma once

// MariaDB pieces for the database service (D12), ported from FloCMS
// Studio's dbserver (the team's own code, D4) without Qt: where a series'
// files are, its config, the SQL FloServer runs, and the .env lines a
// project gets.
//
// One data folder per series ("11.4"): MariaDB can't go back a version on
// the same data, and patch releases of a series share it.
//   <data>/data/mariadb-<series>/   my.ini (my.cnf), installed (once set up), port,
//                                   data/ with mariadbd.err, mariadbd.pid,
//                                   mysqld.sock (not Windows), init.sql (briefly)
//
// Passwords are only ever given to MariaDB's programs in their environment
// (MYSQL_PWD) or on their input, never as an argument. root's first
// password is set by an init file on the first start, before the server
// takes any connection.

#include "floserver/core/ports.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

struct MariaDbFiles {
    std::string series;                  // "11.4"
    std::filesystem::path base_dir;      // the install (holds bin/)
    std::filesystem::path server;        // bin/mariadbd(.exe)
    std::filesystem::path install_db;    // mariadb-install-db(.exe): bin/ or scripts/
    std::filesystem::path admin;         // mariadb-admin(.exe)
    std::filesystem::path client;        // mariadb(.exe)
    std::filesystem::path folder;        // <data>/data/mariadb-<series>
    std::filesystem::path data_dir;      // folder/data
    std::filesystem::path config;        // folder/my.ini (Windows) or my.cnf
    std::filesystem::path error_log;     // data_dir/mariadbd.err
    std::filesystem::path pid_file;      // data_dir/mariadbd.pid
    std::filesystem::path init_file;     // data_dir/init.sql: root's password, first start only
    std::filesystem::path installed_marker;  // folder/installed: install-db finished
    std::filesystem::path socket;        // data_dir/mysqld.sock; empty on Windows
};

// "11.4.13" -> "11.4"; empty when it isn't a version.
[[nodiscard]] std::string mariadb_series(std::string_view version);

// The files for an install of `version` and FloServer's data folder.
// Programs are looked for in bin/, sbin/ (a system's mariadbd) and scripts/
// (the Linux bintar's mariadb-install-db).
[[nodiscard]] MariaDbFiles mariadb_files(const std::filesystem::path& install_dir, const std::string& version,
                                         const std::filesystem::path& data);

// The config: 127.0.0.1 only on `port` (the internal port the service
// relays to), utf8mb4, a small buffer pool (a development server). Paths
// are relative (mariadbd runs in the series' folder), except basedir
// outside Windows. `as_root`: the service runs as root (containers):
// user=root. `with_init_file`: the first start, which sets root's password.
[[nodiscard]] std::string mariadb_config(const MariaDbFiles& files, Port port, bool as_root, bool with_init_file);

// install-db finished: the system database exists, and the marker written
// only after install-db succeeded. A half-set-up folder isn't.
[[nodiscard]] bool mariadb_initialized(const MariaDbFiles& files);

// SQL text: an identifier in backticks, a string in single quotes.
[[nodiscard]] std::string sql_identifier(std::string_view name);
[[nodiscard]] std::string sql_string(std::string_view text);
// A database name for GRANT ... ON <it>.*, where '_' and '%' are wildcards:
// escaped, so the grant is for that one database.
[[nodiscard]] std::string grant_pattern(std::string_view database);

// root's password on every root account, and no anonymous logins.
[[nodiscard]] std::string root_password_sql(const std::string& password);

// A project's database and its own user (@localhost and @127.0.0.1), all
// rights on that database only. Safe to run again: it sets the password
// the secure store holds.
[[nodiscard]] std::string project_database_sql(const std::string& database, const std::string& user,
                                               const std::string& password);

// Shared sites' login (D19): user "www", its database "www", and it may make
// and use any database whose name starts with "www_" (shared_database_sql).
// No project's database can have those names (reserved_database_name).
inline constexpr std::string_view kSharedDatabaseUser = "www";
inline constexpr std::string_view kSharedDatabasePrefix = "www_";

// The shared sites' user (@localhost and @127.0.0.1) and database, all
// rights on "www" and on every "www_..." database, which it may create.
// Safe to run again: it sets the password the secure store holds.
[[nodiscard]] std::string shared_database_sql(const std::string& password);

// The database and user name for a project: its name with '-' as '_'
// ("my-shop" -> "my_shop"), so tools that don't quote names cope.
[[nodiscard]] std::string project_database_name(std::string_view project);

// Names a project's database and user can't have: MariaDB's own accounts
// and databases ("root" would take over the root account), and the shared
// sites' ("www", "www_..."), which their login may use.
[[nodiscard]] bool reserved_database_name(std::string_view name);

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

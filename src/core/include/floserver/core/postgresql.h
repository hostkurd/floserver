#pragma once

// PostgreSQL pieces for the database service (D29): where a series' files
// are, how its data folder is made and the server started, the SQL FloServer
// runs, and how FloServer's own clients log in. The service runs it like
// MariaDB and MySQL (database_service.h): one server per major version
// ("18"), a database and role per project, asleep when idle; its first port
// is 5432.
//
//   <data>/data/postgresql-<series>/   installed (once set up), port,
//                                      postgres.log (the server's output),
//                                      data/ (initdb's: PG_VERSION, postmaster.pid,
//                                             pg_hba.conf, written by FloServer)
//
// Passwords never go in an argument or an environment variable: initdb
// reads the superuser's from a file (--pwfile), the clients from a password
// file (PGPASSFILE), both owner-only and deleted after the run; SQL that
// holds one is read from an owner-only file too.

#include "floserver/core/mariadb.h"  // MariaDbFiles, DatabaseLogin
#include "floserver/core/ports.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

// PostgreSQL's superuser, FloServer's "root" on these servers.
inline constexpr std::string_view kPostgreSqlSuperuser = "postgres";
// The first public port a PostgreSQL server takes (5433, 5434... after it).
inline constexpr Port kFirstPostgreSqlPort = 5432;
// In the data folder: Settings > Database's values and custom.conf, written
// at each start; postgresql.conf includes it at its end.
inline constexpr std::string_view kPostgreSqlSettingsFile = "floserver.conf";

// The files for an install of `version` ("18.6") and FloServer's data
// folder: bin/postgres, initdb, pg_ctl, psql, pg_dump (.exe on Windows), in a
// MariaDbFiles (family "postgresql"): `server` postgres, `install_db`
// initdb, `admin` pg_ctl, `client` psql, `dump` pg_dump, `config` the
// pg_hba.conf FloServer writes, `error_log` postgres.log, `pid_file`
// data/postmaster.pid, `init_file` the superuser's password file for initdb.
[[nodiscard]] MariaDbFiles postgresql_files(const std::filesystem::path& install_dir, const std::string& version,
                                            const std::filesystem::path& data);

// initdb's arguments (run in the series' folder): the data folder "data",
// superuser postgres with the password in `password_file` (a plain name in
// that folder), scram-sha-256, UTF8, the C locale; from PostgreSQL 17 on the
// built-in C.UTF-8 provider, so sorting doesn't depend on Windows' language.
[[nodiscard]] std::vector<std::string> postgresql_initdb_args(int major, const std::string& password_file);

// What postgres gets on its command line, so no setting in a file can change
// it: the data folder, the internal `port` on 127.0.0.1 only, no Unix socket,
// no SSL (127.0.0.1 only), FloServer's pg_hba.conf, scram-sha-256 for new
// passwords, and no failed statement's text in the log (it may hold a
// password). shared_buffers and max_connections are in postgresql.conf
// (set_up), so a custom.conf can change them.
[[nodiscard]] std::vector<std::string> postgresql_server_args(const MariaDbFiles& files, Port port);

// pg_hba.conf: password logins (scram-sha-256) over TCP from 127.0.0.1 only;
// no Unix-socket ("local") logins at all.
[[nodiscard]] std::string postgresql_hba();

// The data folder is set up: initdb finished (PG_VERSION) and the marker
// written only after that.
[[nodiscard]] bool postgresql_initialized(const MariaDbFiles& files);

// SQL text: an identifier in double quotes, a string in single quotes
// (standard_conforming_strings: only ' is doubled).
[[nodiscard]] std::string pg_identifier(std::string_view name);
[[nodiscard]] std::string pg_string(std::string_view text);

// The superuser's password, on the running server.
[[nodiscard]] std::string pg_superuser_password_sql(const std::string& password);

// A project's database and its own role, the database's owner; only that
// role (and the superuser) may connect to it. Safe to run again: it sets the
// password the secure store holds. Run by psql (it uses \gexec: CREATE
// DATABASE can't run inside a DO block).
[[nodiscard]] std::string pg_project_database_sql(const std::string& database, const std::string& role,
                                                  const std::string& password);

// projects.remove's "drop_database": the database (its connections ended)
// and its role, gone for good. Safe to run again.
[[nodiscard]] std::string pg_drop_project_database_sql(const std::string& database, const std::string& role);

// Names a project's database and role can't have on PostgreSQL: its own
// (postgres, template0, template1, public) and any pg_ one.
[[nodiscard]] bool pg_reserved_name(std::string_view name);

// A libpq password file (PGPASSFILE) with one line: `user` on 127.0.0.1:`port`,
// any database, ':' and '\' escaped.
[[nodiscard]] std::string pg_password_file(Port port, const std::string& user, const std::string& password);

// The environment FloServer's psql and pg_dump run with: the password file,
// a 10-second connect limit, only scram-sha-256 accepted (a program that
// took the internal port can't ask for the password in clear), and no
// user's pg_service.conf or psqlrc.
// FloServer's own sessions as the superuser (`superuser`) also get
// PGOPTIONS = pg_quiet_options(), so no statement logging the user turned on
// (log_statement, log_min_duration_statement...) writes the passwords those
// sessions set into the log. A project's role couldn't set those.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> pg_client_env(const std::filesystem::path& password_file,
                                                                             const std::filesystem::path& dir,
                                                                             bool superuser = true);
// "log_statement=none", "log_min_duration_statement=-1"...: statement
// logging off for one session (single-user mode takes them as -c).
[[nodiscard]] const std::vector<std::string>& pg_quiet_settings();
// The same as a PGOPTIONS value: "-c log_statement=none -c ...".
[[nodiscard]] std::string pg_quiet_options();
// The libpq variables taken out of it: none of them may point the client
// elsewhere.
[[nodiscard]] const std::vector<std::string>& pg_client_env_removed();

// psql's arguments: 127.0.0.1:`port`, as `user`, database `database`, no
// psqlrc, no password prompt, stop at the first error, quiet.
[[nodiscard]] std::vector<std::string> psql_args(Port port, const std::string& user, const std::string& database);

}  // namespace floserver::core

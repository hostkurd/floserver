#include "floserver/core/postgresql.h"

#include "floserver/core/database_engine.h"
#include "floserver/core/paths.h"

#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;

namespace {

fs::path program(const fs::path& install_dir, std::string_view name) {
#ifdef _WIN32
    return install_dir / "bin" / path_from_utf8(std::string(name) + ".exe");
#else
    return install_dir / "bin" / path_from_utf8(std::string(name));
#endif
}

}  // namespace

MariaDbFiles postgresql_files(const fs::path& install_dir, const std::string& version, const fs::path& data) {
    MariaDbFiles f;
    f.family = std::string(kPostgreSqlFamily);
    f.series = database_series(kPostgreSqlFamily, version);
    f.base_dir = install_dir;
    f.server = program(install_dir, "postgres");
    f.install_db = program(install_dir, "initdb");
    f.admin = program(install_dir, "pg_ctl");
    f.client = program(install_dir, "psql");
    f.dump = program(install_dir, "pg_dump");
    f.folder = data / "data" / path_from_utf8(database_server_key(kPostgreSqlFamily, f.series));
    f.data_dir = f.folder / "data";
    f.config = f.data_dir / "pg_hba.conf";
    f.error_log = f.folder / "postgres.log";
    f.pid_file = f.data_dir / "postmaster.pid";
    f.init_file = f.folder / "pwfile";
    f.installed_marker = f.folder / "installed";
    return f;
}

std::vector<std::string> postgresql_initdb_args(int major, const std::string& password_file) {
    std::vector<std::string> args = {"-D", "data", "-U", std::string(kPostgreSqlSuperuser), "--pwfile=" + password_file,
                                     "--auth=scram-sha-256", "--encoding=UTF8", "--locale=C", "--no-instructions"};
    if (major >= 17) {
        args.emplace_back("--locale-provider=builtin");
        args.emplace_back("--builtin-locale=C.UTF-8");
    }
    return args;
}

std::vector<std::string> postgresql_server_args(const MariaDbFiles& files, Port port) {
    return {"-D",
            path_to_utf8(files.data_dir.filename()),
            "-c",
            "listen_addresses=127.0.0.1",
            "-c",
            "port=" + std::to_string(port),
            "-c",
            "unix_socket_directories=",
            "-c",
            "ssl=off",
            "-c",
            // "/" on Windows too (postgres takes it; one form in the tests).
            "hba_file=" + path_to_utf8(files.data_dir.filename()) + "/" + path_to_utf8(files.config.filename()),
            "-c",
            "password_encryption=scram-sha-256",
            // A failed statement (ALTER ROLE ... PASSWORD) isn't logged with its text.
            "-c",
            "log_min_error_statement=panic",
            // FloServer reads the server's messages, in English, from its own log.
            "-c",
            "lc_messages=C",
            "-c",
            "logging_collector=off",
            "-c",
            "log_destination=stderr"};
}

const std::vector<std::string>& pg_quiet_settings() {
    // The statements, the parse trees the debug_print_* settings write
    // (ALTER ROLE's password is in them too), and the text a crash or a
    // deadlock report would quote (track_activities).
    static const std::vector<std::string> settings = {
        "log_statement=none",          "log_min_duration_statement=-1", "log_min_duration_sample=-1",
        "log_transaction_sample_rate=0", "debug_print_parse=off",       "debug_print_rewritten=off",
        "debug_print_plan=off",        "track_activities=off"};
    return settings;
}

std::string pg_quiet_options() {
    std::string out;
    for (const std::string& s : pg_quiet_settings()) {
        out += (out.empty() ? "-c " : " -c ") + s;
    }
    return out;
}

std::string postgresql_hba() {
    return "# Written by FloServer at every start: password logins from 127.0.0.1 only.\n"
           "# TYPE  DATABASE  USER  ADDRESS       METHOD\n"
           "host    all       all   127.0.0.1/32  scram-sha-256\n";
}

bool postgresql_initialized(const MariaDbFiles& files) {
    std::error_code ec;
    return fs::is_regular_file(files.data_dir / "PG_VERSION", ec) && fs::is_regular_file(files.installed_marker, ec);
}

std::string pg_identifier(std::string_view name) {
    std::string out = "\"";
    for (const char c : name) {
        if (c == '"') {
            out += '"';
        }
        out += c;
    }
    return out + "\"";
}

std::string pg_string(std::string_view text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'') {
            out += '\'';
        }
        out += c;
    }
    return out + "'";
}

std::string pg_superuser_password_sql(const std::string& password) {
    return "ALTER ROLE " + pg_identifier(kPostgreSqlSuperuser) + " WITH PASSWORD " + pg_string(password) + ";\n";
}

std::string pg_project_database_sql(const std::string& database, const std::string& role, const std::string& password) {
    const std::string r = pg_identifier(role);
    const std::string d = pg_identifier(database);
    std::string sql;
    sql += "SELECT 'CREATE ROLE ' || quote_ident(" + pg_string(role) +
           ") WHERE NOT EXISTS (SELECT FROM pg_roles WHERE rolname = " + pg_string(role) + ")\\gexec\n";
    sql += "ALTER ROLE " + r + " WITH LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOREPLICATION NOBYPASSRLS PASSWORD " +
           pg_string(password) + ";\n";
    sql += "SELECT 'CREATE DATABASE ' || quote_ident(" + pg_string(database) + ") || ' OWNER ' || quote_ident(" +
           pg_string(role) + ") || ' ENCODING ''UTF8''' WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname = " +
           pg_string(database) + ")\\gexec\n";
    sql += "ALTER DATABASE " + d + " OWNER TO " + r + ";\n";
    sql += "REVOKE CONNECT, TEMPORARY ON DATABASE " + d + " FROM PUBLIC;\n";
    sql += "GRANT CONNECT, TEMPORARY ON DATABASE " + d + " TO " + r + ";\n";
    // The server's own databases are the superuser's only (every time: it's cheap).
    sql += "REVOKE CONNECT, TEMPORARY ON DATABASE postgres, template1 FROM PUBLIC;\n";
    return sql;
}

std::string pg_drop_project_database_sql(const std::string& database, const std::string& role) {
    return "DROP DATABASE IF EXISTS " + pg_identifier(database) + " WITH (FORCE);\n" + "DROP ROLE IF EXISTS " +
           pg_identifier(role) + ";\n";
}

bool pg_reserved_name(std::string_view name) {
    return name == "postgres" || name == "template0" || name == "template1" || name == "public" ||
           name.starts_with("pg_");
}

std::string pg_password_file(Port port, const std::string& user, const std::string& password) {
    const auto escape = [](std::string_view s) {
        std::string out;
        for (const char c : s) {
            if (c == ':' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        return out;
    };
    return "127.0.0.1:" + std::to_string(port) + ":*:" + escape(user) + ":" + escape(password) + "\n";
}

std::vector<std::pair<std::string, std::string>> pg_client_env(const fs::path& password_file, const fs::path& dir,
                                                               bool superuser) {
    std::vector<std::pair<std::string, std::string>> env = {{"PGPASSFILE", path_to_utf8(password_file)},
                                                            {"PGCONNECT_TIMEOUT", "10"},
                                                            {"PGREQUIREAUTH", "scram-sha-256"},
                                                            {"PGSYSCONFDIR", path_to_utf8(dir)},
                                                            {"PSQLRC", path_to_utf8(dir / "no-psqlrc")},
                                                            {"PGCLIENTENCODING", "UTF8"}};
    if (superuser) {
        env.emplace_back("PGOPTIONS", pg_quiet_options());
    }
    return env;
}

const std::vector<std::string>& pg_client_env_removed() {
    static const std::vector<std::string> names = {
        "PGPASSWORD", "PGHOST",         "PGHOSTADDR",  "PGPORT",       "PGUSER",       "PGDATABASE",    "PGSERVICE",
        "PGSERVICEFILE", "PGOPTIONS",   "PGSSLMODE",   "PGTARGETSESSIONATTRS", "PGLOADBALANCEHOSTS", "PGCHANNELBINDING",
        "PGGSSENCMODE", "PGKRBSRVNAME", "PGGSSLIB",    "PGAPPNAME",    "PGTZ",         "PGDATESTYLE"};
    return names;
}

std::vector<std::string> psql_args(Port port, const std::string& user, const std::string& database) {
    return {"--host=127.0.0.1", "--port=" + std::to_string(port), "--username=" + user, "--dbname=" + database,
            "--no-psqlrc",      "--no-password",                  "--set=ON_ERROR_STOP=1", "--quiet"};
}

std::vector<std::string> pg_dump_args(const std::string& database, const std::string& file) {
    return {"--dbname=" + database, "--format=plain", "--no-owner", "--no-privileges", "--encoding=UTF8",
            "--file=" + file};
}

std::vector<std::string> pg_load_args(const std::string& database, const std::string& file) {
    return {"--dbname=" + database, "--no-psqlrc",          "--set=ON_ERROR_STOP=1",
            "--quiet",              "--single-transaction", "--file=" + file};
}

std::string pg_empty_database_sql(const std::string& database) {
    return "DROP DATABASE IF EXISTS " + pg_identifier(database) + " WITH (FORCE);\n";
}

}  // namespace floserver::core

// PostgreSQL in the database service (D29): its pieces, and a real
// PostgreSQL (the distribution's, in /usr/lib/postgresql/<major>/bin) run by
// the service when one is installed and the tests don't run as root
// (postgres refuses root).

#include "floserver/core/database_engine.h"
#include "floserver/core/database_service.h"
#include "floserver/core/mariadb.h"
#include "floserver/core/mariadb_settings.h"
#include "floserver/core/paths.h"
#include "floserver/core/postgresql.h"
#include "floserver/core/process.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/secret_store.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace floserver::core;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

bool has(const std::vector<std::string>& args, const std::string& one) {
    return std::find(args.begin(), args.end(), one) != args.end();
}

}  // namespace

TEST_CASE("PostgreSQL's files, set-up and server arguments (D29)") {
    const fs::path data = "/d";
    const MariaDbFiles f = postgresql_files("/r/postgresql/18.6", "18.6", data);
    CHECK(f.family == "postgresql");
    CHECK(f.series == "18");
    CHECK(f.folder == data / "data" / "postgresql-18");
    CHECK(f.data_dir == f.folder / "data");
    CHECK(f.config == f.data_dir / "pg_hba.conf");
    CHECK(f.pid_file == f.data_dir / "postmaster.pid");
    CHECK(f.server.parent_path() == fs::path("/r/postgresql/18.6/bin"));
    CHECK(f.server.stem() == "postgres");
    CHECK(f.install_db.stem() == "initdb");
    CHECK(f.admin.stem() == "pg_ctl");
    CHECK(f.client.stem() == "psql");
    CHECK(f.dump.stem() == "pg_dump");

    const auto a18 = postgresql_initdb_args(18, "pwfile");
    CHECK(has(a18, "--pwfile=pwfile"));
    CHECK(has(a18, "--auth=scram-sha-256"));
    CHECK(has(a18, "--encoding=UTF8"));
    CHECK(has(a18, "--locale-provider=builtin"));
    CHECK(has(a18, "--builtin-locale=C.UTF-8"));
    const auto a16 = postgresql_initdb_args(16, "pwfile");
    CHECK_FALSE(has(a16, "--locale-provider=builtin"));  // 17 and later only
    CHECK(has(a16, "--locale=C"));
    for (const auto& a : a18) {
        CHECK(a.find("secret") == std::string::npos);
    }

    const auto s = postgresql_server_args(f, 33105);
    CHECK(has(s, "listen_addresses=127.0.0.1"));
    CHECK(has(s, "port=33105"));
    CHECK(has(s, "unix_socket_directories="));
    CHECK(has(s, "hba_file=data/pg_hba.conf"));
    CHECK(has(s, "data"));

    const std::string hba = postgresql_hba();
    CHECK(hba.find("host    all       all   127.0.0.1/32  scram-sha-256") != std::string::npos);
    CHECK(hba.find("\nlocal") == std::string::npos);
    CHECK(hba.find("trust") == std::string::npos);
}

TEST_CASE("PostgreSQL on Windows: ASCII paths keep each file in its own folder (D29)") {
    // A shortened path keeps its last name (as Windows' 8.3 names of ASCII names do).
    const ShortPathFunction shorten = [](const fs::path& p) -> fs::path {
        return fs::path("C:/SHORT~1") / p.parent_path().filename() / p.filename();
    };
    MariaDbFiles f = postgresql_files(path_from_utf8("C:/Users/\xD9\x86/rt/postgresql/18.1"), "18.1",
                                      path_from_utf8("C:/Users/\xD9\x86/data"));
    fs::path bad;
    bool missing = false;
    REQUIRE(ascii_mysql_files(f, shorten, bad, missing));
    // initdb's data folder must stay empty: the password file is beside it, the
    // hba file inside it (where hba_file=data/pg_hba.conf points), the log beside it.
    CHECK(f.init_file.parent_path() == f.folder);
    CHECK(f.config.parent_path() == f.data_dir);
    CHECK(f.error_log.parent_path() == f.folder);
    CHECK(f.pid_file.parent_path() == f.data_dir);
    CHECK(f.installed_marker.parent_path() == f.folder);
    CHECK(f.data_dir.filename() == "data");
    CHECK(f.config.filename() == "pg_hba.conf");
    CHECK(f.init_file.filename() == "pwfile");
}

TEST_CASE("PostgreSQL's settings: names it refuses, custom.conf, floserver.conf (D29)") {
    const std::string_view pg = "postgresql";
    CHECK(mariadb_request_series("18", pg) == "18");
    CHECK_FALSE(mariadb_request_series("18.1", pg));
    CHECK_FALSE(mariadb_request_series("18"));  // MariaDB's are two numbers
    CHECK_FALSE(mariadb_option_problem("work_mem", pg));
    CHECK_FALSE(mariadb_option_problem("TimeZone", pg));
    CHECK_FALSE(mariadb_option_problem("auto_explain.log_min_duration", pg));
    for (const char* refused : {"listen_addresses", "port", "unix_socket_directories", "ssl", "ssl_cert_file",
                                "hba_file", "data_directory", "password_encryption", "lc_messages",
                                "logging_collector", "log_min_error_statement", "Listen_Addresses"}) {
        CAPTURE(refused);
        CHECK(mariadb_option_problem(refused, pg));
    }
    CHECK(mariadb_option_problem("work-mem", pg));  // not a PostgreSQL name
    CHECK(mariadb_option_problem("1abc", pg));
    CHECK(mariadb_ini_defaults(pg).size() == 2);
    CHECK(mariadb_custom_ini_file("D", "18", pg).filename() == "custom.conf");

    const FilteredCustomIni f = filter_mariadb_custom_ini(
        "\xEF\xBB\xBF# mine\n"
        "work_mem = 16MB\n"
        "TimeZone='Asia/Baghdad'   # here\n"
        "search_path 'a, b'\n"
        "listen_addresses = '*'\n"
        "include 'other.conf'\n"
        "application_name = 'it''s' # quoted quote\n"
        "bad value here\n"
        "statement_timeout = 'unclosed\n",
        pg);
    CHECK(f.text ==
          "work_mem = '16MB'\n"
          "timezone = 'Asia/Baghdad'\n"
          "search_path = 'a, b'\n"
          "# FloServer left out line 5\n"
          "# FloServer left out line 6\n"
          "application_name = 'it''s'\n"
          "# FloServer left out line 8\n"
          "# FloServer left out line 9\n");
    REQUIRE(f.problems.size() == 4);
    CHECK(f.problems[0].starts_with("line 5: listen_addresses can't be set"));
    CHECK(f.problems[1].starts_with("line 6: include can't be used"));
    CHECK(f.keys == std::vector<std::string>{"application_name", "search_path", "timezone", "work_mem"});

    MariaDbExtras extras;
    extras.ini = {{"shared_buffers", "128MB"}, {"timezone", "it's"}};
    extras.custom_ini = "work_mem = '16MB'\n";
    extras.custom_ini_path = "C:/data/postgresql/18/custom.conf";
    const std::string conf = postgresql_settings_conf(extras);
    CHECK(conf.find("max_connections = '100'\nshared_buffers = '128MB'\ntimezone = 'it''s'\n") != std::string::npos);
    CHECK(conf.find("# C:/data/postgresql/18/custom.conf\nwork_mem = '16MB'\n") != std::string::npos);

    CHECK(parse_postgres_describe_config("work_mem\tuser\tResource Usage\tINTEGER\n"
                                         "TimeZone\tuser\tClient\tSTRING\n"
                                         "not a line\n") == std::vector<std::string>{"timezone", "work_mem"});
}

TEST_CASE("PostgreSQL's SQL, password file and client environment (D29)") {
    CHECK(pg_identifier("my_shop") == "\"my_shop\"");
    CHECK(pg_identifier("a\"b") == "\"a\"\"b\"");
    CHECK(pg_string("it's") == "'it''s'");
    CHECK(pg_string("a\\b") == "'a\\b'");  // standard_conforming_strings: \ is plain
    CHECK(pg_superuser_password_sql("p'w") == "ALTER ROLE \"postgres\" WITH PASSWORD 'p''w';\n");
    const std::string sql = pg_project_database_sql("my_shop", "my_shop", "pw");
    CHECK(sql.find("NOSUPERUSER NOCREATEDB NOCREATEROLE") != std::string::npos);
    CHECK(sql.find("\\gexec") != std::string::npos);
    CHECK(sql.find("REVOKE CONNECT, TEMPORARY ON DATABASE \"my_shop\" FROM PUBLIC;") != std::string::npos);
    CHECK(pg_drop_project_database_sql("my_shop", "my_shop") ==
          "DROP DATABASE IF EXISTS \"my_shop\" WITH (FORCE);\nDROP ROLE IF EXISTS \"my_shop\";\n");
    for (const char* name : {"postgres", "template0", "template1", "public", "pg_x"}) {
        CAPTURE(name);
        CHECK(pg_reserved_name(name));
    }
    CHECK_FALSE(pg_reserved_name("my_shop"));
    CHECK(pg_password_file(33100, "u", "a:b\\c") == "127.0.0.1:33100:*:u:a\\:b\\\\c\n");
    const auto env = pg_client_env("/f/x.pgpass", "/f");
    const auto value = [&env](const std::string& k) {
        for (const auto& [key, v] : env) {
            if (key == k) {
                return v;
            }
        }
        return std::string();
    };
    CHECK(value("PGREQUIREAUTH") == "scram-sha-256");
    CHECK(value("PGPASSFILE").ends_with("x.pgpass"));
    CHECK(std::find(pg_client_env_removed().begin(), pg_client_env_removed().end(), "PGPASSWORD") !=
          pg_client_env_removed().end());
}

TEST_CASE("PostgreSQL's .env lines and DATABASE_URL (D29)") {
    DatabaseLogin login{5432, "my_shop", "my_shop", "p@ss:w"};
    login.family = "postgresql";
    const auto laravel = env_values(EnvStyle::Laravel, login);
    CHECK(laravel.front() == std::pair<std::string, std::string>{"DB_CONNECTION", "pgsql"});
    CHECK(std::find(laravel.begin(), laravel.end(), std::pair<std::string, std::string>{"DB_PORT", "5432"}) !=
          laravel.end());
    CHECK(database_url(login) == "postgresql://my_shop:p%40ss%3Aw@127.0.0.1:5432/my_shop");
    CHECK(database_url(login, true) == "postgresql://my_shop:p%40ss%3Aw@127.0.0.1:5432/my_shop");
    const DatabaseLogin mysql{3306, "a", "a", "b"};
    CHECK(env_values(EnvStyle::Laravel, mysql).front().second == "mysql");
    CHECK(database_url(mysql).starts_with("mysql+pymysql://"));
}

#ifndef _WIN32

namespace {

// The distribution's PostgreSQL: its bin folder, the newest major there.
std::optional<std::pair<fs::path, std::string>> system_postgresql() {
    std::error_code ec;
    std::optional<std::pair<fs::path, std::string>> best;
    int best_major = 0;
    for (fs::directory_iterator it("/usr/lib/postgresql", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = path_to_utf8(it->path().filename());
        const int major = std::atoi(name.c_str());
        if (major > best_major && fs::is_regular_file(it->path() / "bin" / "postgres", ec)) {
            best_major = major;
            best = {it->path() / "bin", name};
        }
    }
    return best;
}

// psql's output for `sql` as `user` on 127.0.0.1:port, or "ERROR ...".
std::string pg_query(const fs::path& bin, Port port, const std::string& user, const std::string& password,
                     const std::string& database, const std::string& sql) {
    const std::string cmd = "PGPASSWORD='" + password + "' PGCONNECT_TIMEOUT=10 '" + path_to_utf8(bin / "psql") +
                            "' -X -q -A -t -w -h 127.0.0.1 -p " + std::to_string(port) + " -U " + user + " -d " + database +
                            " -v ON_ERROR_STOP=1 -c \"" + sql + "\" 2>&1";
    FILE* p = popen(cmd.c_str(), "r");  // NOLINT(cert-env33-c): a test, fixed text
    if (p == nullptr) {
        return "ERROR popen";
    }
    std::string out;
    char buf[512];  // NOLINT(modernize-avoid-c-arrays)
    while (fgets(buf, sizeof buf, p) != nullptr) {
        out += buf;
    }
    const int rc = pclose(p);
    while (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return rc == 0 ? out : "ERROR " + out;
}

nlohmann::json pg_server(const DatabaseService& db) {
    const nlohmann::json status = db.status();
    for (const auto& s : status["servers"]) {
        if (s["family"] == "postgresql") {
            return s;
        }
    }
    return nullptr;
}

template <class F>
bool eventually(F&& f, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (f()) {
            return true;
        }
        std::this_thread::sleep_for(100ms);
    }
    return f();
}

}  // namespace

TEST_CASE("a project's PostgreSQL database: made, logged in to, asleep when idle, its password put right, dropped") {
    const auto pg = system_postgresql();
    if (!pg) {
        REQUIRE_MESSAGE(!get_env("CI"), "PostgreSQL is missing from this CI machine");
        MESSAGE("PostgreSQL isn't installed here; skipped");
        return;
    }
    if (geteuid() == 0) {
        MESSAGE("postgres refuses to run as root; skipped (run the tests as another user)");
        return;
    }
    const auto& [bin, major] = *pg;
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    const std::string version = major + ".0";
    const fs::path r = runtimes_dir(data) / "postgresql" / version;
    fs::create_directories(r / "bin");
    test_util::write_file(r / ".floserver-runtime.json",
                          nlohmann::json{{"schema", 1}, {"name", "postgresql"}, {"version", version},
                                         {"platform", std::string(current_platform())}, {"variant", ""},
                                         {"license", "PostgreSQL"}, {"sha256", std::string(64, 'a')}}
                              .dump());
    std::error_code ec;
    for (const char* t : {"postgres", "initdb", "pg_ctl", "psql", "pg_dump"}) {
        fs::create_symlink(bin / t, r / "bin" / t, ec);
    }

    const fs::path shop = tmp.path() / "my-shop";
    test_util::write_file(shop / "app.py", "print(1)");
    test_util::write_file(shop / "floserver.json", R"({"python": "3.12", "database": "postgresql-)" + major + "\"}");
    const fs::path other = tmp.path() / "other";
    test_util::write_file(other / "floserver.json", R"({"database": "postgresql-)" + major + "\"}");
    const fs::path reserved = tmp.path() / "pg-x";
    test_util::write_file(reserved / "floserver.json", R"({"database": "postgresql-)" + major + "\"}");
    AppConfig app;
    std::string error;
    REQUIRE(add_project(app, shop, "", error));
    REQUIRE(add_project(app, other, "", error));
    REQUIRE(add_project(app, reserved, "", error));
    // Settings > Database's values and custom.conf reach the server; what
    // FloServer sets itself (the port) doesn't move.
    MariaDbSeriesSettings settings;
    settings.ini["work_mem"] = "8MB";
    write_mariadb_series(app, major, settings, "postgresql");
    REQUIRE(save_app_config(app_config_file(data), app, error));
    REQUIRE(ensure_mariadb_custom_ini(data, major, error, "postgresql"));
    test_util::write_file(mariadb_custom_ini_file(data, major, "postgresql"),
                          "statement_timeout = '5s'  # a comment\nport = 1\napplication_name = 'it''s mine'\n");

    const SecretStore secrets(data);
    DatabaseServiceConfig cfg;
    cfg.data_dir = data;
    cfg.idle_after = 3s;
    cfg.reload_every = 1s;
    // Never a port this machine's own PostgreSQL might use.
    cfg.probe = [](Port p) { return p != 5432 && can_listen_on_loopback(p); };
    DatabaseService db(cfg, secrets);
    db.start();

    auto s = pg_server(db);
    REQUIRE_MESSAGE(s.is_object(), db.status().dump());
    CHECK(s["series"] == major);
    CHECK(s["state"] == "sleeping");
    const Port port = s["port"].get<Port>();
    CHECK(port > 5432);
    CHECK(port < 5532);  // PostgreSQL's ports, not MariaDB's 3306...

    ServiceRefusal refusal;
    const auto login = db.project_login("my-shop", refusal);
    REQUIRE_MESSAGE(login, refusal.message);
    CHECK(login->family == "postgresql");
    CHECK(login->database == "my_shop");
    CHECK(login->user == "my_shop");
    CHECK(login->port == port);
    CHECK(pg_server(db)["state"] == "running");
    const fs::path folder = data / "data" / ("postgresql-" + major);
    CHECK(fs::exists(folder / "installed"));
    CHECK_FALSE(fs::exists(folder / "pwfile"));  // initdb's password file is gone

    // Through the service's port, as the project's own role.
    CHECK(pg_query(bin, port, "my_shop", login->password, "my_shop",
                   "CREATE TABLE t (x int); INSERT INTO t VALUES (7); SELECT x FROM t") == "7");
    CHECK(pg_query(bin, port, "my_shop", "wrong", "my_shop", "SELECT 1").starts_with("ERROR"));
    CHECK(pg_query(bin, port, "my_shop", login->password, "my_shop", "CREATE DATABASE x").starts_with("ERROR"));
    CHECK(pg_query(bin, port, "my_shop", login->password, "my_shop", "SHOW server_encoding") == "UTF8");
    CHECK(pg_query(bin, port, "my_shop", login->password, "my_shop", "SHOW work_mem") == "8MB");
    CHECK(pg_query(bin, port, "my_shop", login->password, "my_shop", "SHOW shared_buffers") == "64MB");
    CHECK(pg_query(bin, port, "my_shop", login->password, "my_shop", "SHOW statement_timeout") == "5s");
    CHECK(pg_query(bin, port, "my_shop", login->password, "my_shop", "SHOW application_name") != "1");
    CHECK(pg_query(bin, port, "my_shop", login->password, "my_shop", "SHOW port") != "1");
    {
        // The final settings name custom.conf's refused line, and FloServer's own.
        const auto final_file = db.final_ini(major, refusal, "postgresql");
        REQUIRE_MESSAGE(final_file, refusal.message);
        const std::string text = test_util::read_file(*final_file);
        CHECK(text.find("work_mem = '8MB'") != std::string::npos);
        CHECK(text.find("statement_timeout = '5s'") != std::string::npos);
        CHECK(text.find("# FloServer left out line 2") != std::string::npos);
        CHECK(text.find("#   listen_addresses=127.0.0.1") != std::string::npos);
        // The names this postgres knows, for "did you mean".
        bool timed_out = false;
        const auto known = known_mariadb_options(bin / "postgres", tmp.path() / "known-options-pg.json", error,
                                                 timed_out);
        REQUIRE_MESSAGE(known, error);
        CHECK(std::binary_search(known->begin(), known->end(), "work_mem"));
        CHECK(std::binary_search(known->begin(), known->end(), "timezone"));
    }
    // Asking again: the same password, and the database is kept.
    const auto again = db.project_login("my-shop", refusal);
    REQUIRE(again);
    CHECK(again->password == login->password);
    CHECK(pg_query(bin, port, "my_shop", login->password, "my_shop", "SELECT count(*) FROM t") == "1");
    // Another project can't reach it.
    const auto other_login = db.project_login("other", refusal);
    REQUIRE_MESSAGE(other_login, refusal.message);
    CHECK(pg_query(bin, port, "other", other_login->password, "my_shop", "SELECT 1").starts_with("ERROR"));
    // A name of PostgreSQL's own is refused.
    CHECK_FALSE(db.project_login("pg-x", refusal));
    CHECK(refusal.code == "reserved_name");
    // The superuser's password works; no login without one.
    const auto root = secrets.read("postgresql-" + major + "/root");
    REQUIRE(root);
    CHECK(pg_query(bin, port, "postgres", *root, "postgres", "SELECT 1") == "1");
    CHECK(pg_query(bin, port, "postgres", "", "postgres", "SELECT 1").starts_with("ERROR"));

    // Idle: it sleeps, and the next connection wakes it.
    CHECK(eventually([&] { return pg_server(db)["state"] == "sleeping"; }, 20s));
    CHECK(pg_query(bin, port, "my_shop", login->password, "my_shop", "SELECT x FROM t") == "7");

    // The superuser's saved password lost: a new one is set (single-user
    // mode) at the next start, and the projects' logins still work.
    REQUIRE(db.server_action(major, "stop", refusal, "postgresql"));
    CHECK(eventually([&] { return pg_server(db)["state"] == "sleeping"; }, 20s));
    REQUIRE(secrets.remove("postgresql-" + major + "/root"));
    const auto after = db.project_login("my-shop", refusal, true);
    REQUIRE_MESSAGE(after, refusal.message);
    const auto new_root = secrets.read("postgresql-" + major + "/root");
    REQUIRE(new_root);
    CHECK(*new_root != *root);
    CHECK(pg_query(bin, port, "postgres", *new_root, "postgres", "SELECT 1") == "1");
    CHECK(pg_query(bin, port, "postgres", *root, "postgres", "SELECT 1").starts_with("ERROR"));

    // Backups come later: refused plainly, not half done.
    const std::atomic<bool> cancel{false};
    CHECK_FALSE(db.dump_project_database("my-shop", tmp.path(), cancel, refusal));
    CHECK(refusal.code == "not_supported");

    // Dropped with the project.
    bool dropped = false;
    REQUIRE_MESSAGE(db.drop_project_database("my-shop", refusal, dropped), refusal.message);
    CHECK(dropped);
    CHECK(pg_query(bin, port, "postgres", *new_root, "postgres",
                   "SELECT count(*) FROM pg_database WHERE datname = 'my_shop'") == "0");
    CHECK(pg_query(bin, port, "postgres", *new_root, "postgres",
                   "SELECT count(*) FROM pg_roles WHERE rolname = 'my_shop'") == "0");
    db.stop();
}

#endif

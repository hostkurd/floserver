// MySQL 8.4 beside MariaDB (D28): the family, its files, my.ini, the SQL
// FloServer runs, its settings' place, phpMyAdmin's servers, the data
// folder's clean-up and the signed list's catalog. The real server is
// tested in test_database_service.cpp (FLOSERVER_TEST_MYSQL).

#include "floserver/core/backup.h"
#include "floserver/core/data_folder.h"
#include "floserver/core/database_engine.h"
#include "floserver/core/database_service.h"
#include "floserver/core/mariadb.h"
#include "floserver/core/mariadb_settings.h"
#include "floserver/core/paths.h"
#include "floserver/core/phpmyadmin.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/secret_store.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>

using namespace floserver::core;
namespace fs = std::filesystem;

TEST_CASE("database families: MariaDB and MySQL, named by family and series") {
    CHECK(known_database_family("mariadb"));
    CHECK(known_database_family("mysql"));
    CHECK_FALSE(known_database_family("postgresql"));  // D29
    CHECK_FALSE(known_database_family("MySQL"));
    CHECK(database_families() == std::vector<std::string>{"mariadb", "mysql"});
    CHECK(mysql_protocol_family("mysql"));
    CHECK(mysql_protocol_family("mariadb"));
    CHECK(database_family_title("mysql") == "MySQL");
    CHECK(database_family_title("mariadb") == "MariaDB");
    CHECK(database_server_key("mysql", "8.4") == "mysql-8.4");
    std::string family;
    CHECK(request_database_family("", family));
    CHECK(family == "mariadb");  // requests from before D28
    CHECK(request_database_family("mysql", family));
    CHECK(family == "mysql");
    CHECK_FALSE(request_database_family("oracle", family));
}

TEST_CASE("MySQL's files: mysqld sets itself up, its own folder and logs") {
    const test_util::TempDir tmp;
    const MariaDbFiles f = mariadb_files(tmp.path() / "inst", "8.4.11", tmp.path() / "data", kMySqlFamily);
    CHECK(f.family == "mysql");
    CHECK(f.series == "8.4");
    CHECK(f.folder == tmp.path() / "data" / "data" / "mysql-8.4");
    CHECK(f.data_dir == f.folder / "data");
    CHECK(f.server.filename().string().starts_with("mysqld"));
    CHECK(f.install_db == f.server);
    CHECK(f.admin.filename().string().starts_with("mysqladmin"));
    CHECK(f.client.stem() == "mysql");
    CHECK(f.dump.stem() == "mysqldump");
    CHECK(f.error_log.filename() == "mysqld.err");
    CHECK(f.pid_file.filename() == "mysqld.pid");
    // MariaDB's are as before.
    const MariaDbFiles m = mariadb_files(tmp.path() / "inst", "11.4.13", tmp.path() / "data");
    CHECK(m.family == "mariadb");
    CHECK(m.folder == tmp.path() / "data" / "data" / "mariadb-11.4");
    CHECK(m.error_log.filename() == "mariadbd.err");
}

TEST_CASE("MySQL's my.ini: no X Protocol, no binary log, 127.0.0.1 only, its own collation") {
    const test_util::TempDir tmp;
    const MariaDbFiles f = mariadb_files(tmp.path() / "inst", "8.4.11", tmp.path() / "data", kMySqlFamily);
    const std::string c = mariadb_config(f, 33101, false, true);
    CHECK(c.find("MySQL 8.4") != std::string::npos);
    CHECK(c.find("port=33101") != std::string::npos);
    CHECK(c.find("bind-address=127.0.0.1") != std::string::npos);
    CHECK(c.find("loose-mysqlx=OFF") != std::string::npos);
    CHECK(c.find("skip-log-bin") != std::string::npos);
    CHECK(c.find("collation_server=utf8mb4_0900_ai_ci") != std::string::npos);
    CHECK(c.find("pid-file=mysqld.pid") != std::string::npos);
    CHECK(c.find("log-error=mysqld.err") != std::string::npos);
    CHECK(c.find("init-file=init.sql") != std::string::npos);
    CHECK(c.find("skip-grant-tables=0") != std::string::npos);
#ifdef _WIN32
    // Windows-only options in their loose- form, as for MariaDB.
    CHECK(c.find("loose-named-pipe=0") != std::string::npos);
    CHECK(c.find("loose-shared-memory=0") != std::string::npos);
#endif
    // MySQL's basedir is written everywhere (on Windows mysqld.exe would take it from
    // its own path, read in the ANSI code page), as the path it's given.
    const std::u8string base = f.base_dir.generic_u8string();
    CHECK(c.find("basedir=\"" + std::string(base.begin(), base.end()) + "\"") != std::string::npos);
    // MariaDB's has neither of MySQL's lines.
    const MariaDbFiles m = mariadb_files(tmp.path() / "inst", "11.4.13", tmp.path() / "data");
    const std::string mc = mariadb_config(m, 33100, false, false);
#ifdef _WIN32
    CHECK(mc.find("basedir") == std::string::npos);  // mariadbd.exe finds its own folder (UTF-8)
#else
    CHECK(mc.find("basedir=") != std::string::npos);
#endif
    CHECK(mc.find("mysqlx") == std::string::npos);
    CHECK(mc.find("skip-log-bin") == std::string::npos);
    CHECK(mc.find("collation_server=utf8mb4_unicode_ci") != std::string::npos);

    const auto args = mariadb_command_line_guards(f, 33101);
    CHECK(std::find(args.begin(), args.end(), "--loose-mysqlx=OFF") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--bind-address=127.0.0.1") != args.end());
    const auto margs = mariadb_command_line_guards(m, 33100);
    CHECK(std::find(margs.begin(), margs.end(), "--loose-mysqlx=OFF") == margs.end());
}

TEST_CASE("MySQL's SQL: root on 127.0.0.1 and ::1, no mysql.global_priv, caching_sha2 kept") {
    const std::string root = root_password_sql("Pw'1\\x", kMySqlFamily);
    CHECK(root.find("CREATE USER IF NOT EXISTS 'root'@'127.0.0.1' IDENTIFIED BY 'Pw''1\\\\x';") != std::string::npos);
    CHECK(root.find("CREATE USER IF NOT EXISTS 'root'@'::1'") != std::string::npos);
    CHECK(root.find("GRANT ALL PRIVILEGES ON *.* TO 'root'@'127.0.0.1' WITH GRANT OPTION;") != std::string::npos);
    CHECK(root.find("ALTER USER IF EXISTS 'root'@'localhost' IDENTIFIED BY") != std::string::npos);
    CHECK(root.find("global_priv") == std::string::npos);
    CHECK(root.find("FLUSH") == std::string::npos);
    CHECK(root.find("DROP USER IF EXISTS ''@'localhost'") != std::string::npos);
    // MariaDB's is as before.
    CHECK(root_password_sql("x").find("DELETE FROM mysql.global_priv") != std::string::npos);

    const std::string project = project_database_sql("my_shop", "my_shop", "pw", kMySqlFamily);
    CHECK(project.find("COLLATE utf8mb4_0900_ai_ci") != std::string::npos);
    CHECK(project.find("GRANT ALL PRIVILEGES ON `my\\_shop`.* TO 'my_shop'@'127.0.0.1';") != std::string::npos);
    CHECK(project.find("mysql_native_password") == std::string::npos);  // never a fall-back
    CHECK(project.find("IDENTIFIED WITH") == std::string::npos);        // MySQL's default login method
    CHECK(project.find("DELETE FROM mysql.db") == std::string::npos);
    CHECK(project.find("FLUSH") == std::string::npos);
    CHECK(project_database_sql("my_shop", "my_shop", "pw").find("DELETE FROM mysql.db") != std::string::npos);

    const std::string drop = drop_project_database_sql("my_shop", "my_shop", kMySqlFamily);
    CHECK(drop.find("DROP DATABASE IF EXISTS `my_shop`;") != std::string::npos);
    CHECK(drop.find("DROP USER IF EXISTS 'my_shop'@'::1';") != std::string::npos);
    CHECK(drop.find("FLUSH") == std::string::npos);

    const std::string shared = shared_database_sql("pw", kMySqlFamily);
    CHECK(shared.find("GRANT ALL PRIVILEGES ON `www\\_%`.* TO 'www'@'localhost';") != std::string::npos);
    CHECK(shared.find("utf8mb4_0900_ai_ci") != std::string::npos);

    // MySQL's own names are reserved too.
    for (const char* name : {"sys", "performance_schema", "mysql", "information_schema", "root"}) {
        CAPTURE(name);
        CHECK(reserved_database_name(name));
    }
    CHECK(reserved_database_name(project_database_name("performance-schema")));
}

TEST_CASE("MySQL's dump leaves tablespaces and GTIDs out; MariaDB's is as before") {
    const auto args = mariadb_dump_args("shop", "database.sql", kMySqlFamily);
    CHECK(std::find(args.begin(), args.end(), "--no-tablespaces") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--set-gtid-purged=OFF") != args.end());
    CHECK(args.back() == "shop");
    CHECK(args[args.size() - 2] == "--result-file=database.sql");
    const auto maria = mariadb_dump_args("shop", "database.sql");
    CHECK(std::find(maria.begin(), maria.end(), "--no-tablespaces") == maria.end());
}

TEST_CASE("Settings > Database for MySQL: config.json's \"mysql\", its own custom.ini, X Protocol refused") {
    AppConfig app;
    MariaDbSeriesSettings s;
    s.port = 3307;
    s.keep_running = true;
    s.ini["max_connections"] = "300";
    write_mariadb_series(app, "8.4", s, kMySqlFamily);
    CHECK(app.other.contains("mysql"));
    CHECK_FALSE(app.other.contains("mariadb"));
    const MariaDbSettings read = read_mariadb_settings(app, nullptr, kMySqlFamily);
    REQUIRE(read.contains("8.4"));
    CHECK(read.at("8.4") == s);
    CHECK(read_mariadb_settings(app).empty());  // MariaDB's are its own
    write_mariadb_series(app, "8.4", MariaDbSeriesSettings{}, kMySqlFamily);
    CHECK_FALSE(app.other.contains("mysql"));

    std::vector<std::string> problems;
    app.other["mysql"] = {{"eight", {}}};
    (void)read_mariadb_settings(app, &problems, kMySqlFamily);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("config.json: mysql eight isn't a MySQL series") != std::string::npos);

    const test_util::TempDir tmp;
    CHECK(mariadb_custom_ini_file(tmp.path(), "8.4", kMySqlFamily) == tmp.path() / "mysql" / "8.4" / "custom.ini");
    CHECK(mariadb_custom_ini_file(tmp.path(), "11.4") == tmp.path() / "mariadb" / "11.4" / "custom.ini");
    std::string error;
    REQUIRE(ensure_mariadb_custom_ini(tmp.path(), "8.4", error, kMySqlFamily));
    CHECK(test_util::read_file(tmp.path() / "mysql" / "8.4" / "custom.ini").find("MySQL 8.4") != std::string::npos);
    CHECK(read_mariadb_custom_ini(tmp.path(), "8.4", kMySqlFamily).empty());  // the header only

    // MySQL's own listeners and folders.
    for (const char* name : {"mysqlx", "mysqlx-port", "mysqlx_bind_address", "loose-mysqlx", "admin-address",
                             "admin_port", "create-admin-listener-thread", "innodb_directories", "tmpdir"}) {
        CAPTURE(name);
        CHECK(mariadb_option_problem(name));
    }
    CHECK(mariadb_option_problem("mysqlx_port")->find("127.0.0.1") != std::string::npos);
    for (const char* name : {"max_connections", "innodb_redo_log_capacity", "authentication_policy", "tmp_table_size",
                             "sql_mode"}) {
        CAPTURE(name);
        CHECK_FALSE(mariadb_option_problem(name));
    }
    CHECK(mariadb_ini_defaults(kMySqlFamily).at(1).second == "utf8mb4_0900_ai_ci");
}

TEST_CASE("phpMyAdmin offers MariaDB's and MySQL's servers, each with its own signon session") {
    CHECK(signon_session_name("11.4") == "FloServerSignon_11_4");  // as before D28
    CHECK(signon_session_name("8.4", "mysql") == "FloServerSignon_mysql_8_4");
    const auto listed = phpmyadmin_servers({{"8.4", 3307, "mysql"}, {"11.4", 3306}, {"8.4", 3399, "mysql"},
                                            {"10.11", 3308}, {"18", 5432, "postgresql"}});
    REQUIRE(listed.size() == 3);
    CHECK(listed[0].series == "10.11");
    CHECK(listed[1].series == "11.4");
    CHECK(listed[2].family == "mysql");
    CHECK(listed[2].port == 3307);

    PhpMyAdminConfigSpec spec;
    spec.blowfish_hex = std::string(64, 'a');
    const test_util::TempDir tmp;
    spec.paths = phpmyadmin_paths(tmp.path());
    spec.servers = {{"11.4", 3306}, {"8.4", 3307, "mysql"}};
    std::string error;
    const auto config = phpmyadmin_config(spec, error);
    REQUIRE_MESSAGE(config, error);
    CHECK(config->find("$cfg['Servers'][$i]['verbose'] = 'MariaDB 11.4';") != std::string::npos);
    CHECK(config->find("$cfg['Servers'][$i]['verbose'] = 'MySQL 8.4';") != std::string::npos);
    CHECK(config->find("$cfg['Servers'][$i]['port'] = '3307';") != std::string::npos);
    CHECK(config->find("FloServerSignon_mysql_8_4") != std::string::npos);
    CHECK(config->find("MariaDB 11.4") < config->find("MySQL 8.4"));

    SignonLogin login;
    login.series = "8.4";
    login.family = "mysql";
    login.user = "root";
    login.password = "pw";
    login.port = 3307;
    login.server = 2;
    const auto token = create_signon_token(spec.paths.tokens, login, std::chrono::system_clock::now(), error);
    REQUIRE_MESSAGE(token, error);
    const auto doc = nlohmann::json::parse(test_util::read_file(spec.paths.tokens / (*token + ".json")));
    CHECK(doc["session"] == "FloServerSignon_mysql_8_4");
    CHECK(doc["server"] == 2);
}

TEST_CASE("Remove FloServer: MySQL's data folders go with MariaDB's") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    CHECK(is_uninstall_item(data, data / "data" / "mysql-8.4"));
    CHECK(is_uninstall_item(data, data / "data" / "mariadb-11.4"));
    CHECK_FALSE(is_uninstall_item(data, data / "data" / "mysql-"));
    CHECK_FALSE(is_uninstall_item(data, data / "data" / "mysqlx"));
    CHECK_FALSE(is_uninstall_item(data, data / "mysql-8.4"));
    test_util::write_file(data / "data" / "mysql-8.4" / "data" / "ibdata1", "x");
    test_util::write_file(data / "data" / "other" / "f", "x");
    const auto items = uninstall_items(data, false, true, std::chrono::steady_clock::now() + std::chrono::seconds(5));
    REQUIRE(items.size() == 1);
    CHECK(items[0].path.filename() == "mysql-8.4");
}

TEST_CASE("a backup names MySQL's database however floserver.json writes it") {
    const test_util::TempDir tmp;
    test_util::write_file(tmp.path() / "floserver.json", R"({"php": "8.4", "mysql": "8.4"})");
    CHECK(backup_versions(tmp.path())["database"] == "mysql-8.4");
    test_util::write_file(tmp.path() / "floserver.json", R"({"database": "mysql-8.4"})");
    CHECK(backup_versions(tmp.path())["database"] == "mysql-8.4");
}

TEST_CASE("MySQL user names: 32 characters at most, the same short name every time") {
    const std::string long_name(63, 'a');
    const std::string user = project_database_user(long_name, kMySqlFamily);
    CHECK(user.size() == kMySqlMaxUserName);
    CHECK(user.starts_with(std::string(23, 'a') + "_"));
    CHECK(user == project_database_user(long_name, kMySqlFamily));  // deterministic
    CHECK(user != project_database_user(std::string(62, 'a') + "b", kMySqlFamily));
    CHECK(std::all_of(user.begin() + 24, user.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }));
    // 31 or fewer, and MariaDB's (80): the database's name. 32 is cut too, so a cut
    // name (always 32) is never another project's real one.
    CHECK(project_database_user(std::string(31, 'b'), kMySqlFamily) == std::string(31, 'b'));
    CHECK(project_database_user(std::string(32, 'b'), kMySqlFamily).size() == 32);
    CHECK(project_database_user(std::string(32, 'b'), kMySqlFamily) != std::string(32, 'b'));
    CHECK(project_database_user(long_name, kMariaDbFamily) == long_name);
    CHECK(project_database_user("my_shop", kMySqlFamily) == "my_shop");
}

TEST_CASE("MySQL on Windows: paths as ASCII, their 8.3 forms, or a clear refusal") {
    const ShortPathFunction shorten = [](const fs::path& p) -> fs::path {
        const std::string s = path_to_utf8(p);
        if (s.find("\xD9\x86") != std::string::npos) {  // an Arabic letter: this "drive" keeps 8.3 names
            return fs::path("C:/Users/KARWAN~1") / p.filename();
        }
        return {};
    };
    CHECK(ascii_path(fs::path("C:/Users/karwan/data"), shorten) == std::optional<fs::path>(fs::path("C:/Users/karwan/data")));
    CHECK(ascii_path(path_from_utf8("C:/Users/\xD9\x86/data"), shorten) ==
          std::optional<fs::path>(fs::path("C:/Users/KARWAN~1/data")));
    CHECK_FALSE(ascii_path(path_from_utf8("C:/Users/\xC3\xA9/data"), shorten));  // no short name
    CHECK_FALSE(ascii_path(path_from_utf8("C:/Users/\xC3\xA9/data"), ShortPathFunction()));

    MariaDbFiles f = mariadb_files(path_from_utf8("C:/Users/\xD9\x86/rt/mysql"), "8.4.11",
                                   path_from_utf8("C:/Users/\xD9\x86/data"), kMySqlFamily);
    fs::path bad;
    bool missing = false;
    REQUIRE(ascii_mysql_files(f, shorten, bad, missing));
    for (const fs::path* p : {&f.base_dir, &f.server, &f.client, &f.admin, &f.dump, &f.folder, &f.data_dir, &f.config,
                              &f.error_log, &f.init_file}) {
        const std::string s = path_to_utf8(*p);
        CAPTURE(s);
        CHECK(std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }));
    }
    CHECK(f.config.parent_path() == f.folder);
    CHECK(f.error_log.parent_path() == f.data_dir);
    MariaDbFiles g = mariadb_files(path_from_utf8("C:/Users/\xC3\xA9/rt/mysql"), "8.4.11", "C:/d", kMySqlFamily);
    CHECK_FALSE(ascii_mysql_files(g, shorten, bad, missing));
    CHECK(path_to_utf8(bad).find("\xC3\xA9") != std::string::npos);
    CHECK(missing);  // that install isn't on this disk
    // One that is there but has no short name: its letters are the reason, not a missing file.
    const test_util::TempDir tmp;
    const fs::path here = tmp.path() / path_from_utf8("caf\xC3\xA9");
    fs::create_directories(here / "bin");
    MariaDbFiles h = mariadb_files(here, "8.4.11", tmp.path() / "data", kMySqlFamily);
    CHECK_FALSE(ascii_mysql_files(h, shorten, bad, missing));
    CHECK(bad == here);
    CHECK_FALSE(missing);
    CHECK(windows_short_path("/no/such/path").empty());
}

TEST_CASE("MySQL's root SQL sets root's password first; FloServer's tools read it from an option file") {
    const std::string sql = root_password_sql("pw", kMySqlFamily);
    CHECK(sql.find("ALTER USER IF EXISTS 'root'@'localhost'") < sql.find("CREATE USER"));
    CHECK(root_password_sql("pw").find("ALTER USER IF EXISTS 'root'@'localhost'") == 0);
    CHECK(client_password_option_file(R"(a"b\c#d')") == "[client]\npassword=\"a\\\"b\\\\c#d'\"\n");
    // MySQL's own listeners and logs.
    CHECK(mariadb_option_problem("log_raw"));
    CHECK(mariadb_option_problem("log-raw"));
    const test_util::TempDir tmp;
    const MariaDbFiles f = mariadb_files(tmp.path() / "inst", "8.4.11", tmp.path() / "data", kMySqlFamily);
    CHECK(mariadb_config(f, 33101, false, false).find("secure-file-priv=NULL") != std::string::npos);
    const MariaDbFiles m = mariadb_files(tmp.path() / "inst", "11.4.13", tmp.path() / "data");
    CHECK(mariadb_config(m, 33100, false, false).find("secure-file-priv") == std::string::npos);  // MariaDB won't start with NULL
}

TEST_CASE("a new database server never takes a port another one keeps in its folder (one rule for every family)") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    // An installed MySQL (it never starts here: nothing connects) and a MariaDB folder from before, keeping 3307.
    const fs::path dir = runtimes_dir(data) / "mysql" / "8.4.11";
    fs::create_directories(dir);
    test_util::write_file(dir / ".floserver-runtime.json",
                          nlohmann::json{{"schema", 1}, {"name", "mysql"}, {"version", "8.4.11"},
                                         {"platform", std::string(current_platform())}, {"variant", ""},
                                         {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
    test_util::write_file(data / "data" / "mariadb-10.11" / "port", "3307\n");
    const AppConfig app;
    std::string error;
    REQUIRE(save_app_config(app_config_file(data), app, error));
    DatabaseServiceConfig cfg;
    cfg.data_dir = data;
    cfg.probe = [](Port p) { return p != 3306 && can_listen_on_loopback(p); };
    DatabaseService db(cfg, SecretStore(data));
    db.start();
    const auto servers = db.status()["servers"];
    REQUIRE(servers.size() == 1);
    CHECK(servers[0]["family"] == "mysql");
    CHECK(servers[0]["port"] != 3307);
    CHECK(test_util::read_file(data / "data" / "mysql-8.4" / "port") == std::to_string(servers[0]["port"].get<int>()) + "\n");
    db.stop();
}

TEST_CASE("MySQL database names: a GRANT's escaped name fits its 64 characters") {
    const std::string name = project_database_name("a-very-long-project-name-that-mysql-cannot-take-as-a-user-names");
    const std::string on_mysql = server_database_name(name, kMySqlFamily);
    CHECK(on_mysql != name);
    CHECK(on_mysql == server_database_name(name, kMySqlFamily));
    CHECK(grant_pattern(on_mysql).size() - 2 <= kMySqlMaxIdentifier);  // without its backticks
    CHECK(server_database_name(name, kMariaDbFamily) == name);
    CHECK(server_database_name("my_shop", kMySqlFamily) == "my_shop");
    const std::string plain(62, 'a');  // no '_': nothing to escape
    CHECK(server_database_name(plain, kMySqlFamily) == plain);
    // 63 or more (escaped) is cut, to 63 or 64: never another project's real name (62 at most).
    for (const std::string& longer : {std::string(63, 'a'), std::string(64, 'a'), std::string(31, 'a') + "_" + std::string(31, 'b'),
                                    project_database_name("a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-p-q-r-s-t-u-v-w-x-y-z-0-1-2-3-4")}) {
        CAPTURE(longer);
        const std::string cut = server_database_name(longer, kMySqlFamily);
        const std::size_t escaped = cut.size() + static_cast<std::size_t>(std::count(cut.begin(), cut.end(), '_'));
        CHECK(cut != longer);
        CHECK(escaped >= 63);
        CHECK(escaped <= 64);
    }
    CHECK(project_database_user(on_mysql, kMySqlFamily).size() <= kMySqlMaxUserName);
}

TEST_CASE("MySQL's TLS settings are FloServer's: its tools check the server's own certificate") {
    for (const char* name : {"ssl_ca", "ssl-cert", "ssl_key", "ssl_capath", "ssl_crl", "ssl_crlpath", "tls_version",
                             "tls-ciphersuites", "ssl_cipher", "require_secure_transport", "loose-ssl-ca", "skip-ssl"}) {
        CAPTURE(name);
        const auto problem = mariadb_option_problem(name, kMySqlFamily);
        REQUIRE(problem);
        CHECK(problem->find("certificate") != std::string::npos);
    }
    CHECK_FALSE(mariadb_option_problem("ssl_fips_mode_x", kMySqlFamily));  // not one of them
    CHECK(filter_mariadb_custom_ini("[mysqld]\nssl_ca=/tmp/evil.pem\nmax_connections=50\n", kMySqlFamily).problems.size() ==
          1);
    // MariaDB's tools check no certificate: its TLS settings stay the user's.
    for (const char* name : {"ssl_ca", "ssl-cert", "tls_version", "require_secure_transport", "skip-ssl"}) {
        CAPTURE(name);
        CHECK_FALSE(mariadb_option_problem(name, kMariaDbFamily));
    }
    const FilteredCustomIni maria = filter_mariadb_custom_ini("[mysqld]\nssl_ca=/etc/my-ca.pem\n", kMariaDbFamily);
    CHECK(maria.problems.empty());
    CHECK(maria.text.find("ssl_ca=/etc/my-ca.pem") != std::string::npos);
    AppConfig app;
    MariaDbSeriesSettings one;
    one.ini["ssl_ca"] = "/etc/my-ca.pem";
    write_mariadb_series(app, "11.4", one);
    CHECK(read_mariadb_settings(app).at("11.4").ini.contains("ssl_ca"));
    write_mariadb_series(app, "8.4", one, kMySqlFamily);
    std::vector<std::string> problems;
    CHECK(read_mariadb_settings(app, &problems, kMySqlFamily).at("8.4").ini.empty());  // left out, and said so
    CHECK(problems.size() == 1);
    // MySQL's certificates are its set-up's.
    CHECK(mariadb_option_problem("auto_generate_certs", kMySqlFamily));
    CHECK(mariadb_option_problem("auto-generate-certs", kMySqlFamily));
}

TEST_CASE("MySQL's root check counts only what the init file is responsible for") {
    const std::string sql = root_accounts_check_sql();
    CHECK(sql.find("User = 'root'") != std::string::npos);
    CHECK(sql.find("User = '' AND Host IN ('localhost', '127.0.0.1', '%')") != std::string::npos);
    CHECK(sql.find("User = 'root' AND (authentication_string") == std::string::npos);  // no root on another host
    // The init file drops exactly those anonymous accounts, last.
    const std::string init = root_password_sql("pw", kMySqlFamily);
    CHECK(init.rfind("DROP USER IF EXISTS ''@'localhost', ''@'127.0.0.1', ''@'%';") ==
          init.size() - std::string("DROP USER IF EXISTS ''@'localhost', ''@'127.0.0.1', ''@'%';\n").size());
    CHECK(sql.find("'localhost', '127.0.0.1', '::1'") != std::string::npos);
}

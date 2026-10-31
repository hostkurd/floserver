// Settings → Database (MariaDB): config.json's "mariadb", the option names
// refused, custom.ini's filter, the my.ini's order, mariadbd's option list,
// and the project service's database.get / database.set.

#include "floserver/core/mariadb.h"
#include "floserver/core/mariadb_settings.h"
#include "floserver/core/paths.h"
#include "floserver/core/ports.h"
#include "floserver/core/project_service.h"
#include "floserver/core/runtime_store.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace floserver::core;
namespace fs = std::filesystem;
using nlohmann::json;

TEST_CASE("my.ini names FloServer sets, or that would open MariaDB up, are refused in every spelling") {
    for (const char* ok : {"max_connections", "max-connections", "Max_Allowed_Packet", "sql_mode", "default-time-zone",
                           "character-set-server", "innodb_buffer_pool_size", "general_log", "slow_query_log",
                           "slow-query-log", "log_slow_query", "lc_messages", "wait_timeout", "skip-name-resolve",
                           "loose-max-connections", "port_open_timeout", "extra_max_connections", "innodb"}) {
        CHECK_MESSAGE(!mariadb_option_problem(ok), ok);
    }
    for (const char* refused :
         {"datadir", "basedir", "port", "PORT", "socket", "named-pipe", "named_pipe", "shared-memory", "bind-address",
          "bind_address", "bind", "skip-grant-tables", "skip_grant_tables", "skip-grant", "loose-skip-grant-tables",
          "loose_loose_skip_grant_tables", "disable-grant-tables", "enable-skip-grant-tables", "skip-networking",
          "enable-networking", "init-file", "init_file", "secure-file-priv", "plugin-dir", "plugin_load_add",
          "plugin-load", "log-error", "log_error", "general_log_file", "general-log-f", "slow_query_log_file",
          "log_slow_query_file", "log-bin", "pid-file", "user", "extra-port", "wsrep_on", "wsrep-cluster-address",
          "proxy-protocol-networks", "innodb_data_home_dir", "aria-log-dir-path", "maximum-port", "defaults-extra-file"}) {
        CHECK_MESSAGE(mariadb_option_problem(refused), refused);
    }
    // In plain words, saying why.
    CHECK(mariadb_option_problem("skip-grant-tables")->find("password") != std::string::npos);
    CHECK(mariadb_option_problem("bind-address")->find("127.0.0.1") != std::string::npos);
    CHECK(mariadb_option_problem("port")->find("Settings > Database") != std::string::npos);
    // Not a name at all.
    const std::vector<std::string> bad = {"", "a b", "max=1", "!include", "[mysqld]", "-port", "x\ny", std::string(65, 'a')};
    for (const std::string& name : bad) {
        CHECK_MESSAGE(mariadb_option_problem(name), name);
    }
    CHECK(canonical_mariadb_option(" Max-Connections ") == "max_connections");
    CHECK_FALSE(mariadb_value_problem("STRICT_TRANS_TABLES,NO_ENGINE_SUBSTITUTION"));
    CHECK(mariadb_value_problem("1\n[mysqld]"));
    CHECK(mariadb_value_problem("a\tb"));
    CHECK(mariadb_value_problem(std::string(1001, 'x')));
}

TEST_CASE("config.json's mariadb is read and written per series, keeping what it doesn't know") {
    AppConfig app;
    app.other = json::parse(R"({"php": {"8.5": {"ini": {"memory_limit": "1G"}}},
        "mariadb": {"11.4": {"port": 3307, "keep_running": true, "later": 1,
                             "ini": {"max-connections": "300", "datadir": "C:/x", "wait_timeout": 5}},
                    "eleven": {}, "10.11": {"port": 80}}, "other": true})");
    std::vector<std::string> problems;
    const MariaDbSettings all = read_mariadb_settings(app, &problems);
    REQUIRE(all.contains("11.4"));
    const MariaDbSeriesSettings& s = all.at("11.4");
    CHECK(s.port == std::optional<std::uint16_t>(3307));
    CHECK(s.keep_running);
    CHECK(s.ini == std::map<std::string, std::string>{{"max_connections", "300"}});  // canonical; refused ones left out
    CHECK_FALSE(all.at("10.11").port);  // under 1024
    CHECK(problems.size() == 4);  // datadir, wait_timeout (not text), "eleven", port 80

    MariaDbSeriesSettings changed = s;
    changed.ini["sql_mode"] = "TRADITIONAL";
    changed.port.reset();
    write_mariadb_series(app, "11.4", changed);
    CHECK(app.other["mariadb"]["11.4"]["later"] == 1);  // unknown keys stay
    CHECK_FALSE(app.other["mariadb"]["11.4"].contains("port"));
    CHECK(app.other["mariadb"]["11.4"]["ini"]["sql_mode"] == "TRADITIONAL");
    CHECK(app.other["php"]["8.5"]["ini"]["memory_limit"] == "1G");
    CHECK(app.other["other"] == true);
    // Empty settings take the series out, and then the key.
    write_mariadb_series(app, "10.11", {});
    CHECK_FALSE(app.other["mariadb"].contains("10.11"));
    AppConfig plain;
    write_mariadb_series(plain, "11.4", {});
    CHECK_FALSE(plain.other.contains("mariadb"));
}

TEST_CASE("custom.ini: lines that would change FloServer's own are left out with the reason") {
    const std::string text = "\xEF\xBB\xBF# mine\n"
                             "max_connections = 500\n"
                             "skip-grant-tables\n"
                             "!include C:/evil.cnf\n"
                             "[client]\n"
                             "port=1\n"
                             "[mysqld\n"
                             "loose-bind-address=0.0.0.0\n"
                             "sql-mode=ANSI\r\n"
                             "slow_query_log\n";
    const FilteredCustomIni f = filter_mariadb_custom_ini(text);
    CHECK(f.text.find("max_connections=500\n") != std::string::npos);
    CHECK(f.text.find("sql-mode=ANSI\n") != std::string::npos);
    CHECK(f.text.find("slow_query_log\n") != std::string::npos);
    CHECK(f.text.find("[client]\n") != std::string::npos);
    CHECK(f.text.find("mine") == std::string::npos);  // comments stay in custom.ini
    // Left out: as comments only.
    for (const char* gone : {"\nskip-grant-tables", "\n!include", "\nport=1", "\n[mysqld\n", "\nloose-bind-address"}) {
        CHECK_MESSAGE(f.text.find(gone) == std::string::npos, gone);
    }
    REQUIRE(f.problems.size() == 5);
    CHECK(f.problems[0].starts_with("line 3: "));
    CHECK(f.text.find("# FloServer left out line 3\n") != std::string::npos);
    CHECK(f.keys == std::vector<std::string>{"max_connections", "slow_query_log", "sql_mode"});
}

TEST_CASE("custom.ini: no long line reaches my.ini (MariaDB reads 4 KB at a time)") {
    // The end of a 4 KB line would be read as a line of its own.
    const std::string text = "#" + std::string(4093, 'a') + "skip-grant-tables\n" +
                             "max_connections=" + std::string(5000, '9') + "\n" +
                             "#" + std::string(4093, 'b') + "!includedir /tmp/evil\n" +
                             ";" + std::string(4093, 'c') + "plugin-load-add=evil.so\n" +
                             std::string(2000, 'k') + "=1\n" +
                             "wait_timeout=30\n";
    const FilteredCustomIni f = filter_mariadb_custom_ini(text);
    CHECK(f.text.find("skip-grant") == std::string::npos);
    CHECK(f.text.find("include") == std::string::npos);
    CHECK(f.text.find("plugin") == std::string::npos);
    CHECK(f.text.find("999") == std::string::npos);
    CHECK(f.text.find("wait_timeout=30\n") != std::string::npos);
    CHECK(f.problems.size() == 5);
    for (const std::string& p : f.problems) {
        CHECK(p.size() < 200);
    }
    MariaDbExtras extras;
    extras.ini = {{"sql_mode", std::string(1000, 'x')}};
    extras.custom_ini = f.text + std::string(3000, 'z') + "\n";  // even unfiltered, a long line is dropped
    const std::string c = mariadb_config(mariadb_files("/opt/mariadb", "11.4.5", "/data"), 33100, false, false, &extras);
    std::size_t start = 0;
    while (start < c.size()) {
        const auto end = c.find('\n', start);
        CHECK((end == std::string::npos ? c.size() : end) - start <= 1100);
        start = end == std::string::npos ? c.size() : end + 1;
    }
    CHECK(c.find("zzz") == std::string::npos);
    // FloServer's own last: never without passwords, never off the network.
    CHECK(c.rfind("skip-grant-tables=0") > c.rfind("wait_timeout=30"));
    CHECK(c.find("skip-networking=0") != std::string::npos);
    const auto guards = mariadb_command_line_guards(mariadb_files("/opt/mariadb", "11.4.5", "/data"), 33100);
    CHECK(std::find(guards.begin(), guards.end(), "--bind-address=127.0.0.1") != guards.end());
    CHECK(std::find(guards.begin(), guards.end(), "--skip-grant-tables=0") != guards.end());
    CHECK(std::find(guards.begin(), guards.end(), "--port=33100") != guards.end());
}

TEST_CASE("the my.ini: defaults, Settings' values, custom.ini, then FloServer's own lines last") {
    const MariaDbFiles files = mariadb_files("/opt/mariadb", "11.4.5", "/data");
    MariaDbExtras extras;
    extras.ini = {{"max_connections", "300"}, {"character_set_server", "latin1"}};
    extras.custom_ini = filter_mariadb_custom_ini("[mysqld]\nwait_timeout=60\nport=1\n[client]\nuser=x\n").text;
    extras.custom_ini_path = "/data/mariadb/11.4/custom.ini";
    const std::string c = mariadb_config(files, 33100, false, false, &extras);
    const auto at = [&c](const std::string& s) {
        const auto p = c.find(s);
        CHECK_MESSAGE(p != std::string::npos, s);
        return p;
    };
    CHECK(at("character_set_server=utf8mb4") < at("max_connections=300"));
    CHECK(at("character_set_server=latin1") < at("wait_timeout=60"));  // a later value wins
    CHECK(at("wait_timeout=60") < at("datadir="));
    // FloServer's own: in a [mysqld] of their own after custom.ini's [client].
    const auto own = c.rfind("[mysqld]");
    CHECK(c.find("[client]") < own);
    CHECK(own < at("port=33100"));
    CHECK(own < at("bind-address=127.0.0.1"));
    CHECK(c.find("port=1\n") == std::string::npos);
    CHECK(c.find("user=x") == std::string::npos);
    // Without extras: as before.
    CHECK(mariadb_config(files, 33100, false, false).find("# Your own lines") == std::string::npos);
}

TEST_CASE("mariadbd --verbose --help: the option names, from both lists") {
    const std::string help =
        "Usage: mariadbd [OPTIONS]\n"
        "  --allow-suspicious-udfs\n"
        "                      Allows use of UDFs\n"
        "  -h, --datadir=name  Path to the database root directory\n"
        "  --max-connections=# The number of simultaneous clients\n"
        "  --skip-grant-tables Start without grant tables.\n"
        "\n"
        "Variables (--variable-name=value)\n"
        "and boolean options {FALSE|TRUE}  Value (after reading options)\n"
        "------------------------------------------------------------ -------------\n"
        "character-set-server                                         latin1\n"
        "max-connections                                              151\n"
        "\n"
        "To see what variables a running server is using, type\n";
    CHECK(parse_mariadb_help(help) == std::vector<std::string>{"allow_suspicious_udfs", "character_set_server", "datadir",
                                                               "max_connections", "skip_grant_tables"});
}

TEST_CASE("a root password: 8 to 100 printable characters, no spaces") {
    CHECK_FALSE(root_password_problem("Abcdef12"));
    CHECK_FALSE(root_password_problem("p@ss'w\"rd\\;#"));
    CHECK(root_password_problem("short"));
    CHECK(root_password_problem("has a space"));
    CHECK(root_password_problem(std::string(101, 'a')));
    CHECK(root_password_problem("ümlautpassword"));
    CHECK(root_password_problem("tab\tpassword"));
}

namespace {

void fake_mariadb(const fs::path& data, const std::string& version) {
    const fs::path dir = runtimes_dir(data) / "mariadb" / version;
    fs::create_directories(dir);
    test_util::write_file(dir / ".floserver-runtime.json",
                          json{{"schema", 1}, {"name", "mariadb"}, {"version", version},
                               {"platform", std::string(current_platform())}, {"variant", ""},
                               {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
}

}  // namespace

TEST_CASE("database.get and database.set through the project service") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_mariadb(data, "11.4.5");
    fake_mariadb(data, "10.11.9");
    std::string error;
    REQUIRE(write_file_atomically(app_config_file(data), R"({"projects": [], "keep": {"me": 1}})", error));
    ProjectService service(data);
    ServiceRefusal refusal;

    auto got = service.database_get("11.4", refusal);
    REQUIRE_MESSAGE(got, refusal.message);
    CHECK((*got)["version"] == "11.4.5");
    CHECK((*got)["port"].is_null());
    CHECK((*got)["keep_running"] == false);
    CHECK((*got)["ini"]["innodb_buffer_pool_size"] == "64M");
    CHECK((*got)["changed"].empty());
    CHECK((*got)["custom_ini_exists"] == false);
    CHECK_FALSE(got->contains("known_options"));  // a fake install has no mariadbd to ask
    CHECK_FALSE(service.database_get("11", refusal));
    CHECK(refusal.code == "bad_params");

    bool changed = false;
    // Free ports of this machine's, never 3306.
    const auto free_a = pick_port_from(3400, {}, can_listen_on_loopback);
    REQUIRE(free_a);
    const auto free_b = pick_port_from(static_cast<Port>(*free_a + 1), {}, can_listen_on_loopback);
    REQUIRE(free_b);
    const json port = *free_a;
    const json keep = true;
    got = service.database_set("11.4", {{"max-connections", "300"}, {"wait_timeout", "60"}}, &port, &keep, changed, refusal);
    REQUIRE_MESSAGE(got, refusal.message);
    CHECK(changed);
    CHECK((*got)["changed"] == json{{"max_connections", "300"}, {"wait_timeout", "60"}});
    CHECK((*got)["port"] == *free_a);
    CHECK((*got)["keep_running"] == true);
    auto app = load_app_config(app_config_file(data), error);
    REQUIRE(app);
    CHECK(app->other["keep"]["me"] == 1);
    CHECK(app->other["mariadb"]["11.4"]["ini"]["max_connections"] == "300");

    // Only the port: my.ini is as it was.
    const json other_port = *free_b;
    got = service.database_set("11.4", nullptr, &other_port, nullptr, changed, refusal);
    REQUIRE_MESSAGE(got, refusal.message);
    CHECK_FALSE(changed);
    CHECK((*got)["keep_running"] == true);
    // Null goes back to the default; empty text too.
    got = service.database_set("11.4", {{"wait_timeout", nullptr}, {"max_connections", ""}}, nullptr, nullptr, changed, refusal);
    REQUIRE(got);
    CHECK(changed);
    CHECK((*got)["changed"].empty());

    // Refused, and nothing saved.
    CHECK_FALSE(service.database_set("11.4", {{"skip-grant-tables", "1"}}, nullptr, nullptr, changed, refusal));
    CHECK(refusal.message.find("password") != std::string::npos);
    CHECK_FALSE(service.database_set("11.4", {{"sql_mode", "a\nb"}}, nullptr, nullptr, changed, refusal));
    const json low = 80;
    CHECK_FALSE(service.database_set("11.4", nullptr, &low, nullptr, changed, refusal));
    const json internal = 33105;
    CHECK_FALSE(service.database_set("11.4", nullptr, &internal, nullptr, changed, refusal));
    // Another series' port.
    CHECK_FALSE(service.database_set("10.11", nullptr, &other_port, nullptr, changed, refusal));
    CHECK(refusal.code == "port_taken");

    // custom.ini: made with its header on demand, its refused lines reported.
    const auto custom = service.database_custom_ini("11.4", refusal);
    REQUIRE(custom);
    CHECK(read_small_file(*custom, 4096) == mariadb_custom_ini_header("11.4"));
    got = service.database_get("11.4", refusal);
    REQUIRE(got);
    CHECK((*got)["custom_ini_exists"] == true);
    CHECK_FALSE(got->contains("custom_problems"));
    test_util::write_file(*custom, mariadb_custom_ini_header("11.4") + "max_conections=5\nbind-address=0.0.0.0\n");
    got = service.database_get("11.4", refusal);
    REQUIRE(got);
    REQUIRE(got->contains("custom_problems"));
    CHECK((*got)["custom_problems"].size() == 1);
    CHECK((*got)["custom_keys"] == json{"max_conections"});
}

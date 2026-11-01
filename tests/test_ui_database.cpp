// Settings > Database's model (src/ui-win32/model/database_page): the
// answer read, changes until Save, what Save sends, "did you mean".

#include "model/database_page.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

using namespace floserver::ui;
using nlohmann::json;

namespace {

json reply() {
    return json::parse(R"({
        "series": "11.4", "version": "11.4.5", "port": null, "keep_running": false, "kept_by": ["shop"],
        "ini": {"character_set_server": "utf8mb4", "max_connections": "300"},
        "changed": {"max_connections": "300"},
        "custom_ini": "C:/d/mariadb/11.4/custom.ini", "custom_ini_exists": true,
        "custom_problems": ["line 2: skip-grant-tables can't be set"], "custom_keys": ["max_conections"],
        "used_by": ["blog", "shop", "_shared"], "databases": ["blog"],
        "known_options": ["max_connections", "sql_mode", "grant_tables_x", "name_resolve"],
        "server": {"series": "11.4", "state": "running", "port": 3306, "connections": 2}})");
}

}  // namespace

TEST_CASE("database.get's answer and the server's state from databases.list") {
    const auto s = db_series_from_reply(reply());
    REQUIRE(s);
    CHECK(s->series == "11.4");
    CHECK(s->version == "11.4.5");
    CHECK(s->port == 0);
    CHECK(s->kept_by == std::vector<std::string>{"shop"});
    CHECK(s->custom_problems.size() == 1);
    CHECK(s->server.exists);
    CHECK(s->server.state == "running");
    CHECK(s->server.port == 3306);
    CHECK_FALSE(db_series_from_reply(json::object()));

    const json list = json::parse(R"({"servers": [{"series": "10.11", "state": "sleeping", "port": 3307},
                                                   {"series": "11.4", "state": "failed", "port": 3306, "error": "boom",
                                                    "port_wanted": 3310}]})");
    const DbServer failed = db_server_from_list(list, "11.4");
    CHECK(failed.exists);
    CHECK(failed.error == "boom");
    CHECK(failed.port_wanted == 3310);
    CHECK_FALSE(db_server_from_list(list, "12.0").exists);
    CHECK_FALSE(db_server_from_list(json(), "11.4").exists);

    CHECK(db_state_text(*s, s->server, Lang::En) == "Running on port 3306");
    CHECK(db_state_text(*s, failed, Lang::En).starts_with("Couldn't start: boom"));
    CHECK(db_state_text(*s, failed, Lang::En).find("Port 3310 is in use") != std::string::npos);
    CHECK(db_state_text(*s, DbServer{}, Lang::En).starts_with("Not ready yet"));
    const DbActions running = db_actions(s->server);
    CHECK((!running.start && running.stop && running.restart));
    const DbActions stopped = db_actions(failed);
    CHECK((stopped.start && !stopped.stop && !stopped.restart));
    const DbActions none = db_actions(DbServer{});
    CHECK((!none.start && !none.stop && !none.restart));
}

TEST_CASE("the installed MariaDB series, newest first") {
    const Installed installed = {{"mariadb", {"10.11.9", "11.4.2", "11.4.5"}}, {"php", {"8.5.1"}}};
    const auto versions = installed_db_series(installed);
    REQUIRE(versions.size() == 2);
    CHECK(versions[0] == std::pair<std::string, std::string>{"11.4", "11.4.5"});
    CHECK(versions[1].first == "10.11");
    const json runtimes = json::parse(R"({"installed": [{"name": "mariadb", "version": "11.8.1"}]})");
    CHECK(settings_db_versions(false, {}, runtimes).front().first == "11.8");
    CHECK(settings_db_versions(true, installed, runtimes).front().first == "11.4");
}

TEST_CASE("Settings > Database: changes until Save, and what Save sends") {
    const DbSeries s = *db_series_from_reply(reply());
    DbChanges c;
    CHECK(c.empty());
    CHECK(db_ini_text(s, c, "max_connections") == "300");
    CHECK(db_ini_marked(s, c, "max_connections"));
    CHECK_FALSE(db_ini_marked(s, c, "character_set_server"));  // FloServer's default, not the series' own
    db_set_ini_text(s, c, "wait_timeout", " 60 ");
    db_set_ini_text(s, c, "max_connections", "300");  // as it is: no change
    CHECK(c.ini.size() == 1);
    db_reset_ini(s, c, "max_connections");
    CHECK(db_ini_text(s, c, "max_connections").empty());
    CHECK_FALSE(db_ini_marked(s, c, "max_connections"));

    db_set_port_text(s, c, "3310");
    CHECK_FALSE(db_port_bad(c));
    CHECK(db_port_text(s, c) == "3310");
    db_toggle_keep_running(s, c);
    CHECK(db_keep_running(s, c));
    const json params = database_set_params("11.4", c);
    CHECK(params == json::parse(R"({"series": "11.4", "ini": {"wait_timeout": "60", "max_connections": null},
                                    "port": 3310, "keep_running": true})"));
    db_toggle_keep_running(s, c);
    CHECK_FALSE(c.keep_running);
    for (const char* bad : {"80", "70000", "33a", "123456"}) {
        db_set_port_text(s, c, bad);
        CHECK_MESSAGE(db_port_bad(c), bad);
        CHECK_FALSE(database_set_params("11.4", c).contains("port"));
    }
    db_set_port_text(s, c, "");  // automatic, as it is
    CHECK_FALSE(c.port);
    DbSeries chosen = s;
    chosen.port = 3307;
    DbChanges back;
    db_set_port_text(chosen, back, " ");
    CHECK(database_set_params("11.4", back)["port"].is_null());  // back to automatic
}

TEST_CASE("a my.ini name MariaDB doesn't know: it wouldn't start, and the closest one is offered") {
    const DbSeries s = *db_series_from_reply(reply());
    CHECK(unknown_db_option(s.known_options, "max_conections") == std::optional<std::string>("max_connections"));
    CHECK_FALSE(unknown_db_option(s.known_options, "max-connections"));
    CHECK_FALSE(unknown_db_option(s.known_options, "loose-Max_Connections"));
    CHECK_FALSE(unknown_db_option(s.known_options, "skip-name-resolve"));  // skip- goes with the name
    CHECK(unknown_db_option(s.known_options, "zzzz") == std::optional<std::string>(""));
    CHECK_FALSE(unknown_db_option({}, "zzzz"));  // MariaDB couldn't tell: no warnings
    CHECK(unknown_db_option_text("11.4", "max_conections", "max_connections", Lang::En) ==
          "custom.ini: MariaDB 11.4 has no option named max_conections. Did you mean max_connections?");
}

TEST_CASE("Settings > Database's footer, names and root password rule") {
    const DbSeries s = *db_series_from_reply(reply());
    CHECK(db_restart_text(s, s.server, Lang::En) ==
          "Applies when saved: MariaDB 11.4 restarts (a few seconds) for 2 projects and shared sites.");
    DbServer sleeping = s.server;
    sleeping.state = "sleeping";
    CHECK(db_restart_text(s, sleeping, Lang::En).find("isn't running now") != std::string::npos);
    DbSeries unused = s;
    unused.used_by.clear();
    CHECK(db_restart_text(unused, DbServer{}, Lang::En) == "Applies when saved. Nothing uses MariaDB 11.4 right now.");
    CHECK(db_names_text({"blog", "_shared", "shop"}, Lang::En) == "blog, shop and shared sites");
    CHECK(db_names_text({"_shared"}, Lang::En) == "shared sites");
    CHECK(db_names_text({}, Lang::En).empty());
    CHECK(db_password_problem("short") == std::optional<Str>(Str::DbPasswordShort));
    CHECK(db_password_problem("has space here") == std::optional<Str>(Str::DbPasswordChars));
    CHECK(db_password_problem(std::string(101, 'x')) == std::optional<Str>(Str::DbPasswordLong));
    CHECK_FALSE(db_password_problem("Good-Pass_1"));
    // Every field's key is the canonical name config.json keeps.
    for (const DbIniField& f : settings_db_ini_fields()) {
        CHECK(f.key == db_option_base(f.label));
    }
    CHECK(settings_db_ini_fields().size() == 8);
}

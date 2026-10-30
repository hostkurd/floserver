// The window's model (src/ui-win32/model): words, rows, addresses, colours.

#include "model/theme.h"
#include "model/ui_strings.h"
#include "model/view_model.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

using namespace floserver::ui;
using nlohmann::json;

namespace {
// U+202A (LRE) and U+202C (PDF) in UTF-8, spelled out byte by byte.
const std::string kLri{'\xE2', '\x80', '\xAA'};
const std::string kPdi{'\xE2', '\x80', '\xAC'};

json one(json project) { return json{{"projects", json::array({std::move(project)})}}; }
}  // namespace

TEST_CASE("the language comes from FLOSERVER_LANG, English otherwise") {
    CHECK(lang_from_setting(nullptr) == Lang::En);
    CHECK(lang_from_setting("") == Lang::En);
    CHECK(lang_from_setting("fr") == Lang::En);
    CHECK(lang_from_setting("ar") == Lang::Ar);
    CHECK(lang_from_setting("AR-iq") == Lang::Ar);
    CHECK(lang_from_setting("ku") == Lang::Ku);
    CHECK(lang_from_setting("ckb_IQ") == Lang::Ku);
    CHECK(lang_from_setting("en-GB") == Lang::En);
    CHECK_FALSE(is_rtl(Lang::En));
    CHECK(is_rtl(Lang::Ar));
    CHECK(is_rtl(Lang::Ku));
}

TEST_CASE("every string exists in every language") {
    for (int i = 0; i < static_cast<int>(Str::Count_); ++i) {
        const auto key = static_cast<Str>(i);
        CAPTURE(i);
        CHECK_FALSE(text(Lang::En, key).empty());
        CHECK_FALSE(text(Lang::Ar, key).empty());
        CHECK_FALSE(text(Lang::Ku, key).empty());
    }
}

TEST_CASE("the mockups' Arabic words are used") {
    CHECK(text(Lang::Ar, Str::StateOn) == "يعمل");
    CHECK(text(Lang::Ar, Str::StateAsleep) == "نائم");
    CHECK(text(Lang::Ar, Str::StateOff) == "متوقف");
    CHECK(text(Lang::Ar, Str::Projects) == "المشاريع");
}

TEST_CASE("values are isolated left to right only in right-to-left languages") {
    CHECK(fill(Lang::En, Str::Needs, {"php 8.2"}) == "Needs php 8.2");
    CHECK(fill(Lang::Ar, Str::Needs, {"php 8.2"}) == "يحتاج " + kLri + "php 8.2" + kPdi);
    CHECK(fill_pattern("{1} and {0}", false, {"a", "b"}) == "b and a");
    CHECK(fill_pattern("{2} stays out", false, {"a"}) == " stays out");
    CHECK(fill_pattern("{", false, {"a"}) == "{");
    // Typed text follows its first letter.
    CHECK(isolate_auto("shop 2") == kLri + "shop 2" + kPdi);
    const std::string rle{'\xE2', '\x80', '\xAB'};
    CHECK(isolate_auto("12 متجر") == rle + "12 متجر" + kPdi);
    CHECK(isolate_auto("") == kLri + kPdi);
}

TEST_CASE("rows follow the service's state and run fields") {
    const json list = {{"projects",
                        json::array({
                            {{"name", "shop"}, {"on", true}, {"state", "ready"}, {"run", "running"},
                             {"address", "https://shop.localhost"}, {"php", "8.4.27"},
                             {"database", "mariadb-11.4"}, {"path", "C:/sites/shop"}},
                            {{"name", "api"}, {"on", true}, {"state", "ready"}, {"run", "sleeping"},
                             {"address", "http://api.localhost:8080"}, {"php", "8.5.1"}},
                            {{"name", "landing"}, {"on", false}, {"state", "off"}, {"path", "C:/sites/landing"}},
                            {{"name", "old"}, {"on", true}, {"state", "needs_download"},
                             {"missing", json::array({"php 7.4", "mariadb 10.6"})}},
                            {{"name", "broken"}, {"on", true}, {"state", "problem"}, {"problem", "bad floserver.json"}},
                            {{"name", "crash"}, {"on", true}, {"state", "ready"}, {"run", "failed"},
                             {"address", "http://crash.localhost"}},
                            {{"on", true}},  // no name: skipped
                        })}};
    const auto rows = rows_from_list(list, Lang::En);
    REQUIRE(rows.size() == 6);

    CHECK(rows[0].state == RowState::On);
    CHECK(rows[0].on);
    CHECK(rows[0].address == "https://shop.localhost");
    CHECK(rows[0].detail.empty());
    CHECK(rows[0].chips == std::vector<std::string>{"PHP 8.4.27", "MariaDB 11.4"});

    CHECK(rows[1].state == RowState::Asleep);
    CHECK(rows[1].chips == std::vector<std::string>{"PHP 8.5.1"});

    CHECK(rows[2].state == RowState::Off);
    CHECK_FALSE(rows[2].on);
    CHECK(rows[2].address.empty());
    CHECK(rows[2].detail == "C:/sites/landing");
    CHECK(rows[2].chips.empty());

    CHECK(rows[3].state == RowState::NeedsDownload);
    CHECK(rows[3].detail == "Needs php 7.4, mariadb 10.6");

    CHECK(rows[4].state == RowState::Problem);
    CHECK(rows[4].detail == "bad floserver.json");

    CHECK(rows[5].state == RowState::Problem);
    CHECK(rows[5].detail == "PHP can't start");
    CHECK(rows[5].address.empty());
}

TEST_CASE("Node projects show their version and a server that can't start") {
    const auto rows = rows_from_list(
        json{{"projects", json::array({{{"name", "web"}, {"on", true}, {"state", "ready"}, {"run", "starting"},
                                        {"address", "http://web.localhost"}, {"node", "24.22.1"}},
                                       {{"name", "bad"}, {"on", true}, {"state", "ready"}, {"run", "failed"},
                                        {"node", "24.22.1"}, {"error", "npm run dev ended (exit 1)"}}})}},
        Lang::En);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].state == RowState::Waking);  // its own server is starting
    CHECK(rows[0].address == "http://web.localhost");
    CHECK(rows[0].chips == std::vector<std::string>{"Node 24.22.1"});
    CHECK(rows[1].state == RowState::Problem);
    CHECK(rows[1].detail == "The project's server can't start");
}

TEST_CASE("Apache projects show an Apache chip (D6)") {
    const auto rows = rows_from_list(one({{"name", "wp"}, {"on", true}, {"state", "ready"}, {"run", "failed"},
                                          {"php", "8.4.27"}, {"web", "apache"}}),
                                     Lang::En);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].chips == std::vector<std::string>{"PHP 8.4.27", "Apache"});
    CHECK(rows[0].detail == "The project's server can't start");
}

TEST_CASE("Python projects show a Python chip (D17)") {
    const auto rows = rows_from_list(
        one({{"name", "api"}, {"on", true}, {"state", "ready"}, {"run", "failed"}, {"python", "3.13.18"},
             {"database", "mariadb-11.4"}}),
        Lang::En);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].chips == std::vector<std::string>{"Python 3.13.18", "MariaDB 11.4"});
    CHECK(rows[0].detail == "The project's server can't start");
}

TEST_CASE("an On project that isn't served yet shows its folder") {
    const auto rows = rows_from_list(one({{"name", "new"}, {"on", true}, {"state", "ready"}, {"path", "D:/new"}}),
                                     Lang::Ar);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].state == RowState::On);
    CHECK(rows[0].detail == kLri + "D:/new" + kPdi);
}

TEST_CASE("an Off project that would need a download says so") {
    const auto rows = rows_from_list(
        one({{"name", "x"}, {"on", false}, {"state", "off"}, {"missing", json::array({"php 8.2"})}}), Lang::En);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].state == RowState::Off);
    CHECK(rows[0].detail == "Needs php 8.2");
}

TEST_CASE("odd replies give no rows") {
    CHECK(rows_from_list(json(nullptr), Lang::En).empty());
    CHECK(rows_from_list(json{{"projects", "x"}}, Lang::En).empty());
    CHECK(rows_from_list(json{{"projects", json::array({1, "a", nullptr})}}, Lang::En).empty());
}

TEST_CASE("database chips") {
    CHECK(database_chip("mariadb-11.4") == "MariaDB 11.4");
    CHECK(database_chip("mysql-8.4") == "MySQL 8.4");
    CHECK(database_chip("postgres-17") == "PostgreSQL 17");
    CHECK(database_chip("sqlite") == "sqlite");
}

TEST_CASE("only local project addresses can be opened") {
    for (const char* good : {"http://shop.localhost", "https://shop.localhost", "https://shop.localhost:8443",
                             "http://my-shop.localhost:1", "http://a.b.localhost/", "https://SHOP.LOCALHOST:65535"}) {
        CAPTURE(good);
        CHECK(is_openable_address(good));
    }
    for (const char* bad :
         {"", "shop.localhost", "file:///C:/x", "ftp://shop.localhost", "http://localhost", "http://.localhost",
          "http://shop.localhost:0", "http://shop.localhost:65536", "http://shop.localhost:", "http://shop.localhost:80a",
          "http://shop.localhost/path", "http://shop.localhost.evil.com", "http://evil.com/.localhost",
          "http://user@shop.localhost", "http://shop..localhost", "http://-shop.localhost", "http://shop.localhost?x",
          "http://shop.localhost#x", "http://sh op.localhost", "https://shop.localhost:123456", "javascript:alert(1)",
          "http://shop.localhost\\..\\x", "http://shop.localhost//"}) {
        CAPTURE(bad);
        CHECK_FALSE(is_openable_address(bad));
    }
}

TEST_CASE("search matches the name or the address, ignoring case") {
    ProjectRow row;
    row.name = "My-Shop";
    row.address = "https://my-shop.localhost";
    CHECK(row_matches(row, ""));
    CHECK(row_matches(row, "   "));
    CHECK(row_matches(row, "shop"));
    CHECK(row_matches(row, " SHOP "));
    CHECK(row_matches(row, "localhost"));
    CHECK_FALSE(row_matches(row, "blog"));
}

TEST_CASE("sizes use Western digits and a dot") {
    CHECK(human_bytes(0) == "0.0 KB");
    CHECK(human_bytes(1536) == "1.5 KB");
    CHECK(human_bytes(std::uint64_t{1024} * 1024U) == "1.0 MB");
    CHECK(human_bytes(24536678) == "23.4 MB");
}

TEST_CASE("the footer shows only what the service gave") {
    std::vector<ProjectRow> rows(3);
    rows[0].state = RowState::On;
    rows[0].on = true;
    rows[1].state = RowState::Asleep;
    rows[1].on = true;
    rows[2].state = RowState::Off;

    const json status = {{"memory_bytes", 24536678}};
    const json web = {{"nginx", {{"running", true}, {"http_port", 80}, {"https_port", 443}}}};
    auto f = footer_text(rows, &status, &web, Lang::En);
    CHECK(f.counts == "1 running · 1 sleeping");
    CHECK(f.nginx == "nginx on ports 80 and 443");
    CHECK(f.memory == "Service memory: 23.4 MB");
    CHECK(f.any_running);

    const json no_memory = {{"memory_bytes", nullptr}};
    const json stopped = {{"nginx", {{"running", false}}}};
    f = footer_text(rows, &no_memory, &stopped, Lang::En);
    CHECK(f.memory.empty());
    CHECK(f.nginx == "nginx isn't running");

    f = footer_text({}, nullptr, &stopped, Lang::En);
    CHECK(f.nginx.empty());
    CHECK(f.memory.empty());
    CHECK(f.counts == "0 running · 0 sleeping");
    CHECK_FALSE(f.any_running);

    f = footer_text(rows, &status, &web, Lang::Ar);
    CHECK(f.counts == "يعمل: 1 · نائم: 1");
    CHECK(f.memory == "ذاكرة الخدمة: " + kLri + "23.4 MB" + kPdi);
}

TEST_CASE("colours: both themes are readable") {
    for (const bool dark : {false, true}) {
        CAPTURE(dark);
        const Palette& p = palette(dark);
        CHECK(contrast(p.text, p.card) >= 4.5);
        CHECK(contrast(p.text_subtle, p.card) >= 4.5);
        CHECK(contrast(p.text_subtle, p.footer) >= 4.5);
        CHECK(contrast(p.link, p.card) >= 4.5);
        CHECK(contrast(p.accent_text, p.accent) >= 4.5);
        CHECK(contrast(p.chip_text, p.chip) >= 4.5);
        for (const RowState s :
             {RowState::On, RowState::Asleep, RowState::Waking, RowState::Off, RowState::NeedsDownload,
              RowState::Problem}) {
            CAPTURE(static_cast<int>(s));
            CHECK(contrast(state_colors(p, s).text, state_colors(p, s).fill) >= 4.5);
        }
        CHECK(contrast(p.banner_info.text, p.banner_info.fill) >= 4.5);
        CHECK(contrast(p.banner_error.text, p.banner_error.fill) >= 4.5);
        CHECK(contrast(p.text, p.selected) >= 4.5);
        CHECK(contrast(p.link, p.selected) >= 4.5);
        CHECK(contrast(p.text_subtle, p.panel_box) >= 4.5);
        CHECK(contrast(p.log_text, p.log) >= 4.5);
        CHECK(contrast(p.log_subtle, p.log) >= 4.5);
    }
    CHECK(palette(false).window != palette(true).window);
}

TEST_CASE("rows carry what floserver.json asks for, and an nginx chip") {
    const auto rows = rows_from_list(
        one({{"name", "shop"}, {"on", true}, {"state", "ready"}, {"run", "running"}, {"php", "8.4.27"},
             {"database", "mariadb-11.4"}, {"path", "C:/sites/shop"},
             {"wants", {{"web", "nginx"}, {"php", "8.4"}, {"database", "mariadb-11.4"}}}}),
        Lang::En);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].chips == std::vector<std::string>{"PHP 8.4.27", "MariaDB 11.4", "nginx"});
    CHECK(rows[0].wants.known);
    CHECK(rows[0].wants.php == "8.4");
    CHECK(rows[0].wants.web == "nginx");
    CHECK(rows[0].path == "C:/sites/shop");
    CHECK(rows[0].php == "8.4.27");
}

TEST_CASE("the panel's idle time comes from web.status") {
    const std::vector<ProjectRow> rows;
    CHECK(footer_text(rows, nullptr, nullptr, Lang::En).idle_minutes == 0);
    const json web = {{"idle_after_s", 600}};
    CHECK(footer_text(rows, nullptr, &web, Lang::En).idle_minutes == 10);
    const json quick = {{"idle_after_s", 2}};
    CHECK(footer_text(rows, nullptr, &quick, Lang::En).idle_minutes == 1);
}

TEST_CASE("version series") {
    CHECK(version_series("php", "8.4.27") == "8.4");
    CHECK(version_series("php", "8.4") == "8.4");
    CHECK(version_series("mariadb", "11.4.13") == "11.4");
    CHECK(version_series("node", "24.22.1") == "24");
    CHECK(version_series("node", "24") == "24");
    CHECK(version_series("python", "3.13.18") == "3.13");
}

TEST_CASE("the panel's version pickers list what's installed and what's asked for") {
    const Installed installed = installed_from_list(json{
        {"installed",
         json::array({{{"name", "php"}, {"version", "8.4.27"}},
                      {{"name", "php"}, {"version", "8.10.1"}},
                      {{"name", "php"}, {"version", "8.5.12"}},
                      {{"name", "mariadb"}, {"version", "11.4.13"}},
                      {{"name", "node"}, {"version", "22.23.4"}},
                      {{"name", "node"}, {"version", "24.22.1"}},
                      {{"name", "nginx"}, {"version", "1.30.6"}},
                      {{"version", "1"}}})}});
    CHECK(installed.at("php") == std::vector<std::string>{"8.10.1", "8.5.12", "8.4.27"});

    ProjectRow row;
    row.wants.known = true;
    row.wants.php = "8.4";
    row.wants.database = "mariadb-11.4";

    auto fields = version_fields(row, installed, Lang::En);
    REQUIRE(fields.size() == 4);  // no Python installed or asked for
    const VersionField& php = fields[0];
    CHECK(php.key == "php");
    REQUIRE(php.choices.size() == 4);
    CHECK(php.choices[0].label == "Newest installed");
    CHECK_FALSE(php.choices[0].value);
    CHECK(php.choices[1].label == "8.10");
    CHECK(php.choices[3].value == std::optional<std::string>("8.4"));
    CHECK(php.current == 3);

    const VersionField& db = fields[1];
    CHECK(db.key == "database");
    REQUIRE(db.choices.size() == 2);
    CHECK(db.choices[1].label == "MariaDB 11.4");
    CHECK(db.choices[1].value == std::optional<std::string>("mariadb-11.4"));
    CHECK(db.current == 1);

    CHECK(fields[2].key == "web");
    CHECK(fields[2].current == 0);
    CHECK(fields[3].key == "node");
    REQUIRE(fields[3].choices.size() == 3);
    CHECK(fields[3].choices[1].label == "24");
    CHECK(fields[3].current == 0);  // None

    SUBCASE("asked for but not installed") {
        row.wants.php = "7.4";
        row.wants.python = "3.12";
        row.wants.web = "apache";
        fields = version_fields(row, installed, Lang::En);
        REQUIRE(fields.size() == 5);
        CHECK(fields[0].choices[fields[0].current].label == "7.4 (not installed)");
        CHECK(fields[0].choices[fields[0].current].value == std::optional<std::string>("7.4"));
        CHECK(fields[2].current == 1);
        CHECK(fields[4].key == "python");
        CHECK(fields[4].choices[fields[4].current].label == "3.12 (not installed)");
    }
    SUBCASE("a Node project without PHP says None for PHP") {
        row.wants.php.clear();
        row.wants.node = "24";
        fields = version_fields(row, installed, Lang::En);
        CHECK(fields[0].choices[0].label == "None");
        CHECK(fields[0].current == 0);
        CHECK(fields[3].choices[fields[3].current].label == "24");
    }
}

TEST_CASE("the database port comes from databases.list") {
    const json servers = {{"servers", json::array({{{"series", "11.4"}, {"version", "11.4.13"}, {"port", 3306}},
                                                    {{"series", "11.8"}, {"version", "11.8.9"}, {"port", 3307}}})}};
    CHECK(database_port(servers, "mariadb-11.8") == 3307);
    CHECK(database_port(servers, "mariadb-11.4") == 3306);
    CHECK(database_port(servers, "mariadb-10.6") == 0);
    CHECK(database_port(json::object(), "mariadb-11.4") == 0);
    CHECK(database_port(servers, "") == 0);
}

TEST_CASE("log lines use local time and short durations") {
    const json result = {{"entries", json::array({{{"t", 1000}, {"status", 200}, {"ms", 38}, {"method", "GET"}, {"uri", "/"}},
                                                  {{"t", 2000}, {"status", 302}, {"ms", 1250}, {"method", "POST"},
                                                   {"uri", "/cart"}},
                                                  {{"t", "x"}}})}};
    const auto lines = log_lines(result, [](std::int64_t ms, int& h, int& m, int& s) {
        h = 9;
        m = 5;
        s = static_cast<int>(ms / 1000);
    });
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].time == "09:05:01");
    CHECK(lines[0].duration == "38 ms");
    CHECK(log_text(lines[0]) == "09:05:01  GET  /  200  38 ms");
    CHECK(lines[1].duration == "1.3 s");
    CHECK(log_text(lines[1]) == "09:05:02  POST /cart  302  1.3 s");
    CHECK(log_lines(json::array(), {}).empty());
}

TEST_CASE("the .env lines for the clipboard") {
    const json login = {{"env", json::array({json::array({"DB_TYPE", "mysql"}), json::array({"DB_NAME", "my_shop"}),
                                             json::array({1, 2})})}};
    CHECK(env_text(login) == "DB_TYPE=mysql\r\nDB_NAME=my_shop\r\n");
    CHECK(env_text(json::object()).empty());
}

TEST_CASE("a row's downloads: what's missing, plus nginx when none is installed") {
    ProjectRow row;
    row.missing = {"php", "mariadb 11.4", "php", "bad name", "x 1;rm"};
    Installed installed;
    auto d = downloads_for(row, installed);
    REQUIRE(d.size() == 3);
    CHECK(d[0] == Download{"php", ""});
    CHECK(d[1] == Download{"mariadb", "11.4"});
    CHECK(d[2] == Download{"nginx", ""});
    CHECK(download_button_text(d, Lang::En) == "Download 3 runtimes");

    installed["nginx"] = {"1.30.6"};
    row.missing = {"php 8.2"};
    d = downloads_for(row, installed);
    REQUIRE(d.size() == 1);
    CHECK(download_button_text(d, Lang::En) == "Download PHP 8.2");

    row.missing.clear();
    CHECK(downloads_for(row, installed).empty());
}

TEST_CASE("install progress from jobs.get") {
    CHECK(job_progress_text({{"state", "downloading"}, {"name", "php"}, {"version", "8.5.12"}, {"received", 50},
                             {"total", 200}},
                            Lang::En) == "Downloading PHP 8.5.12… 25%");
    CHECK(job_progress_text({{"state", "queued"}, {"name", "nginx"}}, Lang::En) == "Installing nginx…");
    CHECK(job_progress_text({{"state", "installing"}, {"name", "mariadb"}, {"version", "11.4.13"}}, Lang::En) ==
          "Installing MariaDB 11.4.13…");
    CHECK(job_progress_text({{"state", "done"}, {"name", "php"}}, Lang::En).empty());
    CHECK(job_progress_text(json::object(), Lang::En).empty());
}

TEST_CASE("rows keep what's missing as the service said it") {
    const auto rows = rows_from_list(
        one({{"name", "x"}, {"on", true}, {"state", "needs_download"}, {"missing", json::array({"php", "nginx"})}}),
        Lang::En);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].missing == std::vector<std::string>{"php", "nginx"});
}

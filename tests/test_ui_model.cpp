// The window's model (src/ui-win32/model): words, rows, addresses, colours.

#include "model/php_page.h"
#include "model/ports_page.h"
#include "model/runtimes_page.h"
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
        CHECK(contrast(p.text, p.input_set) >= 4.5);
        CHECK(contrast(p.chip_added.text, p.chip_added.fill) >= 4.5);
        CHECK(contrast(p.link, p.selected) >= 4.5);
        CHECK(contrast(p.text_subtle, p.panel_box) >= 4.5);
        CHECK(contrast(p.log_text, p.log) >= 4.5);
        CHECK(contrast(p.log_subtle, p.log) >= 4.5);
        CHECK(contrast(p.chip_default.text, p.chip_default.fill) >= 4.5);
        CHECK(contrast(p.text, p.nav_selected) >= 4.5);
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
    REQUIRE(php.choices.size() == 5);  // None, Newest installed, 8.10, 8.5, 8.4
    CHECK(php.choices[0].label == "None");
    CHECK(php.choices[0].value == std::optional<std::string>("none"));
    CHECK(php.choices[1].label == "Newest installed");
    CHECK(php.choices[1].value == std::optional<std::string>("newest"));
    CHECK(php.choices[2].label == "8.10");
    CHECK(php.choices[4].value == std::optional<std::string>("8.4"));
    CHECK(php.current == 4);
    CHECK_FALSE(php.implicit);
    for (const Choice& c : php.choices) {
        CHECK(c.value.has_value());  // every PHP choice writes the key
    }
    CHECK(php_off_note(row, Lang::En).empty());

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
    SUBCASE("a Node project without PHP has None ticked, picking it writes false") {
        row.wants.php.clear();
        row.wants.node = "24";
        fields = version_fields(row, installed, Lang::En);
        CHECK(fields[0].choices[0].label == "None");
        CHECK(fields[0].current == 0);
        CHECK(fields[0].implicit);
        CHECK(fields[3].choices[fields[3].current].label == "24");
    }
    SUBCASE("without the key: what's in effect is ticked and still writes the key") {
        row.wants.php.clear();
        row.static_site = true;
        fields = version_fields(row, installed, Lang::En);
        REQUIRE(fields[0].choices.size() == 5);  // nothing "not installed" added
        CHECK(fields[0].current == 0);
        CHECK(fields[0].implicit);
        CHECK(fields[0].choices[0].label == "None");
        CHECK(fields[0].choices[0].value == std::optional<std::string>("none"));
        row.static_site = false;
        fields = version_fields(row, installed, Lang::En);
        CHECK(fields[0].current == 1);
        CHECK(fields[0].implicit);
        CHECK(fields[0].choices[1].label == "Newest installed");
        CHECK(fields[0].choices[1].value == std::optional<std::string>("newest"));
        for (const Lang lang : {Lang::Ar, Lang::Ku}) {
            const auto translated = version_fields(row, installed, lang);
            for (const Choice& c : translated[0].choices) {
                CHECK(c.value.has_value());
            }
        }
    }
    SUBCASE("\"php\": \"newest\" picks Newest installed, even with no .php files yet") {
        row.wants.php = "newest";
        row.static_site = false;
        fields = version_fields(row, installed, Lang::En);
        CHECK(fields[0].choices.size() == 5);
        CHECK(fields[0].current == 1);
        CHECK_FALSE(fields[0].implicit);
    }
    SUBCASE("\"php\": false picks None, and says so when there are .php files") {
        row.wants.php.clear();
        row.wants.php_off = true;
        row.static_site = true;
        fields = version_fields(row, installed, Lang::En);
        CHECK(fields[0].current == 0);
        CHECK_FALSE(fields[0].implicit);
        CHECK(fields[0].choices[0].label == "None");
        CHECK(php_off_note(row, Lang::En).empty());
        row.php_files = true;
        CHECK(php_off_note(row, Lang::En) == "PHP is off: .php files are sent as they are, not run.");
        CHECK_FALSE(php_off_note(row, Lang::Ar).empty());
        CHECK_FALSE(project_uses_php(row));
    }
}

TEST_CASE("projects.list's wants: php false or \"newest\"") {
    const json list = {{"projects", json::array({{{"name", "off"}, {"static", true}, {"php_files", true},
                                                  {"wants", {{"web", "nginx"}, {"php", false}}}},
                                                 {{"name", "newest"}, {"php", "8.5.12"},
                                                  {"wants", {{"web", "nginx"}, {"php", "newest"}}}}})}};
    const auto rows = rows_from_list(list, Lang::En);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].wants.php_off);
    CHECK(rows[0].wants.php.empty());
    CHECK(rows[0].php_files);
    CHECK_FALSE(project_uses_php(rows[0]));
    CHECK(panel_tabs(rows[0]) == std::vector<PanelTab>{PanelTab::General, PanelTab::Log});
    CHECK_FALSE(rows[1].wants.php_off);
    CHECK(rows[1].wants.php == "newest");
    CHECK(project_uses_php(rows[1]));
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
    row.missing = {"php", "mariadb 11.4", "php", "bad name", "x 1;rm", "php 8..2", "php .8"};
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

TEST_CASE("template cards come from templates.list in the window's language") {
    const json list = {{"templates",
                        json::array({{{"id", "wordpress"},
                                      {"title", {{"en", "WordPress"}}},
                                      {"description", {{"en", "Latest WordPress"}, {"ar", "أحدث WordPress"}}},
                                      {"mark", "W"},
                                      {"mark_bg", "#E3F2FD"},
                                      {"mark_fg", "#1565C0"},
                                      {"chips", json::array({"PHP", "MariaDB", "Apache"})},
                                      {"config", {{"php", "8.4"}, {"database", "mariadb-11.4"}, {"web", "apache"}}}},
                                     {{"id", "node-app"}, {"title", {{"en", "Node app"}}}, {"config", {{"node", "24"}}}},
                                     {{"title", {{"en", "no id"}}}}})}};
    const auto cards = cards_from_list(list, Lang::Ar);
    REQUIRE(cards.size() == 2);
    CHECK(cards[0].title == "WordPress");          // no Arabic title: English
    CHECK(cards[0].description == "أحدث WordPress");
    CHECK(cards[0].mark_bg == 0xE3F2FDU);
    CHECK(cards[0].uses_php);
    CHECK(cards[0].config.web == "apache");

    const Installed installed = {{"php", {"8.5.12"}}, {"node", {"24.22.1"}}};
    const auto wp = template_fields(cards[0], installed, Lang::En);
    REQUIRE(wp.size() == 3);  // PHP, Database, Web server
    // The template names PHP 8.4; 8.5 is installed and newer, so it's picked
    // and 8.4 stays in the list to download by hand.
    CHECK(wp[0].choices[wp[0].current].label == "8.5");
    CHECK(wp[0].choices.back().label == "8.4 (not installed)");
    CHECK(wp[1].choices[wp[1].current].label == "MariaDB 11.4 (not installed)");  // none installed
    CHECK(wp[2].current == 1);  // Apache
    const auto node = template_fields(cards[1], installed, Lang::En);
    REQUIRE(node.size() == 1);
    CHECK(node[0].key == "node");
    CHECK(node[0].choices[node[0].current].label == "24");
}

TEST_CASE("template versions: an installed same-or-newer series is picked, an older one isn't") {
    TemplateCard wp;
    wp.uses_php = true;
    wp.config.php = "8.4";
    wp.config.database = "mariadb-11.4";
    // MariaDB 12.1 installed on the first run: WordPress uses it, no download.
    auto f = template_fields(wp, {{"php", {"8.4.20"}}, {"mariadb", {"12.1.2", "11.8.5"}}}, Lang::En);
    REQUIRE(f.size() == 3);
    CHECK(f[0].choices[f[0].current].label == "8.4");
    CHECK(f[1].choices[f[1].current].value == "mariadb-12.1");
    CHECK(f[1].choices.back().label == "MariaDB 11.4 (not installed)");
    // Only an older series: the template's own is kept, to download.
    f = template_fields(wp, {{"php", {"8.3.9"}}, {"mariadb", {"10.11.9"}}}, Lang::En);
    CHECK(f[0].choices[f[0].current].label == "8.4 (not installed)");
    CHECK(f[1].choices[f[1].current].label == "MariaDB 11.4 (not installed)");
}

TEST_CASE("template versions: Settings' default is picked when installed and not older") {
    TemplateCard wp;
    wp.uses_php = true;
    wp.config.php = "8.3";
    wp.config.database = "mariadb-11.4";
    const Installed installed = {{"php", {"8.5.1", "8.4.16", "8.2.30"}}, {"mariadb", {"11.8.5", "11.4.9"}}};
    // No defaults: today's rule (the newest same-or-newer... here 8.3 isn't
    // installed, so the newest installed is picked).
    auto f = template_fields(wp, installed, Lang::En);
    CHECK(f[0].choices[f[0].current].label == "8.5");
    CHECK(f[1].choices[f[1].current].value == "mariadb-11.4");
    // The default 8.4 is newer than the template's 8.3: it wins over 8.5.
    f = template_fields(wp, installed, Lang::En, {{"php", "8.4"}, {"mariadb", "11.8"}});
    CHECK(f[0].choices[f[0].current].value == "8.4");
    CHECK(f[1].choices[f[1].current].value == "mariadb-11.8");
    // An older default isn't given to the template; it keeps today's pick.
    f = template_fields(wp, installed, Lang::En, {{"php", "8.2"}});
    CHECK(f[0].choices[f[0].current].label == "8.5");
    // A default that isn't installed changes nothing.
    f = template_fields(wp, installed, Lang::En, {{"php", "8.1"}, {"mariadb", "12.1"}});
    CHECK(f[0].choices[f[0].current].label == "8.5");
    CHECK(f[1].choices[f[1].current].value == "mariadb-11.4");
    // A template naming no PHP takes the default; one without a database isn't given one.
    TemplateCard bare;
    bare.uses_php = true;
    f = template_fields(bare, installed, Lang::En, {{"php", "8.2"}, {"mariadb", "11.8"}});
    CHECK(f[0].choices[f[0].current].value == "8.2");
    CHECK_FALSE(f[1].choices[f[1].current].value.has_value());
    // Node: the series is the major.
    TemplateCard node;
    node.config.node = "22";
    f = template_fields(node, {{"node", {"24.12.0", "22.21.1"}}}, Lang::En, {{"node", "24"}});
    REQUIRE(f.size() == 1);
    CHECK(f[0].choices[f[0].current].value == "24");
}

TEST_CASE("the defaults come from runtimes.list") {
    const auto d = defaults_from_list({{"defaults", {{"php", "8.5"}, {"node", 24}, {"python", ""}}}});
    CHECK(d.size() == 1);
    CHECK(d.at("php") == "8.5");
    CHECK(defaults_from_list(json()).empty());
}

namespace {
// runtimes.list's full answer as in the Settings mockup.
json runtimes_reply() {
    return {{"installed",
             json::array({{{"name", "php"}, {"version", "8.5.1"}, {"variant", "nts"}, {"folder", "8.5.1"},
                           {"used_by", json::array({"blog", "shop", "wiki", "_shared"})}},
                          {{"name", "php"}, {"version", "8.4.16"}, {"variant", "nts"}, {"folder", "8.4.16"},
                           {"used_by", json::array({"old"})}},
                          {{"name", "mariadb"}, {"version", "11.8.5"}, {"variant", ""}, {"folder", "11.8.5"},
                           {"used_by", json::array({"blog", "shop"})}},
                          {{"name", "node"}, {"version", "24.12.0"}, {"variant", ""}, {"folder", "24.12.0"},
                           {"used_by", json::array()}},
                          {{"name", "nginx"}, {"version", "1.30.6"}, {"variant", ""}, {"folder", "1.30.6"},
                           {"used_by", json::array({"FloServer"})}},
                          {{"name", "apache"}, {"version", "2.4.66"}, {"variant", ""}, {"folder", "2.4.66"},
                           {"used_by", json::array({"_shared"})}}})},
            {"available",
             json::array({{{"name", "php"}, {"version", "8.5.1"}, {"variant", "nts"}, {"size", 31000000}, {"installed", true}},
                          {{"name", "php"}, {"version", "8.5.1"}, {"variant", "ts"}, {"size", 32000000}, {"installed", false}},
                          {{"name", "php"}, {"version", "8.3.29"}, {"variant", "nts"}, {"size", 32505856}},
                          {{"name", "php"}, {"version", "8.2.30"}, {"variant", "nts"}, {"size", 31457280}},
                          {{"name", "mariadb"}, {"version", "11.4.9"}, {"variant", ""}, {"size", 81788928}},
                          {{"name", "node"}, {"version", "22.21.1"}, {"variant", ""}, {"size", 30000000}},
                          {{"name", "python"}, {"version", "3.13.9"}, {"variant", ""}, {"size", 30000000}},
                          {{"name", "python"}, {"version", "3.12.12"}, {"variant", ""}, {"size", 30000000}}})},
            {"list", {{"sequence", 42}, {"expires", "2027-01-28T00:00:00Z"}}},
            {"defaults", json::object()}};
}
}  // namespace

TEST_CASE("Settings > Runtimes: groups and rows from runtimes.list") {
    const json databases = {{"servers", json::array({{{"series", "11.8"}, {"version", "11.8.5"}, {"port", 3306},
                                                       {"state", "running"}}})}};
    const RuntimesPage page = runtimes_page(runtimes_reply(), nullptr, databases, Lang::En);
    CHECK(page.has_list);
    CHECK(page.hint.find("number 42, good until 2027-01-28") != std::string::npos);
    REQUIRE(page.groups.size() == 5);
    CHECK(page.groups[0].title == "PHP");
    CHECK(page.groups[1].title == "MariaDB");
    CHECK(page.groups[2].title == "Web servers");  // full width, after MariaDB
    CHECK(page.groups[0].wide);
    CHECK(page.groups[2].wide);
    CHECK(page.groups[3].title == "Node.js");
    CHECK_FALSE(page.groups[3].wide);

    const auto& php = page.groups[0].rows;
    REQUIRE(php.size() == 4);  // 8.5.1 once (the ts build isn't offered), 8.4.16, 8.3.29, 8.2.30
    CHECK(php[0].version == "8.5.1");
    CHECK(php[0].installed);
    CHECK(php[0].is_default);  // no default set: the newest installed
    CHECK(php[0].usage == "3 projects · Shared sites");
    CHECK(php[0].used_by == "blog, shop, wiki, Shared sites");
    CHECK(php[0].action == RowAction::Remove);
    CHECK(php[1].usage == "1 project");
    CHECK_FALSE(php[1].is_default);
    CHECK(php[2].version == "8.3.29");
    CHECK_FALSE(php[2].installed);
    CHECK(php[2].action == RowAction::Download);
    CHECK(size_text(php[2].size) == "31 MB");

    // The pickers: Newest installed, then each installed series.
    REQUIRE(page.groups[0].picker);
    const VersionField& picker = *page.groups[0].picker;
    REQUIRE(picker.choices.size() == 3);
    CHECK(picker.choices[0].label == "Newest installed");
    CHECK_FALSE(picker.choices[picker.current].value.has_value());
    CHECK(picker.choices[1].value == "8.5");
    CHECK(picker.choices[2].value == "8.4");
    CHECK_FALSE(page.groups[3].picker);  // Node.js has none

    CHECK(page.groups[1].rows[0].usage == "2 projects · running on port 3306");
    CHECK(page.groups[1].rows[1].action == RowAction::Download);

    const auto& node = page.groups[3].rows;
    REQUIRE(node.size() == 2);
    CHECK(node[0].is_default);
    CHECK(node[0].used_by.empty());

    const auto& web = page.groups[2].rows;
    REQUIRE(web.size() == 2);
    CHECK(web[0].label == "nginx 1.30.6");
    CHECK(web[0].always_used);
    CHECK(web[0].action == RowAction::None);
    CHECK(web[1].label == "Apache 2.4.66");
    CHECK(web[1].note.empty());  // the row shows its size or who uses it
    CHECK(web[1].used_by == "Shared sites");
}

TEST_CASE("Settings > Runtimes: the stored default, and what the poll says is installed") {
    json reply = runtimes_reply();
    reply["defaults"] = {{"php", "8.4"}, {"mariadb", "10.11"}};
    const RuntimesPage page = runtimes_page(reply, nullptr, json(), Lang::En);
    const auto& php = page.groups[0].rows;
    CHECK_FALSE(php[0].is_default);
    CHECK(php[1].is_default);
    CHECK(page.groups[0].picker->choices[page.groups[0].picker->current].value == "8.4");
    // A default that isn't installed: shown in the picker, the newest gets the chip.
    const VersionField& db = *page.groups[1].picker;
    CHECK(db.choices[db.current].label == "10.11 (not installed)");
    CHECK(page.groups[1].rows[0].is_default);

    // The 2 s poll wins: 8.4.16 was removed, 8.3.29 installed since the reply.
    const Installed now = {{"php", {"8.5.1", "8.3.29"}}, {"mariadb", {"11.8.5"}}, {"nginx", {"1.30.6"}}};
    const RuntimesPage later = runtimes_page(reply, &now, json(), Lang::En);
    const auto& rows = later.groups[0].rows;
    REQUIRE(rows.size() == 3);  // 8.4.16 isn't on offer, so its row goes
    CHECK(rows[1].version == "8.3.29");
    CHECK(rows[1].installed);
    // Its folder isn't known until the next list reply (it could be a
    // variant build), so it can't be removed yet.
    CHECK(rows[1].folder.empty());
    CHECK(rows[1].action == RowAction::None);
    CHECK(rows[1].used_by.empty());
    // Node and Apache were removed since: only to download now.
    REQUIRE(later.groups.size() == 5);
    CHECK(later.groups[3].title == "Node.js");
    CHECK(later.groups[3].rows[0].action == RowAction::Download);
}

TEST_CASE("Settings > Runtimes: no list yet, and a check that failed") {
    json cached = runtimes_reply();
    cached["list"] = nullptr;
    cached["list_error"] = "there's no runtime list on this computer yet";
    const RuntimesPage page = runtimes_page(cached, nullptr, json(), Lang::En);
    CHECK_FALSE(page.has_list);
    CHECK(page.hint.find("no list of versions") != std::string::npos);
    for (const RuntimeGroup& g : page.groups) {
        for (const RuntimeRow& r : g.rows) {
            CHECK(r.installed);  // only what's installed
        }
    }
    CHECK(page.groups.size() == 4);  // no Python

    // A failed check keeps the versions on offer from before.
    json fresh = cached;
    fresh["installed"] = json::array();
    const json merged = merge_runtimes_reply(runtimes_reply(), fresh);
    CHECK(merged["list"]["sequence"].get<int>() == 42);
    CHECK(merged["available"].size() == 8);
    CHECK(merged["installed"].empty());
    CHECK_FALSE(merged.contains("list_error"));
    // A good answer is taken as it is.
    CHECK(merge_runtimes_reply(cached, runtimes_reply())["installed"].size() == 6);
}

TEST_CASE("Settings > Runtimes: Arabic words and isolated names") {
    const RuntimesPage page = runtimes_page(runtimes_reply(), nullptr, json(), Lang::Ar);
    CHECK(page.groups[0].rows[0].usage == "المشاريع: 3 · المواقع المشتركة");
    CHECK(page.groups[0].rows[0].used_by.starts_with(kLri + "blog" + kPdi + ", "));
    CHECK(page.groups[0].picker->choices[0].label == "أحدث إصدار مثبّت");
}

TEST_CASE("Settings > Runtimes: downloads") {
    CHECK(size_text(std::uint64_t{512} * 1024U) == "512 KB");
    CHECK(size_text(8912896) == "8.5 MB");
    CHECK(size_text(8388608) == "8 MB");
    CHECK(size_text(std::uint64_t{19} * 1024U * 1024U + 300000U) == "19 MB");

    const auto job = runtime_job({{"id", 7}, {"name", "php"}, {"requested", "8.3.29"}, {"version", ""},
                                  {"state", "downloading"}, {"received", std::uint64_t{19} * 1024U * 1024U}, {"total", std::uint64_t{31} * 1024U * 1024U}});
    REQUIRE(job);
    CHECK(job->version == "8.3.29");  // what was asked for until it's known
    CHECK(job_text(*job, Lang::En) == "Downloading… 19 MB of 31 MB");
    REQUIRE(job_fraction(*job));
    CHECK(*job_fraction(*job) == doctest::Approx(19.0 / 31.0));
    RuntimeJob starting = *job;
    starting.state = "checking";
    CHECK(job_text(starting, Lang::En) == "Starting…");
    CHECK_FALSE(job_fraction(starting));
    starting.state = "installing";
    CHECK(job_text(starting, Lang::En) == "Installing…");
    CHECK_FALSE(runtime_job({{"name", "php"}}));

    RuntimeRow row;
    row.name = "php";
    row.version = "8.3.29";
    const std::vector<RuntimeJob> jobs{*job};
    CHECK(job_for(row, jobs) == jobs.data());
    row.version = "8.2.30";
    CHECK(job_for(row, jobs) == nullptr);

    CHECK(settings_install_key("php", "8.3.29") == "_settings/php/8.3.29");
    CHECK(is_settings_key("_settings/php/8.3.29"));
    CHECK_FALSE(is_settings_key("shop"));
    CHECK_FALSE(is_project_name("_settings"));
}

TEST_CASE("new project names") {
    std::vector<ProjectRow> rows(2);
    rows[0].name = "php-app";
    rows[1].name = "php-app-2";
    CHECK(free_project_name("php-app", rows) == "php-app-3");
    CHECK(free_project_name("blog", rows) == "blog");
    CHECK(is_project_name("my-shop2"));
    CHECK_FALSE(is_project_name("My Shop"));
    CHECK_FALSE(is_project_name("-shop"));
    CHECK_FALSE(is_project_name("xn--abc"));
    CHECK_FALSE(is_project_name(""));
}

TEST_CASE("the Shared sites row: from shared.get's answer, without the folders") {
    const Installed installed = {{"php", {"8.5.12", "8.4.27"}}, {"mariadb", {"11.4.13"}}, {"nginx", {"1.30.6"}}};
    // Off: the hint, with the www folder.
    SharedSites s = shared_from_reply({{"on", false}, {"dir", "C:/Users/me/FloServer/www"}, {"web", "apache"},
                                       {"status", nullptr}});
    CHECK(s.known);
    auto row = shared_row(s, installed, Lang::En);
    CHECK(row.state == RowState::Off);
    CHECK(row.detail == "Every folder in C:/Users/me/FloServer/www opens at localhost/<folder>/");
    CHECK(row.address.empty());
    REQUIRE(row.chips.size() == 2);
    CHECK(row.chips[0] == "PHP 8.5");  // no setting: the newest installed
    CHECK(row.chips[1] == "Apache");

    // On and served: the folder under the title, the address to open.
    s = shared_from_reply({{"on", true}, {"dir", "C:/www"}, {"php", "8.4"}, {"web", "nginx"},
                           {"status", {{"address", "http://localhost:8080"}, {"run", "sleeping"}}}});
    row = shared_row(s, installed, Lang::En);
    CHECK(row.state == RowState::Asleep);
    CHECK(row.detail == "C:/www");
    CHECK(row.address == "localhost:8080");
    CHECK(row.chips == std::vector<std::string>{"PHP 8.4", "nginx"});
    CHECK(shared_row(s, installed, Lang::Ar).detail == kLri + "C:/www" + kPdi);

    // Needs download: what's missing, and what to install for it.
    s = shared_from_reply({{"on", true}, {"dir", "C:/www"}, {"web", "apache"},
                           {"status", {{"state", "needs_download"}, {"missing", {"php", "apache"}}}}});
    row = shared_row(s, {}, Lang::En);
    CHECK(row.state == RowState::NeedsDownload);
    CHECK(row.detail == "Needs php, apache");
    CHECK(row.chips == std::vector<std::string>{"Apache"});
    const auto downloads = shared_downloads(s, {});
    REQUIRE(downloads.size() == 3);
    CHECK(downloads[1] == Download{"apache", ""});
    CHECK(downloads[2] == Download{"nginx", ""});
    s.on = false;
    CHECK(shared_downloads(s, {}).empty());

    // A problem: the service's reason, else that they can't start.
    s = shared_from_reply({{"on", true}, {"status", {{"state", "problem"}, {"problem", "the www folder is gone"}}}});
    CHECK(shared_row(s, installed, Lang::En).state == RowState::Problem);
    CHECK(shared_row(s, installed, Lang::En).detail == "the www folder is gone");
    s = shared_from_reply({{"on", true}, {"status", {{"run", "failed"}, {"address", "http://localhost"}}}});
    row = shared_row(s, installed, Lang::En);
    CHECK(row.detail == "Shared sites can't start");
    CHECK(row.address.empty());
}

TEST_CASE("shared sites' address: only localhost with a port") {
    CHECK(is_shared_address("http://localhost"));
    CHECK(is_shared_address("http://localhost:8080"));
    CHECK(is_shared_address("http://localhost:8080/"));
    CHECK(is_shared_address("https://localhost:8443"));  // shared sites over HTTPS
    CHECK(is_shared_address("https://localhost"));
    CHECK_FALSE(is_shared_address("https://localhost.evil.com"));
    CHECK_FALSE(is_shared_address("ftp://localhost"));
    CHECK_FALSE(is_shared_address("http://localhost.evil.com"));
    CHECK_FALSE(is_shared_address("http://localhost:0"));
    CHECK_FALSE(is_shared_address("http://localhost:70000"));
    CHECK_FALSE(is_shared_address("http://localhost:80/x"));
    CHECK_FALSE(is_shared_address("http://localhost:"));
}

TEST_CASE("the Shared sites settings page's pickers") {
    const Installed installed = {{"php", {"8.5.12", "8.4.27"}}, {"mariadb", {"11.4.13"}}};
    SharedSites s;
    auto fields = shared_fields(s, installed, Lang::En);
    REQUIRE(fields.size() == 3);
    CHECK(fields[0].key == "php");
    REQUIRE(fields[0].choices.size() == 3);
    CHECK(fields[0].choices[0].label == "Newest installed");
    CHECK_FALSE(fields[0].choices[0].value);
    CHECK(fields[0].choices[2].value == "8.4");
    CHECK(fields[0].current == 0);
    CHECK(fields[1].key == "database");
    REQUIRE(fields[1].choices.size() == 2);
    CHECK(fields[1].choices[0].label == "None");
    CHECK(fields[1].choices[1].value == "mariadb-11.4");
    CHECK(fields[2].key == "web");
    CHECK(fields[2].choices[fields[2].current].label == "Apache (.htaccess works)");

    s.php = "8.4";
    s.database = "mariadb-11.4";
    s.web = "nginx";
    fields = shared_fields(s, installed, Lang::En);
    CHECK(fields[0].current == 2);
    CHECK(fields[1].current == 1);
    CHECK(fields[2].choices[fields[2].current].value == "nginx");
}

// --- Settings > PHP and the project panel's PHP part (D20) ---------------------------

namespace {
json php_reply() {
    return json{{"series", "8.5"},
                {"version", "8.5.12"},
                {"extensions_fixed", false},
                {"extensions", json::array({json{{"name", "curl"}, {"on", true}, {"base", true}},
                                            json{{"name", "intl"}, {"on", false}, {"base", true}},
                                            json{{"name", "bcmath"}, {"on", true}, {"base", false}},
                                            json{{"name", "zz_odd"}, {"on", false}, {"base", false}},
                                            json{{"name", "xdebug"}, {"on", false}, {"base", false}},
                                            json{{"name", "gone"}, {"on", true}, {"base", false}, {"missing", true}},
                                            json{{"name", "soap"}, {"on", false}, {"base", false}}})},
                {"ini", {{"memory_limit", "1G"}, {"display_errors", "On"}, {"post_max_size", "512M"}}},
                {"changed", {{"memory_limit", "1G"}}},
                {"custom_ini", "C:/data/php/8.5/custom.ini"},
                {"custom_ini_exists", true},
                {"used_by", json::array({"blog", "shop", "_shared"})}};
}
}  // namespace

TEST_CASE("php.get's answer") {
    CHECK_FALSE(php_series_from_reply(json::object()));
    const auto s = php_series_from_reply(php_reply());
    REQUIRE(s);
    CHECK(s->series == "8.5");
    CHECK(s->version == "8.5.12");
    REQUIRE(s->extensions.size() == 7);
    CHECK(s->extensions[5].missing);
    CHECK(s->changed.at("memory_limit") == "1G");
    CHECK(s->custom_ini_exists);
    CHECK(s->used_by.size() == 3);

    auto null_version = php_reply();
    null_version["version"] = nullptr;
    CHECK(php_series_from_reply(null_version)->version.empty());
}

TEST_CASE("the installed PHP series, newest first") {
    const Installed installed{{"php", {"8.4.27", "8.5.1", "8.5.12", "8.3.29"}}, {"node", {"24.1.0"}}};
    const auto series = installed_php_series(installed);
    REQUIRE(series.size() == 3);
    CHECK(series[0] == std::pair<std::string, std::string>{"8.5", "8.5.12"});
    CHECK(series[1].first == "8.4");
    CHECK(series[2].first == "8.3");
    CHECK(installed_php_series(Installed{}).empty());
}

TEST_CASE("extensions: those on first, then the popular ones, then by name; the find box") {
    const auto s = *php_series_from_reply(php_reply());
    const auto ordered = ordered_extensions(s);
    std::vector<std::string> names;
    names.reserve(ordered.size());
    for (const auto& e : ordered) {
        names.push_back(e.name);
    }
    CHECK(names == std::vector<std::string>{"curl", "bcmath", "gone", "soap", "xdebug", "intl", "zz_odd"});
    CHECK(extension_matches("pdo_mysql", "MYS"));
    CHECK(extension_matches("pdo_mysql", "  "));
    CHECK_FALSE(extension_matches("pdo_mysql", "pg"));
    CHECK(is_slow_extension("xdebug"));
    CHECK(settings_php_ini_fields().size() == 8);
    CHECK(project_php_ini_fields().size() == 5);
}

TEST_CASE("Settings > PHP: changes until Save, and only what changed is sent") {
    const auto s = *php_series_from_reply(php_reply());
    PhpChanges c;
    CHECK(c.empty());
    CHECK(extensions_on(s, c) == 3);
    toggle_extension(s, c, "soap");
    CHECK(extension_on(s, c, "soap"));
    CHECK(extensions_on(s, c) == 4);
    toggle_extension(s, c, "soap");  // back as it was: no change
    CHECK(c.empty());
    toggle_extension(s, c, "curl");
    CHECK_FALSE(extension_on(s, c, "curl"));

    // php.ini: the series' value, a change, the same text again, a reset.
    CHECK(ini_text(s, c, "memory_limit") == "1G");
    CHECK(ini_marked(s, c, "memory_limit"));
    CHECK_FALSE(ini_marked(s, c, "post_max_size"));
    set_ini_text(s, c, "post_max_size", " 64M ");
    CHECK(ini_text(s, c, "post_max_size") == "64M");
    CHECK(ini_marked(s, c, "post_max_size"));
    set_ini_text(s, c, "post_max_size", "512M");
    CHECK_FALSE(c.ini.contains("post_max_size"));
    set_ini_text(s, c, "post_max_size", "");  // the default already
    CHECK_FALSE(c.ini.contains("post_max_size"));
    reset_ini(s, c, "memory_limit");
    CHECK_FALSE(ini_marked(s, c, "memory_limit"));
    CHECK(ini_text(s, c, "memory_limit").empty());
    set_ini_text(s, c, "date.timezone", "Asia/Baghdad");

    const json params = php_set_params("8.5", c);
    CHECK(params == json{{"series", "8.5"},
                         {"extensions", {{"curl", false}}},
                         {"ini", {{"memory_limit", nullptr}, {"date.timezone", "Asia/Baghdad"}}}});
    CHECK(php_set_params("8.5", PhpChanges{}) == json{{"series", "8.5"}});
}

TEST_CASE("Settings > PHP: what Save restarts") {
    auto s = *php_series_from_reply(php_reply());
    CHECK(php_restart_text(s, Lang::En) == "Applies when saved: PHP for 2 projects and shared sites restarts (under a second).");
    s.used_by = {"blog"};
    CHECK(php_restart_text(s, Lang::En) == "Applies when saved: PHP for 1 project restarts (under a second).");
    s.used_by = {"_shared"};
    CHECK(php_restart_text(s, Lang::En) == "Applies when saved: PHP for shared sites restarts (under a second).");
    s.used_by.clear();
    CHECK(php_restart_text(s, Lang::En) == "Applies when saved. Nothing uses PHP 8.5 right now.");
    CHECK(php_restart_text(s, Lang::Ar).find(kLri + "8.5" + kPdi) != std::string::npos);
}

TEST_CASE("the project panel's PHP part") {
    const json list = one({{"name", "shop"},
                           {"php", "8.4.27"},
                           {"wants", {{"web", "nginx"}, {"extensions", {"bcmath", "-exif", ""}}, {"php_ini", {{"memory_limit", "1024M"}}}}}});
    const std::vector<ProjectRow> rows = rows_from_list(list, Lang::En);
    REQUIRE(rows.size() == 1);
    const ProjectRow& row = rows[0];
    CHECK(row.wants.extensions == std::vector<std::string>{"bcmath", "-exif"});
    CHECK(project_uses_php(row));
    CHECK(project_php_series(row, {}) == "8.4");

    ProjectRow node;
    node.wants.known = true;
    node.wants.node = "24";
    CHECK_FALSE(project_uses_php(node));
    const ProjectRow plain;
    CHECK(project_uses_php(plain));
    CHECK_FALSE(project_php_series(plain, {}));
    CHECK(project_php_series(plain, {{"php", {"8.3.29", "8.5.1"}}}) == "8.5");

    const ProjectPhp before = project_php_of(row);
    CHECK(before.extensions.at("bcmath"));
    CHECK_FALSE(before.extensions.at("exif"));
    CHECK(before.ini.at("memory_limit") == "1024M");

    const auto s = *php_series_from_reply(php_reply());
    ProjectPhp after = before;
    CHECK(project_extension_on(&s, after, "bcmath"));
    CHECK_FALSE(project_extension_on(&s, after, "intl"));
    toggle_project_extension(&s, after, "soap");    // off in the series: on for the project
    CHECK(after.extensions.at("soap"));
    toggle_project_extension(&s, after, "soap");    // and back: the change goes
    CHECK_FALSE(after.extensions.contains("soap"));
    toggle_project_extension(&s, after, "curl");    // on in the series: off for the project
    CHECK_FALSE(after.extensions.at("curl"));
    toggle_project_extension(&s, after, "bcmath");  // on in the series and the project: off
    toggle_project_extension(&s, after, "bcmath");  // back on, as the series has it: the change goes
    CHECK_FALSE(after.extensions.contains("bcmath"));
    CHECK(project_extension_on(&s, after, "bcmath"));
    after.extensions.erase("exif");
    set_project_ini(after, "memory_limit", "  ");
    set_project_ini(after, "max_execution_time", "300");

    CHECK(project_php_params("shop", before, after) ==
          json{{"name", "shop"},
               {"extensions", {{"curl", "off"}, {"bcmath", nullptr}, {"exif", nullptr}}},
               {"ini", {{"max_execution_time", "300"}, {"memory_limit", nullptr}}}});
    CHECK(project_php_params("shop", before, before) == json{{"name", "shop"}});

    CHECK(parse_ini_line(" date.timezone = Asia/Baghdad ") ==
          std::pair<std::string, std::string>{"date.timezone", "Asia/Baghdad"});
    CHECK(parse_ini_line("short_open_tag =") == std::pair<std::string, std::string>{"short_open_tag", ""});
    CHECK_FALSE(parse_ini_line("no equals"));
    CHECK_FALSE(parse_ini_line(" = 1"));
}

TEST_CASE("the php.ini field a refusal is about") {
    const std::vector<std::string> keys{"memory_limit", "opcache.enable", "opcache.enable_cli", "date.timezone"};
    CHECK(ini_error_key("memory_limit: put the value in double quotes when it contains = ; \" ' { } [ ] or `", keys) ==
          "memory_limit");
    CHECK(ini_error_key("opcache.enable is set by FloServer (opcache always on, edits seen at once) and can't be changed",
                        keys) == "opcache.enable");
    CHECK(ini_error_key("opcache.enable_cli: the value can't contain $", keys) == "opcache.enable_cli");
    CHECK(ini_error_key("memory_limits: no", keys).empty());
    CHECK(ini_error_key("the config can't be saved", keys).empty());
}

TEST_CASE("a late PHP answer for what the user has left is dropped") {
    // Settings shows 8.5: an answer (or error) asked for 8.4 isn't its.
    CHECK(php_answer_matches("_settings", "8.5", "_settings", "8.5"));
    CHECK_FALSE(php_answer_matches("_settings", "8.4", "_settings", "8.5"));
    // The panel: another project's answers, or its old series', aren't the open part's.
    CHECK_FALSE(php_answer_matches("blog", "8.5", "shop", "8.5"));
    CHECK_FALSE(php_answer_matches("shop", "8.3", "shop", "8.5"));
    // projects.php names no series: the project is enough.
    CHECK(php_answer_matches("shop", "", "shop", "8.5"));
    CHECK_FALSE(php_answer_matches("shop", "", "_settings", "8.5"));
}

TEST_CASE("Settings > PHP offline: the versions come from what's on this computer") {
    // Offline there's no list of versions ("list": null), but runtimes.list
    // still says what's installed: Settings > PHP has its versions before
    // the poll has answered, and the list being missing changes nothing.
    const json offline = {{"list", nullptr},
                          {"list_error", "couldn't reach cdn.floserver.org"},
                          {"installed", json::array({{{"name", "php"}, {"version", "8.4.27"}, {"folder", "8.4.27"}},
                                                     {{"name", "php"}, {"version", "8.5.12"}, {"folder", "8.5.12"}},
                                                     {{"name", "nginx"}, {"version", "1.30.6"}, {"folder", "1.30.6"}}})}};
    auto versions = settings_php_versions(false, Installed{}, offline);
    REQUIRE(versions.size() == 2);
    CHECK(versions[0] == std::pair<std::string, std::string>{"8.5", "8.5.12"});
    CHECK(versions[1].first == "8.4");
    // The poll's answer wins once it's there (a version removed since).
    versions = settings_php_versions(true, Installed{{"php", {"8.4.27"}}}, offline);
    REQUIRE(versions.size() == 1);
    CHECK(versions[0].first == "8.4");
    CHECK(settings_php_versions(false, Installed{}, json()).empty());
    // The Runtimes page keeps the installed rows, Remove and the default picker offline.
    const auto page = runtimes_page(offline, nullptr, json(), Lang::En);
    REQUIRE_FALSE(page.groups.empty());
    CHECK_FALSE(page.has_list);
    CHECK(page.groups[0].rows.size() == 2);
    CHECK(page.groups[0].rows[0].action == RowAction::Remove);
    REQUIRE(page.groups[0].picker);
    CHECK(page.groups[0].picker->choices.size() == 3);  // Newest, 8.5, 8.4
}

TEST_CASE("php.ini names PHP doesn't know: a warning, with a close one when there is") {
    const std::vector<std::string> known = {"date.timezone", "disable_classes", "disable_functions", "display_errors",
                                            "memory_limit", "xdebug.mode"};
    CHECK_FALSE(unknown_ini_name(known, "memory_limit"));
    CHECK_FALSE(unknown_ini_name(known, "xdebug.mode"));
    CHECK_FALSE(unknown_ini_name({}, "khalat"));  // PHP couldn't tell: no warnings
    CHECK(unknown_ini_name(known, "disable_function") == "disable_functions");
    CHECK(unknown_ini_name(known, "Memory_Limit") == "memory_limit");   // names are case-sensitive in PHP
    CHECK(unknown_ini_name(known, "memroy_limit") == "memory_limit");   // two letters swapped: one edit
    CHECK(unknown_ini_name(known, "khalat") == "");
    CHECK(unknown_ini_name(known, "dsplay_erors") == "display_errors");
    CHECK(unknown_ini_name(known, "abc") == "");  // too short to guess
    CHECK(unknown_ini_text("8.5", "disable_function", "disable_functions", Lang::En) ==
          "PHP 8.5 has no setting named disable_function. Did you mean disable_functions?");
    CHECK(unknown_ini_text("8.5", "khalat", "", Lang::En) ==
          "PHP 8.5 has no setting named khalat, so it would be ignored.");
    CHECK(unknown_ini_text("8.5", "khalat", "", Lang::Ar).find(isolate_ltr("khalat")) != std::string::npos);

    auto reply = php_reply();
    reply["known_ini"] = json::array({"memory_limit", "date.timezone"});
    CHECK(php_series_from_reply(reply)->known_ini == std::vector<std::string>{"date.timezone", "memory_limit"});
    CHECK(php_series_from_reply(php_reply())->known_ini.empty());
}

TEST_CASE("the panel's tabs: which a project has, the one shown, and their padding") {
    ProjectRow php_site;
    php_site.name = "shop";
    php_site.wants.database = "mariadb-11.4";
    CHECK(panel_tabs(php_site) == std::vector<PanelTab>{PanelTab::General, PanelTab::Php, PanelTab::Database, PanelTab::Log});
    ProjectRow node_app;
    node_app.name = "api";
    node_app.wants.node = "24";
    const auto tabs = panel_tabs(node_app);
    CHECK(tabs == std::vector<PanelTab>{PanelTab::General, PanelTab::Log});
    // The choice is kept across projects; a project without that tab shows General.
    CHECK(shown_panel_tab(tabs, PanelTab::Php) == PanelTab::General);
    CHECK(shown_panel_tab(tabs, PanelTab::Log) == PanelTab::Log);
    // The choice stays PHP while a Node project shows General: once the
    // project asks for PHP (a version picked), the PHP tab is the one to show,
    // and the window enters it then (its php.get included).
    node_app.wants.php = "8.5";
    CHECK(shown_panel_tab(panel_tabs(node_app), PanelTab::Php) == PanelTab::Php);
    ProjectRow static_site;
    static_site.static_site = true;
    CHECK(shown_panel_tab(panel_tabs(static_site), PanelTab::Database) == PanelTab::General);
    // Padding shrinks before labels are cut.
    CHECK(tab_padding({60, 30, 70, 30}, 328, 14, 4) == 14);
    CHECK(tab_padding({90, 30, 110, 50}, 328, 14, 4) == 6);
    CHECK(tab_padding({120, 40, 140, 60}, 328, 14, 4) == 4);
    CHECK(tab_padding({}, 328, 14, 4) == 14);
}

TEST_CASE("Settings > Ports & HTTPS: web.settings.get's answer and what the section says") {
    const json reply = {{"ports", "standard"},
                        {"http_port", 8000},
                        {"https_port", nullptr},
                        {"https", false},
                        {"shared_https", true},
                        {"in_use", {{"http", 8080}, {"https", 8443}}},
                        {"taken", json::array({{{"port", 80}, {"pid", 1234}, {"program", "httpd.exe"}},
                                               {{"port", 443}, {"pid", 4321}},
                                               {{"port", 8443}}})},
                        {"ca",
                         {{"common_name", "FloServer local CA"},
                          {"der", "MII="},
                          {"trusted", "no"},
                          {"viewer", "C:/Users/me/AppData/Local/FloServer/certs/ca.crt"}}}};
    auto s = ports_settings_from_reply(reply);
    REQUIRE(s);
    CHECK(s->ports == "standard");
    CHECK(port_choice_index(s->ports) == 1);
    CHECK(s->http_port == 8000);
    CHECK_FALSE(s->https_port);
    CHECK_FALSE(s->https);
    CHECK(s->shared_https);
    CHECK(ports_in_use_text(*s, Lang::En) == "In use now: http 8080, https 8443.");
    const auto lines = ports_taken_lines(*s, Lang::En);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "Port 80 is in use by httpd.exe.");
    CHECK(lines[1] == "Port 443 is in use by another program (process 4321).");
    CHECK(lines[2] == "Port 8443 is in use by another program.");
    CHECK(lines[3].find("moves to them by itself") != std::string::npos);  // not Automatic: it moves later
    CHECK(s->ca == CaState::NotTrusted);
    CHECK(s->ca_name == "FloServer local CA");
    CHECK(s->ca_viewer.ends_with("ca.crt"));
    CHECK(ca_status_text(*s, Lang::En) == "Windows doesn't trust it yet, so projects open on http.");

    // Automatic: the taken port is said, nothing about moving.
    json automatic = reply;
    automatic["ports"] = "auto";
    automatic["taken"] = json::array({{{"port", 80}, {"program", "System"}}});
    automatic["in_use"] = nullptr;
    automatic["ca"] = nullptr;
    s = ports_settings_from_reply(automatic);
    REQUIRE(s);
    CHECK(ports_taken_lines(*s, Lang::En) == std::vector<std::string>{"Port 80 is in use by System."});
    CHECK(ports_in_use_text(*s, Lang::En) == "Nothing is served now, so no port is in use.");
    CHECK(s->ca == CaState::None);
    CHECK(ca_status_text(*s, Lang::En).starts_with("There isn't one yet."));

    // certs.ensure_ca's answer: the CA now there, the viewer kept.
    s->ca_viewer = "C:/x/ca.crt";
    CHECK(apply_ca_reply(*s, {{"ca", {{"der", "MIIB"}, {"trusted", "no"}, {"common_name", "CA"}}}}));
    CHECK(s->ca_der == "MIIB");
    CHECK(s->ca_viewer == "C:/x/ca.crt");
    CHECK_FALSE(apply_ca_reply(*s, {{"other", 1}}));

    // A damaged CA, trusted, and an answer that isn't web.settings.get's.
    automatic["ca"] = {{"error", "ca.json is damaged"}};
    s = ports_settings_from_reply(automatic);
    REQUIRE(s);
    CHECK(s->ca == CaState::Damaged);
    CHECK(ca_status_text(*s, Lang::En) == "It can't be read: ca.json is damaged");
    automatic["ca"] = {{"der", "MII="}, {"trusted", "yes"}};
    s = ports_settings_from_reply(automatic);
    REQUIRE(s);
    CHECK(s->ca == CaState::Trusted);
    CHECK_FALSE(ports_settings_from_reply(json::object()));
    CHECK_FALSE(ports_settings_from_reply(json::array()));
    automatic["ports"] = "warp";
    CHECK(ports_settings_from_reply(automatic)->ports == "auto");

    // Every language has words for every choice; Arabic keeps the address left to right.
    for (const Lang lang : {Lang::En, Lang::Ar, Lang::Ku}) {
        for (std::size_t i = 0; i < kPortChoices.size(); ++i) {
            CHECK_FALSE(port_choice_label(i, lang).empty());
            CHECK_FALSE(port_choice_note(i, lang).empty());
        }
    }
    CHECK(port_choice_note(1, Lang::En) == "Addresses without a port, such as https://shop.localhost.");
    CHECK(port_choice_note(2, Lang::Ar).find("\xE2\x80\xAAhttps://shop.localhost:8443\xE2\x80\xAC") != std::string::npos);
}

TEST_CASE("Settings > Ports & HTTPS: custom ports as typed, and what a pick sends") {
    Str problem = Str::Count_;
    auto ports = parse_custom_ports(" 8000", "8001 ", problem);
    REQUIRE(ports);
    CHECK(ports->http == 8000);
    CHECK(ports->https == 8001);
    CHECK_FALSE(parse_custom_ports("", "8001", problem));
    CHECK(problem == Str::PortsBadNumber);
    CHECK_FALSE(parse_custom_ports("0", "8001", problem));
    CHECK_FALSE(parse_custom_ports("65536", "8001", problem));
    CHECK_FALSE(parse_custom_ports("80a", "8001", problem));
    CHECK_FALSE(parse_custom_ports("-80", "8001", problem));
    CHECK_FALSE(parse_custom_ports("8000", "8000", problem));
    CHECK(problem == Str::PortsSameNumber);
    CHECK(parse_custom_ports("1", "65535", problem));

    CHECK(port_choice_params(0, std::nullopt) == json{{"ports", "auto"}});
    CHECK(port_choice_params(2, std::nullopt) == json{{"ports", "alternate"}});
    CHECK(port_choice_params(3, CustomPorts{8000, 8001}) ==
          json{{"ports", "custom"}, {"http_port", 8000}, {"https_port", 8001}});
}

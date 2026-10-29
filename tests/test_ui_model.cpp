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
    CHECK(rows[0].state == RowState::On);  // starting shows as On
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
             {RowState::On, RowState::Asleep, RowState::Off, RowState::NeedsDownload, RowState::Problem}) {
            CAPTURE(static_cast<int>(s));
            CHECK(contrast(state_colors(p, s).text, state_colors(p, s).fill) >= 4.5);
        }
        CHECK(contrast(p.banner_info.text, p.banner_info.fill) >= 4.5);
        CHECK(contrast(p.banner_error.text, p.banner_error.fill) >= 4.5);
    }
    CHECK(palette(false).window != palette(true).window);
}

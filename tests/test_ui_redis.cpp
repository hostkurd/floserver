// Redis's model (src/ui-win32/model/redis_page.h, D30): redis.status read,
// Settings > Redis's words in English, Arabic and Kurdish, the changes until
// Save, the project panel's note, the "Redis" chip, and Garnet among the
// runtimes with its parts hidden and offered unticked on the first run.

#include "model/first_run.h"
#include "model/redis_page.h"
#include "model/runtimes_page.h"
#include "model/view_model.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

using namespace floserver::ui;
using nlohmann::json;

namespace {

json status_reply() {
    return json::parse(R"({
        "installed": true, "version": "2.2.4", "dotnet_version": "10.0.14", "state": "asleep",
        "host": "127.0.0.1", "port": 6380, "keys": 42, "refused": 0,
        "settings": {"keep_running": false, "memory_mb": 256},
        "projects": ["api-shop", "blog", "shop"], "phpredis": ["8.4", "8.3"]})");
}

}  // namespace

TEST_CASE("redis.status is read, with the defaults for what's missing") {
    const auto s = redis_status_from_reply(status_reply());
    REQUIRE(s);
    CHECK(s->installed);
    CHECK(s->version == "2.2.4");
    CHECK(s->state == MailState::Asleep);
    CHECK(s->port == 6380);
    CHECK(s->keys == 42);
    CHECK(s->projects_known);
    CHECK(s->projects.size() == 3);
    CHECK(s->phpredis == std::vector<std::string>{"8.4", "8.3"});
    CHECK(s->settings.memory_mb == 256);

    const auto bare = redis_status_from_reply(json::object());
    REQUIRE(bare);
    CHECK_FALSE(bare->installed);
    CHECK(bare->state == MailState::NotInstalled);
    CHECK(bare->port == 6379);
    CHECK_FALSE(bare->projects_known);
    CHECK_FALSE(redis_status_from_reply(json::array()));
    // A state while not installed is still "not installed"; nonsense values are ignored.
    const auto odd = redis_status_from_reply(
        json::parse(R"({"installed": false, "state": "running", "port": 99999, "keys": -3,
                        "settings": {"memory_mb": 1}})"));
    REQUIRE(odd);
    CHECK(odd->state == MailState::NotInstalled);
    CHECK(odd->port == 6379);
    CHECK(odd->keys == 0);
    CHECK(odd->settings.memory_mb == 256);
}

TEST_CASE("Settings > Redis's words: the address, the masked password, the data line, in three languages") {
    const RedisStatus s = *redis_status_from_reply(status_reply());
    CHECK(redis_address_text(s) == "127.0.0.1 : 6380");
    CHECK(redis_address_copy(s) == "127.0.0.1:6380");
    CHECK(redis_password_mask().find('a') == std::string_view::npos);
    CHECK(redis_password_mask().size() == 36);  // twelve bullets, never the password's length
    CHECK(redis_data_text(s, Lang::En) == "42 keys \xC2\xB7 used by 3 projects");
    RedisStatus one = s;
    one.keys = 1;
    one.projects = {"shop"};
    CHECK(redis_data_text(one, Lang::En) == "1 key \xC2\xB7 used by 1 project");
    one.projects.clear();
    CHECK(redis_data_text(one, Lang::En) == "1 key \xC2\xB7 no project has Redis on yet");
    one.projects_known = false;
    CHECK(redis_data_text(one, Lang::En) == "1 key");
    CHECK(redis_flush_question(s, Lang::En).find("42 keys") != std::string::npos);
    for (const Lang lang : {Lang::Ar, Lang::Ku}) {
        CAPTURE(static_cast<int>(lang));
        const std::string data = redis_data_text(s, lang);
        CHECK(data.find("42") != std::string::npos);
        CHECK(data.find("keys") == std::string::npos);  // in the language, not English
        CHECK(redis_flush_question(s, lang).find("42") != std::string::npos);
        for (const Str key : {Str::SettingsRedis, Str::RedisHint, Str::RedisPasswordNote, Str::RedisEnvNote,
                              Str::RedisKeepRunningNote, Str::RedisUnsupported, Str::RedisFlush, Str::PanelRedisNote,
                              Str::FirstRunWhatGarnet, Str::RedisMemoryBad}) {
            CHECK_FALSE(text(lang, key).empty());
            CHECK(text(lang, key) != text(Lang::En, key));
        }
        // Code words stay left to right inside the right-to-left text.
        CHECK(std::string(text(lang, Str::RedisEnvNote)).find("\xE2\x80\xAAREDIS_PASSWORD\xE2\x80\xAC") !=
              std::string::npos);
    }
    CHECK(kSettingsSidebar[6] == Str::SettingsRedis);  // after Mail
    CHECK(kSettingsSidebar[5] == Str::SettingsMail);
}

TEST_CASE("Settings > Redis's changes wait for Save; a bad memory limit isn't sent") {
    RedisStatus s = *redis_status_from_reply(status_reply());
    RedisChanges c;
    CHECK(c.empty());
    CHECK_FALSE(redis_keep_running(s, c));
    redis_toggle_keep_running(s, c);
    CHECK(redis_keep_running(s, c));
    CHECK_FALSE(c.empty());
    redis_toggle_keep_running(s, c);
    CHECK(c.empty());  // back to what's saved

    CHECK(redis_memory_text(s, c) == "256");
    redis_set_memory_text(s, c, "512");
    CHECK(redis_memory_text(s, c) == "512");
    CHECK_FALSE(redis_memory_bad(c));
    redis_set_memory_text(s, c, "256");
    CHECK(c.empty());
    for (const char* bad : {"8", "70000", "12a", "", "-5"}) {
        redis_set_memory_text(s, c, bad);
        CHECK(redis_memory_bad(c));
        CHECK_FALSE(redis_set_params(c).contains("memory_mb"));
    }
    redis_set_memory_text(s, c, "1024");
    redis_toggle_keep_running(s, c);
    const json params = redis_set_params(c);
    CHECK(params == json{{"keep_running", true}, {"memory_mb", 1024}});
    redis_apply(s, c);
    CHECK(s.settings.keep_running);
    CHECK(s.settings.memory_mb == 1024);
}

TEST_CASE("the project panel's Redis note: the prefix, and phpredis or predis for its PHP") {
    ProjectRow row;
    row.name = "api-shop";
    row.php = "8.4.27";
    row.wants.known = true;
    row.wants.redis = true;
    Installed installed;
    CHECK(redis_panel_note(row, installed, Lang::En) == "Install Garnet in Settings \xE2\x86\x92 Redis first.");
    installed["garnet"] = {"2.2.4"};
    installed[std::string(kInstalledPhpRedisSeries)] = {"8.4"};
    std::string note = redis_panel_note(row, installed, Lang::En);
    CHECK(note.starts_with("Shared server, this project's keys start with api-shop_"));
    CHECK(note.find("REDIS_CLIENT=phpredis") != std::string::npos);
    row.php = "8.1.30";
    note = redis_panel_note(row, installed, Lang::En);
    CHECK(note.find("REDIS_CLIENT=predis") != std::string::npos);
    CHECK(redis_uses_phpredis(installed, "8.4.1"));
    CHECK_FALSE(redis_uses_phpredis(installed, "8.5.0"));
    CHECK_FALSE(redis_uses_phpredis(installed, ""));
    CHECK(redis_project_prefix("my shop") == "my_shop_");
    CHECK(redis_switch_value(true) == "on");
    CHECK(redis_switch_value(false) == "off");
    for (const Lang lang : {Lang::Ar, Lang::Ku}) {
        CHECK(redis_panel_note(row, installed, lang).find("api-shop_") != std::string::npos);
    }
}

TEST_CASE("projects.list: \"redis\" in wants gives the row its Redis chip, before the web server") {
    const json list = json::parse(R"({"projects": [
        {"name": "api-shop", "on": true, "state": "ready", "run": "running", "php": "8.4.27",
         "database": "mariadb-11.4", "web": "nginx", "wants": {"php": "8.4", "web": "nginx", "redis": true}},
        {"name": "blog", "on": true, "state": "ready", "php": "8.3.20", "wants": {"web": "nginx"}}]})");
    const auto rows = rows_from_list(list, Lang::En);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].wants.redis);
    REQUIRE(rows[0].chips.size() == 4);
    CHECK(rows[0].chips[2] == "Redis");
    CHECK(rows[0].chips[3] == "nginx");
    CHECK_FALSE(rows[1].wants.redis);
    CHECK(std::find(rows[1].chips.begin(), rows[1].chips.end(), "Redis") == rows[1].chips.end());
}

TEST_CASE("Garnet is a Tools row; its .NET runtime and phpredis aren't shown; the first run offers it unticked") {
    const json reply = json::parse(R"({
        "platform": "windows-x64",
        "installed": [
            {"name": "garnet", "version": "2.2.4", "variant": "", "folder": "2.2.4"},
            {"name": "dotnet", "version": "10.0.14", "variant": "", "folder": "10.0.14"},
            {"name": "php-redis", "version": "6.3.0", "variant": "php8-4", "folder": "6.3.0-php8-4"},
            {"name": "php", "version": "8.4.27", "variant": "nts", "folder": "8.4.27"}],
        "available": [
            {"name": "garnet", "version": "2.2.4", "variant": "", "size": 9000000, "installed": true},
            {"name": "dotnet", "version": "10.0.14", "variant": "", "size": 31000000, "installed": true},
            {"name": "php-redis", "version": "6.3.0", "variant": "php8-4", "size": 300000, "installed": true},
            {"name": "nginx", "version": "1.30.6", "variant": "", "size": 2000000, "installed": false}],
        "list": {"sequence": 4, "expires": "2026-12-01T00:00:00Z", "key": "k", "warnings": []}})");
    const Installed installed = installed_from_list(reply);
    CHECK(installed.at(std::string(kInstalledPhpRedisSeries)) == std::vector<std::string>{"8.4"});
    const RuntimesPage page = runtimes_page(reply, &installed, json(), Lang::En);
    bool garnet = false;
    for (const RuntimeGroup& g : page.groups) {
        for (const RuntimeRow& r : g.rows) {
            CHECK(r.name != "dotnet");
            CHECK(r.name != "php-redis");
            if (r.name == "garnet") {
                garnet = true;
                CHECK(r.label.starts_with("Garnet (Redis) "));
                CHECK(g.title == "Tools");
            }
        }
    }
    CHECK(garnet);

    const json available = json::parse(R"({"list": {"sequence": 4}, "available": [
        {"name": "nginx", "version": "1.30.6", "variant": "", "size": 2000000},
        {"name": "garnet", "version": "2.2.4", "variant": "", "size": 9000000}]})");
    const auto offers = first_run_offers(available);
    const auto it = std::find_if(offers.begin(), offers.end(), [](const FirstRunOffer& o) { return o.name == "garnet"; });
    REQUIRE(it != offers.end());
    CHECK_FALSE(it->ticked);
    CHECK(it->what == Str::FirstRunWhatGarnet);
    CHECK(it->label.find("Garnet") != std::string::npos);
}

// Settings > General's model (src/ui-win32/model/general_page.h): the
// window's preferences, the Run entry, language and theme, the licences.

#include "model/general_page.h"
#include "model/ui_strings.h"

#include "floserver/core/paths.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace floserver::ui;
using nlohmann::json;
namespace fs = std::filesystem;

TEST_CASE("window preferences: defaults, round trip, and what isn't understood") {
    const WindowPrefs defaults;
    CHECK(defaults.lang == LangChoice::System);
    CHECK(defaults.theme == ThemeChoice::System);
    CHECK(defaults.close == CloseAction::Tray);
    CHECK(defaults.start_in_tray);

    CHECK(parse_window_prefs("") == defaults);
    CHECK(parse_window_prefs("not json") == defaults);
    CHECK(parse_window_prefs("[1, 2]") == defaults);
    CHECK(parse_window_prefs(R"({"language": "fr", "theme": 3, "close": "explode"})") == defaults);

    for (const LangChoice lang : kLangChoices) {
        for (const ThemeChoice theme : kThemeChoices) {
            for (const CloseAction close : {CloseAction::Tray, CloseAction::Quit}) {
                for (const bool tray : {true, false}) {
                    const WindowPrefs prefs{lang, theme, close, tray};
                    CHECK(parse_window_prefs(window_prefs_json(prefs)) == prefs);
                }
            }
        }
    }
    const json written = json::parse(window_prefs_json({LangChoice::Ku, ThemeChoice::Dark, CloseAction::Quit, false}));
    CHECK(written["language"] == "ku");
    CHECK(written["theme"] == "dark");
    CHECK(written["close"] == "quit");
    CHECK(written["start_in_tray"] == false);
    CHECK(json::parse(window_prefs_json(defaults))["language"] == "windows");

    // One field unreadable leaves the others.
    const WindowPrefs mixed = parse_window_prefs(R"({"language": "ar", "theme": "purple", "start_in_tray": "yes"})");
    CHECK(mixed.lang == LangChoice::Ar);
    CHECK(mixed.theme == ThemeChoice::System);
    CHECK(mixed.start_in_tray);
}

TEST_CASE("the window's language: the choice, FLOSERVER_LANG, Windows' display languages") {
    const std::vector<std::string> none;
    CHECK(resolve_lang(LangChoice::System, nullptr, none) == Lang::En);
    CHECK(resolve_lang(LangChoice::System, nullptr, {"ar-IQ", "en-US"}) == Lang::Ar);
    CHECK(resolve_lang(LangChoice::System, nullptr, {"ku-Arab-IQ"}) == Lang::Ku);
    CHECK(resolve_lang(LangChoice::System, nullptr, {"ckb-IQ"}) == Lang::Ku);
    // The first one FloServer has.
    CHECK(resolve_lang(LangChoice::System, nullptr, {"fr-FR", "ar-SA", "en-GB"}) == Lang::Ar);
    CHECK(resolve_lang(LangChoice::System, nullptr, {"fr-FR", "de-DE"}) == Lang::En);
    // FLOSERVER_LANG stands in for Windows (screenshots, tests)...
    CHECK(resolve_lang(LangChoice::System, "ku", {"en-US"}) == Lang::Ku);
    CHECK(resolve_lang(LangChoice::System, "", {"ar-IQ"}) == Lang::Ar);  // set but empty: Windows'
    // ...but not for a choice made in Settings.
    CHECK(resolve_lang(LangChoice::En, "ar", {"ar-IQ"}) == Lang::En);
    CHECK(resolve_lang(LangChoice::Ar, nullptr, none) == Lang::Ar);
    CHECK(resolve_lang(LangChoice::Ku, "en", {"en-US"}) == Lang::Ku);
}

TEST_CASE("the Settings sidebar starts with General") {
    CHECK(kSettingsSidebar.front() == Str::SettingsGeneral);
    const std::vector<std::string> en = [] {
        std::vector<std::string> out;
        out.reserve(kSettingsSidebar.size());
        for (const Str s : kSettingsSidebar) {
            out.emplace_back(text(Lang::En, s));
        }
        return out;
    }();
    CHECK(en == std::vector<std::string>{"General", "Runtimes", "PHP", "Ports & HTTPS", "Database", "Apache", "Folders",
                                         "Shared sites"});
}

TEST_CASE("Same as Windows: only Sorani is Kurdish; Kurmanji falls back") {
    // Sorani (Central Kurdish), in Arabic letters.
    for (const char* sorani : {"ckb", "ckb-IQ", "CKB-Arab-IQ", "ckb_IR", "ku", "ku-IQ", "ku-Arab", "ku-Arab-IQ", "KU-arab"}) {
        CAPTURE(sorani);
        CHECK(windows_lang(sorani) == Lang::Ku);
        CHECK(resolve_lang(LangChoice::System, nullptr, {sorani}) == Lang::Ku);
    }
    // Kurmanji, in Latin letters: not ours, so the next language, else English.
    for (const char* kurmanji : {"ku-Latn", "ku-Latn-TR", "KU-LATN-tr", "ku_Latn_SY", "ku-TR-Latn", "kmr", "kmr-Latn-TR",
                                 "ckb-Latn"}) {
        CAPTURE(kurmanji);
        CHECK_FALSE(windows_lang(kurmanji));
        CHECK(resolve_lang(LangChoice::System, nullptr, {kurmanji}) == Lang::En);
    }
    CHECK(resolve_lang(LangChoice::System, nullptr, {"ku-Latn-TR", "ar-IQ"}) == Lang::Ar);
    CHECK(resolve_lang(LangChoice::System, nullptr, {"ku-Latn-TR", "ckb-IQ"}) == Lang::Ku);
    CHECK_FALSE(windows_lang(""));
    CHECK_FALSE(windows_lang("kurdish"));
    // FLOSERVER_LANG the same way.
    CHECK(resolve_lang(LangChoice::System, "ku-Latn-TR", {"ar-IQ"}) == Lang::En);
    CHECK_FALSE(parse_lang("ku-Latn"));
    CHECK(parse_lang("ku-Arab") == Lang::Ku);
}

TEST_CASE("the theme: the choice, FLOSERVER_THEME, Windows' app theme") {
    CHECK_FALSE(resolve_dark(ThemeChoice::System, nullptr, false));
    CHECK(resolve_dark(ThemeChoice::System, nullptr, true));
    CHECK(resolve_dark(ThemeChoice::System, "dark", false));
    CHECK_FALSE(resolve_dark(ThemeChoice::System, "light", true));
    CHECK(resolve_dark(ThemeChoice::System, "", true));
    CHECK(resolve_dark(ThemeChoice::Dark, "light", false));
    CHECK_FALSE(resolve_dark(ThemeChoice::Light, "dark", true));
}

TEST_CASE("language names are in their own script whatever the window shows") {
    CHECK(language_name(Lang::En) == "English");
    CHECK(language_name(Lang::Ar) == "العربية");
    CHECK(language_name(Lang::Ku) == "کوردی");
}

TEST_CASE("the Run entry: the full path in quotes, and --tray") {
    CHECK(run_command(R"(C:\Program Files\FloServer\FloServer.exe)", true, "floserverd") ==
          R"("C:\Program Files\FloServer\FloServer.exe" --tray)");
    CHECK(run_command(R"(C:\FloServer\FloServer.exe)", false, "floserverd") == R"("C:\FloServer\FloServer.exe")");
    CHECK(run_command(R"(\\server\share\FloServer.exe)", false, "floserverd") == R"("\\server\share\FloServer.exe")");
    // A path with letters outside ASCII is kept as it is (UTF-8).
    CHECK(run_command("C:\\Users\\دلشاد\\FloServer.exe", true, "floserverd") == "\"C:\\Users\\دلشاد\\FloServer.exe\" --tray");
    // Not written: nothing, relative, a quote or a line break in it.
    CHECK(run_command("", true, "floserverd").empty());
    CHECK(run_command("FloServer.exe", true, "floserverd").empty());
    CHECK(run_command(R"(C:\a"b\FloServer.exe)", true, "floserverd").empty());
    CHECK(run_command("C:\\a\nb\\FloServer.exe", true, "floserverd").empty());

    CHECK(run_value_name("floserverd") == "FloServer");
    CHECK(run_value_name("floserverd-dev") == "FloServer (dev)");
    CHECK(run_value_name("something") == "FloServer");
}

TEST_CASE("the Run entry of a copy that isn't the default carries its instance") {
    const std::string exe = R"(C:\FloServer\FloServer.exe)";
    CHECK(run_command(exe, true, "floserverd-dev") == R"("C:\FloServer\FloServer.exe" --tray --instance dev)");
    CHECK(run_command(exe, false, "floserverd-test-2") == R"("C:\FloServer\FloServer.exe" --instance test-2)");
    CHECK(run_command(exe, false, "floserverd") == R"("C:\FloServer\FloServer.exe")");
    CHECK(instance_suffix("floserverd-dev") == "dev");
    CHECK(instance_suffix("floserverd").empty());

    const auto entry = parse_run_command(run_command(exe, true, "floserverd-dev"));
    REQUIRE(entry);
    CHECK(entry->exe == exe);
    CHECK(entry->tray);
    CHECK(entry->instance == "dev");

    // FloServer.exe's own arguments.
    const StartArgs both = parse_start_args({"--instance", "dev", "--tray"});
    CHECK(both.tray);
    CHECK(both.instance == "dev");
    CHECK(parse_start_args({}).instance.empty());
    CHECK_FALSE(parse_start_args({}).tray);
    CHECK(parse_start_args({"--instance"}).instance.empty());           // no value
    CHECK(parse_start_args({"--instance", "Dev!"}).instance.empty());   // not an instance name
    CHECK(parse_start_args({"--instance", std::string(40, 'a')}).instance.empty());  // too long
    const StartArgs flag_next = parse_start_args({"--instance", "--tray"});
    CHECK(flag_next.instance.empty());
    CHECK(flag_next.tray);  // not taken as the instance
}

TEST_CASE("Task Manager's Startup apps off mark") {
    CHECK_FALSE(startup_disabled({}));
    CHECK_FALSE(startup_disabled({0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(startup_disabled({0x03, 0x11, 0x22, 0x33, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(startup_disabled({0x01}));
    CHECK_FALSE(startup_disabled({0x06}));
}

TEST_CASE("the Run entry read back") {
    for (const bool tray : {true, false}) {
        const std::string exe = R"(C:\Program Files\FloServer\FloServer.exe)";
        const auto entry = parse_run_command(run_command(exe, tray, "floserverd"));
        REQUIRE(entry);
        CHECK(entry->exe == exe);
        CHECK(entry->tray == tray);
    }
    CHECK_FALSE(parse_run_command(""));
    CHECK_FALSE(parse_run_command("   "));
    // Written by hand, without quotes.
    auto bare = parse_run_command(R"(C:\Tools\FloServer.exe --tray)");
    REQUIRE(bare);
    CHECK(bare->exe == R"(C:\Tools\FloServer.exe)");
    CHECK(bare->tray);
    bare = parse_run_command(R"(C:\Program Files\FloServer\FLOSERVER.EXE)");
    REQUIRE(bare);
    CHECK(bare->exe == R"(C:\Program Files\FloServer\FLOSERVER.EXE)");
    CHECK_FALSE(bare->tray);
    // The flag only as a word of its own.
    const auto other = parse_run_command(R"("C:\FloServer.exe" --trayless)");
    REQUIRE(other);
    CHECK_FALSE(other->tray);
    const auto unclosed = parse_run_command(R"("C:\FloServer.exe)");
    REQUIRE(unclosed);
    CHECK(unclosed->exe == R"(C:\FloServer.exe)");
}

TEST_CASE("licences: the libraries, and the runtimes installed") {
    const auto libraries = library_licences();
    REQUIRE(libraries.size() == 4);
    CHECK(libraries[0].name == "nlohmann/json");
    CHECK(libraries[0].licence == "MIT");
    CHECK(libraries[3].name == "doctest");
    CHECK(libraries[3].test_only);

    const json reply = {
        {"installed",
         json::array({{{"name", "php"}, {"version", "8.4.16"}, {"license", "PHP-3.01"}, {"folder", "8.4.16"}},
                      {{"name", "php"}, {"version", "8.10.1"}, {"license", "PHP-3.01"}, {"folder", "8.10.1"}},
                      {{"name", "mariadb"}, {"version", "11.4.5"}, {"license", "GPL-2.0-only"}},
                      {{"name", "php"}, {"version", "8.4.16"}, {"license", "PHP-3.01"}, {"folder", "8.4.16-nts"}},
                      {{"name", "node"}, {"version", "22.1.0"}},
                      {{"version", "1.0"}}})}};
    const auto runtimes = runtime_licences(reply);
    REQUIRE(runtimes.size() == 4);
    CHECK(runtimes[0].name == "MariaDB");
    CHECK(runtimes[1].name == "Node.js");
    CHECK(runtimes[1].licence.empty());
    CHECK(runtimes[2].name == "PHP");
    CHECK(runtimes[2].version == "8.10.1");  // the newest first
    CHECK(runtimes[3].version == "8.4.16");
    CHECK(runtime_licences(json()).empty());
    CHECK(runtime_licences(json{{"installed", "x"}}).empty());

    const std::string text = licences_text(Lang::En, "0.9.0", &reply);
    CHECK(text.starts_with("FloServer 0.9.0: licences\n"));
    CHECK(text.find("  Monocypher 4.0.2 — BSD-2-Clause OR CC0-1.0  https://monocypher.org\n") != std::string::npos);
    CHECK(text.find("(tests only, not shipped)") != std::string::npos);
    CHECK(text.find("  PHP 8.10.1 — PHP-3.01\n") != std::string::npos);
    CHECK(text.find("  Node.js 22.1.0 — ?\n") != std::string::npos);
    CHECK(licences_text(Lang::En, "0.9.0", nullptr).find("Not read yet") != std::string::npos);
    const json empty = {{"installed", json::array()}};
    CHECK(licences_text(Lang::En, "0.9.0", &empty).find("None yet.") != std::string::npos);
    // No direction marks in the file, whatever the language.
    const std::string arabic = licences_text(Lang::Ar, "0.9.0", &reply);
    CHECK(arabic.find(std::string{'\xE2', '\x80', '\xAA'}) == std::string::npos);
    CHECK(arabic.find("PHP 8.10.1") != std::string::npos);
}

TEST_CASE("window.json: saving keeps keys it doesn't know") {
    const test_util::TempDir dir;
    // A non-ASCII folder, as some Windows user names are.
    const fs::path file = dir.path() / floserver::core::path_from_utf8("کاربەر") / "window.json";
    test_util::write_file(file, R"({"language": "ar", "from_a_newer_floserver": {"x": [1, 2]}, "zoom": 1.25})");
    const WindowPrefsLoad loaded = load_window_prefs_file(file);
    CHECK_FALSE(loaded.damaged);
    CHECK(loaded.prefs.lang == LangChoice::Ar);

    WindowPrefs next = loaded.prefs;
    next.theme = ThemeChoice::Dark;
    fs::path kept;
    std::string error;
    REQUIRE(save_window_prefs_file(file, next, kept, error));
    CHECK(kept.empty());
    const json saved = json::parse(test_util::read_file(file));
    CHECK(saved["theme"] == "dark");
    CHECK(saved["language"] == "ar");
    CHECK(saved["from_a_newer_floserver"]["x"].size() == 2);
    CHECK(saved["zoom"] == 1.25);
    CHECK(load_window_prefs_file(file).prefs == next);

    // Not there yet: the defaults, not damaged; saving makes it (and its folder).
    const fs::path fresh = dir.path() / "new" / "window.json";
    CHECK_FALSE(load_window_prefs_file(fresh).damaged);
    REQUIRE(save_window_prefs_file(fresh, next, kept, error));
    CHECK(kept.empty());
    CHECK(load_window_prefs_file(fresh).prefs == next);
}

TEST_CASE("window.json: a damaged file is kept as .bad, never lost") {
    const test_util::TempDir dir;
    const fs::path file = dir.path() / "window.json";
    test_util::write_file(file, R"({"language": "ar", "theme": )");  // cut short
    const WindowPrefsLoad loaded = load_window_prefs_file(file);
    CHECK(loaded.damaged);
    CHECK(loaded.problem.find("valid JSON") != std::string::npos);
    CHECK(loaded.prefs == WindowPrefs{});
    // Loading alone never changes it.
    CHECK(test_util::read_file(file) == R"({"language": "ar", "theme": )");

    WindowPrefs next;
    next.lang = LangChoice::Ku;
    fs::path kept;
    std::string error;
    REQUIRE(save_window_prefs_file(file, next, kept, error));
    CHECK(kept == dir.path() / "window.json.bad");
    CHECK(test_util::read_file(dir.path() / "window.json.bad") == R"({"language": "ar", "theme": )");
    CHECK(load_window_prefs_file(file).prefs == next);
    CHECK_FALSE(load_window_prefs_file(file).damaged);

    // Damaged again: the first copy stays, the next goes beside it.
    test_util::write_file(file, "[1, 2]");
    REQUIRE(save_window_prefs_file(file, next, kept, error));
    CHECK(kept == dir.path() / "window.json.bad.2");
    CHECK(test_util::read_file(dir.path() / "window.json.bad") == R"({"language": "ar", "theme": )");
    CHECK(test_util::read_file(dir.path() / "window.json.bad.2") == "[1, 2]");
    test_util::write_file(file, "");
    REQUIRE(save_window_prefs_file(file, next, kept, error));
    CHECK(kept == dir.path() / "window.json.bad.3");
    CHECK(window_prefs_bad_copy(file) == dir.path() / "window.json.bad.4");

    // An earlier copy is never written over: the move refuses a name that's taken.
    test_util::write_file(file, "{");
    test_util::write_file(dir.path() / "window.json.bad.4", "made meanwhile");
    REQUIRE(save_window_prefs_file(file, next, kept, error));
    CHECK(kept == dir.path() / "window.json.bad.5");
    CHECK(test_util::read_file(dir.path() / "window.json.bad.4") == "made meanwhile");
    CHECK(test_util::read_file(dir.path() / "window.json.bad.5") == "{");
    CHECK_FALSE(fs::exists(dir.path() / "window.json.new"));  // the new file went in place
    std::string move_error;
    test_util::write_file(dir.path() / "a", "a");
    CHECK_FALSE(floserver::core::rename_no_replace(dir.path() / "a", dir.path() / "window.json.bad", move_error));
    CHECK_FALSE(move_error.empty());
    CHECK(test_util::read_file(dir.path() / "a") == "a");
    CHECK(test_util::read_file(dir.path() / "window.json.bad") == R"({"language": "ar", "theme": )");

    // Unknown values in a valid file aren't damage: kept as they are.
    test_util::write_file(file, R"({"language": "fr", "theme": 3})");
    CHECK_FALSE(load_window_prefs_file(file).damaged);
    REQUIRE(save_window_prefs_file(file, next, kept, error));
    CHECK(kept.empty());
    CHECK(json::parse(test_util::read_file(file))["language"] == "ku");
}

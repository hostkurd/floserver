// Settings > General's model (src/ui-win32/model/general_page.h): the
// window's preferences, the Run entry, language and theme, the licences.

#include "model/general_page.h"
#include "model/ui_strings.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace floserver::ui;
using nlohmann::json;

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
    CHECK(run_command(R"(C:\Program Files\FloServer\FloServer.exe)", true) ==
          R"("C:\Program Files\FloServer\FloServer.exe" --tray)");
    CHECK(run_command(R"(C:\FloServer\FloServer.exe)", false) == R"("C:\FloServer\FloServer.exe")");
    CHECK(run_command(R"(\\server\share\FloServer.exe)", false) == R"("\\server\share\FloServer.exe")");
    // A path with letters outside ASCII is kept as it is (UTF-8).
    CHECK(run_command("C:\\Users\\دلشاد\\FloServer.exe", true) == "\"C:\\Users\\دلشاد\\FloServer.exe\" --tray");
    // Not written: nothing, relative, a quote or a line break in it.
    CHECK(run_command("", true).empty());
    CHECK(run_command("FloServer.exe", true).empty());
    CHECK(run_command(R"(C:\a"b\FloServer.exe)", true).empty());
    CHECK(run_command("C:\\a\nb\\FloServer.exe", true).empty());

    CHECK(run_value_name("floserverd") == "FloServer");
    CHECK(run_value_name("floserverd-dev") == "FloServer (dev)");
    CHECK(run_value_name("something") == "FloServer");
}

TEST_CASE("the Run entry read back") {
    for (const bool tray : {true, false}) {
        const std::string exe = R"(C:\Program Files\FloServer\FloServer.exe)";
        const auto entry = parse_run_command(run_command(exe, tray));
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

// The code editor a project opens in (src/ui-win32/model/code_editor.h,
// approved 3 Nov): window.json's choice, where editors are looked for, and
// which one Automatic picks.

#include "model/code_editor.h"
#include "model/general_page.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace floserver::ui;
using nlohmann::json;
namespace fs = std::filesystem;

TEST_CASE("code editor: the choice in window.json") {
    CHECK(parse_editor_choice(json("auto")) == EditorChoice{});
    CHECK(parse_editor_choice(json()) == EditorChoice{});
    CHECK(parse_editor_choice(json("emacs")) == EditorChoice{});  // a newer FloServer's: Automatic here
    CHECK(parse_editor_choice(json(3)) == EditorChoice{});
    CHECK(parse_editor_choice(json{{"path", ""}}) == EditorChoice{});
    for (const EditorId id : kEditors) {
        const EditorChoice known{EditorChoice::Kind::Known, id, {}};
        CHECK(parse_editor_choice(editor_choice_json(known)) == known);
        CHECK(editor_from_key(editor_key(id)) == id);
    }
    CHECK(editor_choice_json(EditorChoice{}) == "auto");
    CHECK(editor_choice_json({EditorChoice::Kind::Known, EditorId::Sublime, {}}) == "sublime");
    const EditorChoice program{EditorChoice::Kind::Program, EditorId::VsCode, R"(C:\Tools\Notepad++\notepad++.exe)"};
    CHECK(editor_choice_json(program) == json{{"path", R"(C:\Tools\Notepad++\notepad++.exe)"}});
    CHECK(parse_editor_choice(editor_choice_json(program)) == program);
    CHECK(editor_key(EditorId::VsCode) == "vscode");
    CHECK(editor_key(EditorId::Cursor) == "cursor");
    CHECK(editor_key(EditorId::VsCodium) == "vscodium");
    CHECK(editor_key(EditorId::PhpStorm) == "phpstorm");
}

TEST_CASE("code editor: kept in window.json beside the other preferences") {
    WindowPrefs prefs;
    CHECK(prefs.code_editor == EditorChoice{});
    CHECK(json::parse(window_prefs_json(prefs))["code_editor"] == "auto");
    prefs.code_editor = {EditorChoice::Kind::Known, EditorId::Cursor, {}};
    CHECK(parse_window_prefs(window_prefs_json(prefs)) == prefs);
    prefs.code_editor = {EditorChoice::Kind::Program, EditorId::VsCode, "D:/Apps/Zed/zed.exe"};
    CHECK(parse_window_prefs(window_prefs_json(prefs)) == prefs);
    // Keys this version doesn't know stay; an unknown editor reads as Automatic.
    const json existing = {{"future", {{"x", 1}}}, {"code_editor", "helix"}};
    CHECK(parse_window_prefs(existing.dump()).code_editor == EditorChoice{});
    const json written = json::parse(window_prefs_json(WindowPrefs{}, existing));
    CHECK(written["future"]["x"] == 1);
    CHECK(written["code_editor"] == "helix");  // kept while the choice isn't changed (another preference was)
    WindowPrefs changed;
    changed.code_editor = {EditorChoice::Kind::Known, EditorId::VsCode, {}};
    CHECK(json::parse(window_prefs_json(changed, existing))["code_editor"] == "vscode");
    // Equal choices written differently are left as they are too.
    const json custom = {{"code_editor", {{"path", "C:/x/zed.exe"}, {"args", "--new"}}}};
    WindowPrefs program;
    program.code_editor = {EditorChoice::Kind::Program, EditorId::VsCode, "C:/x/zed.exe"};
    CHECK(json::parse(window_prefs_json(program, custom))["code_editor"]["args"] == "--new");
}

TEST_CASE("code editor: where each one is looked for, per user first") {
    const EditorRoots roots{"L", "P", "X"};
    const auto vscode = editor_candidates(EditorId::VsCode, roots);
    REQUIRE(vscode.size() == 3);
    CHECK(vscode[0] == fs::path("L") / "Programs" / "Microsoft VS Code" / "Code.exe");
    CHECK(vscode[1] == fs::path("P") / "Microsoft VS Code" / "Code.exe");
    CHECK(vscode[2] == fs::path("X") / "Microsoft VS Code" / "Code.exe");
    CHECK(editor_candidates(EditorId::Cursor, roots).front() == fs::path("L") / "Programs" / "cursor" / "Cursor.exe");
    CHECK(editor_candidates(EditorId::PhpStorm, roots).front() ==
          fs::path("L") / "Programs" / "PhpStorm" / "bin" / "phpstorm64.exe");
    // A folder Windows doesn't give is skipped, never made relative.
    const auto no_local = editor_candidates(EditorId::VsCode, {{}, "P", {}});
    REQUIRE(no_local.size() == 1);
    CHECK(no_local[0] == fs::path("P") / "Microsoft VS Code" / "Code.exe");
    for (const EditorId id : kEditors) {
        // Always the program itself, never a .cmd that needs cmd.exe and a console.
        CHECK(editor_program(id).extension() == ".exe");
        for (const fs::path& p : editor_candidates(id, roots)) {
            CHECK(p.extension() == ".exe");
        }
    }
    CHECK(phpstorm_folders({"CLion 2024.1", "PhpStorm 2023.3.6", "PhpStorm 2024.10", "PhpStorm 2024.9.1",
                            "PhpStormish", "WebStorm 2024.2"}) ==
          std::vector<std::string>{"PhpStorm 2024.10", "PhpStorm 2024.9.1", "PhpStorm 2023.3.6"});
}

TEST_CASE("code editor: Uninstall entries") {
    CHECK(editor_from_display_name("Microsoft Visual Studio Code (User)") == EditorId::VsCode);
    CHECK(editor_from_display_name("Microsoft Visual Studio Code") == EditorId::VsCode);
    CHECK_FALSE(editor_from_display_name("Microsoft Visual Studio Code - Insiders (User)").has_value());
    CHECK(editor_from_display_name("Cursor (User)") == EditorId::Cursor);
    CHECK(editor_from_display_name("Cursor 0.42.3") == EditorId::Cursor);
    CHECK_FALSE(editor_from_display_name("CursorFX").has_value());
    CHECK_FALSE(editor_from_display_name("Cursor Commander").has_value());
    CHECK(editor_from_display_name("VSCodium (User)") == EditorId::VsCodium);
    CHECK(editor_from_display_name("Sublime Text") == EditorId::Sublime);
    CHECK(editor_from_display_name("Sublime Text 4") == EditorId::Sublime);
    CHECK(editor_from_display_name("Sublime Text Build 4169") == EditorId::Sublime);
    CHECK_FALSE(editor_from_display_name("Sublime Merge").has_value());
    CHECK(editor_from_display_name("PhpStorm 2024.2.1") == EditorId::PhpStorm);
    CHECK_FALSE(editor_from_display_name("Notepad++ (64-bit x64)").has_value());

    CHECK(display_icon_file(R"("C:\Program Files\Microsoft VS Code\Code.exe",0)") ==
          R"(C:\Program Files\Microsoft VS Code\Code.exe)");
    CHECK(display_icon_file(R"(C:\Users\a\AppData\Local\Programs\cursor\Cursor.exe,0)") ==
          R"(C:\Users\a\AppData\Local\Programs\cursor\Cursor.exe)");
    CHECK(display_icon_file(R"(C:\x\sublime_text.exe,-101)") == R"(C:\x\sublime_text.exe)");
    CHECK(display_icon_file(R"(C:\x,y\Code.exe)") == R"(C:\x,y\Code.exe)");
    CHECK(display_icon_file("") == "");
}

TEST_CASE("code editor: what Automatic and a choice open") {
    const std::vector<FoundEditor> none;
    CHECK_FALSE(pick_editor({}, none).has_value());

    const std::vector<FoundEditor> several = {{EditorId::PhpStorm, "ps.exe"},
                                              {EditorId::Cursor, "cursor.exe"},
                                              {EditorId::VsCode, "code.exe"}};
    auto t = pick_editor({}, several);
    REQUIRE(t.has_value());
    CHECK(t->name == "VS Code");  // VS Code first when it's installed
    CHECK(t->program == fs::path("code.exe"));

    const std::vector<FoundEditor> no_vscode = {{EditorId::PhpStorm, "ps.exe"}, {EditorId::Sublime, "subl.exe"},
                                                {EditorId::VsCodium, "codium.exe"}};
    t = pick_editor({}, no_vscode);
    REQUIRE(t.has_value());
    CHECK(t->name == "VSCodium");  // then Cursor, VSCodium, Sublime Text, PhpStorm

    t = pick_editor({EditorChoice::Kind::Known, EditorId::PhpStorm, {}}, several);
    REQUIRE(t.has_value());
    CHECK(t->name == "PhpStorm");
    // A chosen editor that isn't installed: nothing (the user is asked to choose).
    CHECK_FALSE(pick_editor({EditorChoice::Kind::Known, EditorId::Sublime, {}}, several).has_value());

    t = pick_editor({EditorChoice::Kind::Program, EditorId::VsCode, R"(C:\Tools\Notepad++\notepad++.EXE)"}, none);
    REQUIRE(t.has_value());
    CHECK(t->name == "notepad++");
    CHECK_FALSE(pick_editor({EditorChoice::Kind::Program, EditorId::VsCode, {}}, several).has_value());

    // Never a batch file (cmd.exe would run it), whatever it's called.
    CHECK_FALSE(pick_editor({EditorChoice::Kind::Program, EditorId::VsCode, R"(C:\Tools\code.cmd)"}, none).has_value());
    CHECK_FALSE(pick_editor({EditorChoice::Kind::Program, EditorId::VsCode, R"(C:\Tools\run.bat)"}, none).has_value());
    CHECK(is_program_file(R"(C:\Tools\Zed.EXE)"));
    CHECK_FALSE(is_program_file(R"(C:\Tools\.exe)"));
    CHECK_FALSE(is_program_file("editor.exe.lnk"));
    CHECK_FALSE(is_program_file(""));
    CHECK(program_display_name("D:/Apps/Zed/zed.exe") == "zed");
    CHECK(program_display_name("vim") == "vim");
    // The folder is one argument, spaces and all: no command line is put together from text.
    CHECK(editor_arguments(R"(C:\My Sites\shop & co)") == std::vector<std::string>{R"(C:\My Sites\shop & co)"});
}

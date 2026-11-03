#pragma once

// The code editor a project opens in (approved 3 Nov): the choice kept in
// window.json, the editors FloServer knows and where each one installs
// itself, and which one "Automatic" picks. Plain C++ so it's tested on every
// platform; the window looks on disk and in the registry (code_editors.cpp)
// and starts the editor's own program with the folder, never a shell.

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// In Automatic's order: VS Code when it's there, else the first found of the rest.
enum class EditorId : std::uint8_t { VsCode, Cursor, VsCodium, Sublime, PhpStorm };
inline constexpr std::array<EditorId, 5> kEditors = {EditorId::VsCode, EditorId::Cursor, EditorId::VsCodium,
                                                     EditorId::Sublime, EditorId::PhpStorm};

// window.json's "code_editor": "auto", an editor's key, or {"path": "..."}.
struct EditorChoice {
    enum class Kind : std::uint8_t { Auto, Known, Program };
    Kind kind = Kind::Auto;
    EditorId id = EditorId::VsCode;  // Known
    std::string path;                // Program: the .exe the user chose (UTF-8)
    bool operator==(const EditorChoice&) const = default;
};

// "vscode", "cursor", "vscodium", "sublime", "phpstorm".
[[nodiscard]] std::string_view editor_key(EditorId id);
[[nodiscard]] std::optional<EditorId> editor_from_key(std::string_view key);
// "VS Code", "Cursor", "VSCodium", "Sublime Text", "PhpStorm": Latin names, the
// same in every language.
[[nodiscard]] std::string_view editor_name(EditorId id);

// Anything not understood (a newer FloServer's value) is Automatic.
[[nodiscard]] EditorChoice parse_editor_choice(const nlohmann::json& value);
[[nodiscard]] nlohmann::json editor_choice_json(const EditorChoice& choice);

// The program inside an editor's install folder: "Code.exe",
// "bin\\phpstorm64.exe". The GUI program, never a .cmd wrapper (that would
// open a console window, and go through cmd.exe).
[[nodiscard]] std::filesystem::path editor_program(EditorId id);
// Its App Paths name (HKCU/HKLM\...\App Paths\<name>), when it registers one.
[[nodiscard]] std::string_view editor_app_path_name(EditorId id);

// The folders Windows gives (any may be empty: not set).
struct EditorRoots {
    std::filesystem::path local_app_data;     // %LOCALAPPDATA%
    std::filesystem::path program_files;      // %ProgramFiles%
    std::filesystem::path program_files_x86;  // %ProgramFiles(x86)%
};
// Where each editor's installers put it, per user first, in the order looked
// at. PhpStorm's versioned folders (Program Files\JetBrains\PhpStorm 2024.2)
// are found by phpstorm_folders() instead.
[[nodiscard]] std::vector<std::filesystem::path> editor_candidates(EditorId id, const EditorRoots& roots);
// Of the names in Program Files\JetBrains, PhpStorm's install folders, newest first.
[[nodiscard]] std::vector<std::string> phpstorm_folders(const std::vector<std::string>& names);

// An Uninstall entry's DisplayName as an editor ("Microsoft Visual Studio
// Code (User)", "Cursor 0.42.3", "Sublime Text 4", "PhpStorm 2024.2.1").
// Insiders builds and look-alikes are nothing.
[[nodiscard]] std::optional<EditorId> editor_from_display_name(std::string_view name);
// A DisplayIcon value as a file: "\"C:\\x\\Code.exe\",0" is C:\x\Code.exe.
[[nodiscard]] std::string display_icon_file(std::string_view value);

// An editor found on this PC.
struct FoundEditor {
    EditorId id = EditorId::VsCode;
    std::filesystem::path program;
};

// What a project opens in: a name for the button's tooltip and the menu
// ("Open in VS Code"), and the program started with the folder.
struct EditorTarget {
    std::string name;
    std::filesystem::path program;
};
// The choice made with what's found (each editor once, in kEditors' order).
// Nothing when there's no editor to open: Automatic with none found, a
// chosen editor that isn't installed (any more), or no program chosen.
// A chosen program is taken as it is (the window checks it's still there).
[[nodiscard]] std::optional<EditorTarget> pick_editor(const EditorChoice& choice, const std::vector<FoundEditor>& found);
// A chosen program's name: its file name without ".exe" ("notepad++").
[[nodiscard]] std::string program_display_name(const std::filesystem::path& program);

// The arguments an editor is started with: the folder alone, as one argument.
[[nodiscard]] std::vector<std::string> editor_arguments(const std::string& folder);

}  // namespace floserver::ui

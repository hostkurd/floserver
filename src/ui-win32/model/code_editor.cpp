#include "model/code_editor.h"

#include "floserver/core/paths.h"

#include <algorithm>
#include <cctype>

namespace floserver::ui {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

struct EditorInfo {
    EditorId id;
    std::string_view key;
    std::string_view name;
    std::string_view app_path;  // App Paths name, or empty
};

constexpr std::array<EditorInfo, kEditors.size()> kInfo = {{
    {EditorId::VsCode, "vscode", "VS Code", "Code.exe"},
    {EditorId::Cursor, "cursor", "Cursor", "Cursor.exe"},
    {EditorId::VsCodium, "vscodium", "VSCodium", "VSCodium.exe"},
    {EditorId::Sublime, "sublime", "Sublime Text", "sublime_text.exe"},
    {EditorId::PhpStorm, "phpstorm", "PhpStorm", "phpstorm64.exe"},
}};

const EditorInfo& info(EditorId id) {
    for (const EditorInfo& i : kInfo) {
        if (i.id == id) {
            return i;
        }
    }
    return kInfo.front();
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// "2024.10.1" after "2024.9": number by number, a missing part first.
bool version_newer(std::string_view a, std::string_view b) {
    while (!a.empty() || !b.empty()) {
        const auto cut_a = a.find('.');
        const auto cut_b = b.find('.');
        const std::string_view pa = a.substr(0, cut_a);
        const std::string_view pb = b.substr(0, cut_b);
        if (pa != pb) {
            const bool num_a = !pa.empty() && std::all_of(pa.begin(), pa.end(), is_digit);
            const bool num_b = !pb.empty() && std::all_of(pb.begin(), pb.end(), is_digit);
            if (num_a && num_b && pa.size() != pb.size()) {
                return pa.size() > pb.size();
            }
            return pa > pb;
        }
        a = cut_a == std::string_view::npos ? std::string_view{} : a.substr(cut_a + 1);
        b = cut_b == std::string_view::npos ? std::string_view{} : b.substr(cut_b + 1);
    }
    return false;
}

}  // namespace

std::string_view editor_key(EditorId id) { return info(id).key; }

std::optional<EditorId> editor_from_key(std::string_view key) {
    for (const EditorInfo& i : kInfo) {
        if (i.key == key) {
            return i.id;
        }
    }
    return std::nullopt;
}

std::string_view editor_name(EditorId id) { return info(id).name; }

std::string_view editor_app_path_name(EditorId id) { return info(id).app_path; }

EditorChoice parse_editor_choice(const json& value) {
    EditorChoice choice;
    if (value.is_string()) {
        if (const auto id = editor_from_key(value.get<std::string>())) {
            choice.kind = EditorChoice::Kind::Known;
            choice.id = *id;
        }
    } else if (value.is_object() && value.contains("path") && value["path"].is_string()) {
        std::string path = value["path"].get<std::string>();
        if (!path.empty()) {
            choice.kind = EditorChoice::Kind::Program;
            choice.path = std::move(path);
        }
    }
    return choice;
}

json editor_choice_json(const EditorChoice& choice) {
    switch (choice.kind) {
        case EditorChoice::Kind::Known:
            return std::string(editor_key(choice.id));
        case EditorChoice::Kind::Program:
            if (!choice.path.empty()) {
                return json{{"path", choice.path}};
            }
            break;
        case EditorChoice::Kind::Auto:
            break;
    }
    return "auto";
}

fs::path editor_program(EditorId id) {
    switch (id) {
        case EditorId::VsCode:
            return "Code.exe";
        case EditorId::Cursor:
            return "Cursor.exe";
        case EditorId::VsCodium:
            return "VSCodium.exe";
        case EditorId::Sublime:
            return "sublime_text.exe";
        case EditorId::PhpStorm:
            break;
    }
    return fs::path("bin") / "phpstorm64.exe";
}

std::vector<fs::path> editor_candidates(EditorId id, const EditorRoots& roots) {
    std::vector<fs::path> out;
    const auto add = [&](const fs::path& root, std::initializer_list<std::string_view> folders) {
        if (root.empty()) {
            return;
        }
        fs::path p = root;
        for (const std::string_view f : folders) {
            p /= fs::path(std::string(f));
        }
        out.push_back(p / editor_program(id));
    };
    switch (id) {
        case EditorId::VsCode:
            add(roots.local_app_data, {"Programs", "Microsoft VS Code"});
            add(roots.program_files, {"Microsoft VS Code"});
            add(roots.program_files_x86, {"Microsoft VS Code"});
            break;
        case EditorId::Cursor:
            add(roots.local_app_data, {"Programs", "cursor"});
            add(roots.program_files, {"Cursor"});
            break;
        case EditorId::VsCodium:
            add(roots.local_app_data, {"Programs", "VSCodium"});
            add(roots.program_files, {"VSCodium"});
            add(roots.program_files_x86, {"VSCodium"});
            break;
        case EditorId::Sublime:
            add(roots.program_files, {"Sublime Text"});
            add(roots.program_files, {"Sublime Text 3"});
            add(roots.program_files_x86, {"Sublime Text"});
            break;
        case EditorId::PhpStorm:
            // JetBrains Toolbox (2.x) installs here per user.
            add(roots.local_app_data, {"Programs", "PhpStorm"});
            break;
    }
    return out;
}

std::vector<std::string> phpstorm_folders(const std::vector<std::string>& names) {
    std::vector<std::string> out;
    for (const std::string& n : names) {
        if (n == "PhpStorm" || starts_with(n, "PhpStorm ")) {
            out.push_back(n);
        }
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const std::string_view va = std::string_view(a).substr(std::min(a.size(), std::size_t{9}));
        const std::string_view vb = std::string_view(b).substr(std::min(b.size(), std::size_t{9}));
        return version_newer(va, vb);
    });
    return out;
}

std::optional<EditorId> editor_from_display_name(std::string_view name) {
    while (!name.empty() && name.front() == ' ') {
        name.remove_prefix(1);
    }
    if (name.find("Insiders") != std::string_view::npos) {
        return std::nullopt;
    }
    if (starts_with(name, "Microsoft Visual Studio Code")) {
        return EditorId::VsCode;
    }
    if (starts_with(name, "VSCodium")) {
        return EditorId::VsCodium;
    }
    if (name == "Cursor" ||
        (starts_with(name, "Cursor ") && name.size() > 7 && (name[7] == '(' || is_digit(name[7])))) {
        return EditorId::Cursor;
    }
    if (name == "Sublime Text" ||
        (starts_with(name, "Sublime Text ") && name.size() > 13 && (is_digit(name[13]) || starts_with(name.substr(13), "Build")))) {
        return EditorId::Sublime;
    }
    if (name == "PhpStorm" || starts_with(name, "PhpStorm ") || starts_with(name, "JetBrains PhpStorm")) {
        return EditorId::PhpStorm;
    }
    return std::nullopt;
}

std::string display_icon_file(std::string_view value) {
    while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
    }
    if (!value.empty() && value.front() == '"') {
        const auto end = value.find('"', 1);
        return std::string(value.substr(1, end == std::string_view::npos ? std::string_view::npos : end - 1));
    }
    // "C:\x\Code.exe,0": the icon's index after the last comma.
    if (const auto comma = value.rfind(','); comma != std::string_view::npos) {
        std::string_view index = value.substr(comma + 1);
        if (!index.empty() && index.front() == '-') {
            index.remove_prefix(1);
        }
        if (!index.empty() && std::all_of(index.begin(), index.end(), is_digit)) {
            value = value.substr(0, comma);
        }
    }
    while (!value.empty() && value.back() == ' ') {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::optional<EditorTarget> pick_editor(const EditorChoice& choice, const std::vector<FoundEditor>& found) {
    const auto find = [&](EditorId id) -> std::optional<EditorTarget> {
        for (const FoundEditor& f : found) {
            if (f.id == id && !f.program.empty()) {
                return EditorTarget{std::string(editor_name(id)), f.program};
            }
        }
        return std::nullopt;
    };
    switch (choice.kind) {
        case EditorChoice::Kind::Auto:
            for (const EditorId id : kEditors) {
                if (auto t = find(id)) {
                    return t;
                }
            }
            return std::nullopt;
        case EditorChoice::Kind::Known:
            return find(choice.id);
        case EditorChoice::Kind::Program:
            break;
    }
    if (!is_program_file(choice.path)) {
        return std::nullopt;  // nothing chosen, or not a program that can be started without a shell
    }
    const fs::path program = core::path_from_utf8(choice.path);
    return EditorTarget{program_display_name(program), program};
}

bool is_program_file(std::string_view path) {
    if (path.size() <= 4) {
        return false;
    }
    std::string tail(path.substr(path.size() - 4));
    std::transform(tail.begin(), tail.end(), tail.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const char before = path[path.size() - 5];
    return tail == ".exe" && before != '/' && before != '\\';
}

std::string program_display_name(const fs::path& program) {
    std::string text = core::path_to_utf8(program);
    if (const auto slash = text.find_last_of("/\\"); slash != std::string::npos) {
        text.erase(0, slash + 1);
    }
    if (text.size() > 4) {
        std::string tail = text.substr(text.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == ".exe") {
            text.resize(text.size() - 4);
        }
    }
    return text;
}

std::vector<std::string> editor_arguments(const std::string& folder) { return {folder}; }

}  // namespace floserver::ui

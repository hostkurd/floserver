#include "code_editors.h"

#include "win_util.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <string>

namespace floserver::ui {

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kAppPaths[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
constexpr wchar_t kUninstall[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

bool is_file(const fs::path& p) {
    if (p.empty() || !p.is_absolute()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(p.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR found = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &found)) && found != nullptr) {
        out = fs::path(found);
    }
    CoTaskMemFree(found);
    return out;
}

// A text value of an open key; empty when it isn't there.
std::wstring value_of(HKEY key, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        bytes == 0) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value.data(), &bytes) !=
        ERROR_SUCCESS) {
        return {};
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

struct Place {
    HKEY root;
    REGSAM view;
};
// The user's own first, then the machine's (64-bit, then 32-bit programs).
std::array<Place, 3> places() {
    return {{{HKEY_CURRENT_USER, 0}, {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY}}};
}

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// The program an App Paths entry names, when it's there.
fs::path from_app_paths(EditorId id) {
    const std::string_view name = editor_app_path_name(id);
    if (name.empty()) {
        return {};
    }
    const std::wstring key = std::wstring(kAppPaths) + win::widen(name);
    for (const Place& place : places()) {
        HKEY handle = nullptr;
        if (RegOpenKeyExW(place.root, key.c_str(), 0, KEY_QUERY_VALUE | place.view, &handle) != ERROR_SUCCESS) {
            continue;
        }
        const std::wstring value = value_of(handle, nullptr);
        RegCloseKey(handle);
        const fs::path program(win::widen(display_icon_file(win::narrow(value))));
        if (is_file(program) && lower(program.filename().wstring()) == lower(editor_program(id).filename().wstring())) {
            return program;
        }
    }
    return {};
}

// PhpStorm's own installer: Program Files\JetBrains\PhpStorm 2024.2.1, the newest first.
fs::path from_jetbrains_folder(const fs::path& program_files) {
    if (program_files.empty()) {
        return {};
    }
    const fs::path jetbrains = program_files / "JetBrains";
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(jetbrains, ec), end; !ec && it != end; it.increment(ec)) {
        names.push_back(win::narrow(it->path().filename().wstring()));
    }
    for (const std::string& name : phpstorm_folders(names)) {
        const fs::path program = jetbrains / win::widen(name) / editor_program(EditorId::PhpStorm);
        if (is_file(program)) {
            return program;
        }
    }
    return {};
}

// Every Uninstall entry that is one of the editors, for those not found yet.
void from_uninstall_entries(std::vector<FoundEditor>& found) {
    const auto have = [&](EditorId id) {
        return std::any_of(found.begin(), found.end(), [&](const FoundEditor& f) { return f.id == id; });
    };
    for (const Place& place : places()) {
        HKEY list = nullptr;
        if (RegOpenKeyExW(place.root, kUninstall, 0, KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | place.view, &list) !=
            ERROR_SUCCESS) {
            continue;
        }
        std::array<wchar_t, 256> name{};
        for (DWORD i = 0;; ++i) {
            auto size = static_cast<DWORD>(name.size());
            const LSTATUS status = RegEnumKeyExW(list, i, name.data(), &size, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (status != ERROR_SUCCESS) {
                continue;  // a name too long for any of ours
            }
            HKEY entry = nullptr;
            if (RegOpenKeyExW(list, name.data(), 0, KEY_QUERY_VALUE | place.view, &entry) != ERROR_SUCCESS) {
                continue;
            }
            const auto id = editor_from_display_name(win::narrow(value_of(entry, L"DisplayName")));
            if (id && !have(*id)) {
                const fs::path program_name = editor_program(*id);
                fs::path program;
                if (const std::wstring location = value_of(entry, L"InstallLocation"); !location.empty()) {
                    program = fs::path(win::widen(display_icon_file(win::narrow(location)))) / program_name;
                }
                if (!is_file(program)) {
                    // The icon is the program itself, when it ends the same way.
                    program = fs::path(win::widen(display_icon_file(win::narrow(value_of(entry, L"DisplayIcon")))));
                    if (lower(program.filename().wstring()) != lower(program_name.filename().wstring())) {
                        program.clear();
                    }
                }
                if (is_file(program)) {
                    found.push_back({*id, program});
                }
            }
            RegCloseKey(entry);
        }
        RegCloseKey(list);
    }
}

}  // namespace

std::vector<FoundEditor> detect_editors() {
    const EditorRoots roots{known_folder(FOLDERID_LocalAppData), known_folder(FOLDERID_ProgramFiles),
                            known_folder(FOLDERID_ProgramFilesX86)};
    std::vector<FoundEditor> found;
    bool missing = false;
    for (const EditorId id : kEditors) {
        fs::path program;
        for (const fs::path& candidate : editor_candidates(id, roots)) {
            if (is_file(candidate)) {
                program = candidate;
                break;
            }
        }
        if (program.empty()) {
            program = from_app_paths(id);
        }
        if (program.empty() && id == EditorId::PhpStorm) {
            program = from_jetbrains_folder(roots.program_files);
        }
        if (program.empty()) {
            missing = true;
        } else {
            found.push_back({id, program});
        }
    }
    if (missing) {
        from_uninstall_entries(found);
    }
    // In Automatic's order, whatever order they were found in.
    std::stable_sort(found.begin(), found.end(), [](const FoundEditor& a, const FoundEditor& b) {
        return static_cast<int>(a.id) < static_cast<int>(b.id);
    });
    return found;
}

std::optional<EditorTarget> resolve_editor(const EditorChoice& choice, const std::vector<FoundEditor>& found) {
    auto target = pick_editor(choice, found);
    if (target && choice.kind == EditorChoice::Kind::Program && !is_file(target->program)) {
        return std::nullopt;
    }
    return target;
}

}  // namespace floserver::ui

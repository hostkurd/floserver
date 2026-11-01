#include "window_prefs.h"

#include "win_util.h"

#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"

#include <windows.h>

#include <filesystem>
#include <system_error>

namespace floserver::ui {

namespace {

constexpr std::size_t kMaxPrefsBytes = 16 * 1024;

std::optional<std::filesystem::path> prefs_file() {
    auto data = core::data_dir();
    if (!data) {
        return std::nullopt;
    }
    return *data / std::string(kWindowPrefsFile);
}

std::string status_text(LSTATUS status) {
    return core::error_text(std::error_code(static_cast<int>(status), std::system_category()));
}

std::wstring run_key() { return win::widen(kRunKey); }
std::wstring value_name() { return win::widen(run_value_name(core::ipc::default_instance())); }

}  // namespace

WindowPrefs load_window_prefs() {
    const auto file = prefs_file();
    if (!file) {
        return {};
    }
    const auto text = core::read_small_file(*file, kMaxPrefsBytes);
    return text ? parse_window_prefs(*text) : WindowPrefs{};
}

bool save_window_prefs(const WindowPrefs& prefs, std::string& error) {
    const auto file = prefs_file();
    if (!file) {
        error = "there's no folder for FloServer's files (LOCALAPPDATA isn't set)";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(file->parent_path(), ec);
    if (ec) {
        error = core::error_text(ec);
        return false;
    }
    return core::write_file_atomically(*file, window_prefs_json(prefs), error);
}

std::vector<std::string> windows_display_languages() {
    std::vector<std::string> out;
    ULONG count = 0;
    ULONG size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) == FALSE || size == 0) {
        return out;
    }
    std::wstring buffer(size, L'\0');
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size) == FALSE) {
        return out;
    }
    // Names one after another, each ending in a nul, the list in two.
    std::size_t start = 0;
    while (start < buffer.size() && buffer[start] != L'\0') {
        const std::size_t end = buffer.find(L'\0', start);
        const std::size_t stop = end == std::wstring::npos ? buffer.size() : end;
        out.push_back(win::narrow(std::wstring_view(buffer).substr(start, stop - start)));
        start = stop + 1;
    }
    return out;
}

Lang window_lang(const WindowPrefs& prefs) {
    const auto forced = core::get_env("FLOSERVER_LANG");
    return resolve_lang(prefs.lang, forced ? forced->c_str() : nullptr,
                        prefs.lang == LangChoice::System ? windows_display_languages() : std::vector<std::string>{});
}

std::string own_exe_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 6; ++attempt) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) {
            return {};
        }
        if (n < path.size()) {
            path.resize(n);
            return win::narrow(path);
        }
        path.resize(path.size() * 2);  // cut short: a longer path (\\?\ prefixes allow 32k)
    }
    return {};
}

std::optional<RunEntry> read_run_entry(std::string& error) {
    const std::wstring key = run_key();
    const std::wstring name = value_name();
    DWORD bytes = 0;
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                                  nullptr, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) {
        return std::nullopt;  // no entry (or no Run key at all)
    }
    if (status != ERROR_SUCCESS) {
        error = status_text(status);
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    status = RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr,
                          value.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        error = status_text(status);
        return std::nullopt;
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    auto entry = parse_run_command(win::narrow(value));
    if (!entry) {
        return RunEntry{};  // there, but empty: Windows starts nothing; shown as on so it can be turned off
    }
    return entry;
}

bool write_run_entry(bool on, bool tray, std::string& error) {
    const std::wstring key = run_key();
    const std::wstring name = value_name();
    if (!on) {
        const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), name.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            error = status_text(status);
            return false;
        }
        return true;
    }
    const std::string command = run_command(own_exe_path(), tray);
    if (command.empty()) {
        error = "FloServer.exe's path can't be written in the Run entry";
        return false;
    }
    const std::wstring value = win::widen(command);
    HKEY handle = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle,
                                     nullptr);
    if (status != ERROR_SUCCESS) {
        error = status_text(status);
        return false;
    }
    status = RegSetValueExW(handle, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS) {
        error = status_text(status);
        return false;
    }
    return true;
}

}  // namespace floserver::ui

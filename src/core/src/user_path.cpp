#include "floserver/core/user_path.h"

#include "floserver/core/paths.h"
#include "floserver/core/process.h"

#include <algorithm>
#include <cwctype>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace floserver::core {

namespace fs = std::filesystem;

namespace {

std::wstring normal(std::wstring s) {
    while (s.size() > 1 && (s.back() == L'\\' || s.back() == L'/')) {
        s.pop_back();
    }
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c))); });
    return s;
}

std::vector<std::wstring> entries(const std::wstring& path) {
    std::vector<std::wstring> out;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(L';', start);
        std::wstring part = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!part.empty()) {
            out.push_back(std::move(part));
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::wstring joined(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (const std::wstring& p : parts) {
        if (!out.empty()) {
            out += L';';
        }
        out += p;
    }
    return out;
}

#ifdef _WIN32
// The user's saved Path and its type (REG_EXPAND_SZ keeps %VARS% working).
bool read_user_path(std::wstring& value, DWORD& type) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD size = 0;
    type = REG_EXPAND_SZ;
    const LONG r = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size);
    if (r == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        value.clear();
        type = REG_EXPAND_SZ;
        return true;
    }
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    const bool ok = r == ERROR_SUCCESS &&
                    RegQueryValueExW(key, L"Path", nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &size) ==
                        ERROR_SUCCESS;
    RegCloseKey(key);
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    value = buf;
    return ok && (type == REG_SZ || type == REG_EXPAND_SZ);
}

bool write_user_path(const std::wstring& value, DWORD type, std::string& error) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        error = "can't open the user's environment settings";
        return false;
    }
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG r = RegSetValueExW(key, L"Path", 0, type, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) {
        error = "can't save the user's Path (" + std::to_string(r) + ")";
        return false;
    }
    // New terminals started from Explorer pick it up.
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 2000, &ignored);
    return true;
}
#endif

}  // namespace

bool path_has(const std::wstring& path, const std::wstring& dir) {
    const std::wstring want = normal(dir);
    const auto parts = entries(path);
    return std::any_of(parts.begin(), parts.end(), [&want](const std::wstring& p) { return normal(p) == want; });
}

std::wstring path_with(const std::wstring& path, const std::wstring& dir) {
    if (path_has(path, dir)) {
        return path;
    }
    // First: the shims before any other php or node.
    const std::wstring rest = joined(entries(path));
    return rest.empty() ? dir : dir + L";" + rest;
}

std::wstring path_without(const std::wstring& path, const std::wstring& dir) {
    const std::wstring want = normal(dir);
    std::vector<std::wstring> parts = entries(path);
    std::erase_if(parts, [&want](const std::wstring& p) { return normal(p) == want; });
    return joined(parts);
}

PathChange add_to_user_path(const fs::path& dir, std::string& error) {
#ifdef _WIN32
    std::wstring value;
    DWORD type = REG_EXPAND_SZ;
    if (!read_user_path(value, type)) {
        error = "can't read the user's Path";
        return PathChange::Failed;
    }
    if (path_has(value, dir.wstring())) {
        return PathChange::AlreadyDone;
    }
    return write_user_path(path_with(value, dir.wstring()), type, error) ? PathChange::Done : PathChange::Failed;
#else
    (void)dir;
    (void)error;
    return PathChange::Unsupported;
#endif
}

PathChange remove_from_user_path(const fs::path& dir, std::string& error) {
#ifdef _WIN32
    std::wstring value;
    DWORD type = REG_EXPAND_SZ;
    if (!read_user_path(value, type)) {
        error = "can't read the user's Path";
        return PathChange::Failed;
    }
    if (!path_has(value, dir.wstring())) {
        return PathChange::AlreadyDone;
    }
    return write_user_path(path_without(value, dir.wstring()), type, error) ? PathChange::Done : PathChange::Failed;
#else
    (void)dir;
    (void)error;
    return PathChange::Unsupported;
#endif
}

bool on_user_path(const fs::path& dir) {
#ifdef _WIN32
    std::wstring value;
    DWORD type = REG_EXPAND_SZ;
    return read_user_path(value, type) && path_has(value, dir.wstring());
#else
    const std::string path = get_env("PATH").value_or("");
    std::string want = path_to_utf8(dir);
    while (want.size() > 1 && want.back() == '/') {
        want.pop_back();
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(':', start);
        std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (part.size() > 1 && part.back() == '/') {
            part.pop_back();
        }
        if (part == want) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
#endif
}

}  // namespace floserver::core

#include "floserver/core/integration.h"

#include "floserver/core/build_info.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/shim.h"

#include <nlohmann/json.hpp>

#include <cwchar>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace floserver::core {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::size_t kComposerLimit = std::size_t{1024} * 1024;

bool is_file(const fs::path& file) {
    std::error_code ec;
    return fs::is_regular_file(file, ec);
}

#ifdef _WIN32
// The "Path" value under Software\HostKurd\FloCMS Studio in `root`.
std::optional<fs::path> studio_from_registry(HKEY root) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, L"Software\\HostKurd\\FloCMS Studio", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD type = 0;
    DWORD size = 0;
    std::optional<fs::path> found;
    if (RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_SZ &&
        size > 0 && size < 64 * 1024) {
        std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, L"Path", nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &size) ==
                ERROR_SUCCESS &&
            type == REG_SZ) {
            buf.resize(wcsnlen(buf.c_str(), buf.size()));
            if (!buf.empty()) {
                found = fs::path(buf);
            }
        }
    }
    RegCloseKey(key);
    return found;
}
#endif

}  // namespace

fs::path integration_file(const fs::path& data) {
    return data / "integration.json";
}

std::string integration_json(const IntegrationInfo& info) {
    json runtimes = json::array();
    for (const IntegrationRuntime& r : info.runtimes) {
        runtimes.push_back({{"name", r.name}, {"version", r.version}});
    }
    json databases = json::array();
    for (const IntegrationDatabase& d : info.databases) {
        databases.push_back({{"series", d.series}, {"version", d.version}, {"port", d.port}});
    }
    // ordered_json keeps the keys in this order, for people reading the file.
    nlohmann::ordered_json out;
    out["schema"] = kIntegrationSchema;
    out["floserver_version"] = std::string(kVersion);
    out["cli"] = info.cli.empty() ? std::string() : path_to_utf8(info.cli);
    out["shims"] = path_to_utf8(info.shims);
    out["runtimes"] = runtimes;
    out["databases"] = databases;
    return out.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

bool write_integration(const fs::path& data, const std::string& text, std::string& error) {
    const fs::path file = integration_file(data);
    if (const auto old = read_small_file(file, text.size() + 1); old && *old == text) {
        return true;
    }
    return write_file_atomically(file, text, error);
}

IntegrationInfo collect_integration(const fs::path& data, const std::string& database_status_json) {
    IntegrationInfo info;
    if (const auto flo = sibling_program("flo"); flo && is_file(*flo)) {
        info.cli = *flo;
    }
    info.shims = shims_dir(data);
    for (const InstalledRuntime& r : list_installed(runtimes_dir(data))) {
        info.runtimes.push_back({r.name, r.version});
    }
    const json status = json::parse(database_status_json, nullptr, false);
    if (status.is_object() && status.contains("servers") && status["servers"].is_array()) {
        for (const json& s : status["servers"]) {
            if (!s.is_object() || !s.contains("port") || !s["port"].is_number_integer()) {
                continue;
            }
            const int port = s["port"].get<int>();
            if (port <= 0) {
                continue;  // not given a port yet
            }
            info.databases.push_back({s.value("series", std::string()), s.value("version", std::string()), port});
        }
    }
    return info;
}

bool is_flocms_project(const fs::path& dir) {
    const auto text = read_small_file(dir / "composer.json", kComposerLimit);
    if (!text) {
        return false;
    }
    const json composer = json::parse(*text, nullptr, false);
    if (!composer.is_object()) {
        return false;
    }
    if (composer.contains("name") && composer["name"] == "hostkurd/flocms") {
        return true;
    }
    for (const char* section : {"require", "require-dev"}) {
        if (composer.contains(section) && composer[section].is_object() &&
            composer[section].contains("hostkurd/flocms-core")) {
            return true;
        }
    }
    return false;
}

std::optional<fs::path> find_studio() {
#ifdef _WIN32
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        if (const auto p = studio_from_registry(root); p && p->is_absolute() && is_file(*p)) {
            return p;
        }
    }
    return std::nullopt;
#elif defined(__APPLE__)
    std::vector<fs::path> places = {"/Applications"};
    if (const auto home = get_env("HOME")) {
        places.push_back(fs::path(*home) / "Applications");
    }
    for (const fs::path& place : places) {
        const fs::path program = place / "FloCMS Studio.app" / "Contents" / "MacOS" / "FloCMS Studio";
        if (is_file(program)) {
            return program;
        }
    }
    return std::nullopt;
#else
    const auto path = get_env("PATH");
    if (!path) {
        return std::nullopt;
    }
    std::size_t start = 0;
    while (start <= path->size()) {
        const std::size_t end = path->find(':', start);
        const std::string dir = path->substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty() && fs::path(dir).is_absolute()) {
            const fs::path program = fs::path(dir) / "flocms-studio";
            if (is_file(program)) {
                return program;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
#endif
}

bool open_in_studio(const fs::path& studio, const fs::path& folder, std::string& error) {
    return spawn_detached(studio, {"--open", path_to_utf8(folder)}, error);
}

}  // namespace floserver::core

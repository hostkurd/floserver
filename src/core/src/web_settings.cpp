#include "floserver/core/web_settings.h"

#include "floserver/core/paths.h"
#include "floserver/core/project_service.h"

#include <array>
#include <limits>
#include <utility>

#ifdef _WIN32
// winsock2 before windows.h (iphlpapi pulls it in).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <windows.h>

#include <filesystem>
#include <vector>
#endif

namespace floserver::core {

using nlohmann::json;

namespace {

constexpr Port kStandardHttp = 80;
constexpr Port kStandardHttps = 443;
constexpr Port kAlternateHttp = 8080;
constexpr Port kAlternateHttps = 8443;

constexpr std::array<std::pair<PortChoice, std::string_view>, 4> kChoices{{
    {PortChoice::Auto, "auto"},
    {PortChoice::Standard, "standard"},
    {PortChoice::Alternate, "alternate"},
    {PortChoice::Custom, "custom"},
}};

std::optional<PortChoice> choice_from(std::string_view name) {
    for (const auto& [choice, n] : kChoices) {
        if (n == name) {
            return choice;
        }
    }
    return std::nullopt;
}

// A port number in `value`, or nothing (with `error`).
std::optional<Port> port_from(const json& value, std::string_view key, std::string& error) {
    if (!value.is_number_integer()) {
        error = std::string(key) + " must be a port number from 1 to 65535";
        return std::nullopt;
    }
    const auto n = value.get<std::int64_t>();
    if (n < 1 || n > std::numeric_limits<Port>::max()) {
        error = std::string(key) + " must be a port number from 1 to 65535, not " + std::to_string(n);
        return std::nullopt;
    }
    return static_cast<Port>(n);
}

// Reads the keys of `doc` into `out`; the shared part of reading the file and
// applying a change. Every key is optional.
bool read_into(WebSettings& out, const json& doc, std::string_view where, std::string& error) {
    const auto prefix = [&](std::string_view key) { return std::string(where) + std::string(key); };
    if (doc.contains("ports")) {
        const json& v = doc.at("ports");
        const auto choice = v.is_string() ? choice_from(v.get<std::string>()) : std::nullopt;
        if (!choice) {
            error = prefix("ports") + R"( must be "auto", "standard", "alternate" or "custom")";
            return false;
        }
        out.ports = *choice;
    }
    for (const char* key : {"http_port", "https_port"}) {
        if (!doc.contains(key)) {
            continue;
        }
        const json& v = doc.at(key);
        std::optional<Port>& slot = std::string_view(key) == "http_port" ? out.http_port : out.https_port;
        if (v.is_null()) {
            slot.reset();
            continue;
        }
        const auto port = port_from(v, prefix(key), error);
        if (!port) {
            return false;
        }
        slot = port;
    }
    for (const char* key : {"https", "shared_https"}) {
        if (!doc.contains(key)) {
            continue;
        }
        if (!doc.at(key).is_boolean()) {
            error = prefix(key) + " must be true or false";
            return false;
        }
        (std::string_view(key) == "https" ? out.https : out.shared_https) = doc.at(key).get<bool>();
    }
    if (out.ports == PortChoice::Custom) {
        if (!out.http_port || !out.https_port) {
            error = "custom ports need both an http port and an https port";
            return false;
        }
        if (*out.http_port == *out.https_port) {
            error = "the http port and the https port must be different (both are " + std::to_string(*out.http_port) + ")";
            return false;
        }
    }
    return true;
}

json port_json(const std::optional<Port>& port) { return port ? json(*port) : json(nullptr); }

}  // namespace

std::string_view port_choice_name(PortChoice choice) {
    for (const auto& [c, n] : kChoices) {
        if (c == choice) {
            return n;
        }
    }
    return "auto";
}

std::optional<WebSettings> read_web_settings(const AppConfig& app, std::string& error) {
    WebSettings out;
    if (!app.other.is_object() || !app.other.contains("web")) {
        return out;
    }
    const json& doc = app.other.at("web");
    if (!doc.is_object()) {
        error = "\"web\" must be an object";
        return std::nullopt;
    }
    if (!read_into(out, doc, "web.", error)) {
        return std::nullopt;
    }
    return out;
}

void write_web_settings(AppConfig& app, const WebSettings& settings) {
    if (!app.other.is_object()) {
        app.other = json::object();
    }
    json doc = app.other.contains("web") && app.other.at("web").is_object() ? app.other.at("web") : json::object();
    doc["ports"] = std::string(port_choice_name(settings.ports));
    doc.erase("http_port");
    doc.erase("https_port");
    if (settings.http_port) {
        doc["http_port"] = *settings.http_port;
    }
    if (settings.https_port) {
        doc["https_port"] = *settings.https_port;
    }
    doc["https"] = settings.https;
    doc["shared_https"] = settings.shared_https;
    app.other["web"] = std::move(doc);
}

bool apply_web_settings_changes(WebSettings& settings, const json& changes, std::string& error) {
    if (!changes.is_object()) {
        error = "the changes must be an object";
        return false;
    }
    for (const auto& [key, value] : changes.items()) {
        if (key != "ports" && key != "http_port" && key != "https_port" && key != "https" && key != "shared_https") {
            error = "\"" + key + "\" isn't a web setting";
            return false;
        }
    }
    WebSettings next = settings;
    if (!read_into(next, changes, "", error)) {
        return false;
    }
    settings = next;
    return true;
}

std::optional<WebPorts> wanted_web_ports(const WebSettings& settings) {
    switch (settings.ports) {
        case PortChoice::Standard:
            return WebPorts{kStandardHttp, kStandardHttps, false};
        case PortChoice::Alternate:
            return WebPorts{kAlternateHttp, kAlternateHttps, false};
        case PortChoice::Custom:
            if (settings.http_port && settings.https_port) {
                return WebPorts{*settings.http_port, *settings.https_port, false};
            }
            return std::nullopt;
        case PortChoice::Auto:
            break;
    }
    return std::nullopt;
}

ChosenWebPorts choose_web_ports(const WebSettings& settings, const PortProbe& probe) {
    ChosenWebPorts out;
    if (const auto wanted = wanted_web_ports(settings)) {
        for (const Port p : {wanted->http, wanted->https}) {
            if (!probe(p)) {
                out.taken.push_back(p);
            }
        }
        if (out.taken.empty()) {
            out.ports = wanted;
            return out;
        }
    }
    out.ports = pick_web_ports(probe);
    if (out.ports && !out.taken.empty()) {
        out.ports->fallback = true;
    }
    return out;
}

#ifdef _WIN32

namespace {

// The listening sockets' owners for one address family, read into `rows`
// (port in host order, pid).
template <typename Table>
void listeners(ULONG family, std::vector<std::pair<Port, DWORD>>& rows) {
    DWORD size = 0;
    std::vector<unsigned char> buf;
    DWORD result = ERROR_INSUFFICIENT_BUFFER;
    for (int tries = 0; tries < 4 && result == ERROR_INSUFFICIENT_BUFFER; ++tries) {
        buf.resize(size == 0 ? 16 * 1024 : size);
        size = static_cast<DWORD>(buf.size());
        result = GetExtendedTcpTable(buf.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
    }
    if (result != NO_ERROR) {
        return;
    }
    const auto* table = reinterpret_cast<const Table*>(buf.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
        rows.emplace_back(ntohs(static_cast<u_short>(row.dwLocalPort)), row.dwOwningPid);
    }
}

std::string program_of(DWORD pid) {
    if (pid == 4) {
        return "System";  // Windows itself: http.sys (IIS, some chat and sharing apps)
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    std::wstring name(32768, L'\0');
    auto len = static_cast<DWORD>(name.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, name.data(), &len);
    CloseHandle(process);
    if (ok == FALSE) {
        return {};
    }
    name.resize(len);
    return path_to_utf8(std::filesystem::path(name).filename());
}

}  // namespace

std::optional<PortOwner> find_port_owner(Port port) {
    std::vector<std::pair<Port, DWORD>> rows;
    listeners<MIB_TCPTABLE_OWNER_PID>(AF_INET, rows);
    listeners<MIB_TCP6TABLE_OWNER_PID>(AF_INET6, rows);
    for (const auto& [p, pid] : rows) {
        if (p == port && pid != 0) {
            return PortOwner{pid, program_of(pid)};
        }
    }
    return std::nullopt;
}

#else

std::optional<PortOwner> find_port_owner(Port /*port*/) { return std::nullopt; }

#endif

WebSettingsService::WebSettingsService(ProjectService& projects, Hooks hooks)
    : projects_(projects), hooks_(std::move(hooks)) {
    if (!hooks_.probe) {
        hooks_.probe = can_listen_on_loopback;
    }
    if (!hooks_.owner) {
        hooks_.owner = find_port_owner;
    }
}

json WebSettingsService::describe(const WebSettings& settings, bool check_ports) const {
    const auto in_use = hooks_.in_use ? hooks_.in_use() : std::nullopt;
    json out = {{"ports", std::string(port_choice_name(settings.ports))},
                {"http_port", port_json(settings.http_port)},
                {"https_port", port_json(settings.https_port)},
                {"https", settings.https},
                {"shared_https", settings.shared_https},
                {"in_use", in_use ? json{{"http", in_use->http}, {"https", in_use->https}} : json(nullptr)},
                {"taken", json::array()},
                {"ca", hooks_.ca ? hooks_.ca() : json(nullptr)}};
    if (!check_ports) {
        return out;
    }
    // The ports the choice asks for ("auto": 80 and 443) that another
    // program holds. Those nginx listens on now are ours, not taken.
    const WebPorts wanted = wanted_web_ports(settings).value_or(WebPorts{kStandardHttp, kStandardHttps, false});
    for (const Port p : {wanted.http, wanted.https}) {
        if (in_use && (p == in_use->http || p == in_use->https)) {
            continue;
        }
        if (hooks_.probe(p)) {
            continue;
        }
        json taken = {{"port", p}};
        if (const auto owner = hooks_.owner(p)) {
            taken["pid"] = owner->pid;
            if (!owner->program.empty()) {
                taken["program"] = owner->program;
            }
        }
        out["taken"].push_back(std::move(taken));
    }
    return out;
}

std::optional<json> WebSettingsService::get(ServiceRefusal& refusal) const {
    const auto config = projects_.config(refusal);
    if (!config) {
        return std::nullopt;
    }
    std::string error;
    const auto settings = read_web_settings(*config, error);
    if (!settings) {
        refusal = {"config_unreadable", "FloServer's web settings in config.json can't be used: " + error};
        return std::nullopt;
    }
    return describe(*settings, true);
}

std::optional<json> WebSettingsService::set(const json& changes, ServiceRefusal& refusal) {
    WebSettings before;
    WebSettings after;
    const auto saved = projects_.change_config(
        [&](AppConfig& config, ServiceRefusal& why) {
            std::string error;
            // An unusable "web" is replaced by the change rather than kept.
            before = read_web_settings(config, error).value_or(WebSettings{});
            after = before;
            if (!apply_web_settings_changes(after, changes, error)) {
                why = {"bad_params", error};
                return false;
            }
            write_web_settings(config, after);
            return true;
        },
        refusal);
    if (!saved) {
        return std::nullopt;
    }
    if (hooks_.reload) {
        hooks_.reload();
    }
    const bool ports_changed = before.ports != after.ports || wanted_web_ports(before) != wanted_web_ports(after);
    return describe(after, ports_changed);
}

}  // namespace floserver::core

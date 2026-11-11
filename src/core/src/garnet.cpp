#include "floserver/core/garnet.h"

#include "floserver/core/child_process.h"
#include "floserver/core/mailpit.h"  // clean_error_text
#include "floserver/core/paths.h"
#include "floserver/core/version.h"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX's, not in a header
#endif

#include <algorithm>
#include <array>
#include <cwchar>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::optional<Port> port_value(const json& j) {
    if (j.is_number_integer() && j.get<std::int64_t>() > 0 && j.get<std::int64_t>() < 65536) {
        return static_cast<Port>(j.get<std::int64_t>());
    }
    return std::nullopt;
}

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool starts_with_nocase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (upper(text[i]) != upper(prefix[i])) {
            return false;
        }
    }
    return true;
}

const InstalledRuntime* newest_of(const std::vector<InstalledRuntime>& installed, std::string_view name,
                                  std::string_view series = {}, std::string_view variant = {}) {
    const auto wanted = series.empty() ? std::optional<Version>() : Version::parse(series);
    const InstalledRuntime* best = nullptr;
    for (const InstalledRuntime& r : installed) {
        if (r.name != name || (!variant.empty() && r.variant != variant)) {
            continue;
        }
        const auto v = Version::parse(r.version);
        if (!v || (wanted && !wanted->matches(*v))) {
            continue;
        }
        if (best == nullptr || *v > Version::parse(best->version).value_or(Version{})) {
            best = &r;
        }
    }
    return best;
}

// The one plain folder in `dir` (dot folders don't count), or nothing.
std::optional<fs::path> only_folder(const fs::path& dir) {
    std::optional<fs::path> only;
    int folders = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code inner;
        if (it->is_directory(inner) && !it->path().filename().native().starts_with('.')) {
            ++folders;
            only = it->path();
        }
    }
    return folders == 1 ? only : std::nullopt;
}

}  // namespace

bool hidden_runtime(std::string_view name) { return name == kDotnetRuntime || name == kPhpRedisRuntime; }

RedisSettings read_redis_settings(const AppConfig& app, std::vector<std::string>* problems) {
    RedisSettings s;
    if (!app.other.is_object() || !app.other.contains("redis")) {
        return s;
    }
    const json& r = app.other.at("redis");
    const auto problem = [problems](std::string text) {
        if (problems != nullptr) {
            problems->push_back(std::move(text));
        }
    };
    if (!r.is_object()) {
        problem("\"redis\" isn't an object");
        return s;
    }
    if (r.contains("keep_running")) {
        if (r.at("keep_running").is_boolean()) {
            s.keep_running = r.at("keep_running").get<bool>();
        } else {
            problem(R"("redis": "keep_running" is true or false)");
        }
    }
    if (r.contains("memory_mb")) {
        const json& m = r.at("memory_mb");
        if (m.is_number_integer() && m.get<std::int64_t>() >= kMinRedisMemoryMb &&
            m.get<std::int64_t>() <= kMaxRedisMemoryMb) {
            s.memory_mb = static_cast<int>(m.get<std::int64_t>());
        } else {
            problem(R"("redis": "memory_mb" is a number from )" + std::to_string(kMinRedisMemoryMb) + " to " +
                    std::to_string(kMaxRedisMemoryMb));
        }
    }
    if (r.contains("port")) {
        s.port = port_value(r.at("port"));
        if (!s.port) {
            problem(R"("redis": "port" is a port number)");
        }
    }
    return s;
}

void write_redis_settings(AppConfig& app, const RedisSettings& settings) {
    if (!app.other.is_object()) {
        app.other = json::object();
    }
    json& r = app.other["redis"];
    if (!r.is_object()) {
        r = json::object();
    }
    r["keep_running"] = settings.keep_running;
    r["memory_mb"] = settings.memory_mb;
    if (settings.port) {
        r["port"] = *settings.port;
    } else {
        r.erase("port");
    }
}

GarnetPaths garnet_paths(const fs::path& data) {
    GarnetPaths p;
    p.dir = data / "garnet";
    p.config = p.dir / "garnet.conf";
    p.socket = p.dir / "garnet.sock";
    p.checkpoints = p.dir / "checkpoints";
    p.status = p.dir / "status.json";
    p.log = data / "web" / "logs" / "garnet.log";
    return p;
}

std::optional<fs::path> garnet_entry(const fs::path& install_dir) {
#ifdef _WIN32
    const std::array<fs::path, 2> names = {"GarnetServer.dll", "GarnetServer.exe"};
#else
    const std::array<fs::path, 2> names = {"GarnetServer.dll", "GarnetServer"};
#endif
    fs::path dir = install_dir;
    for (int depth = 0; depth < 3; ++depth) {
        for (const fs::path& name : names) {
            std::error_code ec;
            if (fs::is_regular_file(dir / name, ec)) {
                return dir / name;
            }
        }
        const auto next = only_folder(dir);
        if (!next) {
            break;
        }
        dir = *next;
    }
    return std::nullopt;
}

std::optional<fs::path> dotnet_program(const fs::path& dotnet_dir) {
#ifdef _WIN32
    const fs::path p = dotnet_dir / "dotnet.exe";
#else
    const fs::path p = dotnet_dir / "dotnet";
#endif
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? std::optional<fs::path>(p) : std::nullopt;
}

std::optional<GarnetLaunch> garnet_launch(const fs::path& entry, const fs::path& dotnet_dir, std::string& error) {
    // Either kind runs on the .NET install: the apphost finds it through DOTNET_ROOT.
    const auto dotnet = dotnet_dir.empty() ? std::nullopt : dotnet_program(dotnet_dir);
    if (!dotnet) {
        error = "the .NET " + std::string(kGarnetDotnetSeries) + " runtime Garnet needs isn't installed" +
                (dotnet_dir.empty() ? std::string() : " (no dotnet program in " + path_to_utf8(dotnet_dir) + ")") +
                ": install Garnet again in Settings → Runtimes (or: flo install garnet)";
        return std::nullopt;
    }
    if (entry.extension() == ".dll") {
        return GarnetLaunch{*dotnet, {path_to_utf8(entry)}};
    }
    return GarnetLaunch{entry, {}};
}

const InstalledRuntime* newest_garnet(const std::vector<InstalledRuntime>& installed) {
    return newest_of(installed, kGarnetRuntime);
}

const InstalledRuntime* newest_dotnet(const std::vector<InstalledRuntime>& installed) {
    return newest_of(installed, kDotnetRuntime, kGarnetDotnetSeries);
}

std::optional<std::string> garnet_socket_problem(const GarnetPaths& paths) {
    const std::string text = mailpit_socket_text(paths.socket);
    if (std::any_of(text.begin(), text.end(), [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return u < 0x20 || u == 0x7F;
        })) {
        return "FloServer's data folder (" + clean_error_text(path_to_utf8(paths.dir)) +
               ") has a control character in its path, so Garnet can't make its socket there";
    }
    if (text.size() > max_local_socket_path()) {
        return "FloServer's data folder path is too long for Garnet's socket (" + std::to_string(text.size()) +
               " characters; at most " + std::to_string(max_local_socket_path()) +
               "): set a shorter FLOSERVER_DATA_DIR";
    }
    return std::nullopt;
}

std::vector<std::string> garnet_args(const GarnetPaths& paths, int memory_mb, bool windows) {
    std::vector<std::string> args = {"--config-import-path", path_to_utf8(paths.config),
                                     "--bind", "127.0.0.1",
                                     "--port", "0",
                                     "--unixsocket", mailpit_socket_text(paths.socket)};
    if (!windows) {
        // Garnet's validator allows a socket permission only on Linux, macOS
        // and FreeBSD; on Windows the owner-only folder's ACL protects it.
        args.insert(args.end(), {"--unixsocketperm", "600"});
    }
    const std::vector<std::string> rest = {
        "--checkpointdir", path_to_utf8(paths.checkpoints),
        // The default device loads native_device from runtimes/<rid>/native
        // under the working folder, which isn't Garnet's: SAVE failed with it.
        "--device-type", "RandomAccess",
        "--recover",
        "--checkpoint-freq", std::to_string(kGarnetCheckpointSeconds),
        "--lua",
        "--lua-script-timeout", "5000",
        "--lua-memory-management-mode", "Tracked",
        "--lua-script-memory-limit", "64m",
        "--memory", std::to_string(std::clamp(memory_mb, kMinRedisMemoryMb, kMaxRedisMemoryMb)) + "m",
        "--enable-debug-command", "no",
        "--enable-module-command", "no"};
    args.insert(args.end(), rest.begin(), rest.end());
    return args;
}

std::string garnet_config_text(std::string_view password) {
    const json doc = {{"AuthenticationMode", "Password"}, {"Password", std::string(password)}};
    return doc.dump(2) + "\n";
}

std::vector<std::pair<std::string, std::string>> garnet_env(const fs::path& dotnet_dir) {
    const std::string root = path_to_utf8(dotnet_dir);
    return {{"DOTNET_ROOT", root},
            {"DOTNET_ROOT_X64", root},
            {"DOTNET_gcServer", "0"},
            {"DOTNET_CLI_TELEMETRY_OPTOUT", "1"}};
}

std::vector<std::string> garnet_env_names_removed(const std::vector<std::string>& environment) {
    static constexpr std::array<std::string_view, 5> kPrefixes = {"DOTNET_", "COMPLUS_", "CORECLR_", "COR_", "COREHOST_"};
    const auto upper_of = [](std::string_view s) {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(), upper);
        return out;
    };
    std::vector<std::string> kept;
    for (const auto& [name, value] : garnet_env({})) {
        kept.push_back(upper_of(name));
    }
    std::vector<std::string> out;
    for (const std::string& name : environment) {
        const std::string u = upper_of(name);
        const bool dotnet = std::any_of(kPrefixes.begin(), kPrefixes.end(),
                                        [&u](std::string_view prefix) { return u.starts_with(prefix); });
        if (dotnet && std::find(kept.begin(), kept.end(), u) == kept.end() &&
            std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    }
    return out;
}

std::vector<std::string> environment_names() {
    std::vector<std::string> out;
#ifdef _WIN32
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return out;
    }
    for (const wchar_t* p = block; *p != L'\0'; p += std::wcslen(p) + 1) {
        const std::wstring_view entry(p);
        const std::size_t eq = entry.find(L'=', 1);  // "=C:=..." entries start with '='
        if (eq != std::wstring_view::npos && entry.front() != L'=') {
            out.push_back(path_to_utf8(fs::path(std::wstring(entry.substr(0, eq)))));
        }
    }
    FreeEnvironmentStringsW(block);
#else
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string_view entry(*e);
        if (const std::size_t eq = entry.find('='); eq != std::string_view::npos && eq > 0) {
            out.emplace_back(entry.substr(0, eq));
        }
    }
#endif
    return out;
}

FirstBytes classify_first_bytes(std::string_view data) {
    if (data.empty()) {
        return FirstBytes::NeedMore;
    }
    if (data.front() == '*') {
        return FirstBytes::Redis;  // a browser's request can't start with '*'
    }
    // Otherwise only "PING" or "QUIT" alone on the first line.
    static constexpr std::array<std::string_view, 4> kAllowed = {"PING\r\n", "QUIT\r\n", "PING\n", "QUIT\n"};
    for (const std::string_view line : kAllowed) {
        const std::size_t n = std::min(line.size(), data.size());
        if (!starts_with_nocase(line, data.substr(0, n))) {
            continue;
        }
        return data.size() >= line.size() ? FirstBytes::Redis : FirstBytes::NeedMore;
    }
    return FirstBytes::Close;
}

std::string resp_command(const std::vector<std::string_view>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const std::string_view a : args) {
        out += "$" + std::to_string(a.size()) + "\r\n";
        out += a;
        out += "\r\n";
    }
    return out;
}

std::optional<RespReply> take_resp_reply(std::string& buffer) {
    const std::size_t end = buffer.find("\r\n");
    if (buffer.empty() || end == std::string::npos) {
        return std::nullopt;
    }
    RespReply r;
    r.type = buffer.front();
    const std::string line = buffer.substr(1, end - 1);
    switch (r.type) {
        case '+':
        case '-':
        case ':':
        case '*':
            r.text = line;
            buffer.erase(0, end + 2);
            return r;
        case '$': {
            long long n = 0;
            try {
                n = std::stoll(line);
            } catch (const std::exception&) {
                r.type = '-';
                r.text = "a reply FloServer can't read";
                buffer.clear();
                return r;
            }
            if (n < 0) {
                r.null = true;
                buffer.erase(0, end + 2);
                return r;
            }
            const auto size = static_cast<std::size_t>(n);
            if (buffer.size() < end + 2 + size + 2) {
                return std::nullopt;
            }
            r.text = buffer.substr(end + 2, size);
            buffer.erase(0, end + 2 + size + 2);
            return r;
        }
        default:
            r.type = '-';
            r.text = "a reply FloServer can't read";
            buffer.clear();
            return r;
    }
}

std::string redis_prefix(std::string_view project) {
    std::string out;
    out.reserve(project.size() + 1);
    for (const char c : project) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                           c == '-';
        out += plain ? c : '_';
    }
    return out + "_";
}

std::vector<std::pair<std::string, std::string>> redis_env_values(Port port, std::string_view password,
                                                                  std::string_view project, bool phpredis) {
    return {{"REDIS_HOST", "127.0.0.1"},
            {"REDIS_PORT", std::to_string(port)},
            {"REDIS_PASSWORD", std::string(password)},
            {"REDIS_PREFIX", redis_prefix(project)},
            {"REDIS_CLIENT", phpredis ? "phpredis" : "predis"}};
}

std::optional<std::string> php_redis_vs(std::string_view series) {
    const auto v = Version::parse(series);
    if (!v || v->parts().size() != 2 || v->parts()[0] != 8) {
        return std::nullopt;
    }
    return v->parts()[1] >= 4 ? "vs17" : "vs16";
}

std::string php_redis_file_name(std::string_view version, std::string_view series) {
    const auto vs = php_redis_vs(series);
    if (!vs) {
        return {};
    }
    return "php_redis-" + std::string(version) + "-" + std::string(series) + "-nts-" + *vs + "-x64.zip";
}

std::string php_redis_upstream(std::string_view version, std::string_view file) {
    return "https://downloads.php.net/~windows/pecl/releases/redis/" + std::string(version) + "/" + std::string(file);
}

std::optional<std::string> php_redis_series_of_file(std::string_view version, std::string_view file) {
    const std::string head = "php_redis-" + std::string(version) + "-";
    if (!file.starts_with(head)) {
        return std::nullopt;
    }
    const std::string_view rest = file.substr(head.size());
    const std::size_t dash = rest.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    std::string series(rest.substr(0, dash));
    if (php_redis_file_name(version, series) != file) {
        return std::nullopt;
    }
    return series;
}

std::string php_redis_variant(std::string_view series) {
    std::string out = "php";
    for (const char c : series) {
        out += c == '.' ? '-' : c;
    }
    return out;
}

std::optional<std::string> php_redis_series_of_variant(std::string_view variant) {
    if (!variant.starts_with("php")) {
        return std::nullopt;
    }
    std::string series(variant.substr(3));
    std::replace(series.begin(), series.end(), '-', '.');
    const auto v = Version::parse(series);
    if (!v || v->parts().size() != 2 || php_redis_variant(series) != variant) {
        return std::nullopt;
    }
    return series;
}

const InstalledRuntime* php_redis_for(const std::vector<InstalledRuntime>& installed, std::string_view php_version) {
    const auto v = Version::parse(php_version);
    if (!v || v->parts().size() < 2) {
        return nullptr;
    }
    const std::string series = std::to_string(v->parts()[0]) + "." + std::to_string(v->parts()[1]);
    return newest_of(installed, kPhpRedisRuntime, {}, php_redis_variant(series));
}

std::optional<fs::path> php_redis_dll(const fs::path& install_dir) {
    std::error_code ec;
    const fs::path p = install_dir / "php_redis.dll";
    return fs::is_regular_file(p, ec) ? std::optional<fs::path>(p) : std::nullopt;
}

fs::path php_redis_extension(const std::vector<InstalledRuntime>& installed, std::string_view php_version,
                             bool windows) {
    if (!windows || newest_garnet(installed) == nullptr) {
        return {};
    }
    const InstalledRuntime* r = php_redis_for(installed, php_version);
    if (r == nullptr) {
        return {};
    }
    return php_redis_dll(r->dir).value_or(fs::path());
}

}  // namespace floserver::core

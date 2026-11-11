#pragma once

// Redis for the projects (D30), through Microsoft's Garnet
// (github.com/microsoft/garnet, MIT): it speaks RESP, so Redis clients
// (phpredis, predis, ioredis, redis-py) work unchanged, and it runs natively
// on Windows, where modern Redis has no build and Valkey no official one.
// Lua (--lua), pub/sub, transactions, SELECT, BLPOP/BRPOP, sorted sets and
// SCAN work; streams (XADD...) and WAIT don't.
//
// Runtimes (the signed list):
//   garnet     win-x64-based-readytorun.zip, framework-dependent: it needs
//   dotnet     the .NET 10 runtime (Microsoft's dotnet-runtime-10.0.<n>-win-x64.zip),
//              a hidden kind installed with Garnet and never on the user's PATH;
//   php-redis  PECL's phpredis DLL for one PHP series (variant "php8-4"),
//              hidden too, installed with Garnet for each PHP series installed
//              (and with a PHP installed later while Garnet is).
//
// One shared server: the service holds 127.0.0.1:6379 (or 6380... when it's
// taken; chosen once and kept in config.json "redis": {"port"}) and relays
// each connection to Garnet's Unix-domain socket in the owner-only
// <data>/garnet (no other program can take it). Garnet starts on the first
// connection and is stopped when idle (SAVE first, so the data comes back
// with --recover); "keep_running" keeps it up.
//
// Security: a web page can send HTTP to a local port, and a Redis-style
// server reads its lines as commands (with Laravel queues that can end in
// running code). So Garnet always asks for a password (32 random bytes, hex,
// kept in the secure store as "garnet/password"; in its owner-only config
// file, never in an argument), and the relay closes a connection whose first
// bytes look like HTTP before anything reaches Garnet.
//
// Garnet always has a TCP listener besides the socket: it's on 127.0.0.1 port
// 0, so the system picks a free port as it binds (nothing can take it first)
// and nothing uses it; the password guards it too.
//
// <data>/garnet (never cleaned up by Settings → Folders, never backed up;
// Remove FloServer deletes it with the databases):
//   garnet.conf    {"AuthenticationMode": "Password", "Password": ...}, owner-only
//   garnet.sock    Garnet's socket (0600)
//   checkpoints/   what SAVE writes and --recover reads
//   status.json    the last key count, shown while Garnet sleeps
// The log is <data>/web/logs/garnet.log (Clear old logs empties it).

#include "floserver/core/ports.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace floserver::core {

inline constexpr std::string_view kGarnetRuntime = "garnet";
inline constexpr std::string_view kDotnetRuntime = "dotnet";
inline constexpr std::string_view kPhpRedisRuntime = "php-redis";
// The .NET series Garnet's build needs (its runtimeconfig.json rolls forward
// within a major version only).
inline constexpr std::string_view kGarnetDotnetSeries = "10";
inline constexpr Port kFirstRedisPort = 6379;
inline constexpr std::string_view kRedisSecretName = "garnet/password";

// Runtimes the window never shows as their own card (installed as a part of another).
[[nodiscard]] bool hidden_runtime(std::string_view name);

// Settings → Redis, config.json's "redis" (keys this version doesn't know are kept):
//   {"keep_running": false, "memory_mb": 256, "port": 6379}
struct RedisSettings {
    std::optional<Port> port;  // the public port, once chosen
    int memory_mb = 256;       // Garnet's main-log memory (--memory)
    bool keep_running = false;

    bool operator==(const RedisSettings&) const = default;
};

inline constexpr int kDefaultRedisMemoryMb = 256;
inline constexpr int kMinRedisMemoryMb = 16;
inline constexpr int kMaxRedisMemoryMb = 65536;

// A value that can't be used is left at its default (`problems` says which).
[[nodiscard]] RedisSettings read_redis_settings(const AppConfig& app, std::vector<std::string>* problems = nullptr);
// Writes them into AppConfig::other["redis"], keeping its other keys.
void write_redis_settings(AppConfig& app, const RedisSettings& settings);

struct GarnetPaths {
    std::filesystem::path dir;          // <data>/garnet
    std::filesystem::path config;       // garnet.conf
    std::filesystem::path socket;       // garnet.sock
    std::filesystem::path checkpoints;  // checkpoints/
    std::filesystem::path status;       // status.json
    std::filesystem::path log;          // <data>/web/logs/garnet.log
};

[[nodiscard]] GarnetPaths garnet_paths(const std::filesystem::path& data);

// How Garnet is started: `program` with `args` before Garnet's own. A
// framework-dependent build is run as "<dotnet>/dotnet GarnetServer.dll",
// so the .NET install FloServer chose runs it and no other one is looked
// for; a build with only the apphost (GarnetServer.exe) runs as it is, with
// DOTNET_ROOT set.
struct GarnetLaunch {
    std::filesystem::path program;
    std::vector<std::string> args;
};

// GarnetServer.dll (or the apphost, GarnetServer.exe; GarnetServer
// elsewhere) in a Garnet install: at its top, in its one folder, or in
// that folder's one folder (a release zip's layout), never deeper.
[[nodiscard]] std::optional<std::filesystem::path> garnet_entry(const std::filesystem::path& install_dir);
// The dotnet host in a .NET runtime install (dotnet.exe; dotnet elsewhere).
[[nodiscard]] std::optional<std::filesystem::path> dotnet_program(const std::filesystem::path& dotnet_dir);
// The launch for this Garnet entry and .NET install, or nothing (with `error`).
[[nodiscard]] std::optional<GarnetLaunch> garnet_launch(const std::filesystem::path& entry,
                                                        const std::filesystem::path& dotnet_dir, std::string& error);

// The newest installed Garnet, and the newest .NET runtime of kGarnetDotnetSeries.
[[nodiscard]] const InstalledRuntime* newest_garnet(const std::vector<InstalledRuntime>& installed);
[[nodiscard]] const InstalledRuntime* newest_dotnet(const std::vector<InstalledRuntime>& installed);

// Why the socket can't be made in `paths.dir` (a path longer than sun_path
// takes, or with a control character), or nothing when it can.
[[nodiscard]] std::optional<std::string> garnet_socket_problem(const GarnetPaths& paths);

// Garnet's arguments (after the launch's own): its config file (the
// password), 127.0.0.1 port 0, the socket (0600), checkpoints and --recover,
// Lua, the memory size, no DEBUG or MODULE commands. Nothing secret.
[[nodiscard]] std::vector<std::string> garnet_args(const GarnetPaths& paths, int memory_mb);
// garnet.conf's text: Garnet's own JSON config with the password.
[[nodiscard]] std::string garnet_config_text(std::string_view password);
// The environment Garnet gets: DOTNET_ROOT and DOTNET_ROOT_X64 at the .NET
// install, workstation GC (a dev box's memory, not a server's), no telemetry.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> garnet_env(const std::filesystem::path& dotnet_dir);
// .NET variables taken out of Garnet's environment, so nothing set there
// (a startup hook, other dependencies, another runtime) changes what runs.
[[nodiscard]] const std::vector<std::string>& garnet_env_names_removed();

// --- the relay's first look -------------------------------------------------------

enum class FirstBytes : std::uint8_t {
    NeedMore,  // read on (up to kMaxSniffBytes)
    Redis,     // forward
    Http,      // close: a web page's request (or nothing a Redis client sends)
};
inline constexpr std::size_t kMaxSniffBytes = 8192;

// What a connection's first bytes are. A RESP array ('*', what every Redis
// client sends) is Redis. Otherwise it's an inline command, judged on its
// whole first line: one that starts with an HTTP method and a space (GET,
// POST, PUT, HEAD, OPTIONS, DELETE, PATCH, CONNECT, TRACE, PRI; any case) or
// holds " HTTP/", or any line in what came that starts with "Host:",
// "Origin:" or "User-Agent:", is HTTP. kMaxSniffBytes without a line end is HTTP too.
[[nodiscard]] FirstBytes classify_first_bytes(std::string_view data);

// --- RESP, for the service's own commands -----------------------------------------

// One command as a RESP array of bulk strings.
[[nodiscard]] std::string resp_command(const std::vector<std::string_view>& args);

struct RespReply {
    char type = 0;     // '+', '-', ':', '$' (nothing: "$-1"), '*' (only its count)
    std::string text;  // the line or the bulk string
    bool null = false;
};
// The first whole reply in `buffer` (it's taken out), or nothing while it's
// incomplete. Arrays give their count in `text` and their items aren't read.
[[nodiscard]] std::optional<RespReply> take_resp_reply(std::string& buffer);

// --- per project ---------------------------------------------------------------------

// "<project>_": the key prefix that keeps projects apart on the shared
// server; characters other than letters, digits, '-' and '_' become '_'.
[[nodiscard]] std::string redis_prefix(std::string_view project);
// The .env lines for a project with Redis on: REDIS_HOST, REDIS_PORT,
// REDIS_PASSWORD, REDIS_PREFIX and REDIS_CLIENT (phpredis or predis).
[[nodiscard]] std::vector<std::pair<std::string, std::string>> redis_env_values(Port port, std::string_view password,
                                                                                std::string_view project,
                                                                                bool phpredis);

// --- phpredis (PECL's Windows builds) --------------------------------------------------

// The Visual Studio PHP's Windows builds of a series use: "vs17" for 8.4 and
// later, "vs16" for 8.0 to 8.3; nothing for older ones.
[[nodiscard]] std::optional<std::string> php_redis_vs(std::string_view series);
// PECL's file: php_redis-6.3.0-8.4-nts-vs17-x64.zip (empty when php_redis_vs() has none).
[[nodiscard]] std::string php_redis_file_name(std::string_view version, std::string_view series);
// Its original address on downloads.php.net.
[[nodiscard]] std::string php_redis_upstream(std::string_view version, std::string_view file);
// The PHP series in such a file name ("8.4"), checked against php_redis_vs(); nothing otherwise.
[[nodiscard]] std::optional<std::string> php_redis_series_of_file(std::string_view version, std::string_view file);
// The runtime's variant for a series: "php8-4" (the list's variants have no dots).
[[nodiscard]] std::string php_redis_variant(std::string_view series);
// The series back from a variant: "8.4"; nothing for anything else.
[[nodiscard]] std::optional<std::string> php_redis_series_of_variant(std::string_view variant);
// The newest php-redis install for a PHP version's series, or nothing.
[[nodiscard]] const InstalledRuntime* php_redis_for(const std::vector<InstalledRuntime>& installed,
                                                    std::string_view php_version);
// php_redis.dll in such an install (at its top), or nothing.
[[nodiscard]] std::optional<std::filesystem::path> php_redis_dll(const std::filesystem::path& install_dir);
// The DLL a project's php.ini loads for this PHP: when Garnet is installed
// and a matching php-redis is. Windows only (elsewhere the build has its
// own extensions; `windows` is for tests); empty otherwise.
#ifdef _WIN32
inline constexpr bool kPhpRedisPlatform = true;
#else
inline constexpr bool kPhpRedisPlatform = false;
#endif
[[nodiscard]] std::filesystem::path php_redis_extension(const std::vector<InstalledRuntime>& installed,
                                                        std::string_view php_version,
                                                        bool windows = kPhpRedisPlatform);

}  // namespace floserver::core

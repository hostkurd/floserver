// Redis through Garnet (D30): settings, Garnet's arguments and config file,
// the relay's first look at a connection, RESP, the .env lines, phpredis's
// file names and php.ini, "redis" in floserver.json, and the installs that
// come with Garnet (the .NET runtime, phpredis per PHP series).

#include "floserver/core/garnet.h"
#include "floserver/core/mailpit.h"
#include "floserver/core/paths.h"
#include "floserver/core/php_pool.h"
#include "floserver/core/php_settings.h"
#include "floserver/core/project_config.h"
#include "floserver/core/runtime_service.h"

#include "test_util.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <thread>

using namespace floserver::core;
using namespace test_util;
using namespace std::chrono;

namespace {

void fake_install(const fs::path& data, const std::string& name, const std::string& version,
                  const std::string& variant = "", const std::vector<std::string>& files = {}) {
    const std::string folder = version + (variant.empty() || variant == "nts" ? "" : "-" + variant);
    const fs::path dir = runtimes_dir(data) / name / folder;
    fs::create_directories(dir);
    for (const std::string& f : files) {
        fs::create_directories((dir / f).parent_path());
        write_file(dir / f, "x");
    }
    write_file(dir / ".floserver-runtime.json",
               nlohmann::json{{"schema", 1}, {"name", name}, {"version", version}, {"platform", "windows-x64"},
                              {"variant", variant}, {"license", "MIT"}, {"sha256", std::string(64, 'a')}}
                   .dump());
}

}  // namespace

TEST_CASE("Redis settings: defaults, values kept, bad ones left at their default") {
    AppConfig app;
    CHECK(read_redis_settings(app) == RedisSettings{});
    CHECK(read_redis_settings(app).memory_mb == kDefaultRedisMemoryMb);
    CHECK_FALSE(read_redis_settings(app).keep_running);

    app.other = nlohmann::json{{"redis", {{"keep_running", true}, {"memory_mb", 512}, {"port", 6380}, {"later", 1}}}};
    const RedisSettings s = read_redis_settings(app);
    CHECK(s.keep_running);
    CHECK(s.memory_mb == 512);
    CHECK(s.port == Port{6380});

    RedisSettings changed = s;
    changed.memory_mb = 64;
    changed.port.reset();
    write_redis_settings(app, changed);
    CHECK(app.other["redis"]["memory_mb"] == 64);
    CHECK_FALSE(app.other["redis"].contains("port"));
    CHECK(app.other["redis"]["later"] == 1);  // a newer version's key stays

    std::vector<std::string> problems;
    app.other = nlohmann::json{{"redis", {{"keep_running", "yes"}, {"memory_mb", 4}, {"port", 70000}}}};
    const RedisSettings bad = read_redis_settings(app, &problems);
    CHECK(bad == RedisSettings{});
    CHECK(problems.size() == 3);
}

TEST_CASE("Garnet's arguments: the socket, 127.0.0.1 port 0, checkpoints and --recover, never the password") {
    const GarnetPaths p = garnet_paths("/data");
    CHECK(p.dir == fs::path("/data/garnet"));
    CHECK(p.log == fs::path("/data/web/logs/garnet.log"));
    const auto args = garnet_args(p, 256, false);
    const auto value = [&](const std::string& key) {
        const auto it = std::find(args.begin(), args.end(), key);
        return it != args.end() && it + 1 != args.end() ? *(it + 1) : std::string("(none)");
    };
    CHECK(value("--config-import-path") == path_to_utf8(p.config));
    CHECK(value("--bind") == "127.0.0.1");
    CHECK(value("--port") == "0");  // the system picks it as Garnet binds: nothing can take it first
    CHECK(value("--unixsocket") == mailpit_socket_text(p.socket));
    CHECK(value("--unixsocketperm") == "600");
    CHECK(value("--checkpointdir") == path_to_utf8(p.checkpoints));
    CHECK(value("--memory") == "256m");
    CHECK(value("--enable-debug-command") == "no");
    CHECK(value("--enable-module-command") == "no");
    CHECK(std::find(args.begin(), args.end(), "--recover") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--lua") != args.end());
    for (const std::string& a : args) {
        CHECK(a.find("assword") == std::string::npos);
        CHECK(a != "--auth");
    }
    CHECK(value("--device-type") == "RandomAccess");  // the default device's native library isn't found
    CHECK(value("--checkpoint-freq") == "300");
    CHECK(value("--lua-script-timeout") == "5000");
    CHECK(value("--lua-memory-management-mode") == "Tracked");
    CHECK(value("--lua-script-memory-limit") == "64m");
    const auto low = garnet_args(p, 1, false);
    CHECK(low[std::size_t(std::find(low.begin(), low.end(), "--memory") - low.begin()) + 1] == "16m");
    // Windows: no socket permission (Garnet refuses one there), the rest the same.
    const auto win = garnet_args(p, 256, true);
    CHECK(std::find(win.begin(), win.end(), "--unixsocketperm") == win.end());
    CHECK(std::find(win.begin(), win.end(), "--unixsocket") != win.end());
    CHECK(win.size() + 2 == garnet_args(p, 256, false).size());
    CHECK(std::find(win.begin(), win.end(), "RandomAccess") != win.end());

    const auto conf = nlohmann::json::parse(garnet_config_text("abc123"));
    CHECK(conf["AuthenticationMode"] == "Password");
    CHECK(conf["Password"] == "abc123");
}

TEST_CASE("Garnet's environment: its .NET install, and nothing that changes what runs") {
    const auto env = garnet_env("/rt/dotnet/10.0.3");
    const auto get = [&](const std::string& k) {
        for (const auto& [key, v] : env) {
            if (key == k) {
                return v;
            }
        }
        return std::string("(none)");
    };
    CHECK(get("DOTNET_ROOT") == path_to_utf8(fs::path("/rt/dotnet/10.0.3")));
    CHECK(get("DOTNET_ROOT_X64") == get("DOTNET_ROOT"));
    CHECK(get("DOTNET_gcServer") == "0");
    const std::vector<std::string> environment = {"PATH", "DOTNET_STARTUP_HOOKS", "dotnet_additional_deps",
                                                  "DOTNET_ROOT", "DOTNET_gcServer", "COMPlus_EnableDiagnostics",
                                                  "CORECLR_PROFILER", "COR_ENABLE_PROFILING", "COREHOST_TRACE",
                                                  "DOTNET_ROOT(x86)", "HOME", "CORNERSTONE"};
    const auto removed = garnet_env_names_removed(environment);
    const std::vector<std::string> want = {"DOTNET_STARTUP_HOOKS", "dotnet_additional_deps", "COMPlus_EnableDiagnostics",
                                           "CORECLR_PROFILER", "COR_ENABLE_PROFILING", "COREHOST_TRACE",
                                           "DOTNET_ROOT(x86)"};
    CHECK(removed == want);  // DOTNET_ROOT and DOTNET_gcServer are FloServer's own; PATH, HOME and CORNERSTONE aren't .NET's
    CHECK_FALSE(environment_names().empty());
}

TEST_CASE("Garnet's program in its install, the launch through dotnet, and the newest installs") {
    const TempDir tmp;
    fake_install(tmp.path(), "garnet", "2.2.4", "", {"GarnetServer.dll", "GarnetServer.runtimeconfig.json"});
    fake_install(tmp.path(), "garnet", "2.2.3", "", {"win-x64/net10.0/GarnetServer.dll"});
    fake_install(tmp.path(), "garnet", "2.2.1", "", {"a/GarnetServer.dll", "b/x"});
    fake_install(tmp.path(), "dotnet", "10.0.3", "", {"dotnet", "dotnet.exe"});
    fake_install(tmp.path(), "dotnet", "10.0.12", "", {"dotnet", "dotnet.exe"});
    fake_install(tmp.path(), "dotnet", "11.0.1", "", {"dotnet", "dotnet.exe"});
    const auto installed = list_installed(runtimes_dir(tmp.path()));
    const InstalledRuntime* garnet = newest_garnet(installed);
    REQUIRE(garnet != nullptr);
    CHECK(garnet->version == "2.2.4");
    const InstalledRuntime* dotnet = newest_dotnet(installed);
    REQUIRE(dotnet != nullptr);
    CHECK(dotnet->version == "10.0.12");  // the series Garnet's build needs, not 11

    CHECK(garnet_entry(garnet->dir) == garnet->dir / "GarnetServer.dll");
    const fs::path r223 = runtimes_dir(tmp.path()) / "garnet" / "2.2.3";
    CHECK(garnet_entry(r223) == r223 / "win-x64" / "net10.0" / "GarnetServer.dll");
    CHECK_FALSE(garnet_entry(runtimes_dir(tmp.path()) / "garnet" / "2.2.1"));  // two folders: not guessed

    std::string error;
    const auto launch = garnet_launch(garnet->dir / "GarnetServer.dll", dotnet->dir, error);
    REQUIRE(launch);
    CHECK(launch->program == *dotnet_program(dotnet->dir));
    CHECK(launch->args == std::vector<std::string>{path_to_utf8(garnet->dir / "GarnetServer.dll")});
    CHECK_FALSE(garnet_launch(garnet->dir / "GarnetServer.dll", tmp.path() / "nothing", error));
    CHECK(error.find("dotnet") != std::string::npos);
    // The release's layout (2.2.4): net10.0/GarnetServer.exe, a framework-dependent
    // single-file apphost and no DLL. It runs as it is, and needs .NET too.
#ifdef _WIN32
    const char* kApphost = "GarnetServer.exe";
#else
    const char* kApphost = "GarnetServer";
#endif
    fake_install(tmp.path(), "garnet", "2.2.5", "", {std::string("net10.0/") + kApphost, "net10.0/lua54.dll"});
    const fs::path r225 = runtimes_dir(tmp.path()) / "garnet" / "2.2.5";
    REQUIRE(garnet_entry(r225) == r225 / "net10.0" / kApphost);
    const auto apphost = garnet_launch(*garnet_entry(r225), dotnet->dir, error);
    REQUIRE(apphost);
    CHECK(apphost->program == r225 / "net10.0" / kApphost);
    CHECK(apphost->args.empty());
    error.clear();
    CHECK_FALSE(garnet_launch(*garnet_entry(r225), {}, error));
    CHECK(error.find(".NET 10") != std::string::npos);

    CHECK(hidden_runtime("dotnet"));
    CHECK(hidden_runtime("php-redis"));
    CHECK_FALSE(hidden_runtime("garnet"));
    CHECK_FALSE(hidden_runtime("php"));
}

TEST_CASE("the relay's first look: RESP arrays and a bare PING or QUIT pass, everything else is closed") {
    CHECK(classify_first_bytes("") == FirstBytes::NeedMore);
    CHECK(classify_first_bytes("*1\r\n$4\r\nPING\r\n") == FirstBytes::Redis);
    CHECK(classify_first_bytes("*") == FirstBytes::Redis);  // what every client library sends
    for (const char* ok : {"PING\r\n", "ping\r\n", "QUIT\r\n", "Quit\n", "PING\n", "PING\r\n*1\r\n$4\r\nPING\r\n"}) {
        CAPTURE(ok);
        CHECK(classify_first_bytes(ok) == FirstBytes::Redis);
    }
    for (const char* more : {"P", "PI", "pin", "PING", "PING\r", "Q", "QUI", "QUIT\r"}) {
        CAPTURE(more);
        CHECK(classify_first_bytes(more) == FirstBytes::NeedMore);
    }
    CHECK(classify_first_bytes("GET / HTTP/1.1\r\nHost: 127.0.0.1:6379\r\n\r\n") == FirstBytes::Close);
    CHECK(classify_first_bytes("POST /x HTTP/1.1\r\n") == FirstBytes::Close);
    CHECK(classify_first_bytes("G") == FirstBytes::Close);
    for (const char* m : {"PUT", "HEAD", "OPTIONS", "DELETE", "PATCH", "CONNECT", "TRACE", "PRI"}) {
        CHECK(classify_first_bytes(std::string(m) + " * HTTP/1.1\r\n") == FirstBytes::Close);
    }
    // Inline commands other than PING and QUIT, PING with words after it, a leading blank line, TLS.
    for (const char* bad : {"GET key\r\n", "SET a b\r\n", "AUTH secret\n", "FLUSHALL\r\n", "PING hello\r\n",
                            "PINGX\r\n", "\r\nPING\r\n", "\n*1\r\n", " PING\r\n", "Host: x\r\n"}) {
        CAPTURE(bad);
        CHECK(classify_first_bytes(bad) == FirstBytes::Close);
    }
    const std::string tls = {'\x16', '\x03', '\x01', '\x02', '\x00', '\x01'};  // a TLS ClientHello's first bytes
    CHECK(classify_first_bytes(tls) == FirstBytes::Close);
}

TEST_CASE("RESP: commands written, replies read whole or not at all") {
    CHECK(resp_command({"AUTH", "pw"}) == "*2\r\n$4\r\nAUTH\r\n$2\r\npw\r\n");
    CHECK(resp_command({}) == "*0\r\n");

    std::string b = "+OK\r\n:12\r\n-NOAUTH Authentication required.\r\n$5\r\nhel";
    auto r = take_resp_reply(b);
    REQUIRE(r);
    CHECK(r->type == '+');
    CHECK(r->text == "OK");
    r = take_resp_reply(b);
    REQUIRE(r);
    CHECK(r->type == ':');
    CHECK(r->text == "12");
    r = take_resp_reply(b);
    REQUIRE(r);
    CHECK(r->type == '-');
    CHECK(r->text.starts_with("NOAUTH"));
    CHECK_FALSE(take_resp_reply(b));  // the bulk string isn't all there
    b += "lo\r\n$-1\r\n";
    r = take_resp_reply(b);
    REQUIRE(r);
    CHECK(r->type == '$');
    CHECK(r->text == "hello");
    r = take_resp_reply(b);
    REQUIRE(r);
    CHECK(r->null);
    CHECK(b.empty());
    std::string junk = "?what\r\n";
    r = take_resp_reply(junk);
    REQUIRE(r);
    CHECK(r->type == '-');
}

TEST_CASE("a project's Redis lines: the shared server, its own prefix, phpredis or predis") {
    CHECK(redis_prefix("shop") == "shop_");
    CHECK(redis_prefix("api-shop") == "api-shop_");
    CHECK(redis_prefix("my shop.v2") == "my_shop_v2_");
    const auto env = redis_env_values(6380, "pw", "shop", true);
    const std::vector<std::pair<std::string, std::string>> want = {{"REDIS_HOST", "127.0.0.1"},
                                                                   {"REDIS_PORT", "6380"},
                                                                   {"REDIS_PASSWORD", "pw"},
                                                                   {"REDIS_PREFIX", "shop_"},
                                                                   {"REDIS_CLIENT", "phpredis"}};
    CHECK(env == want);
    CHECK(redis_env_values(6379, "pw", "x", false).back().second == "predis");
}

TEST_CASE("phpredis: PECL's file names per PHP series, the variant, and the DLL a project loads") {
    CHECK(php_redis_vs("8.5") == "vs17");
    CHECK(php_redis_vs("8.4") == "vs17");
    CHECK(php_redis_vs("8.3") == "vs16");
    CHECK(php_redis_vs("8.2") == "vs16");
    CHECK_FALSE(php_redis_vs("7.4"));
    CHECK_FALSE(php_redis_vs("8.4.1"));
    CHECK(php_redis_file_name("6.3.0", "8.4") == "php_redis-6.3.0-8.4-nts-vs17-x64.zip");
    CHECK(php_redis_file_name("6.3.0", "8.3") == "php_redis-6.3.0-8.3-nts-vs16-x64.zip");
    CHECK(php_redis_file_name("6.3.0", "7.4").empty());
    CHECK(php_redis_upstream("6.3.0", "php_redis-6.3.0-8.4-nts-vs17-x64.zip") ==
          "https://downloads.php.net/~windows/pecl/releases/redis/6.3.0/php_redis-6.3.0-8.4-nts-vs17-x64.zip");
    CHECK(php_redis_series_of_file("6.3.0", "php_redis-6.3.0-8.5-nts-vs17-x64.zip") == "8.5");
    CHECK_FALSE(php_redis_series_of_file("6.3.0", "php_redis-6.3.0-8.5-ts-vs17-x64.zip"));
    CHECK_FALSE(php_redis_series_of_file("6.3.0", "php_redis-6.3.0-8.5-nts-vs17-x86.zip"));
    CHECK_FALSE(php_redis_series_of_file("6.3.0", "php_redis-6.3.0-8.3-nts-vs17-x64.zip"));  // 8.3 is vs16
    CHECK_FALSE(php_redis_series_of_file("6.2.0", "php_redis-6.3.0-8.4-nts-vs17-x64.zip"));
    CHECK(php_redis_variant("8.4") == "php8-4");
    CHECK(php_redis_series_of_variant("php8-4") == "8.4");
    CHECK_FALSE(php_redis_series_of_variant("php8"));
    CHECK_FALSE(php_redis_series_of_variant("nts"));

    const TempDir tmp;
    fake_install(tmp.path(), "php", "8.4.27", "nts", {"php-cgi.exe"});
    fake_install(tmp.path(), "php-redis", "6.3.0", "php8-4", {"php_redis.dll"});
    fake_install(tmp.path(), "php-redis", "6.2.0", "php8-4", {"php_redis.dll"});
    fake_install(tmp.path(), "php-redis", "6.3.0", "php8-3", {"php_redis.dll"});
    auto installed = list_installed(runtimes_dir(tmp.path()));
    const InstalledRuntime* r = php_redis_for(installed, "8.4.27");
    REQUIRE(r != nullptr);
    CHECK(r->version == "6.3.0");
    CHECK(r->variant == "php8-4");
    CHECK(php_redis_for(installed, "8.5.1") == nullptr);
    const fs::path dll = r->dir / "php_redis.dll";
    // Only while Garnet is installed, and only on Windows.
    CHECK(php_redis_extension(installed, "8.4.27", true).empty());
    fake_install(tmp.path(), "garnet", "2.2.4", "", {"GarnetServer.dll"});
    installed = list_installed(runtimes_dir(tmp.path()));
    CHECK(php_redis_extension(installed, "8.4.27", true) == dll);
    CHECK(php_redis_extension(installed, "8.4.27", false).empty());
    CHECK(php_redis_extension(installed, "8.5.1", true).empty());
}

TEST_CASE("phpredis in a pool's key and php.ini: its own pool, restarted when the DLL changes") {
    PhpIniSettings plain;
    plain.series = "8.4";
    PhpIniSettings with = plain;
    with.redis_extension = "C:/fs/runtimes/php-redis/6.3.0-php8-4/php_redis.dll";
    CHECK(php_settings_key(plain).empty());
    const std::string key = php_settings_key(with);
    CHECK(key.starts_with("+redis-"));
    PhpIniSettings newer = with;
    newer.redis_extension = "C:/fs/runtimes/php-redis/6.4.0-php8-4/php_redis.dll";
    CHECK(php_settings_key(newer) != key);
#ifdef _WIN32
    const TempDir tmp;
    fs::create_directories(tmp.path() / "ext");
    const PhpIniResult made = make_php_ini(tmp.path(), with, tmp.path() / "e.log");
    CHECK(made.text.find("php_redis.dll") != std::string::npos);
#endif
}

TEST_CASE("floserver.json's \"redis\": true turns it on, a wrong value is an error, flo use redis on/off") {
    auto r = parse_project_config(R"({"php": "8.4", "redis": true})");
    REQUIRE(r.config);
    CHECK(r.config->redis);
    CHECK(r.warnings.empty());
    CHECK(write_project_config(*r.config).find("\"redis\": true") != std::string::npos);
    r = parse_project_config(R"({"redis": false})");
    REQUIRE(r.config);
    CHECK_FALSE(r.config->redis);
    CHECK(write_project_config(*r.config).find("redis") == std::string::npos);
    r = parse_project_config(R"({"redis": "yes"})");
    CHECK_FALSE(r.config);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].key == "redis");

    CHECK(parse_config_key("redis") == ConfigKey::Redis);
    std::string error;
    auto text = set_project_config_value(R"({"php": "8.4"})", ConfigKey::Redis, "redis", std::string("on"), error);
    REQUIRE(text);
    CHECK(parse_project_config(*text).config->redis);
    CHECK(parse_project_config(*text).config->php == "8.4");
    text = set_project_config_value(*text, ConfigKey::Redis, "redis", std::string("off"), error);
    REQUIRE(text);
    CHECK(text->find("redis") == std::string::npos);
    CHECK_FALSE(set_project_config_value("{}", ConfigKey::Redis, "redis", std::string("maybe"), error));
    CHECK(error.find("on or off") != std::string::npos);
}

namespace {

const SigningKey kKey(0x33);
const std::string kKeyId = "garnet-test";
const ManifestSource kSource{"https://cdn.example.org/manifest.json", "https://cdn.example.org/manifest.json.sig"};

std::string zip_of(const std::vector<std::pair<std::string, std::string>>& files) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, content] : files) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(),
                                      static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)));
    }
    void* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
    std::string out(static_cast<const char*>(buffer), size);
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return out;
}

struct GarnetWorld {
    TempDir tmp;
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::map<std::string, std::string> files;  // url -> bytes
    nlohmann::json runtimes = nlohmann::json::array();

    void add(const std::string& name, const std::string& version, const std::string& variant,
             const std::vector<std::pair<std::string, std::string>>& content) {
        const std::string bytes = zip_of(content);
        const std::string url = "https://cdn.example.org/" + name + "/" + version + "/" + variant + ".zip";
        nlohmann::json e = {{"name", name},     {"version", version},  {"platform", "windows-x64"},
                            {"archive", "zip"}, {"size", bytes.size()}, {"sha256", sha256_hex(bytes)},
                            {"license", "MIT"}, {"urls", {url}}};
        if (!variant.empty()) {
            e["variant"] = variant;
        }
        runtimes.push_back(e);
        files[url] = bytes;
    }
    void publish() {
        const std::string bytes =
            nlohmann::json{{"schema", 1}, {"sequence", 3}, {"expires", "2026-12-01T00:00:00Z"}, {"runtimes", runtimes}}
                .dump();
        transport->serve(kSource.manifest_url, {200, bytes});
        transport->serve(kSource.signature_url, {200, kKey.sign(bytes, kKeyId)});
        for (const auto& [url, b] : files) {
            transport->serve(url, {200, b});
        }
    }
    [[nodiscard]] RuntimeServiceConfig config() const {
        RuntimeServiceConfig c;
        c.data_dir = tmp.path();
        c.platform = "windows-x64";
        c.keys = {{kKeyId, kKey.pub}};
        c.source = kSource;
        c.now = [] { return *parse_utc_time("2026-11-11T12:00:00Z"); };
        c.sleep = [](milliseconds) {};
        return c;
    }
};

nlohmann::json finished(RuntimeService& s, std::int64_t id) {
    for (int i = 0; i < 2000; ++i) {
        const auto j = s.job(id);
        REQUIRE(j.has_value());
        const std::string state = (*j)["state"];
        if (state == "done" || state == "failed" || state == "cancelled") {
            return *j;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    FAIL("the job didn't finish");
    return {};
}

}  // namespace

TEST_CASE("installing Garnet brings the .NET 10 runtime and phpredis for each PHP series; a later PHP its own") {
    GarnetWorld w;
    w.add("garnet", "2.2.4", "", {{"GarnetServer.dll", "dll"}});
    w.add("dotnet", "10.0.3", "", {{"dotnet.exe", "MZ"}});
    w.add("dotnet", "11.0.0", "", {{"dotnet.exe", "MZ"}});
    w.add("php-redis", "6.3.0", "php8-4", {{"php_redis.dll", "8.4"}});
    w.add("php-redis", "6.3.0", "php8-5", {{"php_redis.dll", "8.5"}});
    w.add("php", "8.5.1", "nts", {{"php-cgi.exe", "MZ"}});
    w.publish();
    fake_install(w.tmp.path(), "php", "8.4.27", "nts", {"php-cgi.exe"});
    fake_install(w.tmp.path(), "php", "8.3.20", "nts", {"php-cgi.exe"});
    RuntimeService s(w.config(), w.transport);
    ServiceRefusal refusal;

    // phpredis alone, by its variant; no variant means none of them.
    const auto bad = s.install("php-redis", "", refusal);
    REQUIRE(bad);
    CHECK(finished(s, *bad)["state"] == "failed");
    CHECK_FALSE(s.install("php-redis", "", refusal, "php 8.4"));
    CHECK(refusal.code == "bad_params");

    const auto id = s.install("garnet", "", refusal);
    REQUIRE(id);
    const auto job = finished(s, *id);
    INFO(job.dump(2));
    CHECK(job["state"] == "done");
    CHECK(job["version"] == "2.2.4");
    const std::string message = job["message"];
    CHECK(message.find("no phpredis for PHP 8.3") != std::string::npos);  // that series' projects use predis
    const auto root = runtimes_dir(w.tmp.path());
    CHECK(fs::is_regular_file(root / "dotnet" / "10.0.3" / "dotnet.exe"));
    CHECK_FALSE(fs::exists(root / "dotnet" / "11.0.0"));
    CHECK(fs::is_regular_file(root / "garnet" / "2.2.4" / "GarnetServer.dll"));
    CHECK(read_file(root / "php-redis" / "6.3.0-php8-4" / "php_redis.dll") == "8.4");
    CHECK_FALSE(fs::exists(root / "php-redis" / "6.3.0-php8-5"));  // no PHP 8.5 yet
    CHECK(job["total"] == job["received"]);

    // A PHP installed while Garnet is brings its phpredis.
    const auto php = s.install("php", "8.5", refusal);
    REQUIRE(php);
    CHECK(finished(s, *php)["state"] == "done");
    CHECK(read_file(root / "php-redis" / "6.3.0-php8-5" / "php_redis.dll") == "8.5");

    // Installing Garnet again installs only what's missing.
    const auto again = s.install("garnet", "", refusal);
    REQUIRE(again);
    const auto job2 = finished(s, *again);
    CHECK(job2["state"] == "done");
    CHECK(job2["total"] == 0);
}

TEST_CASE("Garnet without a .NET runtime in the list isn't installed") {
    GarnetWorld w;
    w.add("garnet", "2.2.4", "", {{"GarnetServer.dll", "dll"}});
    w.add("dotnet", "9.0.10", "", {{"dotnet.exe", "MZ"}});
    w.publish();
    RuntimeService s(w.config(), w.transport);
    ServiceRefusal refusal;
    const auto id = s.install("garnet", "", refusal);
    REQUIRE(id);
    const auto job = finished(s, *id);
    CHECK(job["state"] == "failed");
    CHECK(job["message"].get<std::string>().find(".NET 10") != std::string::npos);
    CHECK_FALSE(fs::exists(runtimes_dir(w.tmp.path()) / "garnet"));
}

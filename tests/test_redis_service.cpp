// Garnet run by the service (D30): woken by a connection, a password always
// asked for (from its config file, never an argument), a web page's request
// closed before anything reaches Garnet, the data saved before it sleeps and
// back after, Flush all, Keep running, a new memory limit.
//
// With fake_garnet (tests/fake_garnet.cpp) always; with the real Garnet
// when FLOSERVER_TEST_GARNET names a Garnet install folder (one with
// GarnetServer.dll) and FLOSERVER_TEST_DOTNET a .NET 10 runtime folder (one
// with the dotnet program). Skipped without them.

#ifndef _WIN32

#include "floserver/core/garnet.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/redis_service.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <thread>

using namespace floserver::core;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

void marker(const fs::path& dir, const std::string& name, const std::string& version) {
    fs::create_directories(dir);
    test_util::write_file(dir / ".floserver-runtime.json",
                          nlohmann::json{{"schema", 1}, {"name", name}, {"version", version},
                                         {"platform", std::string(current_platform())}, {"variant", ""},
                                         {"license", "MIT"}, {"sha256", std::string(64, 'a')}}
                              .dump());
}

void copy_tree(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

// The installs the service looks for: the fake (as dotnet, running a
// GarnetServer.dll that isn't one), or the real ones copied in.
bool install_garnet(const fs::path& data, bool real) {
    const fs::path garnet = runtimes_dir(data) / "garnet" / "2.2.4";
    const fs::path dotnet = runtimes_dir(data) / "dotnet" / "10.0.3";
    if (real) {
        const auto g = get_env("FLOSERVER_TEST_GARNET");
        const auto d = get_env("FLOSERVER_TEST_DOTNET");
        if (!g || !d) {
            return false;
        }
        copy_tree(*g, garnet);
        copy_tree(*d, dotnet);
    } else {
        const fs::path fake = fs::path(FLOSERVER_TEST_FAKE_GARNET);
        fs::create_directories(dotnet);
        fs::create_directories(garnet);
        fs::copy_file(fake, dotnet / "dotnet", fs::copy_options::overwrite_existing);
        test_util::write_file(garnet / "GarnetServer.dll", "not really");
    }
    marker(garnet, "garnet", "2.2.4");
    marker(dotnet, "dotnet", "10.0.3");
    return true;
}

template <class F>
bool eventually(F&& f, std::chrono::milliseconds limit = 20s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (f()) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return f();
}

// Sends `bytes` to the public port and reads what comes back until it's
// closed or 30 s pass.
std::string talk(Port port, const std::string& bytes, std::chrono::milliseconds limit = 30s) {
    const auto c = connect_loopback(port);
    if (!c) {
        return "(no connection)";
    }
    std::string got;
    if (send_all(*c, bytes)) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left <= 0ms) {
                break;
            }
            const auto more = receive_some(*c, left);
            if (!more || more->empty()) {
                break;
            }
            got += *more;
            // Enough when every reply asked for is in.
            std::string copy = got;
            std::size_t replies = 0;
            while (take_resp_reply(copy)) {
                ++replies;
            }
            std::size_t asked = 0;
            for (std::size_t p = 0; (p = bytes.find('*', p)) != std::string::npos; ++p) {
                if (p == 0 || bytes[p - 1] == '\n') {
                    ++asked;
                }
            }
            if (asked > 0 && replies >= asked) {
                break;
            }
        }
    }
    close_socket(*c);
    return got;
}

std::vector<std::string> replies(std::string text) {
    std::vector<std::string> out;
    while (const auto r = take_resp_reply(text)) {
        out.push_back(std::string(1, r->type) + (r->null ? "(nil)" : r->text));
    }
    return out;
}

std::string garnet_cmdlines(const fs::path& data) {
    std::string out;
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        std::string cmd = test_util::read_file(it->path() / "cmdline");
        std::replace(cmd.begin(), cmd.end(), '\0', ' ');
        if (cmd.find(data.string()) != std::string::npos && cmd.find("GarnetServer") != std::string::npos) {
            out += cmd + "\n";
        }
    }
    return out;
}

void run_service_test(bool real) {
    const test_util::TempDir tmp;
    const fs::path& data = tmp.path();
    if (!install_garnet(data, real)) {
        MESSAGE("no Garnet here (FLOSERVER_TEST_GARNET, FLOSERVER_TEST_DOTNET); skipped");
        return;
    }
    // A port from 26379 up, so a Redis on this machine isn't asked.
    AppConfig app;
    RedisSettings rs;
    rs.port = 26379;
    write_redis_settings(app, rs);
    std::string error;
    REQUIRE(save_app_config(app_config_file(data), app, error));

    RedisServiceConfig config;
    config.data_dir = data;
    config.idle_after = 2s;
    config.reload_every = 1s;
    config.first_bytes_limit = 2000ms;
    RedisService service(config, SecretStore(data));
    service.start();
    INFO(service.status().dump(2));
    REQUIRE(service.status()["state"] == "asleep");
    const auto port = service.port();
    REQUIRE(port);
    std::string why;
    const auto password = service.password(why);
    REQUIRE(password);
    CHECK(password->size() == 64);
    CHECK(SecretStore(data).read(std::string(kRedisSecretName)) == *password);
    // Owner-only folder and config file, with the password in it.
    const GarnetPaths paths = garnet_paths(data);
    CHECK((fs::status(paths.dir).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    CHECK((fs::status(paths.config).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    CHECK(test_util::read_file(paths.config).find(*password) != std::string::npos);

    // A web page's request: closed, Garnet not even started.
    const std::string page = "POST / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(*port) +
                             "\r\nContent-Type: text/plain\r\n\r\nFLUSHALL\r\nSET x y\r\n";
    CHECK(talk(*port, page, 5s).empty());
    CHECK(talk(*port, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", 5s).empty());
    CHECK(service.status()["state"] == "asleep");
    CHECK(service.status()["refused"] == 2);

    // A client without the password: woken, but refused.
    auto r = replies(talk(*port, resp_command({"SET", "a", "1"})));
    REQUIRE(r.size() == 1);
    CHECK(r[0].starts_with("-NOAUTH"));
    CHECK(service.status()["state"] == "running");
    // With it: works.
    r = replies(talk(*port, resp_command({"AUTH", *password}) + resp_command({"SET", "shop_k", "v1"}) +
                                    resp_command({"GET", "shop_k"})));
    REQUIRE(r.size() == 3);
    CHECK(r[0] == "+OK");
    CHECK(r[1] == "+OK");
    CHECK(r[2] == "$v1");
    // The same page again while Garnet runs: still never reaches it.
    CHECK(talk(*port, page, 5s).empty());
    if (!real) {
        const std::string received = test_util::read_file(paths.dir / "fake-received.log");
        CHECK(received.find("HTTP/1.1") == std::string::npos);
        CHECK(received.find("FLUSHALL") == std::string::npos);
        CHECK(received.find("shop_k") != std::string::npos);
    }
    // The password is never in Garnet's arguments, its log or the status.
    const std::string cmdline = garnet_cmdlines(data);
    CHECK(cmdline.find("--unixsocket") != std::string::npos);
    CHECK(cmdline.find(*password) == std::string::npos);
    CHECK(test_util::read_file(paths.log).find(*password) == std::string::npos);
    CHECK(service.status().dump().find(*password) == std::string::npos);
    CHECK(eventually([&] { return service.status()["keys"] == 1; }));

    // Asleep after the idle time, saved first; the key is back after waking.
    CHECK(eventually([&] { return service.status()["state"] == "asleep"; }));
    CHECK(garnet_cmdlines(data).empty());
    r = replies(talk(*port, resp_command({"AUTH", *password}) + resp_command({"GET", "shop_k"})));
    REQUIRE(r.size() == 2);
    CHECK(r[1] == "$v1");

    // Keep running: up past the idle time.
    ServiceRefusal refusal;
    CHECK(service.set({{"keep_running", true}}, refusal));
    std::this_thread::sleep_for(3500ms);
    CHECK(service.status()["state"] == "running");
    CHECK_FALSE(service.set({{"memory_mb", 4}}, refusal));
    CHECK(refusal.code == "bad_params");
    CHECK_FALSE(service.set({{"color", "red"}}, refusal));
    // A new memory limit restarts it (saved first: the key stays).
    CHECK(service.set({{"memory_mb", 64}}, refusal));
    CHECK(service.status()["settings"]["memory_mb"] == 64);
    CHECK(eventually([&] { return garnet_cmdlines(data).find("--memory 64m") != std::string::npos; }));
    r = replies(talk(*port, resp_command({"AUTH", *password}) + resp_command({"GET", "shop_k"})));
    REQUIRE(r.size() == 2);
    CHECK(r[1] == "$v1");

    // Flush all: gone, and still gone after a sleep.
    CHECK(service.flush(refusal));
    CHECK(service.status()["keys"] == 0);
    CHECK(service.set({{"keep_running", false}}, refusal));
    CHECK(eventually([&] { return service.status()["state"] == "asleep"; }));
    r = replies(talk(*port, resp_command({"AUTH", *password}) + resp_command({"GET", "shop_k"})));
    REQUIRE(r.size() == 2);
    CHECK(r[1] == "$(nil)");

    // A failed SAVE is kept in the status until one works; a slow one shows "saving".
    if (!real) {
        test_util::write_file(paths.checkpoints / "fail-save", "");
        r = replies(talk(*port, resp_command({"AUTH", *password}) + resp_command({"SET", "x", "1"})));
        REQUIRE(r.size() == 2);
        CHECK(eventually([&] { return service.status()["state"] == "asleep"; }));
        CHECK(service.status().value("save_error", std::string()).find("checkpoint failed") != std::string::npos);
        // Kept in status.json, so a restarted FloServer still says so.
        CHECK(test_util::read_file(paths.status).find("checkpoint failed") != std::string::npos);
        fs::remove(paths.checkpoints / "fail-save");
        test_util::write_file(paths.checkpoints / "slow-save", "");
        r = replies(talk(*port, resp_command({"AUTH", *password}) + resp_command({"SET", "x", "2"})));
        REQUIRE(r.size() == 2);
        CHECK(service.status().contains("save_error"));  // still: no SAVE has worked since
        CHECK(eventually([&] { return service.status()["state"] == "saving"; }));
        CHECK_FALSE(garnet_cmdlines(data).empty());  // saving: still there
        CHECK(eventually([&] { return service.status()["state"] == "asleep"; }));
        CHECK(garnet_cmdlines(data).empty());
        CHECK_FALSE(service.status().contains("save_error"));
        CHECK(test_util::read_file(paths.status).find("save_error") == std::string::npos);
        fs::remove(paths.checkpoints / "slow-save");
        // Garnet's own checkpoint running: SAVE is tried again, before a sleep and after Flush all.
        r = replies(talk(*port, resp_command({"AUTH", *password}) + resp_command({"SET", "y", "1"})));
        REQUIRE(r.size() == 2);
        test_util::write_file(paths.checkpoints / "busy-saves", "3");
        CHECK(eventually([&] { return service.status()["state"] == "asleep"; }));
        CHECK_FALSE(service.status().contains("save_error"));
        CHECK(test_util::read_file(paths.checkpoints / "busy-saves") == "0");
        test_util::write_file(paths.checkpoints / "busy-saves", "2");
        ServiceRefusal busy_refusal;
        CHECK(service.flush(busy_refusal));
        CHECK_FALSE(service.status().contains("save_error"));
        CHECK(test_util::read_file(paths.checkpoints / "busy-saves") == "0");
        CHECK(eventually([&] { return service.status()["state"] == "asleep"; }));
        r = replies(talk(*port, resp_command({"AUTH", *password}) + resp_command({"GET", "y"})));
        REQUIRE(r.size() == 2);
        CHECK(r[1] == "$(nil)");  // flushed, and still flushed after the sleep
        fs::remove(paths.checkpoints / "busy-saves");
    }
    // A Lua script can't hold Garnet: stopped after 5 s.
    {
        const auto started = std::chrono::steady_clock::now();
        r = replies(talk(*port, resp_command({"AUTH", *password}) + resp_command({"EVAL", "while true do end", "0"}),
                         40s));
        REQUIRE(r.size() == 2);
        CHECK(r[1].starts_with("-"));
        CHECK(std::chrono::steady_clock::now() - started < 30s);
    }

    // Held for a removal: stopped, and connections are told it isn't available.
    {
        const RedisService::RemovalHold hold = service.hold_for_removal();
        CHECK(garnet_cmdlines(data).empty());
        r = replies(talk(*port, resp_command({"PING"})));
        REQUIRE(r.size() == 1);
        CHECK(r[0].starts_with("-ERR Redis (Garnet) isn't available"));
    }
    service.stop();
    CHECK(garnet_cmdlines(data).empty());
}

}  // namespace

TEST_CASE("Garnet (a stand-in) behind the service: password, HTTP closed, saved before sleep, flush, keep running") {
    run_service_test(false);
}

TEST_CASE("the real Garnet behind the service, when it's on this machine") {
    if (!get_env("FLOSERVER_TEST_GARNET") || !get_env("FLOSERVER_TEST_DOTNET")) {
        MESSAGE("FLOSERVER_TEST_GARNET / FLOSERVER_TEST_DOTNET aren't set; skipped");
        return;
    }
    run_service_test(true);
}

TEST_CASE("without .NET, or with the port taken, the status says why") {
    const test_util::TempDir tmp;
    const fs::path& data = tmp.path();
    marker(runtimes_dir(data) / "garnet" / "2.2.4", "garnet", "2.2.4");
    test_util::write_file(runtimes_dir(data) / "garnet" / "2.2.4" / "GarnetServer.dll", "x");
    AppConfig app;
    RedisSettings rs;
    rs.port = 26390;
    write_redis_settings(app, rs);
    std::string error;
    REQUIRE(save_app_config(app_config_file(data), app, error));
    RedisServiceConfig config;
    config.data_dir = data;
    RedisService service(config, SecretStore(data));
    service.reload();
    const auto s = service.status();
    INFO(s.dump(2));
    CHECK(s["installed"] == true);
    CHECK(s["state"] == "failed");
    CHECK(s["error"].get<std::string>().find(".NET 10") != std::string::npos);
    ServiceRefusal refusal;
    CHECK_FALSE(service.flush(refusal));
    CHECK(refusal.code == "redis_failed");

    // Not installed: no port held.
    const test_util::TempDir empty;
    RedisServiceConfig none;
    none.data_dir = empty.path();
    RedisService nothing(none, SecretStore(empty.path()));
    nothing.reload();
    CHECK(nothing.status()["state"] == "not_installed");
    CHECK_FALSE(nothing.port());
    CHECK_FALSE(nothing.flush(refusal));
    CHECK(refusal.code == "not_installed");
}

#endif

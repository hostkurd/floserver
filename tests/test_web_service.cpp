// The web service with the real nginx and php-cgi (D6, D11, Q1): serving,
// sleeping when idle, waking under a burst of requests, Keep running, a bad
// project on its own, and nothing left running after stop().
//
// Needs nginx and php-cgi on this machine (Linux CI installs them; Windows
// runs the same flow against the CDN builds in CI's live step). Set
// FLOSERVER_TEST_NGINX / FLOSERVER_TEST_PHP_CGI to use other copies.

#ifndef _WIN32

#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/web_service.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <thread>

using namespace floserver::core;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::optional<fs::path> tool(const char* env, const char* fallback) {
    const auto set = get_env(env);
    const fs::path p = set ? fs::path(*set) : fs::path(fallback);
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? std::optional<fs::path>(p) : std::nullopt;
}

void fake_runtime(const fs::path& data, const std::string& name, const std::string& version, const std::string& variant) {
    const fs::path dir = runtimes_dir(data) / name / version;
    fs::create_directories(dir);
    test_util::write_file(dir / ".floserver-runtime.json",
                          nlohmann::json{{"schema", 1}, {"name", name}, {"version", version},
                                         {"platform", std::string(current_platform())}, {"variant", variant},
                                         {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
}

// GET over plain http; "<status> <body>", or "" when it couldn't connect.
std::string get(Port port, const std::string& host, const std::string& path = "/") {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeval tv{20, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        close(s);
        return "";
    }
    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";  // no chunked replies
    (void)send(s, req.data(), req.size(), 0);
    std::string resp;
    char buf[4096];  // NOLINT(modernize-avoid-c-arrays)
    for (ssize_t n = 0; (n = recv(s, buf, sizeof buf, 0)) > 0;) {
        resp.append(buf, static_cast<std::size_t>(n));
    }
    close(s);
    const auto sp = resp.find(' ');
    const auto body = resp.find("\r\n\r\n");
    if (sp == std::string::npos || body == std::string::npos) {
        return "";
    }
    return resp.substr(sp + 1, 3) + " " + resp.substr(body + 4);
}

bool alive(std::uint32_t pid) { return ::kill(static_cast<pid_t>(pid), 0) == 0; }

template <class F>
bool eventually(F&& f, std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (f()) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return f();
}

}  // namespace

TEST_CASE("projects run with real nginx and php-cgi, sleep when idle and wake on a request") {
    const auto nginx = tool("FLOSERVER_TEST_NGINX", "/usr/sbin/nginx");
    const auto php = tool("FLOSERVER_TEST_PHP_CGI", "/usr/bin/php-cgi");
    if (!nginx || !php) {
        MESSAGE("nginx or php-cgi isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_runtime(data, "nginx", "1.24.0", "");
    fake_runtime(data, "php", "8.3.6", "nts");
    const fs::path nginx_dir = runtimes_dir(data) / "nginx" / "1.24.0";
    fs::create_directories(nginx_dir / "conf");
    fs::create_symlink(*nginx, nginx_dir / "nginx");
    for (const char* f : {"mime.types", "fastcgi_params"}) {
        std::error_code ec;
        fs::copy_file(fs::path("/etc/nginx") / f, nginx_dir / "conf" / f, ec);
        if (ec) {
            test_util::write_file(nginx_dir / "conf" / f, std::string(f) == "mime.types" ? "types { text/html html; }\n"
                                  : "fastcgi_param QUERY_STRING $query_string;\nfastcgi_param REQUEST_METHOD $request_method;\n"
                                    "fastcgi_param CONTENT_TYPE $content_type;\nfastcgi_param CONTENT_LENGTH $content_length;\n"
                                    "fastcgi_param SCRIPT_NAME $fastcgi_script_name;\nfastcgi_param REQUEST_URI $request_uri;\n"
                                    "fastcgi_param SERVER_PROTOCOL $server_protocol;\nfastcgi_param REDIRECT_STATUS 200;\n");
        }
    }
    fs::create_symlink(*php, runtimes_dir(data) / "php" / "8.3.6" / "php-cgi");

    const fs::path shop = tmp.path() / "shop";
    test_util::write_file(shop / "public" / "index.php", "<?php echo 'shop ', $_SERVER['REQUEST_URI'];");
    test_util::write_file(shop / "floserver.json", R"({"php": "8.3", "root": "public"})");
    const fs::path api = tmp.path() / "api";
    test_util::write_file(api / "index.php", "<?php echo 'api';");
    const fs::path broken = tmp.path() / "broken";
    test_util::write_file(broken / "floserver.json", R"({"php": 8})");

    AppConfig app;
    std::string error;
    REQUIRE(add_project(app, shop, "", error));
    REQUIRE(add_project(app, api, "", error));
    REQUIRE(add_project(app, broken, "", error));
    for (Project& p : app.projects) {
        p.on = true;
    }
    REQUIRE(save_app_config(app_config_file(data), app, error));

    // Ports nobody else here uses.
    const auto ports = pick_web_ports([](Port p) { return p > 1024 && can_listen_on_loopback(p); });
    REQUIRE(ports);
    CertStore certs(data);
    WebServiceConfig config;
    config.data_dir = data;
    config.fixed_ports = WebPorts{static_cast<Port>(ports->http + 20000), static_cast<Port>(ports->https + 20000), true};
    config.idle_after = 2s;
    config.reload_every = 1s;
    WebService web(config, certs);
    web.start();
    const Port http = config.fixed_ports->http;

    auto st = web.status();
    INFO(st.dump(2));
    REQUIRE(st["nginx"]["running"] == true);
    const auto nginx_pid = st["nginx"]["pid"].get<std::uint32_t>();
    CHECK(st["sites"]["shop"]["address"] == "http://shop.localhost:" + std::to_string(http));
    CHECK(st["sites"]["shop"]["run"] == "sleeping");
    CHECK_FALSE(st["sites"].contains("broken"));  // a bad file only stops its own project
    CHECK(st["pools"].size() == 1);               // shop and api share PHP 8.3 with no extensions

    SUBCASE("serve, sleep, wake under a burst") {
        CHECK(get(http, "shop.localhost", "/cart?x=1") == "200 shop /cart?x=1");
        CHECK(get(http, "api.localhost") == "200 api");
        CHECK(get(http, "other.localhost").starts_with("404 "));
        REQUIRE(eventually([&] { return web.status()["pools"][0]["state"] == "running"; }));
        const auto php_pid = web.status()["pools"][0]["pid"].get<std::uint32_t>();

        // No PHP request for the idle time: php-cgi is stopped, the port stays.
        REQUIRE(eventually([&] { return web.status()["pools"][0]["state"] == "sleeping"; }, 8s));
        CHECK(eventually([&] { return !alive(php_pid); }, 3s));

        // 30 requests at once while it wakes: every one is served.
        std::atomic<int> ok{0};
        std::vector<std::thread> threads;
        threads.reserve(30);
        for (int i = 0; i < 30; ++i) {
            threads.emplace_back([&] {
                if (get(http, "shop.localhost") == "200 shop /") {
                    ++ok;
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
        CHECK(ok == 30);
    }

    SUBCASE("keep running holds php-cgi awake") {
        AppConfig changed = *load_app_config(app_config_file(data), error);
        find_project(changed, "api")->keep_running = true;
        REQUIRE(save_app_config(app_config_file(data), changed, error));
        st = web.reload();
        CHECK(st["pools"][0]["keep_running"] == true);
        REQUIRE(eventually([&] { return web.status()["pools"][0]["state"] == "running"; }, 3s));
        std::this_thread::sleep_for(3500ms);  // longer than the idle time
        CHECK(web.status()["pools"][0]["state"] == "running");
    }

    SUBCASE("a project turned off is no longer served; none on stops nginx") {
        AppConfig changed = *load_app_config(app_config_file(data), error);
        find_project(changed, "shop")->on = false;
        REQUIRE(save_app_config(app_config_file(data), changed, error));
        (void)web.reload();
        // nginx -s reload is asynchronous: the old worker may answer a moment longer.
        CHECK(eventually([&] { return get(http, "shop.localhost").starts_with("404 "); }, 5s));
        CHECK(get(http, "api.localhost") == "200 api");
        for (Project& p : changed.projects) {
            p.on = false;
        }
        REQUIRE(save_app_config(app_config_file(data), changed, error));
        st = web.reload();
        CHECK(st["nginx"]["running"] == false);
        CHECK(eventually([&] { return !alive(nginx_pid); }, 3s));
        CHECK(get(http, "api.localhost").empty());
    }

    SUBCASE("a config nginx refuses is never loaded, and the pools it still uses stay") {
        CHECK(get(http, "shop.localhost") == "200 shop /");
        // A new pool for shop (another extension list) and a config nginx
        // refuses: nginx keeps the old config, which still sends shop to the
        // old pool, so that pool must stay open.
        test_util::write_file(shop / "floserver.json", R"({"php": "8.3", "root": "public", "extensions": ["intl"]})");
        test_util::write_file(nginx_dir / "conf" / "mime.types", "types { broken");
        st = web.reload();
        CHECK(st["error"].get<std::string>().find("nginx refused") != std::string::npos);
        CHECK(st["nginx"]["running"] == true);
        CHECK(get(http, "shop.localhost") == "200 shop /");
        std::this_thread::sleep_for(1500ms);  // the supervisor's reloads don't ask nginx again or drop it
        CHECK(get(http, "shop.localhost") == "200 shop /");
        CHECK(web.status()["error"].get<std::string>().find("nginx refused") != std::string::npos);
    }

    std::vector<std::uint32_t> pids = {nginx_pid};
    const auto final_status = web.status();
    for (const auto& p : final_status["pools"]) {
        if (p.contains("pid")) {
            pids.push_back(p["pid"].get<std::uint32_t>());
        }
    }
    web.stop();
    for (const auto pid : pids) {
        CHECK_FALSE(alive(pid));
    }
    CHECK(get(http, "api.localhost").empty());
    // A reload arriving after stop (a client mid-request while quitting)
    // starts nothing again.
    st = web.reload();
    CHECK(st["nginx"]["running"] == false);
    CHECK(st["pools"].empty());
    CHECK(get(http, "api.localhost").empty());
}

#endif

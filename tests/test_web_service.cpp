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

#include <algorithm>
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

// This machine's nginx as an installed runtime.
void install_test_nginx(const fs::path& data, const fs::path& nginx) {
    fake_runtime(data, "nginx", "1.24.0", "");
    const fs::path nginx_dir = runtimes_dir(data) / "nginx" / "1.24.0";
    fs::create_directories(nginx_dir / "conf");
    fs::create_symlink(nginx, nginx_dir / "nginx");
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

TEST_CASE("open connections to a port are seen") {
    std::string error;
    auto listener = LoopbackListener::open(0, error);  // any free port
    REQUIRE(listener);
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    REQUIRE(getsockname(static_cast<int>(listener->native()), reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const Port port = ntohs(addr.sin_port);
    CHECK_FALSE(has_open_connections(port));  // listening only
    const int c = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(connect(c, reinterpret_cast<const sockaddr*>(&addr), len) == 0);
    CHECK(has_open_connections(port));  // waiting to be accepted counts
    close(c);
}

TEST_CASE("projects run with real nginx and php-cgi, sleep when idle and wake on a request") {
    const auto nginx = tool("FLOSERVER_TEST_NGINX", "/usr/sbin/nginx");
    const auto php = tool("FLOSERVER_TEST_PHP_CGI", "/usr/bin/php-cgi");
    if (!nginx || !php) {
        MESSAGE("nginx or php-cgi isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    install_test_nginx(data, *nginx);
    const fs::path nginx_dir = runtimes_dir(data) / "nginx" / "1.24.0";
    fake_runtime(data, "php", "8.3.6", "nts");
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

        // Every request to a running pool is answered, whichever worker takes
        // it (on Windows, php-cgi's own workers left 3 in 4 unanswered).
        CHECK(web.status()["pools"][0]["pids"].size() == 4);
        CHECK(web.status()["pools"][0]["ports"].size() == 4);
        int answered = 0;
        for (int i = 0; i < 20; ++i) {
            answered += get(http, "shop.localhost", "/n" + std::to_string(i)) == "200 shop /n" + std::to_string(i) ? 1 : 0;
        }
        CHECK(answered == 20);

        // A worker that ends is replaced; the others keep serving.
        ::kill(static_cast<pid_t>(php_pid), SIGKILL);
        CHECK(eventually([&] {
            const auto pids = web.status()["pools"][0]["pids"];
            return pids.size() == 4 && std::find(pids.begin(), pids.end(), php_pid) == pids.end();
        }, 5s));
        CHECK(get(http, "shop.localhost", "/after") == "200 shop /after");

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
        // reload() returns once nginx answers with the new config, but
        // nginx's old worker may still take a connection for a moment (seen
        // under ASan in CI): harmless for users, so the test waits briefly.
        CHECK(eventually([&] { return get(http, "shop.localhost").starts_with("404 "); }, 3s));
        CHECK(get(http, "api.localhost") == "200 api");
        // Turned on again: served again.
        find_project(changed, "shop")->on = true;
        REQUIRE(save_app_config(app_config_file(data), changed, error));
        (void)web.reload();
        CHECK(eventually([&] { return get(http, "shop.localhost") == "200 shop /"; }, 3s));
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
        if (p.contains("pids")) {
            for (const auto& pid : p["pids"]) {
                pids.push_back(pid.get<std::uint32_t>());
            }
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

TEST_CASE("a Node project is forwarded to its own server, WebSockets included, and sleeps when idle") {
    const auto nginx = tool("FLOSERVER_TEST_NGINX", "/usr/sbin/nginx");
    std::optional<fs::path> node;
    for (const char* dir : {"/usr/local/bin", "/usr/bin", "/opt/node22/bin"}) {
        if (!node) {
            node = tool("FLOSERVER_TEST_NODE", (std::string(dir) + "/node").c_str());
        }
    }
    if (!nginx || !node) {
        MESSAGE("nginx or node isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    install_test_nginx(data, *nginx);
    fake_runtime(data, "node", "22.0.0", "");
    fs::create_directories(runtimes_dir(data) / "node" / "22.0.0" / "bin");
    fs::create_symlink(fs::canonical(*node), runtimes_dir(data) / "node" / "22.0.0" / "bin" / "node");

    // A server that answers with the URL and Host, and echoes on a WebSocket-style upgrade.
    const fs::path web = tmp.path() / "web";
    test_util::write_file(web / "floserver.json", R"({"node": "22", "command": "exec node server.js"})");
    test_util::write_file(web / "server.js", R"(
        const server = require("http").createServer((req, res) => {
            res.end("node " + req.url + " " + req.headers.host + " " + req.headers["x-forwarded-proto"]);
        });
        server.on("upgrade", (req, socket) => {
            socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n");
            socket.on("data", (d) => socket.write("echo:" + d));
        });
        server.listen(Number(process.env.PORT), process.env.HOST);
    )");
    AppConfig app;
    std::string error;
    REQUIRE(add_project(app, web, "", error));
    app.projects[0].on = true;
    REQUIRE(save_app_config(app_config_file(data), app, error));

    const auto ports = pick_web_ports([](Port p) { return p > 1024 && can_listen_on_loopback(p); });
    REQUIRE(ports);
    CertStore certs(data);
    WebServiceConfig config;
    config.data_dir = data;
    config.fixed_ports = WebPorts{static_cast<Port>(ports->http + 21000), static_cast<Port>(ports->https + 21000), true};
    config.idle_after = 2s;
    config.reload_every = 1s;
    WebService service(config, certs);
    service.start();
    const Port http = config.fixed_ports->http;

    auto st = service.status();
    INFO(st.dump(2));
    REQUIRE(st["nginx"]["running"] == true);
    CHECK(st["sites"]["web"]["app"] == "node");
    CHECK(st["sites"]["web"]["run"] == "sleeping");
    CHECK(st["pools"].empty());

    CHECK(get(http, "web.localhost", "/cart?x=1") == "200 node /cart?x=1 web.localhost http");
    CHECK(service.status()["sites"]["web"]["run"] == "running");
    const auto pid = service.status()["apps"]["web"]["pid"].get<std::uint32_t>();

    // An upgraded connection passes both ways through nginx and the relay.
    {
        const int s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(http);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        timeval tv{10, 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        REQUIRE(connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);
        const std::string req = "GET /hmr HTTP/1.1\r\nHost: web.localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
        (void)send(s, req.data(), req.size(), 0);
        std::string got;
        char buf[1024];  // NOLINT(modernize-avoid-c-arrays)
        while (got.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = recv(s, buf, sizeof buf, 0);
            if (n <= 0) {
                break;
            }
            got.append(buf, static_cast<std::size_t>(n));
        }
        CHECK(got.starts_with("HTTP/1.1 101"));
        (void)send(s, "ping", 4, 0);
        const ssize_t n = recv(s, buf, sizeof buf, 0);
        CHECK(std::string(buf, static_cast<std::size_t>(std::max<ssize_t>(n, 0))) == "echo:ping");
        // While it's open the app doesn't sleep.
        std::this_thread::sleep_for(3s);
        CHECK(service.status()["sites"]["web"]["run"] == "running");
        close(s);
    }

    // Idle: nginx lets its kept-alive connections go after 10 s, then the
    // idle time (2 s) runs out.
    REQUIRE(eventually([&] { return service.status()["sites"]["web"]["run"] == "sleeping"; }, 25s));
    CHECK(eventually([&] { return !alive(pid); }, 3s));
    CHECK(get(http, "web.localhost", "/again") == "200 node /again web.localhost http");

    // Turned off: the server stops, the address is gone.
    const auto pid2 = service.status()["apps"]["web"]["pid"].get<std::uint32_t>();
    app.projects[0].on = false;
    REQUIRE(save_app_config(app_config_file(data), app, error));
    st = service.reload();
    CHECK_FALSE(st["apps"].contains("web"));
    CHECK(eventually([&] { return !alive(pid2); }, 3s));
    service.stop();
}

TEST_CASE("an Apache project is served by the shared Apache behind nginx, .htaccess and all (D6)") {
    const auto nginx = tool("FLOSERVER_TEST_NGINX", "/usr/sbin/nginx");
    const auto php = tool("FLOSERVER_TEST_PHP_CGI", "/usr/bin/php-cgi");
    const auto apache = tool("FLOSERVER_TEST_APACHE", "/usr/sbin/apache2");
    std::error_code ec;
    const fs::path modules = get_env("FLOSERVER_TEST_APACHE_MODULES").value_or("/usr/lib/apache2/modules");
    if (!nginx || !php || !apache || !fs::exists(modules / "mod_proxy_fcgi.so", ec)) {
        MESSAGE("nginx, php-cgi or Apache with mod_proxy_fcgi isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    install_test_nginx(data, *nginx);
    fake_runtime(data, "php", "8.3.6", "nts");
    fs::create_symlink(*php, runtimes_dir(data) / "php" / "8.3.6" / "php-cgi");
    // This machine's Apache as an installed runtime.
    fake_runtime(data, "apache", "2.4.58", "");
    const fs::path apache_dir = runtimes_dir(data) / "apache" / "2.4.58";
    fs::create_directories(apache_dir / "bin");
    fs::create_directories(apache_dir / "conf");
    fs::create_symlink(*apache, apache_dir / "bin" / "apache2");
    fs::create_directory_symlink(modules, apache_dir / "modules");
    fs::copy_file("/etc/mime.types", apache_dir / "conf" / "mime.types", ec);
    if (ec) {
        test_util::write_file(apache_dir / "conf" / "mime.types", "text/plain txt\n");
    }

    // WordPress's own .htaccess: what isn't a file goes to index.php.
    const fs::path wp = tmp.path() / "wp";
    test_util::write_file(wp / "floserver.json", R"({"web": "apache", "php": "8.3"})");
    test_util::write_file(wp / ".htaccess", "RewriteEngine On\nRewriteBase /\nRewriteRule ^index\\.php$ - [L]\n"
                                            "RewriteCond %{REQUEST_FILENAME} !-f\nRewriteCond %{REQUEST_FILENAME} !-d\n"
                                            "RewriteRule . /index.php [L]\n");
    test_util::write_file(wp / "index.php", "<?php echo 'wp ', $_SERVER['REQUEST_URI'], ' ', $_SERVER['SCRIPT_NAME'], ' ',"
                                            " $_SERVER['PATH_INFO'] ?? '-', ' ', $_SERVER['HTTPS'] ?? 'off';");
    test_util::write_file(wp / "readme.txt", "plain file");
    // A second Apache project: the same Apache, its own host.
    const fs::path blog = tmp.path() / "blog";
    test_util::write_file(blog / "floserver.json", R"({"web": "apache", "php": "8.3"})");
    test_util::write_file(blog / "index.php", "<?php echo 'blog';");
    test_util::write_file(wp / ".env", "SECRET=1");
    // Apache's workers may run as nobody here (root in a container).
    for (const auto& e : fs::recursive_directory_iterator(tmp.path())) {
        fs::permissions(e.path(), fs::perms::others_read | fs::perms::others_exec, fs::perm_options::add, ec);
    }
    fs::permissions(tmp.path(), fs::perms::others_read | fs::perms::others_exec, fs::perm_options::add, ec);

    AppConfig app;
    std::string error;
    REQUIRE(add_project(app, wp, "", error));
    REQUIRE(add_project(app, blog, "", error));
    for (Project& p : app.projects) {
        p.on = true;
    }
    REQUIRE(save_app_config(app_config_file(data), app, error));

    const auto ports = pick_web_ports([](Port p) { return p > 1024 && can_listen_on_loopback(p); });
    REQUIRE(ports);
    CertStore certs(data);
    WebServiceConfig config;
    config.data_dir = data;
    config.fixed_ports = WebPorts{static_cast<Port>(ports->http + 22000), static_cast<Port>(ports->https + 22000), true};
    config.idle_after = 2s;
    config.reload_every = 1s;
    WebService service(config, certs);
    service.start();
    const Port http = config.fixed_ports->http;

    auto st = service.status();
    INFO(st.dump(2));
    REQUIRE(st["nginx"]["running"] == true);
    CHECK(st["sites"]["wp"]["app"] == "apache");
    CHECK(st["sites"]["wp"]["run"] == "sleeping");

    const std::string first = get(http, "wp.localhost", "/2026/10/hello-world/");
    INFO(read_small_file(web_dir(data) / "apps" / "_apache.log", 1 << 20).value_or(""));
    INFO(read_small_file(web_dir(data) / "logs" / "apache-error.log", 1 << 20).value_or(""));
    CHECK(first == "200 wp /2026/10/hello-world/ /index.php - off");
    CHECK(service.status()["sites"]["wp"]["run"] == "running");
    CHECK(get(http, "wp.localhost", "/index.php/a/b") == "200 wp /index.php/a/b /index.php /a/b off");
    CHECK(get(http, "wp.localhost", "/readme.txt") == "200 plain file");
    CHECK(get(http, "blog.localhost") == "200 blog");
    CHECK(get(http, "wp.localhost", "/.env").starts_with("404 "));
    CHECK(get(http, "wp.localhost", "/.htaccess").starts_with("404 "));
    // Apache and its pool sleep; the next request wakes both.
    REQUIRE(eventually([&] { return service.status()["sites"]["wp"]["run"] == "sleeping"; }, 25s));
    CHECK(eventually([&] { return service.status()["pools"][0]["state"] == "sleeping"; }, 10s));
    CHECK(get(http, "wp.localhost", "/again") == "200 wp /again /index.php - off");

    for (Project& p : app.projects) {
        p.on = false;
    }
    REQUIRE(save_app_config(app_config_file(data), app, error));
    st = service.reload();
    CHECK_FALSE(st["apps"].contains("_apache"));
    service.stop();
}

#endif

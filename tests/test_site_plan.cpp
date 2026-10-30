#include "floserver/core/apache_config.h"
#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/site_plan.h"

#include "test_util.h"

#include <doctest/doctest.h>

#include <map>

using namespace floserver::core;
namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
const fs::path kBase = "C:/fs";
#else
const fs::path kBase = "/fs";
#endif

InstalledRuntime runtime(const std::string& name, const std::string& version, const std::string& variant = {}) {
    InstalledRuntime r;
    r.name = name;
    r.version = version;
    r.variant = variant;
    r.dir = kBase / "runtimes" / name / version;
    return r;
}

Project project(const std::string& name, bool on = true, bool keep = false) {
    return {name, kBase / "projects" / name, on, keep};
}

// Configs by project name; a project not listed has the defaults.
ConfigReader reader(std::map<std::string, std::string> files) {
    return [files = std::move(files)](const Project& p) {
        const auto it = files.find(p.name);
        if (it == files.end()) {
            ConfigResult r;
            r.config = ProjectConfig{};
            return r;
        }
        return parse_project_config(it->second);
    };
}

const std::vector<InstalledRuntime> kInstalled = {
    runtime("php", "8.4.27"), runtime("php", "8.4.3"),  runtime("php", "8.5.12"), runtime("php", "8.3.36", "ts"),
    runtime("mariadb", "11.4.13"), runtime("node", "24.22.1"), runtime("nginx", "1.30.6"),
    runtime("python", "3.13.18"),
};

}  // namespace

TEST_CASE("sites get the newest matching PHP and share pools by version and extensions") {
    AppConfig config;
    config.projects = {project("shop"), project("blog"), project("api"), project("legacy"), project("off", false),
                       project("plain")};
    const auto plan = plan_sites(config,
                                 reader({{"shop", R"({"php": "8.4", "extensions": ["intl", "gd"], "root": "public"})"},
                                         {"blog", R"({"php": "8.4.27", "extensions": ["gd", "intl", "gd"]})"},
                                         {"api", R"({"php": "8.4"})"},
                                         {"legacy", R"({"php": "8.3"})"}}),
                                 kInstalled, std::nullopt);
    REQUIRE(plan.sites.size() == 6);
    REQUIRE(plan.pools.size() == 3);

    const PlannedSite& shop = plan.sites[0];
    CHECK(shop.state == SiteState::Ready);
    CHECK(shop.doc_root == kBase / "projects" / "shop" / "public");
    REQUIRE(shop.pool);
    CHECK(plan.pools[*shop.pool].key == "php-8.4.27+gd+intl");
    CHECK(plan.pools[*shop.pool].php_dir == kBase / "runtimes" / "php" / "8.4.27");

    // Same version and extensions (order and repeats don't matter): same pool.
    CHECK(plan.sites[1].pool == shop.pool);
    CHECK(plan.pools[*shop.pool].sites == std::vector<std::string>{"shop", "blog"});
    // Same version, no extensions: its own pool.
    CHECK(plan.pools[*plan.sites[2].pool].key == "php-8.4.27");

    // Only a thread-safe 8.3 is installed: pools need the NTS build.
    CHECK(plan.sites[3].state == SiteState::NeedsDownload);
    CHECK(plan.sites[3].missing == std::vector<std::string>{"php 8.3"});
    CHECK_FALSE(plan.sites[3].pool);

    CHECK(plan.sites[4].state == SiteState::Off);
    // No floserver.json: the newest installed PHP.
    CHECK(plan.pools[*plan.sites[5].pool].key == "php-8.5.12");
}

TEST_CASE("the default PHP is used when a project names none") {
    AppConfig config;
    config.projects = {project("plain")};
    auto plan = plan_sites(config, reader({}), kInstalled, std::string("8.4"));
    CHECK(plan.pools.at(0).version == "8.4.27");
    plan = plan_sites(config, reader({}), kInstalled, std::string("7.4"));
    CHECK(plan.sites[0].state == SiteState::NeedsDownload);
    CHECK(plan.sites[0].missing == std::vector<std::string>{"php 7.4"});
    plan = plan_sites(config, reader({}), {}, std::nullopt);
    CHECK(plan.sites[0].missing == std::vector<std::string>{"php"});
}

TEST_CASE("missing databases, bad files and not-yet-supported projects are reported") {
    AppConfig config;
    config.projects = {project("db"), project("broken"), project("apache"), project("node"), project("both")};
    const auto plan = plan_sites(config,
                                 reader({{"db", R"({"php": "8.5", "database": "mariadb-11.8"})"},
                                         {"broken", R"({"php": 8})"},
                                         {"apache", R"({"web": "apache"})"},
                                         {"node", R"({"node": "24"})"},
                                         {"both", R"({"php": "8.5", "node": "22", "database": "mariadb-11.4"})"}}),
                                 kInstalled, std::nullopt);
    CHECK(plan.sites[0].state == SiteState::NeedsDownload);
    CHECK(plan.sites[0].missing == std::vector<std::string>{"mariadb 11.8"});
    CHECK(plan.sites[1].state == SiteState::Problem);
    CHECK(plan.sites[1].problem.starts_with("php "));  // the file reader adds "floserver.json: "
    CHECK(plan.sites[2].state == SiteState::NeedsDownload);  // Apache isn't installed in this list
    CHECK(plan.sites[2].missing == std::vector<std::string>{"apache"});
    CHECK(plan.sites[3].state == SiteState::Ready);  // a Node app (D11)
    CHECK(plan.sites[4].state == SiteState::NeedsDownload);
    CHECK(plan.sites[4].missing == std::vector<std::string>{"node 22"});
    CHECK(plan.sites[4].database == DatabaseSpec{"mariadb", "11.4"});
    CHECK(plan.pools.empty());
}

TEST_CASE("Node projects without PHP run their own server; with PHP, Node is only a requirement") {
    AppConfig config;
    config.projects = {project("web", true, true), project("old"), project("mixed"), project("api")};
    auto plan = plan_sites(config,
                           reader({{"web", R"({"node": "24", "command": "node server.js"})"},
                                   {"old", R"({"node": "18"})"},
                                   {"mixed", R"({"php": "8.5", "node": "24"})"},
                                   {"api", R"({"node": "24", "database": "mariadb-11.8"})"}}),
                           kInstalled, std::nullopt);
    REQUIRE(plan.apps.size() == 1);
    const AppSpec& web = plan.apps[0];
    CHECK(web.name == "web");
    CHECK(web.version == "24.22.1");
    CHECK(web.runtime_dir == kBase / "runtimes" / "node" / "24.22.1");
    CHECK(web.dir == kBase / "projects" / "web");
    CHECK(web.command == "node server.js");
    CHECK(web.keep_running);
    CHECK(plan.sites[0].state == SiteState::Ready);
    CHECK(plan.sites[0].app == std::optional<std::size_t>(0));
    CHECK_FALSE(plan.sites[0].pool);
    CHECK(plan.sites[1].state == SiteState::NeedsDownload);
    CHECK(plan.sites[1].missing == std::vector<std::string>{"node 18"});
    CHECK(plan.sites[2].pool.has_value());  // PHP serves it
    CHECK_FALSE(plan.sites[2].app);
    CHECK(plan.sites[3].missing == std::vector<std::string>{"mariadb 11.8"});

    // Ports from 9300, kept by name, never one a pool or another app has.
    REQUIRE(assign_pool_ports(plan, {}, [](Port) { return true; }));
    REQUIRE(assign_app_ports(plan, {}, [](Port p) { return p != 9300; }));
    CHECK(plan.apps[0].port == 9301);
    REQUIRE(assign_app_ports(plan, {{"web", 9310}, {"gone", 9302}}, [](Port) { return true; }));
    CHECK(plan.apps[0].port == 9310);
    plan.pools[0].ports = {9300, 9301, 9302, 9303};
    REQUIRE(assign_app_ports(plan, {{"web", 9301}}, [](Port) { return true; }));
    CHECK(plan.apps[0].port == 9304);
}

TEST_CASE("Python projects run their own server; Python serves when Node is named too (D17)") {
    AppConfig config;
    config.projects = {project("api"), project("full"), project("old"), project("tools")};
    const auto plan = plan_sites(config,
                                 reader({{"api", R"({"python": "3.13"})"},
                                         {"full", R"({"python": "3.13", "node": "24"})"},
                                         {"old", R"({"python": "3.12"})"},
                                         {"tools", R"({"php": "8.5", "python": "3.13"})"}}),
                                 kInstalled, std::nullopt);
    REQUIRE(plan.apps.size() == 2);
    CHECK(plan.apps[0].kind == AppKind::Python);
    CHECK(plan.apps[0].version == "3.13.18");
    CHECK(plan.apps[0].runtime_dir == kBase / "runtimes" / "python" / "3.13.18");
    CHECK(plan.apps[1].kind == AppKind::Python);
    CHECK(plan.sites[2].state == SiteState::NeedsDownload);
    CHECK(plan.sites[2].missing == std::vector<std::string>{"python 3.12"});
    CHECK(plan.sites[3].pool.has_value());  // PHP serves; Python is only a tool there
}

TEST_CASE("Apache projects are PHP projects served by the shared Apache (D6)") {
    AppConfig config;
    config.projects = {project("wp"), project("shop"), project("old")};
    auto installed = kInstalled;
    installed.push_back(runtime("apache", "2.4.69"));
    installed.push_back(runtime("apache", "2.4.62"));
    const auto plan = plan_sites(config,
                                 reader({{"wp", R"({"web": "apache", "php": "8.4"})"},
                                         {"shop", R"({"php": "8.4"})"},
                                         {"old", R"({"web": "apache", "node": "24"})"}}),
                                 installed, std::nullopt);
    REQUIRE(plan.sites.size() == 3);
    CHECK(plan.sites[0].state == SiteState::Ready);
    CHECK(plan.sites[0].apache);
    REQUIRE(plan.sites[0].pool);
    CHECK(plan.sites[0].pool == plan.sites[1].pool);  // the same pool as nginx's PHP 8.4 project
    CHECK_FALSE(plan.sites[1].apache);
    CHECK(plan.sites[2].apache);  // Apache serves it; Node is only a tool there
    CHECK_FALSE(plan.sites[2].app);
    CHECK(plan.apache_version == "2.4.69");
    CHECK(plan.apache_dir == kBase / "runtimes" / "apache" / "2.4.69");
    CHECK(plan.apps.empty());

    // Folder names Apache reads as a pattern: that project alone can't run.
    config.projects = {project("old"), project("wp")};
    config.projects[0].dir = kBase / "projects" / "shop [old]";
    const auto bad = plan_sites(config, reader({{"old", R"({"web": "apache"})"}, {"wp", R"({"web": "apache"})"}}),
                                installed, std::nullopt);
    CHECK(bad.sites[0].state == SiteState::Problem);
    CHECK(bad.sites[0].problem.find("Apache can't use") != std::string::npos);
    CHECK(bad.sites[1].state == SiteState::Ready);
}

TEST_CASE("the Apache config: one host per project, PHP to the pools, .htaccess read") {
    ApacheSpec spec;
    spec.server_root = kBase / "runtimes" / "apache" / "2.4.69" / "Apache24";
    spec.modules_dir = spec.server_root / "modules";
    spec.work_dir = kBase / "web";
    spec.sites = {{"wp", kBase / "projects" / "wp", {9101, 9102}}, {"docs", kBase / "projects" / "docs", {}}};
    std::string error;
    const auto text = make_apache_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->find("Listen 127.0.0.1:{port}\n") != std::string::npos);
    CHECK(text->find("ServerName wp.localhost") != std::string::npos);
    CHECK(text->find("AllowOverride All") != std::string::npos);
    // Both workers, split by the client port's last digit.
    CHECK(text->find("<If \"-f %{REQUEST_FILENAME} && %{REMOTE_PORT} =~ /[0-4]$/\">\n"
                     "            SetHandler \"proxy:fcgi://127.0.0.1:9101/\"") != std::string::npos);
    CHECK(text->find("<ElseIf \"-f %{REQUEST_FILENAME}\">\n"
                     "            SetHandler \"proxy:fcgi://127.0.0.1:9102/\"") != std::string::npos);
    CHECK(text->find("CGIPassAuth On") != std::string::npos);
    CHECK(text->find(R"(ProxyFCGISetEnvIf "true" !REDIRECT_URL)") != std::string::npos);
    CHECK(text->find("KeepAliveTimeout 15") != std::string::npos);
    CHECK(text->find("ServerName wp.localhost\n") != std::string::npos);  // plain http until there's a CA
    spec.sites[0].https = true;
    CHECK(make_apache_config(spec, error)->find("ServerName https://wp.localhost\n") != std::string::npos);
    spec.sites[0].https = false;
    CHECK(text->find("balancer") == std::string::npos);
    CHECK(text->find("PidFile " + *nginx_quoted_path(kBase / "web" / "apache" / "httpd-{port}.pid")) !=
          std::string::npos);
    CHECK(text->find("ProxyFCGIBackendType GENERIC") != std::string::npos);
    CHECK(text->find("SetEnvIf X-Forwarded-Proto \"^https$\" HTTPS=on") != std::string::npos);
    CHECK(text->find("RedirectMatch 404") != std::string::npos);
    CHECK(text->find("LoadModule rewrite_module") != std::string::npos);
    CHECK(text->find("mod_mpm_event.so") == std::string::npos);  // Windows has its own MPM
    // A static site gets no PHP handler.
    const auto docs = text->find("ServerName docs.localhost");
    REQUIRE(docs != std::string::npos);
    CHECK(text->find("SetHandler", docs) == std::string::npos);

    spec.posix = true;
    CHECK(make_apache_config(spec, error)->find("mod_mpm_event.so") != std::string::npos);
    spec.sites = {{"bad", kBase / "projects" / "a|b", {9101}}};
    CHECK_FALSE(make_apache_config(spec, error));
    CHECK(error.find("has a |") != std::string::npos);
    spec.sites = {{"Bad Name", kBase / "projects" / "x", {}}};
    CHECK_FALSE(make_apache_config(spec, error));
}

TEST_CASE("the nginx config forwards a Node project to its port, WebSockets too") {
    NginxSpec spec;
    spec.nginx_dir = kBase / "runtimes" / "nginx" / "1.30.6";
    spec.work_dir = kBase / "web";
    spec.ports = {80, 443, false};
    NginxSite app;
    app.name = "web";
    app.doc_root = kBase / "projects" / "web";
    app.app_port = 9300;
    spec.sites = {app};
    std::string error;
    const auto text = make_nginx_config(spec, error);
    REQUIRE(text);
    CHECK(text->find("upstream floserver_app_9300 {\n        server 127.0.0.1:9300 max_fails=0;") != std::string::npos);
    CHECK(text->find("map $http_upgrade $floserver_connection") != std::string::npos);
    CHECK(text->find("proxy_pass http://floserver_app_9300;") != std::string::npos);
    CHECK(text->find("proxy_set_header Connection $floserver_connection;") != std::string::npos);
    CHECK(text->find("proxy_set_header Host $http_host;") != std::string::npos);  // keeps :8080
    CHECK(text->find("proxy_set_header X-Forwarded-Port $server_port;") != std::string::npos);
    CHECK(text->find("server_name web.localhost;") != std::string::npos);
    // The app serves everything itself: no document root, no dot-file rule.
    CHECK(text->find("root \"") == std::string::npos);
    CHECK(text->find("location ~* /\\.") == std::string::npos);
    CHECK(text->find("fastcgi_pass") == std::string::npos);
}

TEST_CASE("keep running on any project keeps its shared pool awake") {
    AppConfig config;
    config.projects = {project("a"), project("b", true, true), project("c")};
    const auto plan = plan_sites(config, reader({{"c", R"({"php": "8.4"})"}}), kInstalled, std::nullopt);
    REQUIRE(plan.pools.size() == 2);
    CHECK(plan.pools[0].keep_running);
    CHECK_FALSE(plan.pools[1].keep_running);
}

TEST_CASE("one unusable folder name only stops that project") {
    AppConfig config;
    config.projects = {project("good"), {"bad", kBase / "projects" / "$shop", true, false, {}}};
    const auto plan = plan_sites(config, reader({}), kInstalled, std::nullopt);
    CHECK(plan.sites[0].state == SiteState::Ready);
    CHECK(plan.sites[1].state == SiteState::Problem);
    CHECK(plan.sites[1].problem.find('$') != std::string::npos);
    CHECK(plan.pools.size() == 1);
}

TEST_CASE("the project's folder file is read from the disk") {
    const test_util::TempDir tmp;
    const Project p{"shop", tmp.path() / "shop", true, false};
    CHECK_FALSE(read_project_config_file(p).config);  // folder missing
    fs::create_directories(p.dir);
    CHECK(read_project_config_file(p).config == ProjectConfig{});  // no file: defaults
    test_util::write_file(p.dir / "floserver.json", R"({"php": "8.2", "root": "public"})");
    const auto r = read_project_config_file(p);
    REQUIRE(r.config);
    CHECK(r.config->php == "8.2");
    test_util::write_file(p.dir / "floserver.json", R"({"php": 8})");
    const auto bad = read_project_config_file(p);
    REQUIRE_FALSE(bad.config);
    CHECK(bad.errors.at(0).message.starts_with("floserver.json: php "));
    test_util::write_file(p.dir / "floserver.json", std::string(std::size_t{70} * 1024, ' '));
    CHECK_FALSE(read_project_config_file(p).config);
    fs::remove(p.dir / "floserver.json");
    fs::create_directory(p.dir / "floserver.json");  // not a plain file
    CHECK_FALSE(read_project_config_file(p).config);
}

#ifndef _WIN32  // symlinks need extra rights on Windows; the check is the same code
TEST_CASE("a root that leads outside the project through a link is refused") {
    const test_util::TempDir tmp;
    const Project p{"shop", tmp.path() / "shop", true, false, {}};
    fs::create_directories(p.dir);
    fs::create_directories(tmp.path() / "elsewhere");
    fs::create_directory_symlink(tmp.path() / "elsewhere", p.dir / "public");
    test_util::write_file(p.dir / "floserver.json", R"({"root": "public"})");
    const auto r = read_project_config_file(p);
    REQUIRE_FALSE(r.config);
    CHECK(r.errors.at(0).message.find("outside the project") != std::string::npos);

    // A link that stays inside is fine, and so is a root that doesn't exist yet.
    fs::remove(p.dir / "public");
    fs::create_directories(p.dir / "web");
    fs::create_directory_symlink(p.dir / "web", p.dir / "public");
    CHECK(read_project_config_file(p).config);
    test_util::write_file(p.dir / "floserver.json", R"({"root": "dist/site"})");
    CHECK(read_project_config_file(p).config);
}
#endif

TEST_CASE("pools get one port per worker from 9100, avoid taken ports and stay put across reloads") {
    const auto pool = [](const char* key) { return PhpPool{key, "", {}, {}, {}, false, {}}; };
    WebPlan plan;
    plan.pools = {pool("php-8.4.27"), pool("php-8.5.12")};
    const auto probe = [](Port p) { return p != 9100; };  // 9100 is someone else's
    REQUIRE(assign_pool_ports(plan, {}, probe, 2));
    CHECK(plan.pools[0].ports == std::vector<Port>{9101, 9102});
    CHECK(plan.pools[1].ports == std::vector<Port>{9103, 9104});

    // Next reload: 8.4 gone, a new pool appears. 8.5 keeps its ports even
    // though the probe would now say they're taken (the service holds them),
    // and the new pool doesn't take 8.4's while it may still be winding down.
    const std::map<std::string, std::vector<Port>> previous = {{"php-8.4.27", {9101, 9102}},
                                                               {"php-8.5.12", {9103, 9104}}};
    WebPlan next;
    next.pools = {pool("php-8.5.12"), pool("php-8.3.36")};
    const auto busy = [](Port p) { return p < 9100 || p > 9104; };
    REQUIRE(assign_pool_ports(next, previous, busy, 2));
    CHECK(next.pools[0].ports == std::vector<Port>{9103, 9104});
    CHECK(next.pools[1].ports == std::vector<Port>{9105, 9106});

    // A different worker count gives fresh ports.
    WebPlan more;
    more.pools = {pool("php-8.5.12")};
    REQUIRE(assign_pool_ports(more, previous, busy, 3));
    CHECK(more.pools[0].ports == std::vector<Port>{9105, 9106, 9107});

    WebPlan full;
    full.pools = {pool("php-8.4.27")};
    CHECK_FALSE(assign_pool_ports(full, {}, [](Port) { return false; }, 2));
}

TEST_CASE("addresses show the port only when it isn't the usual one") {
    CHECK(site_address("shop", {80, 443, false}, true) == "https://shop.localhost");
    CHECK(site_address("shop", {80, 443, false}, false) == "http://shop.localhost");
    CHECK(site_address("shop", {8080, 8443, true}, true) == "https://shop.localhost:8443");
    CHECK(site_address("shop", {8080, 8443, true}, false) == "http://shop.localhost:8080");
}

TEST_CASE("nginx paths are quoted with forward slashes; unsafe ones are refused") {
    CHECK(nginx_quoted_path(kBase / "projects" / "my shop" / "public") ==
          "\"" + kBase.generic_string() + "/projects/my shop/public\"");
    CHECK(nginx_quoted_path(kBase / path_from_utf8("\xD9\x85\xD8\xAA\xD8\xAC\xD8\xB1")) ==
          "\"" + kBase.generic_string() + "/\xD9\x85\xD8\xAA\xD8\xAC\xD8\xB1\"");  // UTF-8 kept
    CHECK_FALSE(nginx_quoted_path(kBase / "a$b"));
    CHECK_FALSE(nginx_quoted_path(kBase / "a\"b"));
    CHECK_FALSE(nginx_quoted_path(kBase / "a\nb"));
    CHECK_FALSE(nginx_quoted_path(""));
}

TEST_CASE("the nginx config serves PHP and static sites on loopback") {
    NginxSpec spec;
    spec.nginx_dir = kBase / "runtimes" / "nginx" / "1.30.6";
    spec.work_dir = kBase / "web";
    spec.ports = {80, 443, false};
    spec.ipv6 = true;
    spec.sites = {{"shop", kBase / "projects" / "shop" / "public", {9101, 9102}, kBase / "web" / "logs" / "php-8.4.27.log",
                   NginxTls{kBase / "certs" / "shop.pem", kBase / "certs" / "shop.key"}, std::nullopt, {}},
                  {"docs", kBase / "projects" / "docs", {}, {}, std::nullopt, std::nullopt, {}}};
    spec.sites[0].request_log = kBase / "web" / "logs" / "sites" / "shop.log";
    std::string error;
    const auto text = make_nginx_config(spec, error);
    REQUIRE(text);
    const auto has = [&text](const std::string& s) { return text->find(s) != std::string::npos; };
    const auto count = [&text](const std::string& s) {
        std::size_t n = 0;
        for (auto at = text->find(s); at != std::string::npos; at = text->find(s, at + 1)) {
            ++n;
        }
        return n;
    };

    // Every request goes to the window's log: at the server and again in the
    // PHP location, which has its own access_log and so inherits none.
    CHECK(has("log_format floserver_requests '$msec $status $request_time $request_method $request_uri';"));
    const std::string request_log = "access_log \"" + kBase.generic_string() + "/web/logs/sites/shop.log\" floserver_requests;";
    CHECK(count(request_log) == 2);
    CHECK(count("floserver_requests;") == 2);  // docs has no log set

    CHECK(has("listen 127.0.0.1:80 default_server;"));
    CHECK(has("listen [::1]:80 default_server;"));
    CHECK(has("ssl_reject_handshake on;"));
    CHECK(has("server_name shop.localhost;"));
    CHECK(has("return 301 https://shop.localhost$request_uri;"));
    CHECK(has("listen 127.0.0.1:443 ssl;"));
    CHECK(has("upstream floserver_php_9101 {\n        server 127.0.0.1:9101 max_fails=0;\n        server 127.0.0.1:9102 max_fails=0;\n    }"));
    CHECK(has("fastcgi_pass floserver_php_9101;"));
    CHECK(has("fastcgi_param HTTPS on;"));
    CHECK(has("access_log \"" + kBase.generic_string() + "/web/logs/php-8.4.27.log\" floserver_idle;"));
    CHECK(has("root \"" + kBase.generic_string() + "/projects/shop/public\";"));
    CHECK(has("try_files $uri $uri/ =404;"));  // docs: static only
    // Caseless, so /x.PHP runs (or is hidden) instead of being sent as source.
    CHECK(has(R"(location ~* [^/]\.php(/|$) {)"));
    CHECK(has(R"(location ~* /\.(?!well-known/) {)"));
    CHECK_FALSE(has("location ~ "));
    CHECK_FALSE(has("0.0.0.0"));
    CHECK_FALSE(has("listen 80"));
    CHECK_FALSE(has("config-id"));  // only when the spec has one
    // The static site has no PHP location and no https.
    const auto docs = text->find("# Project docs");
    REQUIRE(docs != std::string::npos);
    CHECK(text->find("fastcgi_pass", docs) == std::string::npos);
    CHECK(text->find("ssl", docs) == std::string::npos);

    SUBCASE("fallback ports and no IPv6") {
        spec.ports = {8080, 8443, true};
        spec.ipv6 = false;
        const auto t = make_nginx_config(spec, error);
        REQUIRE(t);
        CHECK(t->find("return 301 https://shop.localhost:8443$request_uri;") != std::string::npos);
        CHECK(t->find("[::1]") == std::string::npos);
    }
    SUBCASE("an unsafe path or name refuses the whole config") {
        spec.sites[1].doc_root = kBase / "projects" / "$docs";
        CHECK_FALSE(make_nginx_config(spec, error));
        CHECK(error.find("$docs") != std::string::npos);
        spec.sites[1].doc_root = kBase / "projects" / "docs";
        spec.sites[1].name = "Docs; include /etc/passwd";
        CHECK_FALSE(make_nginx_config(spec, error));
    }
    SUBCASE("no sites still answers with a 404") {
        spec.sites.clear();
        const auto t = make_nginx_config(spec, error);
        REQUIRE(t);
        CHECK(t->find("There's no FloServer project") != std::string::npos);
        CHECK(t->find("ssl") == std::string::npos);
    }
}

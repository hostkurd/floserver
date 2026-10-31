// Settings → Ports & HTTPS (web_settings.h): config.json's "web", the port
// choice and who holds a port, the requests, and the nginx and Apache
// configs for the redirect and https://localhost/. Served for real in
// test_web_service.cpp.

#include "floserver/core/apache_config.h"
#include "floserver/core/nginx_config.h"
#include "floserver/core/project_service.h"
#include "floserver/core/shared_sites.h"
#include "floserver/core/web_settings.h"

#include "test_util.h"

#include <doctest/doctest.h>

#include <set>

using namespace floserver::core;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

#ifdef _WIN32
const fs::path kBase = "C:/ws";
#else
const fs::path kBase = "/ws";
#endif

AppConfig with_web(const json& web) {
    AppConfig app;
    app.other["web"] = web;
    return app;
}

// Every port free except `taken`.
PortProbe free_except(std::set<Port> taken) {
    return [taken = std::move(taken)](Port p) { return !taken.contains(p); };
}

}  // namespace

TEST_CASE("web settings: the defaults, what's read and what's refused") {
    std::string error;
    auto s = read_web_settings(AppConfig{}, error);
    REQUIRE(s);
    CHECK(s->ports == PortChoice::Auto);
    CHECK(s->https);
    CHECK_FALSE(s->shared_https);
    CHECK_FALSE(wanted_web_ports(*s));

    s = read_web_settings(with_web({{"ports", "standard"}, {"https", false}, {"shared_https", true}}), error);
    REQUIRE_MESSAGE(s, error);
    CHECK(s->ports == PortChoice::Standard);
    CHECK_FALSE(s->https);
    CHECK(s->shared_https);
    CHECK(wanted_web_ports(*s) == WebPorts{80, 443, false});
    s = read_web_settings(with_web({{"ports", "alternate"}}), error);
    REQUIRE(s);
    CHECK(wanted_web_ports(*s) == WebPorts{8080, 8443, false});
    s = read_web_settings(with_web({{"ports", "custom"}, {"http_port", 8000}, {"https_port", 8001}}), error);
    REQUIRE_MESSAGE(s, error);
    CHECK(wanted_web_ports(*s) == WebPorts{8000, 8001, false});
    // Custom ports are kept while another choice is picked.
    s = read_web_settings(with_web({{"ports", "auto"}, {"http_port", 8000}, {"https_port", 8001}}), error);
    REQUIRE(s);
    CHECK(s->http_port == Port{8000});
    CHECK_FALSE(wanted_web_ports(*s));

    for (const json& bad : {json("auto"), json({{"ports", "fast"}}), json({{"ports", 80}}), json({{"https", "yes"}}),
                            json({{"http_port", 0}}), json({{"http_port", 70000}}), json({{"http_port", "80"}}),
                            json({{"ports", "custom"}, {"http_port", 8000}}),
                            json({{"ports", "custom"}, {"http_port", 8000}, {"https_port", 8000}})}) {
        error.clear();
        CHECK_FALSE(read_web_settings(with_web(bad), error));
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("web settings are written beside the rest of config.json, unknown keys kept") {
    AppConfig app = with_web({{"later", 1}});
    app.other["shared"] = {{"on", true}};
    WebSettings s;
    s.ports = PortChoice::Custom;
    s.http_port = 8000;
    s.https_port = 8001;
    s.shared_https = true;
    write_web_settings(app, s);
    CHECK(app.other["web"]["later"] == 1);
    CHECK(app.other["web"]["ports"] == "custom");
    CHECK(app.other["web"]["http_port"] == 8000);
    CHECK(app.other["shared"]["on"] == true);
    std::string error;
    CHECK(read_web_settings(app, error) == s);

    // Changes: only the keys given; one bad value changes nothing.
    WebSettings changed = s;
    REQUIRE(apply_web_settings_changes(changed, {{"ports", "standard"}}, error));
    CHECK(changed.ports == PortChoice::Standard);
    CHECK(changed.http_port == Port{8000});
    const WebSettings before = changed;
    CHECK_FALSE(apply_web_settings_changes(changed, {{"https", false}, {"https_port", -1}}, error));
    CHECK(changed == before);
    CHECK_FALSE(apply_web_settings_changes(changed, {{"colour", "red"}}, error));
    CHECK(error.find("colour") != std::string::npos);
    CHECK_FALSE(apply_web_settings_changes(changed, {{"ports", "custom"}, {"http_port", 9000}, {"https_port", 9000}}, error));
    CHECK(error.find("different") != std::string::npos);
}

TEST_CASE("the port choice: the pair asked for when it's free, else the automatic one with what was taken") {
    WebSettings s;
    auto chosen = choose_web_ports(s, free_except({}));
    CHECK(chosen.ports == WebPorts{80, 443, false});
    CHECK(chosen.taken.empty());
    chosen = choose_web_ports(s, free_except({80}));
    CHECK(chosen.ports == WebPorts{8080, 8443, true});
    CHECK(chosen.taken.empty());  // auto asked for nothing

    s.ports = PortChoice::Alternate;
    chosen = choose_web_ports(s, free_except({}));
    CHECK(chosen.ports == WebPorts{8080, 8443, false});  // even with 80 and 443 free

    s.ports = PortChoice::Standard;
    chosen = choose_web_ports(s, free_except({443}));
    CHECK(chosen.taken == std::vector<Port>{443});
    CHECK(chosen.ports == WebPorts{8080, 8443, true});

    s.ports = PortChoice::Custom;
    s.http_port = 8000;
    s.https_port = 8001;
    chosen = choose_web_ports(s, free_except({8000, 8001, 80}));
    CHECK(chosen.taken == std::vector<Port>{8000, 8001});
    CHECK(chosen.ports == WebPorts{8080, 8443, true});
    chosen = choose_web_ports(s, free_except({80}));
    CHECK(chosen.ports == WebPorts{8000, 8001, false});
}

TEST_CASE("web.settings.get and .set: saved in config.json, ports checked, the owner named") {
    const test_util::TempDir tmp;
    ProjectService projects(tmp.path() / "data");
    std::set<Port> taken = {80};
    std::optional<WebPorts> in_use = WebPorts{8080, 8443, true};
    int reloads = 0;
    int lookups = 0;
    WebSettingsService::Hooks hooks;
    hooks.in_use = [&] { return in_use; };
    hooks.reload = [&] { ++reloads; };
    hooks.ca = [] { return json(nullptr); };
    hooks.probe = [&](Port p) { return !taken.contains(p); };
    hooks.owner = [&](Port p) -> std::optional<PortOwner> {
        ++lookups;
        if (p == 80) {
            return PortOwner{1234, "httpd.exe"};
        }
        return std::nullopt;
    };
    WebSettingsService service(projects, hooks);
    ServiceRefusal refusal;

    // Auto, 80 held by Apache from XAMPP: said, with the program; 443 is free.
    auto r = service.get(refusal);
    REQUIRE_MESSAGE(r, refusal.message);
    CHECK((*r)["ports"] == "auto");
    CHECK((*r)["https"] == true);
    CHECK((*r)["shared_https"] == false);
    CHECK((*r)["in_use"] == json{{"http", 8080}, {"https", 8443}});
    REQUIRE((*r)["taken"].size() == 1);
    CHECK((*r)["taken"][0] == json{{"port", 80}, {"pid", 1234}, {"program", "httpd.exe"}});
    CHECK((*r)["ca"].is_null());
    CHECK(lookups == 1);
    CHECK(reloads == 0);

    // Not a port change: no port check, no lookup.
    r = service.set({{"https", false}}, refusal);
    REQUIRE_MESSAGE(r, refusal.message);
    CHECK((*r)["https"] == false);
    CHECK((*r)["taken"].empty());
    CHECK(lookups == 1);
    CHECK(reloads == 1);

    // A port change: checked again. The ports nginx is on are ours, not taken.
    in_use = WebPorts{8000, 8001, false};
    taken = {8000, 8001, 9999};
    r = service.set({{"ports", "custom"}, {"http_port", 8000}, {"https_port", 8001}}, refusal);
    REQUIRE_MESSAGE(r, refusal.message);
    CHECK((*r)["ports"] == "custom");
    CHECK((*r)["http_port"] == 8000);
    CHECK((*r)["taken"].empty());
    CHECK(reloads == 2);

    // Another program on one of them, nobody known: the port alone.
    in_use = WebPorts{8080, 8443, true};
    r = service.set({{"https_port", 9999}}, refusal);
    REQUIRE_MESSAGE(r, refusal.message);
    REQUIRE((*r)["taken"].size() == 2);
    CHECK((*r)["taken"][0] == json{{"port", 8000}});
    CHECK((*r)["taken"][1] == json{{"port", 9999}});

    // Saved: config.json has it, beside what was there.
    std::string error;
    const auto app = load_app_config(app_config_file(tmp.path() / "data"), error);
    REQUIRE(app);
    CHECK(app->other["web"]["ports"] == "custom");
    CHECK(app->other["web"]["https_port"] == 9999);
    CHECK(app->other["web"]["https"] == false);

    // Refused: nothing saved, nothing reloaded.
    CHECK_FALSE(service.set({{"ports", "fast"}}, refusal));
    CHECK(refusal.code == "bad_params");
    CHECK_FALSE(service.set({{"http_port", 9999}}, refusal));  // the same as https now
    CHECK(reloads == 3);
    r = service.get(refusal);
    REQUIRE(r);
    CHECK((*r)["http_port"] == 8000);

    // nginx not running: nothing in use, and every port asked for is checked.
    in_use.reset();
    taken = {9999};
    r = service.get(refusal);
    REQUIRE(r);
    CHECK((*r)["in_use"].is_null());
    REQUIRE((*r)["taken"].size() == 1);
    CHECK((*r)["taken"][0]["port"] == 9999);
}

TEST_CASE("an unreadable web setting is refused, and a change replaces it") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    test_util::write_file(app_config_file(data), R"({"projects": [], "web": {"ports": 8080}})");
    ProjectService projects(data);
    WebSettingsService service(projects, {});
    ServiceRefusal refusal;
    CHECK_FALSE(service.get(refusal));
    CHECK(refusal.code == "config_unreadable");
    const auto r = service.set({{"ports", "alternate"}}, refusal);
    REQUIRE_MESSAGE(r, refusal.message);
    CHECK((*r)["ports"] == "alternate");
}

TEST_CASE("the owner of a port: named on Windows, not looked up elsewhere") {
#ifdef _WIN32
    // Nothing listens on port 1 on a CI runner.
    CHECK_FALSE(find_port_owner(1));
#else
    CHECK_FALSE(find_port_owner(22));
#endif
}

TEST_CASE("the nginx config: http redirects to https with a 302; https://localhost/ only for localhost") {
    NginxSpec spec;
    spec.nginx_dir = kBase / "runtimes" / "nginx";
    spec.work_dir = kBase / "web";
    spec.ports = {8080, 8443, true};
    spec.sites = {{"shop", kBase / "projects" / "shop", {9100}, {}, NginxTls{kBase / "s.pem", kBase / "s.key"},
                   std::nullopt, {}}};
    NginxShared shared;
    shared.dir = kBase / "www";
    shared.list_script = kBase / "web" / "shared" / "shared-sites-list.php";
    shared.php_ports = {9101};
    spec.shared = shared;
    std::string error;
    auto text = make_nginx_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    // A 302: a browser would keep a 301 after HTTPS is turned off.
    CHECK(text->find("return 302 https://shop.localhost:8443$request_uri;") != std::string::npos);
    CHECK(text->find("return 301") == std::string::npos);
    // Shared sites stay http without their switch.
    CHECK(text->find("return 302 https://localhost") == std::string::npos);

    spec.shared->tls = NginxTls{kBase / "localhost.pem", kBase / "localhost.key"};
    text = make_nginx_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->find("return 302 https://localhost:8443$request_uri;") != std::string::npos);
    CHECK(text->find("ssl_certificate " + *nginx_quoted_path(kBase / "localhost.pem") + ";") != std::string::npos);
    // localhost's own server is on the https port, with HTTPS for PHP; the folders' stays on http.
    const auto local = text->find("server_name localhost;\n        root");
    REQUIRE(local != std::string::npos);
    const auto server_start = text->rfind("server {", local);
    CHECK(text->substr(server_start, local - server_start).find("listen 127.0.0.1:8443 ssl;") != std::string::npos);
    const auto folders = text->find("floserver_folder>");
    REQUIRE(folders != std::string::npos);
    const auto folder_start = text->rfind("server {", folders);
    CHECK(text->substr(folder_start, folders - folder_start).find("listen 127.0.0.1:8080;") != std::string::npos);
    CHECK(text->find("fastcgi_param HTTPS on;") != std::string::npos);

    // Only shared sites have a certificate: the https port still turns away other names.
    spec.sites[0].tls.reset();
    text = make_nginx_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->find("ssl_reject_handshake on;") != std::string::npos);
    CHECK(text->find("return 302 https://shop.localhost") == std::string::npos);

    // Standard ports: no port in the address.
    spec.ports = {80, 443, false};
    text = make_nginx_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->find("return 302 https://localhost$request_uri;") != std::string::npos);
}

TEST_CASE("shared sites' addresses and Apache's name say https when it's on") {
    CHECK(shared_address(WebPorts{8080, 8443, true}, true) == "https://localhost:8443");
    CHECK(shared_address(WebPorts{80, 443, false}, true) == "https://localhost");
    CHECK(shared_address(WebPorts{8080, 8443, true}) == "http://localhost:8080");

    ApacheSpec spec;
    spec.server_root = kBase / "runtimes" / "apache" / "Apache24";
    spec.modules_dir = spec.server_root / "modules";
    spec.work_dir = kBase / "web";
    spec.shared = ApacheShared{kBase / "www", {9105}, true};
    std::string error;
    auto text = make_apache_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->find("ServerName https://localhost\n") != std::string::npos);
    spec.shared->https = false;
    text = make_apache_config(spec, error);
    REQUIRE(text);
    CHECK(text->find("ServerName localhost\n") != std::string::npos);

    // The list page: folder links on the http port when the page is on https.
    const std::string script = shared_list_script(kBase / "www", kBase / "config.json", WebServer::Nginx, 8080);
    CHECK(script.find("$folder_port = $https ? ':8080' : $port;") != std::string::npos);
    CHECK(shared_list_script(kBase / "www", kBase / "config.json", WebServer::Nginx).find("$https ? '' : $port") !=
          std::string::npos);
}

// Shared sites (D19): settings, the folder scan, the list page, the plan and
// the nginx and Apache configs. Served for real in test_web_service.cpp.

#include "floserver/core/apache_config.h"
#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_service.h"
#include "floserver/core/shared_sites.h"
#include "floserver/core/site_plan.h"

#include "test_util.h"

#include <doctest/doctest.h>

using namespace floserver::core;
namespace fs = std::filesystem;
using nlohmann::json;

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

AppConfig with_shared(const json& shared) {
    AppConfig app;
    app.other["shared"] = shared;
    return app;
}

ConfigReader no_files() {
    return [](const Project&) {
        ConfigResult r;
        r.config = ProjectConfig{};
        return r;
    };
}

}  // namespace

TEST_CASE("shared sites settings: defaults, what's read and what's refused") {
    std::string error;
    auto s = read_shared_config(AppConfig{}, error);
    REQUIRE(s);
    CHECK_FALSE(s->on);
    CHECK(s->web == WebServer::Apache);  // .htaccess works by default
    CHECK_FALSE(s->php);

    s = read_shared_config(with_shared({{"on", true}, {"dir", path_to_utf8(kBase / "www")}, {"php", "8.4"},
                                        {"web", "nginx"}, {"database", "mariadb-11.4"}}),
                           error);
    REQUIRE_MESSAGE(s, error);
    CHECK(s->on);
    CHECK(s->dir == kBase / "www");
    CHECK(s->php == "8.4");
    CHECK(s->web == WebServer::Nginx);
    REQUIRE(s->database);
    CHECK(s->database->version == "11.4");

    // Written back the way it's read.
    AppConfig app;
    write_shared_config(app, *s);
    CHECK(read_shared_config(app, error) == s);

    CHECK_FALSE(read_shared_config(with_shared({{"on", "yes"}}), error));
    CHECK_FALSE(read_shared_config(with_shared({{"dir", "relative/www"}}), error));
    CHECK_FALSE(read_shared_config(with_shared({{"php", "eight"}}), error));
    CHECK(error.find("shared.php") != std::string::npos);
    CHECK_FALSE(read_shared_config(with_shared({{"web", "iis"}}), error));
    CHECK_FALSE(read_shared_config(with_shared("on"), error));
}

TEST_CASE("the www folder's folders: what each one is, and names a project has") {
    const test_util::TempDir tmp;
    const fs::path www = tmp.path() / "www";
    test_util::write_file(www / "blog" / "index.php", "<?php");
    test_util::write_file(www / "site" / "index.html", "<p>");
    test_util::write_file(www / "spa" / "index.html", "<p>");
    test_util::write_file(www / "spa" / "package.json", "{}");  // a built front end
    test_util::write_file(www / "chat" / "package.json", "{}");
    test_util::write_file(www / "api" / "requirements.txt", "fastapi");
    test_util::write_file(www / "old" / "lib.php", "<?php");
    test_util::write_file(www / "My Shop" / "index.php", "<?php");
    test_util::write_file(www / "shop" / "index.php", "<?php");
    test_util::write_file(www / ".git" / "config", "x");
    test_util::write_file(www / "readme.txt", "a file, not a folder");
    fs::create_directories(www / "empty");

    AppConfig app;
    app.projects.push_back({"shop", tmp.path() / "shop-project", true, false});
    const auto folders = scan_shared_folders(www, app);
    std::map<std::string, SharedFolder> by;
    for (const SharedFolder& f : folders) {
        by[f.name] = f;
    }
    CHECK(folders.size() == 9);
    CHECK_FALSE(by.contains(".git"));
    CHECK(by["blog"].kind == SharedKind::Php);
    CHECK(by["old"].kind == SharedKind::Php);
    CHECK(by["site"].kind == SharedKind::Static);
    CHECK(by["spa"].kind == SharedKind::Static);
    CHECK(by["chat"].kind == SharedKind::Node);
    CHECK(by["api"].kind == SharedKind::Python);
    CHECK(by["empty"].kind == SharedKind::Empty);
    CHECK(by["blog"].host_name);
    CHECK_FALSE(by["My Shop"].host_name);  // only at localhost/My%20Shop/
    CHECK(by["shop"].clash == "shop");
    CHECK_FALSE(by["blog"].clash);
    CHECK(scan_shared_folders(tmp.path() / "missing", app).empty());

    const std::string page =
        shared_index_page(folders, www, "8.5.12", WebServer::Apache, "http://localhost:8080", ":8080");
    CHECK(page.find("href=\"http://localhost:8080/blog/\"") != std::string::npos);
    CHECK(page.find("href=\"http://blog.localhost:8080/\"") != std::string::npos);
    CHECK(page.find("href=\"http://localhost:8080/My%20Shop/\"") != std::string::npos);
    CHECK(page.find("My Shop.localhost") == std::string::npos);
    CHECK(page.find("shop.localhost is the project shop") != std::string::npos);
    CHECK(page.find("Node.js app") != std::string::npos);
    CHECK(page.find("PHP 8.5.12 · Apache") != std::string::npos);
    // Names are escaped.
    const std::vector<SharedFolder> odd = {{"<b>x", SharedKind::Static, false, std::nullopt}};
    const std::string escaped = shared_index_page(odd, www, "8.5", WebServer::Nginx, "http://localhost", "");
    CHECK(escaped.find("<b>x") == std::string::npos);
    CHECK(escaped.find("&lt;b&gt;x") != std::string::npos);

    CHECK(shared_address(WebPorts{8080, 8443, true}) == "http://localhost:8080");
    CHECK(shared_address(WebPorts{80, 443, true}) == "http://localhost");
}

TEST_CASE("shared sites are planned like a PHP project: a pool, Apache or nginx, what's missing") {
    const std::vector<InstalledRuntime> installed = {runtime("php", "8.5.12"), runtime("php", "8.4.27"),
                                                     runtime("nginx", "1.30.6"), runtime("apache", "2.4.69")};
    const json on = {{"on", true}, {"dir", path_to_utf8(kBase / "www")}};

    WebPlan plan = plan_sites(AppConfig{}, no_files(), installed, std::nullopt);
    CHECK_FALSE(plan.shared);  // off

    plan = plan_sites(with_shared(on), no_files(), installed, std::nullopt);
    REQUIRE(plan.shared);
    CHECK(plan.shared->name == kSharedSiteName);
    CHECK(plan.shared->state == SiteState::Ready);
    CHECK(plan.shared->doc_root == kBase / "www");
    CHECK(plan.shared->apache);
    REQUIRE(plan.shared->pool);
    CHECK(plan.pools.at(*plan.shared->pool).version == "8.5.12");  // the newest
    CHECK(plan.apache_version == "2.4.69");

    json nginx = on;
    nginx["web"] = "nginx";
    nginx["php"] = "8.4";
    plan = plan_sites(with_shared(nginx), no_files(), installed, std::nullopt);
    REQUIRE(plan.shared);
    CHECK_FALSE(plan.shared->apache);
    CHECK(plan.pools.at(*plan.shared->pool).version == "8.4.27");

    // A project on the same PHP shares the pool.
    AppConfig app = with_shared(nginx);
    app.projects.push_back({"blog", kBase / "projects" / "blog", true, false});
    plan = plan_sites(app, [](const Project&) { return parse_project_config(R"({"php": "8.4"})"); }, installed,
                      std::nullopt);
    REQUIRE(plan.shared);
    CHECK(plan.pools.size() == 1);
    CHECK(plan.sites.front().pool == plan.shared->pool);

    plan = plan_sites(with_shared(on), no_files(), {runtime("php", "8.5.12"), runtime("nginx", "1.30.6")},
                      std::nullopt);
    REQUIRE(plan.shared);
    CHECK(plan.shared->state == SiteState::NeedsDownload);
    CHECK(plan.shared->missing == std::vector<std::string>{"apache"});

    json old = on;
    old["php"] = "7.4";
    plan = plan_sites(with_shared(old), no_files(), installed, std::nullopt);
    CHECK(plan.shared->missing == std::vector<std::string>{"php 7.4"});

    plan = plan_sites(with_shared({{"on", 3}}), no_files(), installed, std::nullopt);
    REQUIRE(plan.shared);
    CHECK(plan.shared->state == SiteState::Problem);

    json percent = on;
    percent["dir"] = path_to_utf8(kBase / "100%");
    plan = plan_sites(with_shared(percent), no_files(), installed, std::nullopt);
    CHECK(plan.shared->state == SiteState::Problem);  // Apache reads % in VirtualDocumentRoot
}

TEST_CASE("the nginx config: localhost and <folder>.localhost, PHP or Apache behind") {
    NginxSpec spec;
    spec.nginx_dir = kBase / "runtimes" / "nginx";
    spec.work_dir = kBase / "web";
    spec.ports = {8080, 8443, true};
    spec.sites = {{"shop", kBase / "projects" / "shop", {}, {}, std::nullopt, std::nullopt, {}}};
    NginxShared shared;
    shared.dir = kBase / "www";
    shared.list_page = kBase / "web" / "shared" / "shared-sites-list.html";
    shared.php_ports = {9101, 9102};
    spec.shared = shared;
    std::string error;
    auto text = make_nginx_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->find("server_name localhost;") != std::string::npos);
    CHECK(text->find(R"(server_name "~^(?<floserver_folder>[a-z0-9]([a-z0-9-]*[a-z0-9])?)\.localhost$";)") !=
          std::string::npos);
    const std::string www = nginx_quoted_path(kBase / "www")->substr(1);
    CHECK(text->find("root \"" + www.substr(0, www.size() - 1) + "/$floserver_folder\";") != std::string::npos);
    CHECK(text->find("error_page 403 404 = @floserver_shared_list;") != std::string::npos);
    CHECK(text->find("rewrite ^ /shared-sites-list.html break;") != std::string::npos);
    CHECK(text->find("upstream floserver_php_9101") != std::string::npos);
    CHECK(text->find("try_files $uri $uri/ =404;") != std::string::npos);  // localhost/x: no front controller
    CHECK(text->find("server_name shop.localhost;") != std::string::npos);  // the project keeps its name

    // Apache: everything goes to it.
    spec.shared->php_ports.clear();
    spec.shared->app_port = 9300;
    text = make_nginx_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->find("upstream floserver_app_9300") != std::string::npos);
    CHECK(text->find("proxy_pass http://floserver_app_9300;") != std::string::npos);
    CHECK(text->find("fastcgi_pass") == std::string::npos);

    // No shared sites: none of it.
    spec.shared.reset();
    text = make_nginx_config(spec, error);
    REQUIRE(text);
    CHECK(text->find("floserver_folder") == std::string::npos);
}

TEST_CASE("the Apache config: shared sites last, each folder its own root") {
    ApacheSpec spec;
    spec.server_root = kBase / "runtimes" / "apache" / "2.4.69" / "Apache24";
    spec.modules_dir = spec.server_root / "modules";
    spec.work_dir = kBase / "web";
    spec.sites = {{"wp", kBase / "projects" / "wp", {9101}}};
    spec.shared = ApacheShared{kBase / "www", {9105, 9106}, kBase / "web" / "shared" / "shared-sites-list.html"};
    std::string error;
    auto text = make_apache_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->find("LoadModule vhost_alias_module") != std::string::npos);
    const auto project = text->find("ServerName wp.localhost");
    const auto shared = text->find("ServerAlias *.localhost");
    REQUIRE(project != std::string::npos);
    REQUIRE(shared != std::string::npos);
    CHECK(project < shared);
    const std::string www = nginx_quoted_path(kBase / "www")->substr(1);
    const std::string bare = www.substr(0, www.size() - 1);
    CHECK(text->find("VirtualDocumentRoot \"" + bare + "/%1\"") != std::string::npos);
    CHECK(text->find("Alias \"/__floserver_shared__.html\"") != std::string::npos);
    CHECK(text->find("DirectoryIndex index.php index.html index.htm /__floserver_shared__.html") != std::string::npos);
    CHECK(text->find("SetHandler \"proxy:fcgi://127.0.0.1:9105/\"") != std::string::npos);
    // SCRIPT_NAME under <folder>.localhost leaves the folder out.
    CHECK(text->find(bare + R"(\\E/[^/]+\\Q\\E(/.*)$|i" SCRIPT_NAME "$1")") != std::string::npos);

    spec.shared->dir = kBase / "100%";
    CHECK_FALSE(make_apache_config(spec, error));
}

TEST_CASE("the service keeps shared sites settings in config.json") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    const fs::path www = tmp.path() / "my www";
    test_util::write_file(www / "blog" / "index.php", "<?php");
    ProjectService service(data);
    ServiceRefusal refusal;

    auto r = service.shared(refusal);
    REQUIRE(r);
    CHECK((*r)["on"] == false);
    CHECK((*r)["web"] == "apache");

    r = service.set_shared({{"on", true}, {"dir", path_to_utf8(www)}, {"php", "8.4"}, {"web", "nginx"}}, refusal);
    REQUIRE_MESSAGE(r, refusal.message);
    CHECK((*r)["on"] == true);
    CHECK((*r)["php"] == "8.4");
    CHECK((*r)["web"] == "nginx");
    REQUIRE((*r)["folders"].size() == 1);
    CHECK((*r)["folders"][0]["name"] == "blog");
    CHECK((*r)["folders"][0]["kind"] == "php");

    // Projects stay as they were.
    std::string error;
    const auto app = load_app_config(app_config_file(data), error);
    REQUIRE(app);
    CHECK(app->projects.empty());
    CHECK(app->other["shared"]["on"] == true);

    r = service.set_shared({{"php", nullptr}}, refusal);
    REQUIRE(r);
    CHECK_FALSE(r->contains("php"));
    CHECK_FALSE(service.set_shared({{"php", "eight"}}, refusal));
    CHECK(refusal.code == "bad_params");
    CHECK_FALSE(service.set_shared({{"dir", path_to_utf8(tmp.path() / "missing")}}, refusal));
    CHECK_FALSE(service.set_shared({{"colour", "red"}}, refusal));
    CHECK_FALSE(service.set_shared({{"web", "iis"}}, refusal));
}

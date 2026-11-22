// Adminer (D35, an optional tool for PostgreSQL): the reserved name, its site
// plan and nginx server, index.php, servers.json and the one-click tokens.
// index.php itself runs under real nginx and php-cgi in test_web_service.cpp.

#include "floserver/core/adminer.h"
#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/phpmyadmin.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/site_plan.h"

#include "test_util.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>

using namespace floserver::core;
namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
const fs::path kBase = "C:/fs";
#else
const fs::path kBase = "/fs";
#endif

InstalledRuntime runtime(const std::string& name, const std::string& version) {
    InstalledRuntime r;
    r.name = name;
    r.version = version;
    r.dir = kBase / "runtimes" / name / version;
    return r;
}

ConfigResult defaults(const Project&) {
    ConfigResult r;
    r.config = ProjectConfig{};
    return r;
}

}  // namespace

TEST_CASE("adminer is kept for Adminer: new projects can't take the name") {
    CHECK(reserved_project_name("adminer"));
    CHECK_FALSE(reserved_project_name("adminer-2"));
    CHECK(reserved_name_message("adminer") == "adminer.localhost is kept for Adminer; choose another name");
    CHECK(reserved_name_message("phpmyadmin").find("phpMyAdmin") != std::string::npos);

    const test_util::TempDir tmp;
    fs::create_directories(tmp.path() / "adminer");
    AppConfig app;
    std::string error;
    CHECK_FALSE(add_project(app, tmp.path() / "adminer", "adminer", error));
    CHECK(error.find("Adminer") != std::string::npos);
    const auto added = add_project(app, tmp.path() / "adminer", "", error);
    REQUIRE(added);
    CHECK(added->name == "adminer-2");
}

TEST_CASE("Adminer's language, links, servers.json and file") {
    CHECK(adminer_lang("ar") == "ar");
    CHECK(adminer_lang("ku") == "en");  // Adminer has no Kurdish
    CHECK(adminer_lang("en") == "en");
    CHECK(adminer_lang("") == "en");
    CHECK(adminer_url("https://adminer.localhost:8443/", "ab", "ar") ==
          "https://adminer.localhost:8443/?floserver_token=ab&lang=ar");
    CHECK(adminer_servers_json({5433, 5432, 5433, 0}) == "[\"127.0.0.1:5432\",\"127.0.0.1:5433\"]\n");
    CHECK(adminer_servers_json({}) == "[]\n");
    CHECK(adminer_file(kBase / "runtimes" / "adminer" / "5.4.1", "5.4.1") ==
          kBase / "runtimes" / "adminer" / "5.4.1" / "adminer-5.4.1.php");
    const AdminerPaths p = adminer_paths(kBase / "data");
    CHECK(p.index == kBase / "data" / "adminer" / "index.php");
    CHECK(p.servers == kBase / "data" / "adminer" / "servers.json");
}

TEST_CASE("Adminer's index.php: its paths, PostgreSQL only, the token's claim, then Adminer") {
    const AdminerPaths paths = adminer_paths(kBase / "data");
    const auto script = adminer_index_script(paths, kBase / "runtimes" / "adminer" / "5.4.1" / "adminer-5.4.1.php");
    REQUIRE(script);
    const std::string base = path_to_utf8(kBase);
    CHECK(script->starts_with("<?php\n"));
    CHECK(script->find("const FLOSERVER_ADMINER = '" + base + "/runtimes/adminer/5.4.1/adminer-5.4.1.php';") !=
          std::string::npos);
    CHECK(script->find("const FLOSERVER_SERVERS = '" + base + "/data/adminer/servers.json';") != std::string::npos);
    CHECK(script->find("const FLOSERVER_TOKENS = '" + base + "/data/adminer/tokens';") != std::string::npos);
    CHECK(script->find("session_save_path(FLOSERVER_SESSIONS);") != std::string::npos);
    CHECK(script->find("'server', 'sqlite'") != std::string::npos);  // other drivers refused
    CHECK(script->find("'driver' => 'pgsql'") != std::string::npos);
    CHECK(script->find("@rename($file, $claimed)") != std::string::npos);
    CHECK(script->ends_with("require FLOSERVER_ADMINER;\n"));
    // A path PHP can't hold: no script.
    CHECK_FALSE(adminer_index_script(paths, fs::path("bad\npath.php")));
}

TEST_CASE("Adminer tokens: owner-only, holding the login; old ones are removed") {
    const test_util::TempDir tmp;
    const fs::path tokens = tmp.path() / "tokens";
    AdminerLogin login;
    login.user = "shop";
    login.password = "s3cret\"pass";
    login.port = 5432;
    login.database = "shop";
    std::string error;
    const auto now = std::chrono::system_clock::now();
    const auto token = create_adminer_token(tokens, login, now, error);
    REQUIRE_MESSAGE(token, error);
    CHECK(is_signon_token(*token));
    const fs::path file = tokens / (*token + ".json");
    REQUIRE(fs::is_regular_file(file));
#ifndef _WIN32
    CHECK((fs::status(file).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
    const auto doc = nlohmann::json::parse(test_util::read_file(file));
    CHECK(doc.at("user").get<std::string>() == "shop");
    CHECK(doc.at("password").get<std::string>() == "s3cret\"pass");
    CHECK(doc.at("port").get<int>() == 5432);
    CHECK(doc.at("db").get<std::string>() == "shop");
    const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    CHECK(doc.at("expires").get<std::int64_t>() - now_s == kSignonTokenLife.count());

    // The superuser's: no database.
    login.database.reset();
    const auto root = create_adminer_token(tokens, login, now, error);
    REQUIRE(root);
    CHECK_FALSE(nlohmann::json::parse(test_util::read_file(tokens / (*root + ".json"))).contains("db"));

    fs::last_write_time(file, fs::file_time_type::clock::now() - kSignonTokenLife - std::chrono::seconds{5});
    REQUIRE(create_adminer_token(tokens, login, now, error));
    CHECK_FALSE(fs::exists(file));

    login.port = 0;
    CHECK_FALSE(create_adminer_token(tokens, login, now, error));
}

TEST_CASE("Adminer is planned when installed: its own pool on the newest PHP, with PostgreSQL's extensions") {
    AppConfig config;
    config.projects = {{"shop", kBase / "projects" / "shop", true, false}};

    WebPlan plan = plan_sites(config, defaults, {runtime("php", "8.4.27")}, std::nullopt);
    CHECK_FALSE(plan.adminer);

    const std::vector<InstalledRuntime> installed = {runtime("php", "8.3.36"), runtime("php", "8.4.27"),
                                                     runtime("adminer", "5.4.1"), runtime("phpmyadmin", "5.2.3")};
    plan = plan_sites(config, defaults, installed, std::string("8.3"));
    REQUIRE(plan.adminer);
    CHECK(plan.adminer->name == "_adminer");
    CHECK(plan.adminer->state == SiteState::Ready);
    CHECK(plan.adminer->doc_root == kBase / "runtimes" / "adminer" / "5.4.1");
    CHECK(plan.adminer_version == "5.4.1");
    REQUIRE(plan.adminer->pool);
    const PhpPool& pool = plan.pools.at(*plan.adminer->pool);
    CHECK(pool.version == "8.4.27");
    CHECK(pool.key.ends_with("-adminer"));
    CHECK(pool.sites == std::vector<std::string>{"_adminer"});
    CHECK(std::find(pool.settings.extensions.begin(), pool.settings.extensions.end(), "pgsql") !=
          pool.settings.extensions.end());
    CHECK(std::find(pool.settings.extensions.begin(), pool.settings.extensions.end(), "pdo_pgsql") !=
          pool.settings.extensions.end());
    CHECK(std::is_sorted(pool.settings.extensions.begin(), pool.settings.extensions.end()));
    CHECK(pool.settings.project_ini.at("display_errors") == "Off");
    // phpMyAdmin's pool and the project's are other ones.
    REQUIRE(plan.phpmyadmin);
    CHECK(*plan.phpmyadmin->pool != *plan.adminer->pool);
    CHECK(*plan.sites.front().pool != *plan.adminer->pool);

    plan = plan_sites(config, defaults, {runtime("adminer", "5.4.1")}, std::nullopt);
    REQUIRE(plan.adminer);
    CHECK(plan.adminer->state == SiteState::NeedsDownload);
    CHECK(plan.adminer->missing == std::vector<std::string>{"php"});

    config.projects.push_back({"adminer", kBase / "projects" / "adminer", false, false});
    plan = plan_sites(config, defaults, installed, std::nullopt);
    REQUIRE(plan.adminer);
    CHECK(plan.adminer->state == SiteState::Problem);
    CHECK(plan.adminer->problem.find("adminer.localhost") != std::string::npos);

    // Adminer doesn't hold on to a PHP.
    config.projects.pop_back();
    for (const auto& [key, who] : runtime_users(config, defaults, installed, std::string("8.3"))) {
        CHECK(std::find(who.begin(), who.end(), "_adminer") == who.end());
    }
}

TEST_CASE("nginx serves Adminer at adminer.localhost: FloServer's index.php only") {
    NginxSpec spec;
    spec.nginx_dir = kBase / "nginx";
    spec.work_dir = kBase / "web";
    spec.ports = {8080, 8443, false};
    NginxAdminer adm;
    adm.index = kBase / "data" / "adminer" / "index.php";
    adm.php_ports = {9120, 9121};
    adm.php_access_log = kBase / "web" / "logs" / "adminer-idle.log";
    spec.adminer = adm;
    std::string error;
    auto text = make_nginx_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    const std::string base = path_to_utf8(kBase);
    CHECK(text->find("upstream floserver_php_9120 {") != std::string::npos);
    CHECK(text->find("server_name adminer.localhost;") != std::string::npos);
    CHECK(text->find("location ~ ^/(index\\.php)?$ {") != std::string::npos);
    CHECK(text->find("fastcgi_param SCRIPT_FILENAME \"" + base + "/data/adminer/index.php\";") != std::string::npos);
    CHECK(text->find("access_log \"" + base + "/web/logs/adminer-idle.log\" floserver_idle;") != std::string::npos);
    CHECK(text->find("ssl_certificate") == std::string::npos);

    spec.adminer->tls = NginxTls{kBase / "certs" / "adminer.pem", kBase / "certs" / "adminer.key"};
    text = make_nginx_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->find("return 302 https://adminer.localhost:8443$request_uri;") != std::string::npos);
    CHECK(text->find("fastcgi_param HTTPS on;") != std::string::npos);

    spec.adminer->php_ports.clear();
    CHECK_FALSE(make_nginx_config(spec, error));
}

// phpMyAdmin (an optional tool): the reserved name, its site plan and nginx
// server, config.inc.php, the install's stub, and the one-click login tokens.
// The signon script itself runs under real nginx and php-cgi in
// test_web_service.cpp.

#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/phpmyadmin.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/secret_store.h"
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

const std::string kSecret = std::string(32, 'a') + std::string(32, '7');

}  // namespace

TEST_CASE("phpmyadmin is kept for phpMyAdmin: new projects can't take the name") {
    CHECK(reserved_project_name("phpmyadmin"));
    CHECK_FALSE(reserved_project_name("phpmyadmin-2"));
    CHECK_FALSE(reserved_project_name("shop"));

    const test_util::TempDir tmp;
    fs::create_directories(tmp.path() / "phpmyadmin");
    fs::create_directories(tmp.path() / "PhpMyAdmin2");
    AppConfig app;
    std::string error;
    CHECK_FALSE(add_project(app, tmp.path() / "PhpMyAdmin2", "phpmyadmin", error));
    CHECK(error.find("phpMyAdmin") != std::string::npos);
    // A folder called phpmyadmin gets the next free name.
    const auto added = add_project(app, tmp.path() / "phpmyadmin", "", error);
    REQUIRE(added);
    CHECK(added->name == "phpmyadmin-2");
}

TEST_CASE("phpMyAdmin's language, signon session names and links") {
    CHECK(phpmyadmin_lang("en") == "en");
    CHECK(phpmyadmin_lang("ar") == "ar");
    CHECK(phpmyadmin_lang("ar-IQ") == "ar");
    CHECK(phpmyadmin_lang("ku") == "ckb");
    CHECK(phpmyadmin_lang("CKB") == "ckb");
    CHECK(phpmyadmin_lang("fr") == "en");
    CHECK(phpmyadmin_lang("") == "en");
    CHECK(signon_session_name("11.4") == "FloServerSignon_11_4");
    CHECK(signon_url("https://phpmyadmin.localhost:8443/", "ab", "ku") ==
          "https://phpmyadmin.localhost:8443/floserver-signon.php?token=ab&lang=ckb");
    CHECK(is_signon_token(std::string(64, 'a')));
    CHECK_FALSE(is_signon_token(std::string(64, 'A')));
    CHECK_FALSE(is_signon_token(std::string(63, 'a')));
    CHECK_FALSE(is_signon_token("../" + std::string(61, 'a')));
}

TEST_CASE("config.inc.php: the secret, one server per series, signon with a login form to fall back on") {
    PhpMyAdminConfigSpec spec;
    spec.blowfish_hex = kSecret;
    spec.paths = phpmyadmin_paths(kBase / "data");
    spec.servers = {{"11.4", 3307}, {"10.11", 3306}, {"11.4", 3399}, {"bad", 1}, {"11.8", 0}};
    spec.lang = "ku";
    std::string error;
    const auto text = phpmyadmin_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->starts_with("<?php\n"));
    CHECK(text->find("$cfg['blowfish_secret'] = hex2bin('" + kSecret + "');") != std::string::npos);
    const std::string data = path_to_utf8((kBase / "data").generic_string());  // PHP gets forward slashes
    CHECK(text->find("$cfg['TempDir'] = '" + data + "/phpmyadmin/tmp';") != std::string::npos);
    CHECK(text->find("$cfg['SessionSavePath'] = '" + data + "/phpmyadmin/sessions';") != std::string::npos);
    CHECK(text->find("$cfg['DefaultLang'] = 'ckb';") != std::string::npos);
    CHECK(text->find("$cfg['VersionCheck'] = false;") != std::string::npos);
    CHECK(text->find("$cfg['AllowArbitraryServer'] = false;") != std::string::npos);
    // 10.11 first, then 11.4 once; nothing for an unusable entry.
    const auto first = text->find("'MariaDB 10.11'");
    const auto second = text->find("'MariaDB 11.4'");
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    CHECK(first < second);
    CHECK(text->find("'MariaDB 11.4'", second + 1) == std::string::npos);
    CHECK(text->find("'3307'") != std::string::npos);
    CHECK(text->find("3399") == std::string::npos);
    CHECK(text->find("MariaDB bad") == std::string::npos);
    CHECK(text->find("MariaDB 11.8") == std::string::npos);
    CHECK(text->find("$cfg['Servers'][$i]['host'] = '127.0.0.1';") != std::string::npos);
    CHECK(text->find("isset($_COOKIE['FloServerSignon_11_4']) ? 'signon' : 'cookie'") != std::string::npos);
    CHECK(text->find("$cfg['Servers'][$i]['SignonSession'] = 'FloServerSignon_10_11';") != std::string::npos);
    CHECK(text->find("$cfg['Servers'][$i]['SignonURL'] = './floserver-signon.php';") != std::string::npos);
    CHECK(phpmyadmin_config_lang(*text) == "ckb");
    CHECK(phpmyadmin_servers(spec.servers).size() == 2);

    spec.blowfish_hex = "short";
    CHECK_FALSE(phpmyadmin_config(spec, error));

    // A path with a quote is escaped for PHP; a control character is refused.
    const auto stub = phpmyadmin_config_stub(kBase / "it's" / "config.inc.php");
    REQUIRE(stub);
    CHECK(stub->find("require '" + path_to_utf8(kBase) + "/it\\'s/config.inc.php';") != std::string::npos);
    CHECK_FALSE(phpmyadmin_config_stub(kBase / "a\nb"));
    const auto script = phpmyadmin_signon_script(spec.paths);
    REQUIRE(script);
    CHECK(script->find("const FLOSERVER_TOKENS = '" + data + "/phpmyadmin/tokens';") != std::string::npos);
}

TEST_CASE("config.inc.php is written when its inputs change, with a secret made once and kept") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    std::vector<PhpMyAdminServer> servers = {{"11.4", 3306}};
    const SecretStore secrets(data);
    PhpMyAdminConfigWriter writer(data, secrets, [&servers] { return servers; });
    const PhpMyAdminPaths paths = phpmyadmin_paths(data);
    std::string error;
    std::vector<PhpMyAdminServer> listed;
    REQUIRE_MESSAGE(writer.update(error, &listed), error);
    REQUIRE(listed.size() == 1);
    CHECK(listed.front().series == "11.4");
    const auto secret = secrets.read("phpmyadmin/blowfish");
    REQUIRE(secret);
    CHECK(secret->size() == 64);
    const std::string first = test_util::read_file(paths.config);
    CHECK(first.find(*secret) != std::string::npos);
    CHECK(first.find("'DefaultLang'] = 'en'") != std::string::npos);
    CHECK(fs::is_directory(paths.tokens));
    CHECK(fs::is_directory(paths.sessions));
    CHECK(fs::is_directory(paths.temp));
#ifndef _WIN32
    CHECK((fs::status(paths.config).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    CHECK((fs::status(paths.tokens).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif

    // Nothing changed: the file isn't written again.
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours{1};
    fs::last_write_time(paths.config, old);
    REQUIRE(writer.update(error));
    CHECK(fs::last_write_time(paths.config) == old);

    // A new server and a new language are.
    servers.push_back({"10.11", 3307});
    writer.set_lang("ar");
    REQUIRE(writer.update(error, &listed));
    CHECK(listed.size() == 2);
    CHECK(listed.front().series == "10.11");
    const std::string second = test_util::read_file(paths.config);
    CHECK(second.find("'DefaultLang'] = 'ar'") != std::string::npos);
    CHECK(second.find("MariaDB 10.11") != std::string::npos);

    // After a restart: the same secret, the language the file has.
    PhpMyAdminConfigWriter again(data, secrets, [&servers] { return servers; });
    REQUIRE(again.update(error));
    CHECK(test_util::read_file(paths.config) == second);
    secrets.remove("phpmyadmin/blowfish");  // Windows keeps it in Credential Manager
}

TEST_CASE("signon tokens: random, owner-only, holding the login; old ones are removed") {
    const test_util::TempDir tmp;
    const fs::path tokens = tmp.path() / "tokens";
    SignonLogin login;
    login.server = 2;
    login.series = "11.4";
    login.user = "shop";
    login.password = "s3cret\"pass";
    login.port = 3307;
    login.only_db = "shop";
    std::string error;
    const auto now = std::chrono::system_clock::now();
    const auto token = create_signon_token(tokens, login, now, error);
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
    CHECK(doc.at("host").get<std::string>() == "127.0.0.1");
    CHECK(doc.at("port").get<int>() == 3307);
    CHECK(doc.at("server").get<int>() == 2);
    CHECK(doc.at("session").get<std::string>() == "FloServerSignon_11_4");
    CHECK(doc.at("only_db").get<std::string>() == "shop");
    const auto expires = doc.at("expires").get<std::int64_t>();
    const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    CHECK(expires - now_s == kSignonTokenLife.count());

    // Each token is new.
    const auto other = create_signon_token(tokens, login, now, error);
    REQUIRE(other);
    CHECK(*other != *token);

    // Older than its life: gone when the next one is made.
    fs::last_write_time(file, fs::file_time_type::clock::now() - kSignonTokenLife - std::chrono::seconds{5});
    test_util::write_file(tokens / (std::string(64, 'b') + ".json.claimed-1"), "{}");
    fs::last_write_time(tokens / (std::string(64, 'b') + ".json.claimed-1"),
                        fs::file_time_type::clock::now() - std::chrono::minutes{5});
    REQUIRE(create_signon_token(tokens, login, now, error));
    CHECK_FALSE(fs::exists(file));
    CHECK_FALSE(fs::exists(tokens / (std::string(64, 'b') + ".json.claimed-1")));
    CHECK(fs::exists(tokens / (*other + ".json")));

    // An incomplete login makes no token.
    login.user.clear();
    CHECK_FALSE(create_signon_token(tokens, login, now, error));
}

TEST_CASE("phpMyAdmin is planned when installed: its own pool on the newest PHP, quiet from PHP 8.4") {
    AppConfig config;
    config.projects = {{"shop", kBase / "projects" / "shop", true, false}};

    // Not installed: not planned.
    WebPlan plan = plan_sites(config, defaults, {runtime("php", "8.4.27")}, std::nullopt);
    CHECK_FALSE(plan.phpmyadmin);

    const std::vector<InstalledRuntime> installed = {runtime("php", "8.3.36"), runtime("php", "8.4.27"),
                                                     runtime("phpmyadmin", "5.2.2"), runtime("phpmyadmin", "5.2.3")};
    plan = plan_sites(config, defaults, installed, std::string("8.3"));
    REQUIRE(plan.phpmyadmin);
    CHECK(plan.phpmyadmin->name == "_phpmyadmin");
    CHECK(plan.phpmyadmin->state == SiteState::Ready);
    CHECK(plan.phpmyadmin->doc_root == kBase / "runtimes" / "phpmyadmin" / "5.2.3");
    CHECK(plan.phpmyadmin_version == "5.2.3");
    REQUIRE(plan.phpmyadmin->pool);
    const PhpPool& pool = plan.pools.at(*plan.phpmyadmin->pool);
    CHECK(pool.version == "8.4.27");  // the newest, whatever projects default to
    CHECK(pool.key.ends_with("-phpmyadmin"));
    CHECK(pool.sites == std::vector<std::string>{"_phpmyadmin"});
    CHECK(pool.settings.project_ini.at("display_errors") == "Off");
    CHECK(pool.settings.project_ini.at("error_reporting") == "E_ALL & ~E_DEPRECATED");
    // The project's pool is another one.
    REQUIRE(plan.sites.front().pool);
    CHECK(*plan.sites.front().pool != *plan.phpmyadmin->pool);

    // On PHP 8.3 its notices are reported (to the log), its own pool all the same.
    plan = plan_sites(config, defaults, {runtime("php", "8.3.36"), runtime("phpmyadmin", "5.2.3")}, std::nullopt);
    REQUIRE(plan.phpmyadmin);
    REQUIRE(plan.phpmyadmin->pool);
    const PhpPool& older = plan.pools.at(*plan.phpmyadmin->pool);
    CHECK(older.settings.project_ini.at("display_errors") == "Off");
    CHECK_FALSE(older.settings.project_ini.contains("error_reporting"));
    CHECK(plan.pools.size() == 2);

    // A project with phpMyAdmin's very php.ini values still gets another pool.
    const ConfigReader same = [](const Project&) {
        return parse_project_config(
            R"({"php": "8.4", "php_ini": {"display_errors": "Off", "display_startup_errors": "Off",
                "error_reporting": "E_ALL & ~E_DEPRECATED"}})");
    };
    plan = plan_sites(config, same, installed, std::nullopt);
    REQUIRE(plan.phpmyadmin);
    REQUIRE(plan.sites.front().pool);
    REQUIRE(plan.phpmyadmin->pool);
    CHECK(plan.pools.at(*plan.sites.front().pool).version == "8.4.27");
    CHECK(*plan.sites.front().pool != *plan.phpmyadmin->pool);
    CHECK(plan.pools.at(*plan.phpmyadmin->pool).sites == std::vector<std::string>{"_phpmyadmin"});

    // No PHP: waits for one.
    plan = plan_sites(config, defaults, {runtime("phpmyadmin", "5.2.3")}, std::nullopt);
    REQUIRE(plan.phpmyadmin);
    CHECK(plan.phpmyadmin->state == SiteState::NeedsDownload);
    CHECK(plan.phpmyadmin->missing == std::vector<std::string>{"php"});

    // A project that already has the name keeps its address.
    config.projects.push_back({"phpmyadmin", kBase / "projects" / "phpmyadmin", false, false});
    plan = plan_sites(config, defaults, installed, std::nullopt);
    REQUIRE(plan.phpmyadmin);
    CHECK(plan.phpmyadmin->state == SiteState::Problem);
    CHECK(plan.phpmyadmin->problem.find("phpmyadmin.localhost") != std::string::npos);

    // phpMyAdmin doesn't hold on to a PHP: removing it moves phpMyAdmin to the next.
    config.projects.pop_back();
    const auto users = runtime_users(config, defaults, installed, std::string("8.3"));
    for (const auto& [key, who] : users) {
        CHECK(std::find(who.begin(), who.end(), "_phpmyadmin") == who.end());
    }
}

TEST_CASE("nginx serves phpMyAdmin at phpmyadmin.localhost, its signon script from outside the install") {
    NginxSpec spec;
    spec.nginx_dir = kBase / "nginx";
    spec.work_dir = kBase / "web";
    spec.ports = {8080, 8443, false};
    NginxPhpMyAdmin pma;
    pma.dir = kBase / "runtimes" / "phpmyadmin" / "5.2.3";
    pma.signon_script = kBase / "data" / "phpmyadmin" / "floserver-signon.php";
    pma.php_ports = {9104, 9105};
    pma.php_access_log = kBase / "web" / "logs" / "pma-idle.log";
    spec.phpmyadmin = pma;
    std::string error;
    auto text = make_nginx_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    const std::string base = path_to_utf8(kBase);
    CHECK(text->find("upstream floserver_php_9104 {") != std::string::npos);
    CHECK(text->find("server_name phpmyadmin.localhost;") != std::string::npos);
    CHECK(text->find("root \"" + base + "/runtimes/phpmyadmin/5.2.3\";") != std::string::npos);
    CHECK(text->find("location = /floserver-signon.php {") != std::string::npos);
    CHECK(text->find("fastcgi_param SCRIPT_FILENAME \"" + base + "/data/phpmyadmin/floserver-signon.php\";") !=
          std::string::npos);
    CHECK(text->find("^/(setup|examples|libraries|templates|vendor|sql|test|tmp|po|locale|scripts)(/|$)") !=
          std::string::npos);
    CHECK(text->find("location ~* ^/config\\. {") != std::string::npos);
    CHECK(text->find("listen 127.0.0.1:8080;") != std::string::npos);
    CHECK(text->find("ssl_certificate") == std::string::npos);
    CHECK(text->find("access_log \"" + base + "/web/logs/pma-idle.log\" floserver_idle;") != std::string::npos);

    // With a certificate: https, and http redirects there.
    spec.phpmyadmin->tls = NginxTls{kBase / "certs" / "pma.pem", kBase / "certs" / "pma.key"};
    text = make_nginx_config(spec, error);
    REQUIRE_MESSAGE(text, error);
    CHECK(text->find("return 302 https://phpmyadmin.localhost:8443$request_uri;") != std::string::npos);
    CHECK(text->find("listen 127.0.0.1:8443 ssl;") != std::string::npos);
    CHECK(text->find("fastcgi_param HTTPS on;") != std::string::npos);

    // No pool: refused, not written half.
    spec.phpmyadmin->php_ports.clear();
    CHECK_FALSE(make_nginx_config(spec, error));
}

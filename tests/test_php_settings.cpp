// PHP settings (D20): per series in config.json, per project in
// floserver.json, the php.ini they make, the pool keys, the shim, and the
// service's requests.

#include "floserver/core/paths.h"
#include "floserver/core/php_pool.h"
#include "floserver/core/php_settings.h"
#include "floserver/core/project_service.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/shared_sites.h"
#include "floserver/core/shim.h"
#include "floserver/core/site_plan.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>

using namespace floserver::core;
namespace fs = std::filesystem;
using nlohmann::json;
using test_util::TempDir;

namespace {

#ifdef _WIN32
const fs::path kBase = "C:/fs";
#else
const fs::path kBase = "/fs";
#endif

// Where `needle` starts in `text`; npos (the largest) when it isn't there.
std::size_t at(const std::string& text, const std::string& needle) { return text.find(needle); }

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

InstalledRuntime runtime(const std::string& name, const std::string& version) {
    InstalledRuntime r;
    r.name = name;
    r.version = version;
    r.dir = kBase / "runtimes" / name / version;
    return r;
}

Project project(const std::string& name) { return {name, kBase / "projects" / name, true, false}; }

void fake_install(const fs::path& data, const std::string& name, const std::string& version) {
    const fs::path dir = runtimes_dir(data) / name / version;
    test_util::write_file(dir / std::string(kRuntimeMarker),
                          json{{"schema", 1}, {"name", name}, {"version", version},
                               {"platform", std::string(current_platform())}, {"variant", name == "php" ? "nts" : ""},
                               {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
#ifdef _WIN32
    test_util::write_file(dir / "php.exe", "");
#else
    test_util::write_file(dir / "bin" / "php", "");
#endif
}

AppConfig with_php(const json& php) {
    AppConfig app;
    app.other["php"] = php;
    return app;
}

}  // namespace

TEST_CASE("php.ini: FloServer's defaults, the series, its custom.ini, the project, in that order") {
    PhpIniSettings s;
    s.series = "8.5";
    s.series_ini = {{"memory_limit", "1G"}, {"date.timezone", "Asia/Baghdad"}};
    s.custom_ini = "; mine\nmax_input_vars = 5000";
    s.project_ini = {{"memory_limit", "2G"}};
    const PhpIniResult made = make_php_ini("/nowhere/php", s, "/logs/x.log");
    const std::string& t = made.text;
    INFO(t);
    // php.ini is read top to bottom and a later value wins.
    CHECK(at(t, "memory_limit = 512M") < at(t, "memory_limit = 1G"));
    CHECK(at(t, "memory_limit = 1G") < at(t, "max_input_vars = 5000"));
    CHECK(at(t, "max_input_vars = 5000") < at(t, "memory_limit = 2G"));
    CHECK(at(t, "date.timezone = Asia/Baghdad") != std::string::npos);
    CHECK(at(t, "; PHP 8.5 (Settings -> PHP)") != std::string::npos);
    CHECK(at(t, "max_input_vars = 5000\n") != std::string::npos);  // a line end added after custom.ini
    CHECK(at(t, "opcache.enable = 1") != std::string::npos);

    // Keys FloServer manages are left out, as are values on more than one line,
    // even when they got into the settings by hand.
    PhpIniSettings bad;
    bad.series_ini = {{"extension", "evil"}, {"error_log", "/tmp/x"}, {"EXTENSION_DIR", "/x"}, {"zend_extension", "x"},
                      {"a", "1\nextension=evil"}};
    bad.project_ini = {{"error_log", "/tmp/y"}};
    const std::string t2 = make_php_ini("/nowhere/php", bad, "/logs/x.log").text;
    CHECK(at(t2, "evil") == std::string::npos);
    CHECK(at(t2, "/tmp/") == std::string::npos);
    CHECK(at(t2, "EXTENSION_DIR") == std::string::npos);
    CHECK(at(t2, "zend_extension = x") == std::string::npos);
}

TEST_CASE("ini keys and values are checked") {
    CHECK_FALSE(php_ini_key_problem("memory_limit"));
    CHECK_FALSE(php_ini_key_problem("opcache.jit_buffer_size"));
    CHECK(php_ini_key_problem("opcache.enable"));
    CHECK(php_ini_key_problem("opcache.validate_timestamps"));
    // Values PHP can't parse would drop every line after them.
    CHECK_FALSE(php_ini_value_problem("512M"));
    CHECK_FALSE(php_ini_value_problem("E_ALL & ~E_DEPRECATED"));
    CHECK_FALSE(php_ini_value_problem(R"(C:\tmp\uploads)"));
    CHECK_FALSE(php_ini_value_problem(R"("/usr/sbin/sendmail -t -i -f a=b")"));
    CHECK(php_ini_value_problem("/usr/sbin/sendmail -t -i -f a=b"));
    CHECK(php_ini_value_problem(R"("abc)"));
    CHECK(php_ini_value_problem(R"("a"b")"));
    CHECK(php_ini_value_problem(R"("C:\tmp\")"));
    CHECK(php_ini_value_problem("${HOME}"));
    CHECK(php_ini_value_problem(R"("${HOME}")"));
    CHECK(php_ini_value_problem("on ; comment"));
    CHECK_FALSE(php_ini_key_problem("xdebug.mode"));
    CHECK(php_ini_key_problem("extension"));
    CHECK(php_ini_key_problem("Zend_Extension"));
    CHECK(php_ini_key_problem("extension_dir"));
    CHECK(php_ini_key_problem("error_log"));
    CHECK(php_ini_key_problem(""));
    CHECK(php_ini_key_problem("a b"));
    CHECK(php_ini_key_problem("a=b"));
    CHECK(php_ini_key_problem("[PHP]"));
    CHECK_FALSE(php_ini_value_problem("E_ALL & ~E_DEPRECATED"));
    CHECK_FALSE(php_ini_value_problem(""));
    CHECK(php_ini_value_problem("1G\nextension=x"));
    CHECK(php_ini_value_problem("1G\r"));
    CHECK(php_ini_value_problem(std::string(1001, 'a')));
    CHECK(php_series_of("8.5.12") == "8.5");
    CHECK(php_series_of("08.5") == "8.5");
    CHECK_FALSE(php_series_of("8"));
}

TEST_CASE("the extensions in an ext folder") {
    const TempDir tmp;
    for (const char* f : {"php_gd.dll", "php_XDEBUG.dll", "php_pdo_pgsql.dll", "readme.txt", "php_.dll", "php_a-b.dll"}) {
        test_util::write_file(tmp.path() / f, "");
    }
    CHECK(php_extensions_in(tmp.path()) == std::vector<std::string>{"gd", "pdo_pgsql", "xdebug"});
    CHECK(php_extensions_in(tmp.path() / "missing").empty());
}

TEST_CASE("config.json's php: read leniently, written keeping what this version doesn't know") {
    AppConfig app = with_php({{"8.5",
                               {{"extensions", {{"bcmath", true}, {"exif", false}, {"opcache", false}, {"Bad", true}}},
                                {"ini", {{"memory_limit", "1G"}, {"error_log", "/x"}, {"max_input_vars", 5000}}},
                                {"future", 1}}},
                              {"8", {{"ini", {{"memory_limit", "1G"}}}}},
                              {"8.4.1", json::object()},
                              {"8.3", "nope"}});
    app.other["defaults"] = {{"php", "8.5"}};
    std::vector<std::string> problems;
    const PhpSettings s = read_php_settings(app, &problems);
    REQUIRE(s.size() == 1);
    const PhpSeriesSettings& s85 = s.at("8.5");
    CHECK(s85.extensions == std::map<std::string, bool>{{"bcmath", true}, {"exif", false}});  // opcache isn't offered
    CHECK(s85.ini == std::map<std::string, std::string>{{"memory_limit", "1G"}});
    CHECK(problems.size() == 6);  // Bad, error_log, the number, "8", "8.4.1", "8.3"

    PhpSeriesSettings changed = s85;
    changed.ini["memory_limit"] = "2G";
    write_php_series(app, "8.5", changed);
    CHECK(app.other["php"]["8.5"]["future"] == 1);
    CHECK(app.other["php"]["8.5"]["ini"] == json{{"memory_limit", "2G"}});
    CHECK(app.other["php"].contains("8"));  // other entries stay as they were
    CHECK(app.other["defaults"]["php"] == "8.5");

    write_php_series(app, "8.4", PhpSeriesSettings{});
    CHECK_FALSE(app.other["php"].contains("8.4"));

    // "08.5" written by hand is the same series: it would hide "8.5" from
    // the reader (it sorts first), so writing "8.5" removes it.
    AppConfig odd;
    odd.other["php"] = {{"08.5", {{"ini", {{"memory_limit", "1G"}}}}}};
    write_php_series(odd, "8.5", changed);
    CHECK_FALSE(odd.other["php"].contains("08.5"));
    CHECK(read_php_settings(odd).at("8.5") == changed);
    AppConfig empty;
    write_php_series(empty, "8.5", changed);
    write_php_series(empty, "8.5", PhpSeriesSettings{});
    CHECK_FALSE(empty.other.contains("php"));  // nothing left: the key goes

    // The file round trip.
    const TempDir tmp;
    std::string error;
    REQUIRE(save_app_config(app_config_file(tmp.path()), app, error));
    const auto loaded = load_app_config(app_config_file(tmp.path()), error);
    REQUIRE(loaded);
    CHECK(read_php_settings(*loaded).at("8.5") == changed);
}

TEST_CASE("floserver.json: extensions with - turn one off, php_ini sets values") {
    auto r = parse_project_config(
        R"({"php": "8.5", "extensions": ["bcmath", "-exif", "opcache"], "php_ini": {"memory_limit": "1024M", "max_input_vars": 5000}})");
    REQUIRE(r.config);
    CHECK(r.config->extensions == std::vector<std::string>{"bcmath"});
    CHECK(r.config->extensions_off == std::vector<std::string>{"exif"});
    CHECK(r.config->php_ini ==
          std::map<std::string, std::string>{{"memory_limit", "1024M"}, {"max_input_vars", "5000"}});
    CHECK(r.warnings.size() == 1);  // opcache: always on
    // Written back the same way.
    const auto again = parse_project_config(write_project_config(*r.config));
    REQUIRE(again.config);
    CHECK(*again.config == *r.config);

    const auto error_at = [](std::string_view text, const std::string& key) {
        const auto res = parse_project_config(text);
        if (res.config) {
            return false;
        }
        return std::any_of(res.errors.begin(), res.errors.end(), [&key](const ConfigProblem& p) { return p.key == key; });
    };
    CHECK(error_at(R"({"extensions": ["gd", "-gd"]})", "extensions[1]"));
    CHECK(error_at(R"({"extensions": ["--gd"]})", "extensions[0]"));
    CHECK(error_at(R"({"php_ini": {"error_log": "/tmp/x"}})", "php_ini.error_log"));
    CHECK(error_at(R"({"php_ini": {"extension": "evil"}})", "php_ini.extension"));
    CHECK(error_at(R"({"php_ini": {"memory_limit": "1G\nextension=evil"}})", "php_ini.memory_limit"));
    CHECK(error_at(R"({"php_ini": {"display_errors": true}})", "php_ini.display_errors"));
    CHECK(error_at(R"({"php_ini": ["memory_limit"]})", "php_ini"));
    const auto plain = parse_project_config(R"({"php_ini": {"extension": "evil"}})");
    CHECK(plain.errors.front().message.find("extensions list") != std::string::npos);
}

TEST_CASE("a project's PHP changes are written into floserver.json, keeping its other keys") {
    std::string error;
    const std::string text = R"({"root": "public", "php": "8.5", "extensions": ["soap", "-exif"], "x-team": 1})";
    auto out = set_project_php_settings(text, {{"bcmath", "on"}, {"exif", std::nullopt}, {"gd", "off"}},
                                        {{"memory_limit", "1024M"}}, error);
    REQUIRE_MESSAGE(out, error);
    const json doc = json::parse(*out);
    CHECK(doc["extensions"] == json{"soap", "bcmath", "-gd"});
    CHECK(doc["php_ini"] == json{{"memory_limit", "1024M"}});
    CHECK(doc["x-team"] == 1);
    CHECK(out->find(R"("root": "public")") < out->find(R"("php": "8.5")"));  // order kept

    // Back to the series' settings: the keys go when they're empty.
    out = set_project_php_settings(*out, {{"soap", std::nullopt}, {"bcmath", std::nullopt}, {"gd", std::nullopt}},
                                   {{"memory_limit", std::nullopt}}, error);
    REQUIRE(out);
    CHECK_FALSE(json::parse(*out).contains("extensions"));
    CHECK_FALSE(json::parse(*out).contains("php_ini"));

    CHECK_FALSE(set_project_php_settings("{}", {}, {{"error_log", "x"}}, error));
    CHECK_FALSE(set_project_php_settings("{}", {}, {{"memory_limit", "1\n2"}}, error));
    CHECK_FALSE(set_project_php_settings("{}", {{"opcache", "off"}}, {}, error));
    CHECK_FALSE(set_project_php_settings("{}", {{"gd", "maybe"}}, {}, error));
    CHECK_FALSE(set_project_php_settings("{", {{"gd", "on"}}, {}, error));
    CHECK_FALSE(set_project_php_settings(R"({"extensions": "gd"})", {{"gd", "on"}}, {}, error));
    CHECK(set_project_php_settings("", {{"bcmath", "on"}}, {}, error));  // no file yet
}

TEST_CASE("effective settings: base, then the series, then the project; the key follows them") {
    PhpSeriesSettings series;
    series.extensions = {{"bcmath", true}, {"exif", true}, {"zip", false}};
    series.ini = {{"memory_limit", "1G"}};
    ProjectConfig pc;
    pc.extensions = {"soap", "zip"};
    pc.extensions_off = {"exif", "gd", "opcache"};
    pc.php_ini = {{"memory_limit", "2G"}};
    const PhpIniSettings s = effective_php_settings("8.5", &series, "x = 1\n", &pc);
    CHECK(s.extensions == std::vector<std::string>{"bcmath", "soap"});
    CHECK(s.removed == std::vector<std::string>{"gd"});
    CHECK(s.series_ini.at("memory_limit") == "1G");
    CHECK(s.project_ini.at("memory_limit") == "2G");

    // Nothing beyond extra extensions: the key looks as it did before D20.
    CHECK(php_settings_key(effective_php_settings("8.5", nullptr, "", nullptr)).empty());
    ProjectConfig only_ext;
    only_ext.extensions = {"soap", "bcmath", "gd"};
    CHECK(php_settings_key(effective_php_settings("8.5", nullptr, "", &only_ext)) == "+bcmath+soap");

    // Anything else adds a short hash: same settings, same key; any change, another.
    const std::string key = php_settings_key(s);
    CHECK(key.starts_with("+bcmath+soap-"));
    CHECK(key.size() == std::string("+bcmath+soap-").size() + 8);
    CHECK(php_settings_key(effective_php_settings("8.5", &series, "x = 1\n", &pc)) == key);
    CHECK(php_settings_key(effective_php_settings("8.5", &series, "x = 2\n", &pc)) != key);
    pc.php_ini["memory_limit"] = "3G";
    CHECK(php_settings_key(effective_php_settings("8.5", &series, "x = 1\n", &pc)) != key);
}

TEST_CASE("pools: projects with different php.ini values don't share; series settings and custom.ini change keys") {
    const std::vector<InstalledRuntime> installed = {runtime("php", "8.5.12"), runtime("php", "8.4.27")};
    AppConfig app;
    app.projects = {project("shop"), project("blog"), project("api"), project("old")};
    const auto files = reader({{"shop", R"({"php": "8.5", "php_ini": {"memory_limit": "1G"}})"},
                               {"blog", R"({"php": "8.5"})"},
                               {"api", R"({"php": "8.5", "php_ini": {"memory_limit": "1G"}})"},
                               {"old", R"({"php": "8.4"})"}});
    std::vector<std::string> asked;
    const CustomIniReader custom = [&asked](const std::string& series) {
        asked.push_back(series);
        return series == "8.5" ? std::string("max_input_vars = 5000\n") : std::string();
    };

    WebPlan plan = plan_sites(app, files, installed, std::nullopt);
    REQUIRE(plan.pools.size() == 3);
    CHECK(plan.sites[0].pool == plan.sites[2].pool);  // the same values: shared
    CHECK(plan.sites[0].pool != plan.sites[1].pool);
    CHECK(plan.pools.at(*plan.sites[1].pool).key == "php-8.5.12");
    CHECK(plan.pools.at(*plan.sites[3].pool).key == "php-8.4.27");
    const std::string shop_key = plan.pools.at(*plan.sites[0].pool).key;
    CHECK(shop_key.starts_with("php-8.5.12-"));
    CHECK(plan.pools.at(*plan.sites[0].pool).settings.project_ini.at("memory_limit") == "1G");

    // custom.ini: read once per series in use, and part of the key.
    plan = plan_sites(app, files, installed, std::nullopt, custom);
    CHECK(asked == std::vector<std::string>{"8.5", "8.4"});
    CHECK(plan.pools.at(*plan.sites[1].pool).key.starts_with("php-8.5.12-"));
    CHECK(plan.pools.at(*plan.sites[0].pool).key != shop_key);
    CHECK(plan.pools.at(*plan.sites[3].pool).key == "php-8.4.27");  // 8.4 has no custom.ini

    // Series settings apply to every project on the series, and only to it.
    app.other["php"] = {{"8.5", {{"ini", {{"memory_limit", "768M"}}}, {"extensions", {{"bcmath", true}}}}}};
    plan = plan_sites(app, files, installed, std::nullopt);
    const PhpPool& blog = plan.pools.at(*plan.sites[1].pool);
    CHECK(blog.key.starts_with("php-8.5.12+bcmath-"));
    CHECK(blog.settings.series_ini.at("memory_limit") == "768M");
    CHECK(plan.pools.at(*plan.sites[3].pool).key == "php-8.4.27");
}

TEST_CASE("shared sites get the series' settings and share a pool with a project without its own") {
    const std::vector<InstalledRuntime> installed = {runtime("php", "8.5.12")};
    AppConfig app;
    app.other["shared"] = {{"on", true}, {"dir", path_to_utf8(kBase / "www")}, {"web", "nginx"}};
    app.other["php"] = {{"8.5", {{"ini", {{"memory_limit", "768M"}}}}}};
    app.projects = {project("blog"), project("shop")};
    const auto files = reader({{"blog", R"({"php": "8.5"})"}, {"shop", R"({"php": "8.5", "php_ini": {"memory_limit": "1G"}})"}});
    const WebPlan plan = plan_sites(app, files, installed, std::nullopt,
                                    [](const std::string&) { return std::string("x = 1\n"); });
    REQUIRE(plan.shared);
    REQUIRE(plan.shared->pool);
    const PhpPool& pool = plan.pools.at(*plan.shared->pool);
    CHECK(pool.settings.series_ini.at("memory_limit") == "768M");
    CHECK(pool.settings.custom_ini == "x = 1\n");
    CHECK(pool.settings.project_ini.empty());
    CHECK(pool.sites == std::vector<std::string>{"blog", std::string(kSharedSiteName)});
    CHECK(plan.sites[1].pool != plan.shared->pool);
}

TEST_CASE("the php shim gets the series' settings and the project's own") {
    const TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_install(data, "php", "8.5.12");
    const AppConfig app = with_php({{"8.5", {{"ini", {{"memory_limit", "768M"}, {"date.timezone", "UTC"}}}}}});
    std::string error;
    REQUIRE(save_app_config(app_config_file(data), app, error));
    test_util::write_file(php_custom_ini_file(data, "8.5"), "max_input_vars = 5000\n");
    const fs::path shop = tmp.path() / "shop";
    test_util::write_file(shop / "floserver.json", R"({"php": "8.5", "php_ini": {"memory_limit": "1G"}})");

    const auto php = resolve_shim("php", shop, data, error);
    REQUIRE_MESSAGE(php, error);
    REQUIRE(php->args.size() == 2);
    const fs::path ini = path_from_utf8(php->args[1]);
    CHECK(path_to_utf8(ini.filename()).starts_with("8.5.12-nts-"));
    const std::string text = test_util::read_file(ini);
    INFO(text);
    CHECK(at(text, "memory_limit = 768M") < at(text, "max_input_vars = 5000"));
    CHECK(at(text, "max_input_vars = 5000") < at(text, "memory_limit = 1G"));
    CHECK(at(text, "date.timezone = UTC") != std::string::npos);
}

TEST_CASE("the service's PHP requests: per series and per project") {
    const TempDir data;
    fake_install(data.path(), "php", "8.5.12");
    fake_install(data.path(), "php", "8.4.27");
#ifdef _WIN32
    for (const char* f : {"php_gd.dll", "php_intl.dll", "php_bcmath.dll", "php_xdebug.dll", "php_opcache.dll"}) {
        test_util::write_file(runtimes_dir(data.path()) / "php" / "8.5.12" / "ext" / f, "");
    }
#endif
    const fs::path shop = data.path() / "work" / "shop";
    test_util::write_file(shop / "floserver.json", R"({"php": "8.5", "root": "public"})");
    test_util::write_file(shop / "public" / "index.php", "");
    ProjectService service(data.path());
    ServiceRefusal refusal;
    REQUIRE(service.add(shop, "", refusal));
    std::string error;
    auto config = load_app_config(app_config_file(data.path()), error);
    REQUIRE(config);
    config->other["future"] = {{"kept", true}};
    REQUIRE(save_app_config(app_config_file(data.path()), *config, error));

    auto got = service.php_get("8.5", refusal);
    REQUIRE(got);
    CHECK((*got)["series"] == "8.5");
    CHECK((*got)["version"] == "8.5.12");
    CHECK((*got)["used_by"] == json{"shop"});
    CHECK((*got)["ini"]["memory_limit"] == "512M");  // FloServer's default
    CHECK((*got)["changed"].empty());
    CHECK((*got)["custom_ini"] == path_to_utf8(php_custom_ini_file(data.path(), "8.5")));
    CHECK((*got)["custom_ini_exists"] == false);  // made only when asked for
    const json& first = (*got)["extensions"][0];
    CHECK(first["name"] == "curl");
    CHECK(first["base"] == true);
    CHECK(first["on"] == true);
#ifdef _WIN32
    CHECK((*got)["extensions_fixed"] == false);
    // Base ones the install lacks are marked; its other ones follow, opcache never.
    CHECK(first["missing"] == true);
    std::vector<std::string> names;
    for (const auto& e : (*got)["extensions"]) {
        names.push_back(e["name"].get<std::string>());
    }
    CHECK(std::find(names.begin(), names.end(), "xdebug") != names.end());
    CHECK(std::find(names.begin(), names.end(), "opcache") == names.end());
#else
    CHECK((*got)["extensions_fixed"] == true);
    CHECK((*got)["extensions"].size() == base_php_extensions().size());
#endif

    // Set: true/false/null, values or null; only changes from the base are kept.
    auto set = service.php_set("8.5", {{"bcmath", true}, {"exif", false}, {"gd", false}, {"curl", true}},
                               {{"memory_limit", "1G"}, {"date.timezone", "Asia/Baghdad"}}, refusal);
    REQUIRE(set);
    CHECK((*set)["changed"] == json{{"date.timezone", "Asia/Baghdad"}, {"memory_limit", "1G"}});
    CHECK((*set)["ini"]["memory_limit"] == "1G");
    config = load_app_config(app_config_file(data.path()), error);
    REQUIRE(config);
    CHECK(config->other["php"]["8.5"]["extensions"] == json{{"bcmath", true}, {"gd", false}});
    CHECK(config->other["future"]["kept"] == true);
    set = service.php_set("8.5", {{"gd", nullptr}}, {{"memory_limit", nullptr}}, refusal);
    REQUIRE(set);
    CHECK((*set)["changed"] == json{{"date.timezone", "Asia/Baghdad"}});

    CHECK_FALSE(service.php_set("8", json(), json(), refusal));
    CHECK(refusal.code == "bad_params");
    CHECK_FALSE(service.php_set("8.5.12", json(), json(), refusal));
    CHECK_FALSE(service.php_set("8.5", {{"opcache", false}}, json(), refusal));
    CHECK_FALSE(service.php_set("8.5", {{"gd", "on"}}, json(), refusal));
    CHECK_FALSE(service.php_set("8.5", json(), {{"error_log", "/x"}}, refusal));
    CHECK(refusal.message.find("set by FloServer") != std::string::npos);
    CHECK_FALSE(service.php_set("8.5", json(), {{"memory_limit", "1G\nextension=x"}}, refusal));
    CHECK_FALSE(service.php_set("8.5", json(), {{"memory_limit", 1}}, refusal));
    CHECK_FALSE(service.php_get("x", refusal));

    // custom.ini: made with its header, which on its own changes nothing.
    const auto custom = service.php_custom_ini("8.5", refusal);
    REQUIRE(custom);
    CHECK(test_util::read_file(*custom).starts_with("; Your own php.ini settings for PHP 8.5"));
    CHECK(read_php_custom_ini(data.path(), "8.5").empty());
    test_util::write_file(*custom, "; edited\n");  // never overwritten
    REQUIRE(service.php_custom_ini("8.5", refusal));
    CHECK(test_util::read_file(*custom) == "; edited\n");
    CHECK(read_php_custom_ini(data.path(), "8.5") == "; edited\n");
    CHECK((*service.php_get("8.5", refusal))["custom_ini_exists"] == true);

    // The final php.ini, to look at: the series' values, then custom.ini.
    REQUIRE(service.php_set("8.5", json(), {{"memory_limit", "768M"}}, refusal));
    test_util::write_file(*custom, "display_errors = Off\n");
    auto final_ini = service.php_final_ini("8.5", refusal);
    REQUIRE_MESSAGE(final_ini, refusal.message);
    CHECK(final_ini->filename() == "final-php.ini");
    const std::string text = test_util::read_file(*final_ini);
    CHECK(text.starts_with("; What PHP 8.5.12 uses"));
    const auto series_at = text.find("memory_limit = 768M");
    const auto custom_at = text.find("display_errors = Off");
    REQUIRE(series_at != std::string::npos);
    REQUIRE(custom_at != std::string::npos);
    CHECK(series_at < custom_at);
    // Written fresh each time, though it's left read-only.
    REQUIRE(service.php_set("8.5", json(), {{"memory_limit", "640M"}}, refusal));
    final_ini = service.php_final_ini("8.5", refusal);
    REQUIRE_MESSAGE(final_ini, refusal.message);
    CHECK(test_util::read_file(*final_ini).find("memory_limit = 640M") != std::string::npos);
    CHECK_FALSE(service.php_final_ini("8.3", refusal));  // not installed
    CHECK(refusal.code == "not_installed");
    REQUIRE(service.php_set("8.5", json(), {{"memory_limit", nullptr}}, refusal));
    test_util::write_file(*custom, "; edited\n");

    // A project's own changes, in its floserver.json; the reply says them.
    const auto p = service.project_php("shop", {{"bcmath", "off"}, {"soap", "on"}}, {{"memory_limit", "2G"}}, refusal);
    REQUIRE_MESSAGE(p, refusal.message);
    CHECK((*p)["wants"]["extensions"] == json{"soap", "-bcmath"});
    CHECK((*p)["wants"]["php_ini"] == json{{"memory_limit", "2G"}});
    const json file = json::parse(test_util::read_file(shop / "floserver.json"));
    CHECK(file["root"] == "public");
    CHECK(file["php_ini"]["memory_limit"] == "2G");
    CHECK_FALSE(service.project_php("shop", json(), {{"extension", "x"}}, refusal));
    CHECK(refusal.code == "config_refused");
    CHECK_FALSE(service.project_php("shop", {{"gd", true}}, json(), refusal));
    CHECK(refusal.code == "bad_params");
    CHECK_FALSE(service.project_php("nope", json(), json(), refusal));
    CHECK(refusal.code == "unknown_project");
}

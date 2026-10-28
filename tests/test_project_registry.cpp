#include "floserver/core/project_registry.h"

#include "floserver/core/paths.h"
#include "test_util.h"

#include <doctest/doctest.h>

using namespace floserver::core;
using test_util::TempDir;
namespace fs = std::filesystem;

TEST_CASE("project names are DNS labels for <name>.localhost") {
    CHECK_FALSE(project_name_problem("my-shop"));
    CHECK_FALSE(project_name_problem("a"));
    CHECK_FALSE(project_name_problem("shop2"));
    CHECK_FALSE(project_name_problem(std::string(63, 'a')));

    CHECK(project_name_problem(""));
    CHECK(project_name_problem(std::string(64, 'a')));
    CHECK(project_name_problem("My-Shop"));
    CHECK(project_name_problem("my_shop"));
    CHECK(project_name_problem("my.shop"));
    CHECK(project_name_problem("-shop"));
    CHECK(project_name_problem("shop-"));
    CHECK(project_name_problem("xn--shop"));
    CHECK(project_name_problem("shop\xD9\x85"));  // non-ASCII
}

TEST_CASE("a name is made from the folder name") {
    CHECK(project_name_from("my-shop") == "my-shop");
    CHECK(project_name_from("My Shop_2") == "my-shop-2");
    CHECK(project_name_from("  --api..demo--  ") == "api-demo");
    CHECK(project_name_from("") == "project");
    CHECK(project_name_from("\xD9\x85\xD8\xAA\xD8\xAC\xD8\xB1") == "project");  // Arabic only
    CHECK(project_name_from("shop \xD9\x85\xD8\xAA\xD8\xAC\xD8\xB1 2") == "shop-2");
    CHECK(project_name_from("xn--abc") == "xn-abc");  // dashes collapse, so never "xn--"
    CHECK(project_name_from(std::string(80, 'b')).size() == 63);
}

TEST_CASE("projects are added, found, named uniquely and removed") {
    const TempDir tmp;
    fs::create_directories(tmp.path() / "My Shop");
    fs::create_directories(tmp.path() / "other" / "My Shop");
    fs::create_directories(tmp.path() / "api");

    AppConfig config;
    std::string error;
    const auto shop = add_project(config, tmp.path() / "My Shop", "", error);
    REQUIRE(shop);
    CHECK(shop->name == "my-shop");
    CHECK_FALSE(shop->on);

    SUBCASE("the same folder twice is refused, also with a trailing separator") {
        CHECK_FALSE(add_project(config, tmp.path() / "My Shop", "", error));
        CHECK(error.find("already the project my-shop") != std::string::npos);
        CHECK_FALSE(add_project(config, tmp.path() / "My Shop" / "", "", error));
    }
    SUBCASE("a second folder with the same name gets -2") {
        const auto second = add_project(config, tmp.path() / "other" / "My Shop" / "", "", error);
        REQUIRE(second);
        CHECK(second->name == "my-shop-2");
        CHECK(second->dir.filename() == "My Shop");
    }
    SUBCASE("a chosen name is checked") {
        CHECK_FALSE(add_project(config, tmp.path() / "api", "my-shop", error));
        CHECK(error.find("already a project") != std::string::npos);
        CHECK_FALSE(add_project(config, tmp.path() / "api", "API", error));
        const auto api = add_project(config, tmp.path() / "api", "backend", error);
        REQUIRE(api);
        CHECK(api->name == "backend");
    }
    SUBCASE("missing and relative folders are refused") {
        CHECK_FALSE(add_project(config, tmp.path() / "missing", "", error));
        CHECK_FALSE(add_project(config, "api", "", error));
    }
    SUBCASE("find, folder lookup and remove") {
        CHECK(find_project(config, "my-shop") != nullptr);
        CHECK(find_project(config, "nope") == nullptr);
        const Project* p = project_for_folder(config, tmp.path() / "My Shop" / ".");
        REQUIRE(p != nullptr);
        CHECK(p->name == "my-shop");
        CHECK(remove_project(config, "my-shop"));
        CHECK_FALSE(remove_project(config, "my-shop"));
        CHECK(config.projects.empty());
        CHECK(fs::is_directory(tmp.path() / "My Shop"));  // never touched
    }
}

TEST_CASE("config.json round-trips and keeps keys it doesn't use") {
    const TempDir tmp;
    const fs::path file = app_config_file(tmp.path());
    std::string error;

    SUBCASE("a missing file is an empty config") {
        const auto c = load_app_config(file, error);
        REQUIRE(c);
        CHECK(c->projects.empty());
        CHECK_FALSE(c->projects_dir);
    }

    SUBCASE("round trip") {
        fs::create_directories(tmp.path() / "shop \xD9\x85");
        AppConfig config;
        config.projects_dir = tmp.path() / "projects";
        config.other = {{"language", "ku"}, {"window", {{"width", 900}}}};
        REQUIRE(add_project(config, tmp.path() / "shop \xD9\x85", "", error));
        config.projects[0].on = true;
        config.projects[0].keep_running = true;
        REQUIRE(save_app_config(file, config, error));

        const auto back = load_app_config(file, error);
        REQUIRE(back);
        REQUIRE(back->projects.size() == 1);
        CHECK(back->projects[0] == config.projects[0]);
        CHECK(back->projects_dir == config.projects_dir);
        CHECK(back->other["language"] == "ku");
        CHECK(back->other["window"]["width"] == 900);
        CHECK_FALSE(back->other.contains("projects"));
    }

    SUBCASE("a damaged file is reported, not replaced") {
        test_util::write_file(file, "{ not json");
        CHECK_FALSE(load_app_config(file, error));
        CHECK(error.find("not valid JSON") != std::string::npos);
    }
    SUBCASE("bad entries are reported") {
        test_util::write_file(file, R"({"projects": [{"name": "Bad Name", "path": "/x"}]})");
        CHECK_FALSE(load_app_config(file, error));
        test_util::write_file(file, R"({"projects": [{"name": "a", "path": "relative"}]})");
        CHECK_FALSE(load_app_config(file, error));
        test_util::write_file(file, R"({"projects": [{"name": "a", "path": "/x"}, {"name": "a", "path": "/y"}]})");
        CHECK_FALSE(load_app_config(file, error));
        CHECK(error.find("used twice") != std::string::npos);
        test_util::write_file(file, R"({"projects": "none"})");
        CHECK_FALSE(load_app_config(file, error));
    }
}

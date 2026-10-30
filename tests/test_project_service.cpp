#include "floserver/core/project_service.h"

#include "floserver/core/paths.h"
#include "floserver/core/runtime_store.h"
#include "test_util.h"

#include <doctest/doctest.h>

using namespace floserver::core;
namespace fs = std::filesystem;
using test_util::TempDir;

namespace {

// A fake installed runtime: just the folder and its marker.
void fake_install(const fs::path& data, const std::string& name, const std::string& version) {
    const fs::path dir = runtimes_dir(data) / name / version;
    test_util::write_file(dir / std::string(kRuntimeMarker),
                          nlohmann::json{{"schema", 1}, {"name", name}, {"version", version}, {"platform", std::string(current_platform())},
                                         {"variant", name == "php" ? "nts" : ""}, {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
}

}  // namespace

TEST_CASE("the project service adds, describes, switches and removes projects") {
    const TempDir data;
    const fs::path shop = data.path() / "work" / "shop";
    test_util::write_file(shop / "floserver.json", R"({"php": "8.4", "database": "mariadb-11.4", "root": "public"})");
    fake_install(data.path(), "php", "8.4.27");

    ProjectService service(data.path());
    ServiceRefusal refusal;

    const auto added = service.add(shop, "", refusal);
    REQUIRE(added);
    CHECK((*added)["name"] == "shop");
    CHECK((*added)["on"] == false);
    // Off, but it already says what it would need.
    CHECK((*added)["state"] == "off");
    CHECK((*added)["missing"] == nlohmann::json::array({"mariadb 11.4"}));

    fake_install(data.path(), "mariadb", "11.4.13");
    const auto on = service.set("shop", true, true, refusal);
    REQUIRE(on);
    CHECK((*on)["state"] == "ready");
    CHECK((*on)["php"] == "8.4.27");
    CHECK((*on)["keep_running"] == true);
    CHECK((*on)["database"] == "mariadb-11.4");

    const auto list = service.list(refusal);
    REQUIRE(list);
    REQUIRE((*list)["projects"].size() == 1);
    CHECK((*list)["projects"][0]["state"] == "ready");

    CHECK_FALSE(service.set("nope", true, std::nullopt, refusal));
    CHECK(refusal.code == "unknown_project");
    CHECK_FALSE(service.add(shop, "", refusal));
    CHECK(refusal.code == "bad_params");

    // A new service (a restart) reads the same list back.
    const ProjectService again(data.path());
    const auto relisted = again.list(refusal);
    REQUIRE(relisted);
    CHECK((*relisted)["projects"][0]["on"] == true);

    CHECK(service.remove("shop", refusal));
    CHECK_FALSE(service.remove("shop", refusal));
    CHECK(fs::is_regular_file(shop / "floserver.json"));
}

TEST_CASE("the window changes one version in a project's floserver.json") {
    const TempDir data;
    const fs::path shop = data.path() / "work" / "shop";
    test_util::write_file(shop / "floserver.json", R"({"root": "public", "php": "8.4", "x-note": "kept"})");
    fake_install(data.path(), "php", "8.4.27");
    fake_install(data.path(), "php", "8.2.35");

    ProjectService service(data.path());
    ServiceRefusal refusal;
    REQUIRE(service.add(shop, "", refusal));
    REQUIRE(service.set("shop", true, std::nullopt, refusal));

    const auto listed = service.list(refusal);
    REQUIRE(listed);
    // As floserver.json says it, next to what it resolved to.
    CHECK((*listed)["projects"][0]["wants"] == nlohmann::json{{"web", "nginx"}, {"php", "8.4"}});
    CHECK((*listed)["projects"][0]["php"] == "8.4.27");

    const auto changed = service.use("shop", "php", std::string("8.2"), refusal);
    REQUIRE(changed);
    CHECK((*changed)["php"] == "8.2.35");
    CHECK((*changed)["wants"]["php"] == "8.2");
    const auto text = read_small_file(shop / "floserver.json", 4096);
    REQUIRE(text);
    CHECK(text->find("\"x-note\": \"kept\"") != std::string::npos);
    CHECK(text->find("\"root\": \"public\"") < text->find("\"php\""));

    const auto web = service.use("shop", "web", std::string("apache"), refusal);
    REQUIRE(web);
    CHECK((*web)["wants"]["web"] == "apache");
    const auto none = service.use("shop", "database", std::nullopt, refusal);
    REQUIRE(none);
    CHECK_FALSE((*none)["wants"].contains("database"));

    CHECK_FALSE(service.use("nope", "php", std::string("8.4"), refusal));
    CHECK(refusal.code == "unknown_project");
    CHECK_FALSE(service.use("shop", "root", std::string("/"), refusal));
    CHECK(refusal.code == "bad_params");
    CHECK_FALSE(service.use("shop", "php", std::string("newest"), refusal));
    CHECK(refusal.code == "config_refused");
    // Kept with its real, long name (Windows runners have a short temp path).
    const auto folder = service.folder("shop", refusal);
    REQUIRE(folder);
    CHECK(fs::equivalent(*folder, shop));
    CHECK_FALSE(service.folder("nope", refusal));
}

TEST_CASE("a damaged config.json is never overwritten") {
    const TempDir data;
    fs::create_directories(data.path() / "shop");
    test_util::write_file(app_config_file(data.path()), "{ hand edited, broken");
    ProjectService service(data.path());
    ServiceRefusal refusal;
    CHECK_FALSE(service.list(refusal));
    CHECK(refusal.code == "config_unreadable");
    CHECK_FALSE(service.add(data.path() / "shop", "", refusal));
    CHECK(refusal.code == "config_unreadable");
    CHECK(test_util::read_file(app_config_file(data.path())) == "{ hand edited, broken");
}

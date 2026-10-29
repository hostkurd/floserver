// integration.json and opening projects in FloCMS Studio (D14).

#include "floserver/core/integration.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/shim.h"
#include "test_util.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

using namespace floserver::core;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

void fake_runtime(const fs::path& data, const std::string& name, const std::string& version) {
    const fs::path dir = runtimes_dir(data) / name / version;
    fs::create_directories(dir);
    test_util::write_file(dir / ".floserver-runtime.json",
                          json{{"schema", 1}, {"name", name}, {"version", version},
                               {"platform", std::string(current_platform())}, {"variant", ""},
                               {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
}

}  // namespace

TEST_CASE("integration.json: what Studio reads, and never a secret") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_runtime(data, "php", "8.4.27");
    fake_runtime(data, "php", "8.5.12");
    fake_runtime(data, "mariadb", "11.4.13");

    const json status = json::parse(
        R"({"servers":[{"series":"11.4","version":"11.4.13","port":3306,"internal_port":33100,"state":"asleep"},)"
        R"({"series":"11.8","version":"11.8.9","port":0},{"series":"x","port":"3307"},{"series":1,"port":3308},)"
        R"({"series":"11.6","version":7,"port":3309},"junk"]})");
    const IntegrationInfo info = collect_integration(data, status);
    CHECK(info.shims == shims_dir(data));
    // No port yet, a port or series that isn't what it should be: left out. A
    // version that isn't text: kept, without it.
    REQUIRE(info.databases.size() == 2);
    CHECK(info.databases[0].series == "11.4");
    CHECK(info.databases[0].version == "11.4.13");
    CHECK(info.databases[0].port == 3306);
    CHECK(info.databases[1].series == "11.6");
    CHECK(info.databases[1].version.empty());
    CHECK(info.runtimes.size() == 3);

    const json doc = json::parse(integration_json(info));
    CHECK(doc["schema"] == kIntegrationSchema);
    CHECK(doc["shims"] == path_to_utf8(shims_dir(data)));
    CHECK(doc["databases"][0]["port"] == 3306);
    CHECK_FALSE(doc["databases"][0].contains("internal_port"));
    bool has_85 = false;
    for (const json& r : doc["runtimes"]) {
        has_85 = has_85 || (r["name"] == "php" && r["version"] == "8.5.12");
    }
    CHECK(has_85);
    // The keys, and nothing else.
    for (const auto& [key, value] : doc.items()) {
        CHECK((key == "schema" || key == "floserver_version" || key == "cli" || key == "shims" || key == "runtimes" ||
               key == "databases"));
    }

    // Bad status text: no databases, the rest still there.
    CHECK(collect_integration(data, json("not an object")).databases.empty());
    CHECK(collect_integration(data, json::object()).runtimes.size() == 3);
    CHECK(collect_integration(data, json{{"servers", 5}}).databases.empty());
}

TEST_CASE("integration.json: written when it changed, kept when it didn't") {
    const test_util::TempDir tmp;
    const fs::path& data = tmp.path();
    std::string error;
    IntegrationInfo info;
    info.shims = shims_dir(data);
    const std::string first = integration_json(info);
    CHECK(integration_json(info) == first);  // the same info, the same text
    REQUIRE(write_integration(data, first, error));
    CHECK(test_util::read_file(integration_file(data)) == first);

    const auto before = fs::last_write_time(integration_file(data));
    REQUIRE(write_integration(data, first, error));
    CHECK(fs::last_write_time(integration_file(data)) == before);

    info.databases.push_back({"11.4", "11.4.13", 3307});
    const std::string second = integration_json(info);
    CHECK(second != first);
    REQUIRE(write_integration(data, second, error));
    CHECK(test_util::read_file(integration_file(data)) == second);
}

TEST_CASE("A FloCMS project, by Studio's rule") {
    const test_util::TempDir tmp;
    const fs::path& dir = tmp.path();
    CHECK_FALSE(is_flocms_project(dir));  // no composer.json

    test_util::write_file(dir / "composer.json", R"({"name":"hostkurd/flocms"})");
    CHECK(is_flocms_project(dir));
    test_util::write_file(dir / "composer.json", R"({"require":{"hostkurd/flocms-core":"^2.0"}})");
    CHECK(is_flocms_project(dir));
    test_util::write_file(dir / "composer.json", R"({"require-dev":{"hostkurd/flocms-core":"^2.0"}})");
    CHECK(is_flocms_project(dir));

    test_util::write_file(dir / "composer.json", R"({"require":{"laravel/framework":"^11"}})");
    CHECK_FALSE(is_flocms_project(dir));
    test_util::write_file(dir / "composer.json", R"({"name":"someone/hostkurd-flocms"})");
    CHECK_FALSE(is_flocms_project(dir));
    test_util::write_file(dir / "composer.json", R"({"require":["hostkurd/flocms-core"]})");
    CHECK_FALSE(is_flocms_project(dir));
    test_util::write_file(dir / "composer.json", "{ not json");
    CHECK_FALSE(is_flocms_project(dir));
    fs::remove(dir / "composer.json");
    fs::create_directories(dir / "composer.json");  // a folder by that name
    CHECK_FALSE(is_flocms_project(dir));
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("Studio on Linux: flocms-studio on the PATH") {
    const test_util::TempDir tmp;
    // PATH is put back however the test ends.
    struct RestorePath {
        std::optional<std::string> old = get_env("PATH");
        RestorePath() = default;
        RestorePath(const RestorePath&) = delete;
        RestorePath& operator=(const RestorePath&) = delete;
        RestorePath(RestorePath&&) = delete;
        RestorePath& operator=(RestorePath&&) = delete;
        ~RestorePath() {
            if (old) {
                setenv("PATH", old->c_str(), 1);  // NOLINT(concurrency-mt-unsafe): tests run on one thread
            } else {
                unsetenv("PATH");  // NOLINT(concurrency-mt-unsafe): tests run on one thread
            }
        }
    } const restore;
    const fs::path bin = tmp.path() / "bin";
    fs::create_directories(bin);
    setenv("PATH", ("relative:" + bin.string()).c_str(), 1);  // NOLINT(concurrency-mt-unsafe): one thread
    CHECK_FALSE(find_studio().has_value());
    test_util::write_file(bin / "flocms-studio", "");
    const auto found = find_studio();
    REQUIRE(found.has_value());
    CHECK(*found == bin / "flocms-studio");
}
#endif

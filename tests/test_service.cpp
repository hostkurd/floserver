// Runs the real floserverd from the build: start, status, a second copy,
// an unknown request, the project list, stop.

#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/service_client.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <random>
#include <thread>

using namespace floserver::core;

#ifndef FLOSERVER_TEST_BIN_DIR
#error "FLOSERVER_TEST_BIN_DIR must name the folder with floserverd"
#endif

namespace {

std::string fresh_instance_suffix() {
    std::random_device rd;
    return "svc-" + std::to_string(current_pid()) + "-" + std::to_string(rd() % 100000);
}

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    (void)_putenv_s(name, value.c_str());
#else
    (void)setenv(name, value.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
#endif
}

}  // namespace

TEST_CASE("the real service starts, answers, refuses a second copy and stops") {
    // The service reads FLOSERVER_INSTANCE, so it meets only this test.
    set_env("FLOSERVER_INSTANCE", fresh_instance_suffix());
    const std::string instance = ipc::default_instance();
    // ...and keeps its files here, never in the real user folder.
    const test_util::TempDir data;
    set_env("FLOSERVER_DATA_DIR", path_to_utf8(data.path()));

#ifdef _WIN32
    const std::filesystem::path daemon = std::filesystem::path(FLOSERVER_TEST_BIN_DIR) / "floserverd.exe";
#else
    const std::filesystem::path daemon = std::filesystem::path(FLOSERVER_TEST_BIN_DIR) / "floserverd";
#endif
    REQUIRE(std::filesystem::is_regular_file(daemon));

    std::string error;
    REQUIRE(spawn_detached(daemon, {}, error));
    std::optional<ServiceClient> client;
    for (int i = 0; i < 100 && !client; ++i) {  // up to 10 s on a slow CI machine
        client = ServiceClient::connect(instance, error);
        if (!client) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    INFO(error);
    REQUIRE(client.has_value());

    const auto status = client->call("status");
    REQUIRE(status.has_value());
    REQUIRE(status->ok);
    CHECK(status->result["pid"].get<std::uint32_t>() != current_pid());
    CHECK(status->result["uptime_s"].is_number_integer());
    CHECK(status->result.contains("memory_bytes"));

    const auto unknown = client->call("make_coffee");
    REQUIRE(unknown.has_value());
    CHECK_FALSE(unknown->ok);
    CHECK(unknown->error_code == "unknown_method");

    // Projects over the channel: add a folder, see it, turn it on, forget it.
    const std::filesystem::path shop = data.path() / "My Shop";
    std::filesystem::create_directories(shop);
    const auto added = client->call("projects.add", {{"path", path_to_utf8(shop)}});
    REQUIRE(added.has_value());
    INFO(added->error_message);
    REQUIRE(added->ok);
    CHECK(added->result["name"] == "my-shop");
    CHECK(added->result["state"] == "off");
    const auto relative = client->call("projects.add", {{"path", "My Shop"}});
    REQUIRE(relative.has_value());
    CHECK(relative->error_code == "bad_params");
    const auto turned_on = client->call("projects.set", {{"name", "my-shop"}, {"on", true}});
    REQUIRE(turned_on.has_value());
    REQUIRE(turned_on->ok);
    CHECK(turned_on->result["on"] == true);
    CHECK(turned_on->result["state"] == "needs_download");  // no PHP installed in this data folder
    const auto listed = client->call("projects.list");
    REQUIRE(listed.has_value());
    REQUIRE(listed->ok);
    REQUIRE(listed->result["projects"].size() == 1);
    // Empty braces are a null json, not an object: the window's Open in
    // Studio asked projects.list that way and was refused as a bad request
    // (answered with id 0, so the call read it as no answer: "FloServer
    // stopped answering", 3 Nov). Still a request, and the next one too.
    const auto braces = client->call("projects.list", {});
    REQUIRE(braces.has_value());
    CHECK(braces->ok);
    const auto after = client->call("status");
    REQUIRE(after.has_value());
    CHECK(after->ok);
    // Params that aren't an object are refused, as an error for that request:
    // the connection stays and the next request is answered.
    const auto refused = client->call("projects.list", nlohmann::json::array());
    REQUIRE(refused.has_value());
    CHECK_FALSE(refused->ok);
    CHECK(refused->error_code == "bad_request");
    const auto still = client->call("status");
    REQUIRE(still.has_value());
    CHECK(still->ok);
    CHECK(std::filesystem::is_regular_file(data.path() / "config.json"));
    const auto removed = client->call("projects.remove", {{"name", "my-shop"}});
    REQUIRE(removed.has_value());
    CHECK(removed->ok);
    // Its folder, as the service had it: the window checks it before the Recycle Bin.
    CHECK(removed->result["path"] == listed->result["projects"][0]["path"]);
    const auto gone = client->call("projects.remove", {{"name", "my-shop"}});
    REQUIRE(gone.has_value());
    CHECK(gone->error_code == "unknown_project");
    CHECK(std::filesystem::is_directory(shop));

    // Removed with its database (approved 2 Nov): the names are the
    // service's own, a reserved one is refused (the project stays), and
    // with no MariaDB installed there's nothing to drop.
    {
        const std::filesystem::path db_shop = data.path() / "db-shop";
        test_util::write_file(db_shop / "floserver.json", R"({"php": "8.5", "database": "mariadb-11.4"})");
        const std::filesystem::path root = data.path() / "root";
        test_util::write_file(root / "floserver.json", R"({"php": "8.5", "database": "mariadb-11.4"})");
        for (const auto& dir : {db_shop, root}) {
            const auto add = client->call("projects.add", {{"path", path_to_utf8(dir)}});
            REQUIRE(add.has_value());
            REQUIRE_MESSAGE(add->ok, add->error_message);
        }
        const auto bad = client->call("projects.remove", {{"name", "db-shop"}, {"drop_database", "yes"}});
        REQUIRE(bad.has_value());
        CHECK(bad->error_code == "bad_params");
        const auto reserved = client->call("projects.remove", {{"name", "root"}, {"drop_database", true}});
        REQUIRE(reserved.has_value());
        CHECK(reserved->error_code == "reserved_name");
        const auto nobody = client->call("projects.remove", {{"name", "nobody"}, {"drop_database", true}});
        REQUIRE(nobody.has_value());
        CHECK(nobody->error_code == "unknown_project");
        // A database name in the request is never used.
        const auto dropped = client->call("projects.remove",
                                          {{"name", "db-shop"}, {"drop_database", true}, {"database", "mysql"}});
        REQUIRE(dropped.has_value());
        CHECK_MESSAGE(dropped->ok, dropped->error_message);
        CHECK(dropped->result["database_dropped"] == false);  // none to drop: not said to be deleted
        const auto plain = client->call("projects.remove", {{"name", "root"}, {"drop_database", false}});
        REQUIRE(plain.has_value());
        CHECK_MESSAGE(plain->ok, plain->error_message);
        const auto left = client->call("projects.list");
        REQUIRE(left.has_value());
        CHECK(left->result["projects"].empty());
        CHECK(std::filesystem::is_regular_file(db_shop / "floserver.json"));
    }

    // Settings → Folders over the channel: where projects are made (what
    // projects.list hands the New project wizard), the data folder's size,
    // and a clean-up.
    const test_util::TempDir elsewhere;
    const auto folders = client->call("folders.get");
    REQUIRE(folders.has_value());
    INFO(folders->error_message);
    REQUIRE(folders->ok);
    CHECK(folders->result["custom"] == false);
    CHECK(folders->result["data_dir"] == path_to_utf8(data.path()));
    CHECK(folders->result["data_bytes"].get<std::uint64_t>() > 0);  // config.json at least
    CHECK(folders->result["data_complete"] == true);
    const auto moved = client->call("folders.set", {{"projects_dir", path_to_utf8(elsewhere.path())}});
    REQUIRE(moved.has_value());
    INFO(moved->error_message);
    REQUIRE(moved->ok);
    CHECK(moved->result["custom"] == true);
    const auto listed_again = client->call("projects.list");
    REQUIRE(listed_again.has_value());
    std::string real_error;
    const auto real = real_folder(elsewhere.path(), real_error);  // long names, as the service keeps it
    REQUIRE(real);
    CHECK(path_from_utf8(listed_again->result["projects_dir"].get<std::string>()) == *real);
    const auto inside = client->call("folders.set", {{"projects_dir", path_to_utf8(data.path() / "runtimes")}});
    REQUIRE(inside.has_value());
    CHECK_FALSE(inside->ok);
    const auto reset = client->call("folders.set", {{"projects_dir", nullptr}});
    REQUIRE(reset.has_value());
    CHECK(reset->result["custom"] == false);
    test_util::write_file(data.path() / "downloads" / "php-8.5.1.zip.part", "half");
    const auto cleaned = client->call("folders.clean", {{"what", "leftovers"}});
    REQUIRE(cleaned.has_value());
    REQUIRE(cleaned->ok);
    CHECK(cleaned->result["freed"] == 4);
    CHECK(cleaned->result["leftovers_bytes"] == 0);
    CHECK(cleaned->result["skipped"] == 0);
    CHECK(cleaned->result["skipped_items"].is_array());
    CHECK(cleaned->result["skipped_items"].empty());
    CHECK_FALSE(std::filesystem::exists(data.path() / "downloads" / "php-8.5.1.zip.part"));
    const auto bad_clean = client->call("folders.clean", {{"what", "everything"}});
    REQUIRE(bad_clean.has_value());
    CHECK(bad_clean->error_code == "bad_params");

    // The certificate authority: made on request, never trusted by the
    // service itself (that needs the user's confirmation), removed on request.
    const auto none = client->call("certs.status");
    REQUIRE(none.has_value());
    REQUIRE(none->ok);
    CHECK(none->result["ca"].is_null());
    const auto made = client->call("certs.ensure_ca");
    REQUIRE(made.has_value());
    INFO(made->error_message);
    if (made->ok) {  // a build without crypto refuses instead
        CHECK(made->result["ca"]["trusted"] != "yes");
        CHECK(made->result["ca"]["der"].is_string());
        const auto again = client->call("certs.status");
        REQUIRE(again.has_value());
        CHECK(again->result["ca"]["fingerprint"] == made->result["ca"]["fingerprint"]);
        const auto removed_ca = client->call("certs.remove");
        REQUIRE(removed_ca.has_value());
        CHECK(removed_ca->ok);
        CHECK_FALSE(std::filesystem::exists(data.path() / "certs"));
    }

    // A second service for the same user and instance says so and exits.
    const auto second = run_and_wait(daemon, {}, std::chrono::seconds(30), error);
    INFO(error);
    REQUIRE(second.has_value());
    CHECK(*second == 0);
    const auto again = client->call("status");
    REQUIRE(again.has_value());
    CHECK(again->result["pid"] == status->result["pid"]);

    client.reset();
    REQUIRE(stop_service(instance, error));
    CHECK_FALSE(ipc::connect(instance, error).has_value());
    // Stopping when it isn't running is fine.
    CHECK(stop_service(instance, error));
}

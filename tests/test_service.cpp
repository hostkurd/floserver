// Runs the real floserverd from the build: start, status, a second copy,
// an unknown request, stop.

#include "floserver/core/ipc.h"
#include "floserver/core/process.h"
#include "floserver/core/service_client.h"

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

    // A second service for the same user and instance doesn't take over.
    REQUIRE(spawn_detached(daemon, {}, error));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto again = client->call("status");
    REQUIRE(again.has_value());
    CHECK(again->result["pid"] == status->result["pid"]);

    client.reset();
    REQUIRE(stop_service(instance, error));
    CHECK_FALSE(ipc::connect(instance, error).has_value());
    // Stopping when it isn't running is fine.
    CHECK(stop_service(instance, error));
}

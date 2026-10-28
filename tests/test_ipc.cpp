#include "floserver/core/ipc.h"
#include "floserver/core/process.h"

#include <doctest/doctest.h>

#include <chrono>
#include <random>
#include <string>
#include <thread>

using namespace floserver::core;
using namespace std::chrono_literals;

namespace {

// A channel name no other test run uses.
std::string unique_instance(const char* what) {
    std::random_device rd;
    return std::string("test-") + what + "-" + std::to_string(current_pid()) + "-" + std::to_string(rd() % 100000);
}

std::unique_ptr<ipc::Server> listen_ok(const std::string& instance) {
    std::unique_ptr<ipc::Server> server;
    std::string error;
    const auto status = ipc::Server::listen(instance, server, error);
    INFO(error);
    REQUIRE(status == ipc::ListenStatus::Listening);
    REQUIRE(server != nullptr);
    return server;
}

}  // namespace

TEST_CASE("a client and the server exchange messages both ways") {
    const auto instance = unique_instance("echo");
    auto server = listen_ok(instance);

    std::thread echo([&] {
        auto c = server->accept();
        REQUIRE(c.has_value());
        while (auto m = c->receive()) {
            if (!c->send("echo:" + *m)) {
                break;
            }
        }
    });

    std::string error;
    auto client = ipc::connect(instance, error);
    INFO(error);
    REQUIRE(client.has_value());
    CHECK(error.empty());
    for (const std::string& m : {std::string("hello"), std::string(), std::string(200000, 'x'),
                                std::string("\xd9\x83\xd9\x88\xd8\xb1\xd8\xaf\xdb\x8c")}) {
        REQUIRE(client->send(m));
        const auto reply = client->receive();
        REQUIRE(reply.has_value());
        CHECK(*reply == "echo:" + m);
    }
    client->close();
    echo.join();
}

TEST_CASE("connecting when nothing listens gives nothing, without an error") {
    std::string error;
    CHECK_FALSE(ipc::connect(unique_instance("none"), error).has_value());
    CHECK(error.empty());
}

TEST_CASE("only one server per instance") {
    const auto instance = unique_instance("single");
    auto first = listen_ok(instance);
    std::unique_ptr<ipc::Server> second;
    std::string error;
    CHECK(ipc::Server::listen(instance, second, error) == ipc::ListenStatus::AlreadyRunning);
    CHECK(second == nullptr);

    // Once the first is gone, the name can be claimed again.
    first.reset();
    auto again = listen_ok(instance);
    CHECK(again != nullptr);
}

TEST_CASE("stop() wakes a thread waiting in accept()") {
    const auto instance = unique_instance("stop");
    auto server = listen_ok(instance);
    std::optional<ipc::Connection> got;
    bool returned = false;
    std::thread waiter([&] {
        got = server->accept();
        returned = true;
    });
    std::this_thread::sleep_for(100ms);
    server->stop();
    server->stop();  // twice is fine
    waiter.join();
    CHECK(returned);
    CHECK_FALSE(got.has_value());
    CHECK_FALSE(server->accept().has_value());
}

TEST_CASE("an oversized message breaks the connection instead of being read") {
    const auto instance = unique_instance("big");
    auto server = listen_ok(instance);
    std::optional<std::string> received = std::string("not set");
    bool still_valid = true;
    std::thread reader([&] {
        auto c = server->accept();
        REQUIRE(c.has_value());
        received = c->receive();
        still_valid = c->valid();
    });
    std::string error;
    auto client = ipc::connect(instance, error);
    REQUIRE(client.has_value());
    CHECK_FALSE(client->send(std::string(ipc::kMaxMessage + 1, 'x')));  // refused before sending
    CHECK(client->send(std::string(ipc::kMaxMessage, 'y')));            // the limit itself is fine
    reader.join();
    REQUIRE(received.has_value());
    CHECK(received->size() == ipc::kMaxMessage);
    CHECK(still_valid);
}

TEST_CASE("a hang-up mid-message is reported, not hung on") {
    const auto instance = unique_instance("hangup");
    auto server = listen_ok(instance);
    std::optional<std::string> received = std::string("not set");
    std::thread reader([&] {
        auto c = server->accept();
        REQUIRE(c.has_value());
        received = c->receive();
    });
    {
        std::string error;
        auto client = ipc::connect(instance, error);
        REQUIRE(client.has_value());
    }  // closed without sending anything
    reader.join();
    CHECK_FALSE(received.has_value());
}

TEST_CASE("instance names are checked") {
    std::unique_ptr<ipc::Server> server;
    std::string error;
    for (const char* bad : {"", "../x", "a/b", "UPPER", "a b", "a\\b"}) {
        CAPTURE(bad);
        CHECK(ipc::Server::listen(bad, server, error) == ipc::ListenStatus::Failed);
        CHECK_FALSE(ipc::connect(bad, error).has_value());
    }
}

#ifndef _WIN32
#include <sys/stat.h>

TEST_CASE("the socket and its folder are private to this user") {
    const auto instance = unique_instance("perm");
    auto server = listen_ok(instance);
    const std::string path = ipc::endpoint_path(instance);
    struct stat st{};
    REQUIRE(stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 077U) == 0);
    const std::string dir = path.substr(0, path.rfind('/'));
    REQUIRE(stat(dir.c_str(), &st) == 0);
    CHECK((st.st_mode & 077U) == 0);
}
#endif

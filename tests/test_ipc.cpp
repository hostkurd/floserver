#include "floserver/core/ipc.h"
#include "floserver/core/process.h"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace floserver::core;
using namespace std::chrono_literals;

namespace {

// Writes raw bytes to the channel, bypassing the message framing.
bool raw_send(const std::string& instance, const std::uint8_t* data, std::size_t size) {
#ifdef _WIN32
    const std::string path = ipc::endpoint_path(instance);
    const std::wstring wide(path.begin(), path.end());
    const HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(h);
    return ok;
#else
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = ipc::endpoint_path(instance);
    if (path.size() >= sizeof addr.sun_path) {
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    const bool ok = fd >= 0 && connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0 &&
                    write(fd, data, size) == static_cast<ssize_t>(size);
    if (fd >= 0) {
        close(fd);
    }
    return ok;
#endif
}

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
        if (!c) {
            return;  // the client's checks below then fail
        }
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

TEST_CASE("messages up to the limit pass; a sender refuses a longer one") {
    const auto instance = unique_instance("big");
    auto server = listen_ok(instance);
    std::optional<std::string> received;
    bool still_valid = false;
    std::thread reader([&] {
        if (auto c = server->accept()) {
            received = c->receive();
            still_valid = c->valid();
        }
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

TEST_CASE("a receiver refuses a length over the limit without reading it") {
    const auto instance = unique_instance("hdr");
    auto server = listen_ok(instance);
    std::optional<std::string> received = std::string("not set");
    bool still_valid = true;
    std::thread reader([&] {
        if (auto c = server->accept()) {
            received = c->receive();
            still_valid = c->valid();
        }
    });
    // Send a raw header announcing kMaxMessage + 1 bytes, as a hostile client could.
    const std::uint32_t too_long = ipc::kMaxMessage + 1;
    const std::array<std::uint8_t, 4> header = {
        static_cast<std::uint8_t>(too_long), static_cast<std::uint8_t>(too_long >> 8U),
        static_cast<std::uint8_t>(too_long >> 16U), static_cast<std::uint8_t>(too_long >> 24U)};
    REQUIRE(raw_send(instance, header.data(), header.size()));
    reader.join();
    CHECK_FALSE(received.has_value());
    CHECK_FALSE(still_valid);
}

TEST_CASE("a client gives up when the service doesn't answer") {
    const auto instance = unique_instance("slow");
    auto server = listen_ok(instance);
    std::optional<ipc::Connection> held;
    std::thread silent([&] { held = server->accept(); });  // accepts, never replies
    std::string error;
    auto client = ipc::connect(instance, error);
    REQUIRE(client.has_value());
    REQUIRE(client->set_timeout(std::chrono::milliseconds(300)));
    REQUIRE(client->send("ping"));
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(client->receive().has_value());
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    CHECK_FALSE(client->valid());
    silent.join();
}

TEST_CASE("a hang-up mid-message is reported, not hung on") {
    const auto instance = unique_instance("hangup");
    auto server = listen_ok(instance);
    std::optional<std::string> received = std::string("not set");
    std::thread reader([&] {
        if (auto c = server->accept()) {
            received = c->receive();
        }
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

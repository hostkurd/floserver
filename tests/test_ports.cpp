#include "floserver/core/ports.h"

#include <doctest/doctest.h>

#include <set>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace floserver::core;

namespace {

#ifdef _WIN32
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

enum class Where : std::uint8_t { Loopback4, Any4, Loopback6 };

// Another program listening on a port, for the real-probe tests.
// can_listen_on_loopback has already started Winsock on Windows.
class Holder {
public:
    Holder(Where where, Port port) {
        if (where == Where::Loopback6) {
            fd_ = socket(AF_INET6, SOCK_STREAM, 0);
            int v6only = 1;
            setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only),
                       static_cast<SockLen>(sizeof v6only));
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(port);
            addr.sin6_addr = in6addr_loopback;
            listening_ = bind(fd_, reinterpret_cast<const sockaddr*>(&addr),
                              static_cast<SockLen>(sizeof addr)) == 0;
        } else {
            fd_ = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(where == Where::Any4 ? INADDR_ANY : INADDR_LOOPBACK);
            listening_ = bind(fd_, reinterpret_cast<const sockaddr*>(&addr),
                              static_cast<SockLen>(sizeof addr)) == 0;
        }
        listening_ = listening_ && listen(fd_, 1) == 0;
    }
    ~Holder() {
#ifdef _WIN32
        closesocket(fd_);
#else
        close(fd_);
#endif
    }
    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;
    Holder(Holder&&) = delete;
    Holder& operator=(Holder&&) = delete;
    [[nodiscard]] bool listening() const { return listening_; }

private:
#ifdef _WIN32
    SOCKET fd_;
#else
    int fd_;
#endif
    bool listening_ = false;
};

PortProbe busy(std::set<Port> taken) {
    return [taken = std::move(taken)](Port p) { return !taken.contains(p); };
}
}  // namespace

TEST_CASE("web ports: 80 and 443 when both are free (Q4)") {
    const auto w = pick_web_ports(busy({}));
    REQUIRE(w.has_value());
    CHECK(w->http == 80);
    CHECK(w->https == 443);
    CHECK_FALSE(w->fallback);
}

TEST_CASE("web ports: either of 80/443 taken means the fallback pair") {
    for (Port taken : {Port{80}, Port{443}}) {
        CAPTURE(taken);
        const auto w = pick_web_ports(busy({taken}));
        REQUIRE(w.has_value());
        CHECK(w->http == 8080);
        CHECK(w->https == 8443);
        CHECK(w->fallback);
    }
}

TEST_CASE("web ports: the next pair when 8080/8443 is taken too") {
    const auto w = pick_web_ports(busy({80, 8443}));
    REQUIRE(w.has_value());
    CHECK(w->http == 8081);
    CHECK(w->https == 8444);
}

TEST_CASE("web ports: nothing when no pair is free") {
    CHECK_FALSE(pick_web_ports([](Port) { return false; }).has_value());
}

TEST_CASE("database ports start at 3306 and skip ports in use (D12)") {
    CHECK(pick_port_from(kFirstDatabasePort, {}, busy({})) == Port{3306});
    // XAMPP's MySQL on 3306: never touch it, take the next one.
    CHECK(pick_port_from(kFirstDatabasePort, {}, busy({3306})) == Port{3307});
    // Ports the service already gave out are skipped even if they look free.
    CHECK(pick_port_from(kFirstDatabasePort, {3306, 3307}, busy({3308})) == Port{3309});
}

TEST_CASE("port search stops at the try limit and at 65535") {
    CHECK_FALSE(pick_port_from(3306, {}, [](Port) { return false; }, 5).has_value());
    CHECK(pick_port_from(65535, {}, busy({})) == Port{65535});
    CHECK_FALSE(pick_port_from(65535, {65535}, busy({})).has_value());
}

TEST_CASE("the real probe sees a port someone else listens on as taken") {
    for (const Where where : {Where::Loopback4, Where::Any4, Where::Loopback6}) {
        CAPTURE(static_cast<int>(where));
        const std::optional<Port> port = pick_port_from(47000, {}, can_listen_on_loopback, 500);
        REQUIRE(port.has_value());
        CHECK(can_listen_on_loopback(*port));  // probing doesn't keep the port
        {
            const Holder holder(where, *port);
            if (!holder.listening()) {
                REQUIRE(where == Where::Loopback6);  // only a machine without IPv6 may skip
                continue;
            }
            CHECK_FALSE(can_listen_on_loopback(*port));
        }
        CHECK(can_listen_on_loopback(*port));
    }
}

TEST_CASE("the IPv6 loopback check gives a steady answer") {
    // Without IPv6 (this CI sandbox has none) nginx must not be told to listen on [::1].
    const bool first = loopback_ipv6_available();
    CHECK(loopback_ipv6_available() == first);
}

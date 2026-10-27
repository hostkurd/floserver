#include "floserver/core/ports.h"

#include <limits>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace floserver::core {

#ifdef _WIN32

namespace {

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockSession() {
        if (ok_) {
            WSACleanup();
        }
    }
    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
    [[nodiscard]] bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

}  // namespace

bool can_listen_on_loopback(Port port) {
    static const WinsockSession winsock;
    if (!winsock.ok()) {
        return false;
    }
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return false;
    }
    // Exclusive use, so a port held by anyone else (including http.sys,
    // which IIS and some chat apps use for port 80) reads as taken.
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               static_cast<int>(sizeof exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool free = bind(s, reinterpret_cast<const sockaddr*>(&addr), static_cast<int>(sizeof addr)) == 0 &&
                      listen(s, 1) == 0;
    closesocket(s);
    return free;
}

#else

bool can_listen_on_loopback(Port port) {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return false;
    }
    // SO_REUSEADDR only skips our own TIME_WAIT leftovers; it never lets us
    // share a port another program is listening on.
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool free = bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0 &&
                      listen(s, 1) == 0;
    close(s);
    return free;
}

#endif

std::optional<WebPorts> pick_web_ports(const PortProbe& probe) {
    if (probe(80) && probe(443)) {
        return WebPorts{80, 443, false};
    }
    constexpr Port kHttpFallback = 8080;
    constexpr Port kHttpsFallback = 8443;
    constexpr Port kPairs = 20;
    for (Port i = 0; i < kPairs; ++i) {
        const auto http = static_cast<Port>(kHttpFallback + i);
        const auto https = static_cast<Port>(kHttpsFallback + i);
        if (probe(http) && probe(https)) {
            return WebPorts{http, https, true};
        }
    }
    return std::nullopt;
}

std::optional<Port> pick_port_from(Port first, const std::set<Port>& taken, const PortProbe& probe,
                                   unsigned max_tries) {
    constexpr unsigned kHighest = std::numeric_limits<Port>::max();
    for (unsigned port = first; port <= kHighest && port - first < max_tries; ++port) {
        const auto p = static_cast<Port>(port);
        if (!taken.contains(p) && probe(p)) {
            return p;
        }
    }
    return std::nullopt;
}

}  // namespace floserver::core

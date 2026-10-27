#include "floserver/core/ports.h"

#include <limits>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace floserver::core {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
using SockLen = int;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
void close_socket(NativeSocket s) { closesocket(s); }
bool family_unsupported() {
    const int e = WSAGetLastError();
    return e == WSAEAFNOSUPPORT || e == WSAEADDRNOTAVAIL;
}

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
    WinsockSession(WinsockSession&&) = delete;
    WinsockSession& operator=(WinsockSession&&) = delete;
    [[nodiscard]] bool ok() const { return ok_; }

private:
    bool ok_ = false;
};
#else
using NativeSocket = int;
using SockLen = socklen_t;
constexpr NativeSocket kNoSocket = -1;
void close_socket(NativeSocket s) { close(s); }
bool family_unsupported() { return errno == EAFNOSUPPORT || errno == EADDRNOTAVAIL; }
#endif

enum class Listen : std::uint8_t { Free, Taken, NoSuchAddress };

// Tries to listen on 127.0.0.1:port or [::1]:port. No SO_REUSEADDR: on macOS
// it would let us bind 127.0.0.1 while another program listens on 0.0.0.0,
// and we never connect, so there is no TIME_WAIT of ours to skip.
Listen try_listen(bool ipv6, Port port) {
    const NativeSocket s = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) {
        return family_unsupported() ? Listen::NoSuchAddress : Listen::Taken;
    }
#ifdef _WIN32
    // Exclusive use, so a port held by anyone else (including http.sys,
    // which IIS and some chat apps use for port 80) reads as taken.
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               static_cast<int>(sizeof exclusive));
#endif
    int bound = -1;
    if (ipv6) {
        int v6only = 1;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only),
                   static_cast<int>(sizeof v6only));
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        addr.sin6_addr = in6addr_loopback;
        bound = bind(s, reinterpret_cast<const sockaddr*>(&addr), static_cast<SockLen>(sizeof addr));
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bound = bind(s, reinterpret_cast<const sockaddr*>(&addr), static_cast<SockLen>(sizeof addr));
    }
    Listen result = Listen::Free;
    if (bound != 0) {
        result = family_unsupported() ? Listen::NoSuchAddress : Listen::Taken;
    } else if (listen(s, 1) != 0) {
        result = Listen::Taken;
    }
    close_socket(s);
    return result;
}

}  // namespace

bool can_listen_on_loopback(Port port) {
#ifdef _WIN32
    static const WinsockSession winsock;
    if (!winsock.ok()) {
        return false;
    }
#endif
    // Browsers try [::1] first for *.localhost, and Node and Vite listen there
    // by default, so a port is free only if both loopback addresses are.
    // A machine without IPv6 counts as free on that side.
    return try_listen(false, port) == Listen::Free && try_listen(true, port) != Listen::Taken;
}

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

#pragma once

// Port choice for the web server and databases (D7, D12, Q4).
//
// A port counts as free only if we can listen on it ourselves. We never
// connect to a port to test it: connecting and dropping can upset another
// stack's server (the FloCMS Studio lesson). Everything listens on 127.0.0.1.

#include <cstdint>
#include <functional>
#include <optional>
#include <set>

namespace floserver::core {

using Port = std::uint16_t;

// Asks the operating system whether we could listen on 127.0.0.1:port and
// [::1]:port now without sharing the port with anyone, including a program
// listening on 0.0.0.0 or [::]. Never connects.
[[nodiscard]] bool can_listen_on_loopback(Port port);

// True when this machine has IPv6 loopback ([::1]), so servers listen there
// as well as on 127.0.0.1. nginx won't start if told to listen on an address
// the machine doesn't have. Never connects.
[[nodiscard]] bool loopback_ipv6_available();

// Lets tests replace the operating-system check.
using PortProbe = std::function<bool(Port)>;

struct WebPorts {
    Port http;
    Port https;
    bool fallback;  // true when 80/443 were taken and 8080/8443 (or later) are used
};

// Q4: 80 and 443 when both are free, else the first free pair from
// 8080/8443, 8081/8444, ... Returns nothing when no pair is free.
[[nodiscard]] std::optional<WebPorts> pick_web_ports(const PortProbe& probe);

// D12: the first free port from `first` upwards that isn't already given out
// by the service (`taken`). 3306 for the first MariaDB, then 3307, 3308...
[[nodiscard]] std::optional<Port> pick_port_from(Port first, const std::set<Port>& taken,
                                                 const PortProbe& probe,
                                                 unsigned max_tries = 100);

inline constexpr Port kFirstDatabasePort = 3306;

}  // namespace floserver::core

#pragma once

// HTTPS GET for the downloader (D5). The transport only moves bytes; every
// rule that makes a download safe (signature, size, SHA-256) is applied above
// it, in downloader.cpp, so tests can run it with a fake transport.
//
// What a transport must do:
//   - https only, TLS 1.2 or newer, the system's certificate checks;
//   - follow at most 5 redirects, each to https (never https -> http);
//   - call `body` with the body of a 200 reply as it arrives, and stop as
//     soon as `body` returns false;
//   - give up when the server stays silent past the timeouts, and end the
//     transfer once `limits.max_time` has passed or `*limits.cancel` turns
//     true, even while a read is waiting (a server that sends one byte every
//     29 seconds mustn't hold the service).
//
// Windows uses WinHTTP. Other platforms have no transport yet (D3: they come
// after Windows), so make_system_transport() returns nothing there.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace floserver::core {

struct HttpReply {
    // 0 when no HTTP reply arrived (no network, TLS failure, timeout...),
    // and `error` says why.
    int status = 0;
    std::string error;
    // True when the body callback asked to stop.
    bool stopped = false;
    // The server's Date header, if it sent a valid one (the clock hint).
    std::optional<std::chrono::sys_seconds> server_date;
    std::optional<std::uint64_t> content_length;
};

// Return false to stop the transfer.
using HttpBody = std::function<bool(std::span<const std::uint8_t>)>;

struct HttpLimits {
    // The whole request, from connecting to the last byte.
    std::chrono::seconds max_time{60};
    // When set and true, the transfer stops as soon as possible.
    const std::atomic<bool>* cancel = nullptr;
};

// Any method, with headers and a body, for the Publisher's uploads (D34.4).
// Redirects aren't followed. The reply's body is kept whatever its status,
// up to `max_reply` bytes (an error answer explains itself).
struct HttpRequest {
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;  // Host and Content-Length are added
    std::uint64_t body_size = 0;
    // Fills `out` with the body's next bytes and returns how many, called
    // until body_size bytes came; 0 before that ends the request (a read error).
    std::function<std::size_t(std::span<std::uint8_t> out)> source;
    std::size_t max_reply = std::size_t{1024} * 1024;
};

struct HttpResponse {
    int status = 0;  // 0: no reply, `error` says why
    std::string error;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // The first header of that name, ignoring case.
    [[nodiscard]] std::optional<std::string> header(std::string_view name) const;
};

class HttpTransport {
public:
    HttpTransport() = default;
    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;
    HttpTransport(HttpTransport&&) = delete;
    HttpTransport& operator=(HttpTransport&&) = delete;
    virtual ~HttpTransport() = default;

    // May be called from several threads at once. A transfer ended by
    // `limits` returns status 0 with `error` set.
    [[nodiscard]] virtual HttpReply get(const std::string& url, const HttpLimits& limits,
                                        const HttpBody& body) = 0;

    // Sends `request` (http.h's rules for https and timeouts). Transports
    // that only download answer with an error.
    [[nodiscard]] virtual HttpResponse send(const HttpRequest& request, const HttpLimits& limits);
};

// The platform's transport, or nothing where there isn't one yet.
[[nodiscard]] std::unique_ptr<HttpTransport> make_system_transport();

// "Wed, 28 Oct 2026 19:12:28 GMT" (RFC 9110 IMF-fixdate only).
[[nodiscard]] std::optional<std::chrono::sys_seconds> parse_http_date(std::string_view text);

}  // namespace floserver::core

// WinHTTP transport (http.h). Compiled on every platform, empty outside Windows.

#ifdef _WIN32

#include "floserver/core/build_info.h"
#include "floserver/core/http.h"

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

// Older Windows SDKs and MinGW headers lack the TLS 1.3 flag.
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace floserver::core {

namespace {

struct HandleCloser {
    void operator()(HINTERNET h) const noexcept {
        if (h != nullptr) {
            WinHttpCloseHandle(h);
        }
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

constexpr int kResolveTimeoutMs = 15000;
constexpr int kConnectTimeoutMs = 15000;
constexpr int kSendTimeoutMs = 15000;
constexpr int kReceiveTimeoutMs = 30000;
constexpr DWORD kMaxRedirects = 5;

std::wstring widen(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr,
                                      nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string describe(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT:
            return "the server didn't answer in time";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return "the server's name couldn't be found (no internet?)";
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return "couldn't connect to the server";
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
            return "the secure connection failed (certificate or TLS problem)";
        case ERROR_WINHTTP_REDIRECT_FAILED:
            return "the server redirected to an address that isn't allowed";
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        case ERROR_WINHTTP_INVALID_URL:
            return "the address isn't a valid https address";
        default:
            break;
    }
    std::array<wchar_t, 512> text{};
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   GetModuleHandleW(L"winhttp.dll"), code, 0, text.data(),
                                   static_cast<DWORD>(text.size()), nullptr);
    std::string message = narrow(std::wstring_view(text.data(), n));
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ' ||
                                message.back() == '.')) {
        message.pop_back();
    }
    if (message.empty()) {
        message = "network error";
    }
    return message + " (" + std::to_string(code) + ")";
}

std::optional<std::wstring> query_string(HINTERNET request, DWORD what) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, what, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 || bytes > 4096) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, what, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    value.resize(bytes / sizeof(wchar_t));
    return value;
}

// Closes the request handle from another thread when the deadline passes or
// the caller cancels: WinHTTP then fails the blocked call at once
// (ERROR_WINHTTP_OPERATION_CANCELLED). Owns the request handle.
class Watchdog {
public:
    Watchdog(HINTERNET request, const HttpLimits& limits)
        : request_(request),
          cancel_(limits.cancel),
          deadline_(std::chrono::steady_clock::now() + limits.max_time),
          thread_([this] { watch(); }) {}
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    Watchdog(Watchdog&&) = delete;
    Watchdog& operator=(Watchdog&&) = delete;
    ~Watchdog() {
        bool close = false;
        {
            const std::lock_guard lock(mutex_);
            done_ = true;
            close = !fired_;
        }
        changed_.notify_all();
        thread_.join();
        if (close) {
            WinHttpCloseHandle(request_);
        }
    }
    [[nodiscard]] HINTERNET request() const { return request_; }
    // Stops watching. Returns why the transfer was ended, or empty if it wasn't.
    [[nodiscard]] std::string finish() {
        const std::lock_guard lock(mutex_);
        done_ = true;
        return reason_;
    }

private:
    void watch() {
        std::unique_lock lock(mutex_);
        while (!done_) {
            const bool cancelled = cancel_ != nullptr && cancel_->load();
            if (cancelled || std::chrono::steady_clock::now() >= deadline_) {
                fired_ = true;
                reason_ = cancelled ? "cancelled" : "the download took too long";
                WinHttpCloseHandle(request_);
                return;
            }
            changed_.wait_for(lock, std::chrono::milliseconds(200));
        }
    }

    HINTERNET request_;
    const std::atomic<bool>* cancel_;
    std::chrono::steady_clock::time_point deadline_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_ = false;
    bool fired_ = false;
    std::string reason_;
    std::thread thread_;  // last: starts after the members above exist
};

class WinHttpTransport final : public HttpTransport {
public:
    explicit WinHttpTransport(Handle session) : session_(std::move(session)) {}

    HttpReply get(const std::string& url, const HttpLimits& limits, const HttpBody& body) override {
        HttpReply reply;
        const std::wstring wide_url = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (wide_url.empty() || !WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts) ||
            parts.nScheme != INTERNET_SCHEME_HTTPS) {
            reply.error = "the address isn't a valid https address";
            return reply;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty()) {
            path = L"/";
        }

        const Handle connection(WinHttpConnect(session_.get(), host.c_str(), parts.nPort, 0));
        if (!connection) {
            reply.error = describe(GetLastError());
            return reply;
        }
        HINTERNET raw = WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (raw == nullptr) {
            reply.error = describe(GetLastError());
            return reply;
        }
        Watchdog watchdog(raw, limits);
        exchange(watchdog.request(), body, reply);
        if (const std::string why = watchdog.finish(); !why.empty()) {
            reply = HttpReply{};
            reply.error = why;
        }
        return reply;
    }

private:
    // Sends the request and streams the reply into `body`.
    static void exchange(HINTERNET request, const HttpBody& body, HttpReply& reply) {
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            reply.error = describe(GetLastError());
            return;
        }
        // The redirect policy already refuses https -> http; check the final
        // address anyway, as the rule matters more than the option.
        if (const auto final_url = query_url(request); !final_url || final_url->rfind(L"https://", 0) != 0) {
            reply.error = "the server redirected to an address that isn't https";
            return;
        }
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
            reply.error = describe(GetLastError());
            return;
        }
        reply.status = static_cast<int>(status);
        if (const auto date = query_string(request, WINHTTP_QUERY_DATE)) {
            reply.server_date = parse_http_date(narrow(*date));
        }
        ULONGLONG length = 0;
        size = sizeof(length);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                                WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX)) {
            reply.content_length = static_cast<std::uint64_t>(length);
        }
        if (status != 200) {
            return;
        }
        std::vector<std::uint8_t> buffer(std::size_t{64} * 1024);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
                reply.error = describe(GetLastError());
                reply.status = 0;
                return;
            }
            if (got == 0) {
                return;
            }
            if (!body(std::span<const std::uint8_t>(buffer.data(), got))) {
                reply.stopped = true;
                return;
            }
        }
    }

    static std::optional<std::wstring> query_url(HINTERNET request) {
        DWORD bytes = 0;
        WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &bytes);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 || bytes > 65536) {
            return std::nullopt;
        }
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, value.data(), &bytes)) {
            return std::nullopt;
        }
        value.resize(bytes / sizeof(wchar_t));
        while (!value.empty() && value.back() == L'\0') {
            value.pop_back();
        }
        return value;
    }

    Handle session_;
};

}  // namespace

std::unique_ptr<HttpTransport> make_system_transport() {
    const std::wstring agent = L"FloServer/" + widen(kVersion);
    Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        // Windows 8.0 and older lack automatic proxy; D3 starts at Windows 10,
        // but fall back rather than fail.
        session.reset(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0));
    }
    if (!session) {
        return nullptr;
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // Windows 10 without TLS 1.3 in WinHTTP
        if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            return nullptr;
        }
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    DWORD redirects = kMaxRedirects;
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)) ||
        !WinHttpSetOption(session.get(), WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects,
                          sizeof(redirects)) ||
        !WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
                            kReceiveTimeoutMs)) {
        return nullptr;
    }
    return std::make_unique<WinHttpTransport>(std::move(session));
}

}  // namespace floserver::core

#endif  // _WIN32

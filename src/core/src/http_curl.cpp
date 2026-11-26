// libcurl transport (http.h) for Linux and macOS (D36.6). Compiled only when
// CMake finds libcurl outside Windows (FLOSERVER_HAVE_CURL).

#if !defined(_WIN32) && defined(FLOSERVER_HAVE_CURL)

#include "floserver/core/build_info.h"
#include "floserver/core/http.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace floserver::core {

namespace {

constexpr long kConnectTimeoutSeconds = 15;
// "Silent" means fewer than kLowSpeedBytes in kLowSpeedSeconds, the same 30 s
// WinHTTP waits for a read.
constexpr long kLowSpeedBytes = 1;
constexpr long kLowSpeedSeconds = 30;
constexpr long kMaxRedirects = 5;

struct EasyCloser {
    void operator()(CURL* c) const noexcept { curl_easy_cleanup(c); }
};
using Easy = std::unique_ptr<CURL, EasyCloser>;

struct ListFreer {
    void operator()(curl_slist* l) const noexcept { curl_slist_free_all(l); }
};
using List = std::unique_ptr<curl_slist, ListFreer>;

void global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string describe(CURLcode code) {
    switch (code) {
        case CURLE_OPERATION_TIMEDOUT:
            return "the server didn't answer in time";
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return "the server's name couldn't be found (no internet?)";
        case CURLE_COULDNT_CONNECT:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
            return "couldn't connect to the server";
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CERTPROBLEM:
            return "the secure connection failed (certificate or TLS problem)";
        case CURLE_TOO_MANY_REDIRECTS:
            return "the server redirected too many times";
        case CURLE_UNSUPPORTED_PROTOCOL:
            return "the server redirected to an address that isn't allowed";
        case CURLE_URL_MALFORMAT:
            return "the address isn't a valid https address";
        default:
            break;
    }
    return std::string(curl_easy_strerror(code)) + " (" + std::to_string(static_cast<int>(code)) + ")";
}

bool starts_with_https(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    return url.size() > kScheme.size() &&
           std::equal(kScheme.begin(), kScheme.end(), url.begin(), [](char a, char b) {
               return a == (b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : b);
           });
}

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

// What a transfer shares with curl's callbacks.
struct Transfer {
    const HttpLimits* limits = nullptr;
    std::chrono::steady_clock::time_point deadline;
    std::string ended;  // why the transfer was ended by us, empty otherwise

    // get()
    const HttpBody* body = nullptr;
    bool stopped = false;
    std::optional<std::chrono::sys_seconds> date;
    std::optional<std::uint64_t> length;
    CURL* easy = nullptr;

    // send()
    std::vector<std::pair<std::string, std::string>> headers;
    std::string reply;
    std::size_t max_reply = 0;
    const HttpRequest* request = nullptr;
    std::uint64_t sent = 0;
};

int on_progress(void* user, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/,
                curl_off_t /*ulnow*/) {
    auto* t = static_cast<Transfer*>(user);
    if (t->limits->cancel != nullptr && t->limits->cancel->load()) {
        t->ended = "cancelled";
        return 1;
    }
    if (std::chrono::steady_clock::now() >= t->deadline) {
        t->ended = "the download took too long";
        return 1;
    }
    return 0;
}

// Header lines of every reply (each redirect too): a status line starts a
// new reply, so what's kept belongs to the last one.
std::size_t on_get_header(char* data, std::size_t size, std::size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const std::size_t n = size * count;
    const std::string_view line(data, n);
    if (line.starts_with("HTTP/")) {
        t->date.reset();
        t->length.reset();
        return n;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return n;
    }
    std::string name(line.substr(0, colon));
    std::transform(name.begin(), name.end(), name.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    const std::string value = trim(line.substr(colon + 1));
    if (name == "date") {
        t->date = parse_http_date(value);
    } else if (name == "content-length") {
        std::uint64_t v = 0;
        bool ok = !value.empty() && value.size() <= 19;
        for (const char c : value) {
            ok = ok && c >= '0' && c <= '9';
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (ok) {
            t->length = v;
        }
    }
    return n;
}

std::size_t on_get_body(char* data, std::size_t size, std::size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const std::size_t n = size * count;
    long status = 0;
    curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &status);
    // Only a 200's body reaches the caller; anything else ends the transfer
    // (its status is reported, its body isn't needed).
    if (status != 200) {
        return 0;
    }
    if (!(*t->body)(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data), n))) {
        t->stopped = true;
        return 0;
    }
    return n;
}

std::size_t on_send_header(char* data, std::size_t size, std::size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const std::size_t n = size * count;
    const std::string_view line(data, n);
    if (line.starts_with("HTTP/")) {
        t->headers.clear();
        return n;
    }
    const auto colon = line.find(':');
    if (colon != std::string_view::npos && t->headers.size() < 200) {
        t->headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    return n;
}

std::size_t on_send_body(char* data, std::size_t size, std::size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const std::size_t n = size * count;
    const std::size_t room = t->max_reply > t->reply.size() ? t->max_reply - t->reply.size() : 0;
    t->reply.append(data, std::min(n, room));
    return n;  // the rest is dropped, the reply still completes
}

std::size_t on_read(char* out, std::size_t size, std::size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    const std::uint64_t left = t->request->body_size - t->sent;
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(left, size * count));
    if (want == 0) {
        return 0;
    }
    const std::size_t got =
        t->request->source ? t->request->source(std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(out), want))
                           : 0;
    if (got == 0 || got > want) {
        t->ended = "the upload couldn't be read";
        return CURL_READFUNC_ABORT;
    }
    t->sent += got;
    return got;
}

class CurlTransport final : public HttpTransport {
public:
    explicit CurlTransport(std::string ca_file) : ca_file_(std::move(ca_file)) { global_init(); }

    HttpReply get(const std::string& url, const HttpLimits& limits, const HttpBody& body) override {
        HttpReply reply;
        if (!starts_with_https(url)) {
            reply.error = "the address isn't a valid https address";
            return reply;
        }
        const Easy easy(curl_easy_init());
        if (!easy) {
            reply.error = "couldn't start a download";
            return reply;
        }
        Transfer t;
        t.limits = &limits;
        t.deadline = std::chrono::steady_clock::now() + limits.max_time;
        t.body = &body;
        t.easy = easy.get();
        CURL* c = easy.get();
        common(c, url, limits, t);
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, kMaxRedirects);
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_get_header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &t);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_get_body);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);

        const CURLcode code = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        char* effective = nullptr;
        curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &effective);
        reply.server_date = t.date;
        reply.content_length = t.length;
        if (!t.ended.empty()) {
            reply.error = t.ended;
            return reply;
        }
        if (t.stopped) {
            reply.status = static_cast<int>(status);
            reply.stopped = true;
            return reply;
        }
        // Checked again here, whatever curl's own protocol limits did.
        if (effective != nullptr && !starts_with_https(effective)) {
            reply.error = "the server redirected to an address that isn't allowed";
            return reply;
        }
        if (code == CURLE_OK || (code == CURLE_WRITE_ERROR && status != 200)) {
            reply.status = static_cast<int>(status);
            return reply;
        }
        reply.error = describe(code);
        return reply;
    }

    HttpResponse send(const HttpRequest& request, const HttpLimits& limits) override {
        HttpResponse response;
        if (!starts_with_https(request.url) || request.method.empty()) {
            response.error = "the address isn't a valid https address";
            return response;
        }
        const Easy easy(curl_easy_init());
        if (!easy) {
            response.error = "couldn't start a request";
            return response;
        }
        Transfer t;
        t.limits = &limits;
        t.deadline = std::chrono::steady_clock::now() + limits.max_time;
        t.max_reply = request.max_reply;
        t.request = &request;
        t.easy = easy.get();
        CURL* c = easy.get();
        common(c, request.url, limits, t);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
        List headers;
        for (const auto& [k, v] : request.headers) {
            std::string line = k;
            line += ": ";
            line += v;
            if (line.find_first_of("\r\n") != std::string::npos) {
                response.error = "a request header isn't valid";
                return response;
            }
            headers.reset(curl_slist_append(headers.release(), line.c_str()));
        }
        // Large uploads would otherwise wait for "100 Continue" first.
        headers.reset(curl_slist_append(headers.release(), "Expect:"));
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers.get());
        if (request.method == "HEAD") {
            curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        } else if (request.body_size > 0) {
            curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(request.body_size));
            curl_easy_setopt(c, CURLOPT_READFUNCTION, on_read);
            curl_easy_setopt(c, CURLOPT_READDATA, &t);
        } else if (request.method == "POST") {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
        }
        if (request.method != "GET" && request.method != "HEAD") {
            curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        }
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_send_header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &t);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_send_body);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);

        const CURLcode code = curl_easy_perform(c);
        if (!t.ended.empty()) {
            response.error = t.ended;
            return response;
        }
        if (code != CURLE_OK) {
            response.error = describe(code);
            return response;
        }
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        response.status = static_cast<int>(status);
        response.headers = std::move(t.headers);
        response.body = std::move(t.reply);
        return response;
    }

private:
    void common(CURL* c, const std::string& url, const HttpLimits& limits, Transfer& t) const {
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_USERAGENT, agent_.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500  // 7.85.0
        curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(c, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
        curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
        curl_easy_setopt(c, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytes);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, kLowSpeedSeconds);
        // The overall limit is enforced by on_progress (with the cancel
        // flag); this is a backstop a second later.
        const auto seconds = std::max<long>(1, static_cast<long>(limits.max_time.count()) + 1);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, seconds);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &t);
        curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, nullptr);  // bytes as sent: sizes and hashes are of them
        if (!ca_file_.empty()) {
            curl_easy_setopt(c, CURLOPT_CAINFO, ca_file_.c_str());
            curl_easy_setopt(c, CURLOPT_NOPROXY, "*");
        }
    }

    std::string ca_file_;
    std::string agent_ = std::string("FloServer/") + kVersion;
};

}  // namespace

std::unique_ptr<HttpTransport> make_system_transport() { return std::make_unique<CurlTransport>(std::string()); }

std::unique_ptr<HttpTransport> make_test_transport(const std::filesystem::path& ca_file) {
    return std::make_unique<CurlTransport>(ca_file.string());
}

}  // namespace floserver::core

#endif

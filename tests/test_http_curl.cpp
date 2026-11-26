// The libcurl transport (D36.6) against a real nginx over HTTPS, with a
// certificate from FloServer's own CA: the rules http.h asks of every
// transport. Needs nginx (Linux CI installs it) and a build with libcurl;
// skipped otherwise.

#ifndef _WIN32

#include "floserver/core/certificates.h"
#include "floserver/core/child_process.h"
#include "floserver/core/encoding.h"
#include "floserver/core/process.h"
#include "floserver/core/http.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

using namespace floserver::core;
namespace fs = std::filesystem;

namespace {

std::uint16_t free_port() {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t len = sizeof(a);
    std::uint16_t port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) {
        port = ntohs(a.sin_port);
    }
    close(s);
    return port;
}

bool answers(std::uint16_t port) {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    const bool ok = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    close(s);
    return ok;
}

struct Server {
    test_util::TempDir dir;
    std::unique_ptr<ChildProcess> nginx;
    std::uint16_t port = 0;
    std::uint16_t http_port = 0;
    fs::path ca;
    std::string body;  // what /file serves

    [[nodiscard]] std::string url(const std::string& path) const {
        return "https://dl.localhost:" + std::to_string(port) + path;
    }
};

std::unique_ptr<Server> start_server() {
    const fs::path nginx_bin = get_env("FLOSERVER_TEST_NGINX").value_or("/usr/sbin/nginx");
    std::error_code ec;
    if (!fs::is_regular_file(nginx_bin, ec)) {
        return nullptr;
    }
    auto s = std::make_unique<Server>();
    const fs::path root = s->dir.path();
    std::string error;
    CertStore store(root / "data");
    const auto ca = store.ensure_ca(error);
    REQUIRE_MESSAGE(ca, error);
    const auto site = store.ensure_site("dl", error);
    REQUIRE_MESSAGE(site, error);
    s->ca = ca->certificate;

    fs::create_directories(root / "www");
    fs::create_directories(root / "logs");
    fs::create_directories(root / "temp");
    for (int i = 0; i < 4096; ++i) {
        s->body += "line " + std::to_string(i) + "\n";
    }
    test_util::write_file(root / "www" / "file", s->body);
    test_util::write_file(root / "www" / "slow", std::string(4096, 'x'));
    s->port = free_port();
    s->http_port = free_port();
    const std::string p = std::to_string(s->port);
    const std::string hp = std::to_string(s->http_port);
    const std::string conf =
        std::string(geteuid() == 0 ? "user root;\n" : "") +
        "worker_processes 1;\n"
        "pid " + (root / "nginx.pid").string() + ";\n"
        "error_log " + (root / "logs" / "error.log").string() + ";\n"
        "events {}\n"
        "http {\n"
        "  access_log off;\n"
        "  client_body_temp_path " + (root / "temp").string() + ";\n"
        "  proxy_temp_path " + (root / "temp").string() + ";\n"
        "  fastcgi_temp_path " + (root / "temp").string() + ";\n"
        "  uwsgi_temp_path " + (root / "temp").string() + ";\n"
        "  scgi_temp_path " + (root / "temp").string() + ";\n"
        "  server {\n"
        "    listen 127.0.0.1:" + hp + ";\n"
        "    location / { return 200 'plain http\\n'; }\n"
        "  }\n"
        "  server {\n"
        "    listen 127.0.0.1:" + p + " ssl;\n"
        "    server_name dl.localhost;\n"
        "    ssl_certificate " + site->certificate.string() + ";\n"
        "    ssl_certificate_key " + site->key.string() + ";\n"
        "    root " + (root / "www").string() + ";\n"
        "    location = /file { default_type application/octet-stream; }\n"
        "    location = /slow { default_type application/octet-stream; limit_rate 1; }\n"
        "    location = /missing { return 404 'not here\\n'; }\n"
        "    location = /hop { return 302 /file; }\n"
        "    location = /to-http { return 302 http://127.0.0.1:" + hp + "/; }\n"
        "    location = /loop { return 302 /loop; }\n"
        "    location = /put { return 201 'stored $content_length\\n'; }\n"
        "    location = /big { return 200 '0123456789abcdefghij'; }\n"
        "  }\n"
        "}\n";
    test_util::write_file(root / "nginx.conf", conf);

    ChildOptions o;
    o.program = nginx_bin;
    o.args = {"-p", root.string() + "/", "-c", (root / "nginx.conf").string(), "-g", "daemon off;"};
    o.working_dir = root;
    o.output_log = root / "logs" / "nginx.out";
    s->nginx = ChildProcess::start(o, error);
    REQUIRE_MESSAGE(s->nginx, error);
    for (int i = 0; i < 100 && !answers(s->port); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE_MESSAGE(answers(s->port), test_util::read_file(root / "logs" / "error.log"));
    return s;
}

}  // namespace

TEST_CASE("the libcurl transport follows http.h's rules") {
    auto probe = make_test_transport("/nonexistent");
    if (!probe) {
        MESSAGE("this build has no libcurl; skipped");
        return;
    }
    const auto server = start_server();
    if (!server) {
        MESSAGE("nginx isn't installed here; skipped");
        return;
    }
    const auto http = make_test_transport(server->ca);
    HttpLimits limits;
    limits.max_time = std::chrono::seconds(20);

    SUBCASE("a 200's body arrives whole, with its length and the server's date") {
        std::string got;
        const auto r = http->get(server->url("/file"), limits, [&](std::span<const std::uint8_t> b) {
            got.append(reinterpret_cast<const char*>(b.data()), b.size());
            return true;
        });
        CHECK(r.error == "");
        CHECK(r.status == 200);
        CHECK_FALSE(r.stopped);
        CHECK(got == server->body);
        REQUIRE(r.content_length);
        CHECK(*r.content_length == server->body.size());
        CHECK(r.server_date.has_value());
    }

    SUBCASE("the body callback can stop the transfer") {
        std::size_t calls = 0;
        const auto r = http->get(server->url("/file"), limits, [&](std::span<const std::uint8_t>) {
            ++calls;
            return false;
        });
        CHECK(r.stopped);
        CHECK(r.status == 200);
        CHECK(calls == 1);
    }

    SUBCASE("another status is reported, its body isn't delivered") {
        bool called = false;
        const auto r = http->get(server->url("/missing"), limits, [&](std::span<const std::uint8_t>) {
            called = true;
            return true;
        });
        CHECK(r.status == 404);
        CHECK(r.error == "");
        CHECK_FALSE(called);
    }

    SUBCASE("an https redirect is followed") {
        std::string got;
        const auto r = http->get(server->url("/hop"), limits, [&](std::span<const std::uint8_t> b) {
            got.append(reinterpret_cast<const char*>(b.data()), b.size());
            return true;
        });
        CHECK(r.status == 200);
        CHECK(got == server->body);
        REQUIRE(r.content_length);
        CHECK(*r.content_length == server->body.size());
        CHECK(r.server_date.has_value());
    }

    SUBCASE("a redirect to http is refused") {
        bool called = false;
        const auto r = http->get(server->url("/to-http"), limits, [&](std::span<const std::uint8_t>) {
            called = true;
            return true;
        });
        CHECK_FALSE(called);
        CHECK(r.status == 0);
        CHECK(r.error == "the server redirected to an address that isn't allowed");
    }

    SUBCASE("more than 5 redirects are refused") {
        const auto r = http->get(server->url("/loop"), limits, [](std::span<const std::uint8_t>) { return true; });
        CHECK(r.status == 0);
        CHECK(r.error == "the server redirected too many times");
    }

    SUBCASE("only https addresses are fetched") {
        const auto r = http->get("http://127.0.0.1:" + std::to_string(server->http_port) + "/", limits,
                                 [](std::span<const std::uint8_t>) { return true; });
        CHECK(r.status == 0);
        CHECK(r.error == "the address isn't a valid https address");
    }

    SUBCASE("a certificate from a CA that isn't trusted is refused") {
        const test_util::TempDir other;
        std::string error;
        CertStore store(other.path());
        const auto ca = store.ensure_ca(error);
        REQUIRE(ca);
        const auto stranger = make_test_transport(ca->certificate);
        const auto r = stranger->get(server->url("/file"), limits, [](std::span<const std::uint8_t>) { return true; });
        CHECK(r.status == 0);
        CHECK(r.error == "the secure connection failed (certificate or TLS problem)");
    }

    SUBCASE("the wrong host name is refused") {
        const auto r = http->get("https://other.localhost:" + std::to_string(server->port) + "/file", limits,
                                 [](std::span<const std::uint8_t>) { return true; });
        CHECK(r.status == 0);
        CHECK(r.error == "the secure connection failed (certificate or TLS problem)");
    }

    SUBCASE("a trickling server is ended at max_time") {
        HttpLimits quick;
        quick.max_time = std::chrono::seconds(2);
        const auto start = std::chrono::steady_clock::now();
        const auto r = http->get(server->url("/slow"), quick, [](std::span<const std::uint8_t>) { return true; });
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK(r.status == 0);
        CHECK(r.error == "the download took too long");
        CHECK(took < std::chrono::seconds(5));
    }

    SUBCASE("cancel ends a transfer that is waiting") {
        std::atomic<bool> cancel{false};
        HttpLimits l;
        l.max_time = std::chrono::seconds(60);
        l.cancel = &cancel;
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            cancel = true;
        });
        const auto start = std::chrono::steady_clock::now();
        const auto r = http->get(server->url("/slow"), l, [](std::span<const std::uint8_t>) { return true; });
        stopper.join();
        CHECK(r.error == "cancelled");
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    }

    SUBCASE("send() uploads a body and keeps the reply") {
        const std::string payload(300000, 'p');
        std::size_t at = 0;
        HttpRequest req;
        req.method = "PUT";
        req.url = server->url("/put");
        req.headers = {{"Content-Type", "application/octet-stream"}, {"X-Test", "1"}};
        req.body_size = payload.size();
        req.source = [&](std::span<std::uint8_t> out) {
            const std::size_t n = std::min(out.size(), payload.size() - at);
            std::copy_n(payload.data() + at, n, out.data());
            at += n;
            return n;
        };
        const auto r = http->send(req, limits);
        CHECK(r.error == "");
        CHECK(r.status == 201);
        CHECK(r.body == "stored 300000\n");
        CHECK(r.header("content-type").has_value());
        CHECK(at == payload.size());
    }

    SUBCASE("send() sends an empty PUT with Content-Length 0") {
        HttpRequest req;
        req.method = "PUT";
        req.url = server->url("/put");
        const auto r = http->send(req, limits);
        CHECK(r.status == 201);
        CHECK(r.body == "stored 0\n");
    }

    SUBCASE("send() keeps at most max_reply bytes of the reply") {
        HttpRequest req;
        req.method = "GET";
        req.url = server->url("/big");
        req.max_reply = 5;
        const auto r = http->send(req, limits);
        CHECK(r.status == 200);
        CHECK(r.body == "01234");
    }

    SUBCASE("send() refuses a body on GET and a broken upload") {
        HttpRequest get;
        get.method = "GET";
        get.url = server->url("/file");
        get.body_size = 3;
        CHECK(http->send(get, limits).error == "a GET request can't carry a body");

        HttpRequest put;
        put.method = "PUT";
        put.url = server->url("/put");
        put.body_size = 10;
        put.source = [](std::span<std::uint8_t>) { return std::size_t{0}; };
        const auto r = http->send(put, limits);
        CHECK(r.status == 0);
        CHECK(r.error == "the upload couldn't be read");
    }

    SUBCASE("send() refuses a header with a line break, a bad name or a NUL") {
        for (const auto& [k, v] : std::vector<std::pair<std::string, std::string>>{
                 {"", "x"}, {"Bad:Name", "x"}, {"Bad Name", "x"}, {"Bad(Name", "x"}, {"Bad;Name", "x"},
                 {"X", std::string("a\0b", 3)}}) {
            HttpRequest bad;
            bad.method = "GET";
            bad.url = server->url("/file");
            bad.headers = {{k, v}};
            CHECK(http->send(bad, limits).error == "a request header isn't valid");
        }
    }

    SUBCASE("send() refuses a header with a line break") {
        HttpRequest req;
        req.method = "GET";
        req.url = server->url("/file");
        req.headers = {{"X-Bad", "a\r\nInjected: yes"}};
        const auto r = http->send(req, limits);
        CHECK(r.status == 0);
        CHECK(r.error == "a request header isn't valid");
    }
}

#endif

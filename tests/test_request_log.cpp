#include "floserver/core/request_log.h"

#include "test_util.h"

#include <doctest/doctest.h>

using namespace floserver::core;
namespace fs = std::filesystem;

TEST_CASE("request log lines are read from nginx's floserver_requests format") {
    const std::string text = "1793306687.123 200 0.038 GET /\n"
                             "1793306687.5 404 0.002 GET /favicon.ico\n"
                             "not a line at all\n"
                             "1793306690.000 302 0.044 POST /cart?item=7&q=a\\x22b\n"
                             "1793306691.000 200 0.001 get /lowercase-method\n"
                             "1793306692.000 99 0.001 GET /bad-status\n"
                             "1793306693.000 200 0.010 GET /still-being-written";
    const auto entries = parse_request_log(text, 50, false);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].time_ms == 1793306687123);
    CHECK(entries[0].status == 200);
    CHECK(entries[0].duration_ms == 38);
    CHECK(entries[0].method == "GET");
    CHECK(entries[0].uri == "/");
    CHECK(entries[1].time_ms == 1793306687500);
    CHECK(entries[2].uri == "/cart?item=7&q=a\\x22b");
    CHECK(entries[2].duration_ms == 44);

    SUBCASE("only the newest are kept, oldest first") {
        const auto last = parse_request_log(text, 2, false);
        REQUIRE(last.size() == 2);
        CHECK(last[0].uri == "/favicon.ico");
        CHECK(last[1].method == "POST");
    }
    SUBCASE("a read that starts mid-line drops the partial line") {
        const auto tail = parse_request_log("87.123 200 0.038 GET /\n1793306687.5 404 0.002 GET /x\n", 50, true);
        REQUIRE(tail.size() == 1);
        CHECK(tail[0].uri == "/x");
    }
    SUBCASE("long addresses are cut") {
        const std::string uri = "/" + std::string(1000, 'a');
        const auto cut = parse_request_log("1.0 200 0.0 GET " + uri + "\n", 50, false);
        REQUIRE(cut.size() == 1);
        CHECK(cut[0].uri.size() == kRequestLogUriLimit);
        CHECK(cut[0].uri.ends_with("..."));
    }
    SUBCASE("a request nginx couldn't read is logged with - as its method") {
        const auto bad = parse_request_log("1.0 400 0.0 - /\n", 50, false);
        REQUIRE(bad.size() == 1);
        CHECK(bad[0].method == "-");
    }
    SUBCASE("CRLF lines and non-ASCII bytes") {
        CHECK(parse_request_log("1.0 200 0.0 GET /a\r\n", 50, false).size() == 1);
        CHECK(parse_request_log("1.0 200 0.0 GET /\xC3\xA9\n", 50, false).empty());
    }
}

TEST_CASE("the request log is read from the end of the file") {
    const test_util::TempDir dir;
    const fs::path file = request_log_file(dir.path(), "shop");
    CHECK(file == dir.path() / "logs" / "sites" / "shop.log");
    CHECK(read_request_log(file, 10).empty());  // no file yet

    std::string text;
    for (int i = 0; i < 3000; ++i) {
        text += "1793306687.000 200 0.001 GET /page/" + std::to_string(i) + "\n";
    }
    test_util::write_file(file, text);
    const auto entries = read_request_log(file, 5);
    REQUIRE(entries.size() == 5);
    CHECK(entries.back().uri == "/page/2999");
    CHECK(entries.front().uri == "/page/2995");
    CHECK(read_request_log(file, 100000).size() == kRequestLogMaxEntries);

    // A 64 KiB tail after older lines still ends on the newest one.
    std::string exact;
    std::size_t line_count = 0;
    while (exact.size() < std::size_t{64} * 1024) {
        exact += "1793306687.000 200 0.001 GET /p/" + std::to_string(line_count++) + "\n";
    }
    const std::string head = "1793306687.000 200 0.001 GET /before\n";
    test_util::write_file(file, head + exact.substr(exact.size() - std::size_t{64} * 1024));
    const auto tail = read_request_log(file, 100000);
    REQUIRE_FALSE(tail.empty());
    CHECK(tail.back().uri == "/p/" + std::to_string(line_count - 1));
}

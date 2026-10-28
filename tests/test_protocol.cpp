#include "floserver/core/protocol.h"

#include <doctest/doctest.h>

using namespace floserver::core::protocol;
using nlohmann::json;

TEST_CASE("requests round-trip") {
    std::string code;
    const auto r = parse_request(make_request(7, "status", {{"a", 1}}), code);
    REQUIRE(r.has_value());
    CHECK(r->id == 7);
    CHECK(r->method == "status");
    CHECK(r->params == json{{"a", 1}});
    CHECK(parse_request(R"({"v":1,"id":2,"method":"quit"})", code)->params == json::object());
}

TEST_CASE("bad requests say why") {
    std::string code;
    CHECK_FALSE(parse_request("nope", code));
    CHECK(code == "bad_request");
    CHECK_FALSE(parse_request(R"({"v":2,"id":1,"method":"status"})", code));
    CHECK(code == "unsupported_version");
    for (const char* bad : {R"({"v":1,"method":"status"})", R"({"v":1,"id":"1","method":"status"})",
                            R"({"v":1,"id":1})", R"({"v":1,"id":1,"method":"Status"})",
                            R"({"v":1,"id":1,"method":"a b"})", R"({"v":1,"id":1,"method":"x","params":[]})"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_request(bad, code));
        CHECK(code == "bad_request");
    }
}

TEST_CASE("replies round-trip") {
    auto r = parse_reply(make_result(3, {{"pid", 42}}));
    REQUIRE(r.has_value());
    CHECK(r->ok);
    CHECK(r->id == 3);
    CHECK(r->result["pid"] == 42);

    r = parse_reply(make_error(4, "unknown_method", "no such thing"));
    REQUIRE(r.has_value());
    CHECK_FALSE(r->ok);
    CHECK(r->error_code == "unknown_method");
    CHECK(r->error_message == "no such thing");

    CHECK_FALSE(parse_reply("[]"));
    CHECK_FALSE(parse_reply(R"({"v":1,"id":1})"));
    CHECK_FALSE(parse_reply(R"({"v":2,"id":1,"result":{}})"));
}

TEST_CASE("a reply carrying text that isn't UTF-8 is still sent") {
    // A localized Windows error message in a non-UTF-8 code page.
    const std::string bad = "can't write: \xe4\xe5 \xff";
    std::string text;
    CHECK_NOTHROW(text = floserver::core::protocol::make_error(3, "failed", bad));
    const auto reply = floserver::core::protocol::parse_reply(text);
    REQUIRE(reply.has_value());
    CHECK(reply->error_message.find("can't write") == 0);
}

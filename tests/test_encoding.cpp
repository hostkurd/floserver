#include "floserver/core/encoding.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace floserver::core;

namespace {
std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }
}  // namespace

TEST_CASE("base64 matches RFC 4648 examples both ways") {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"", ""},         {"f", "Zg=="},         {"fo", "Zm8="},        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (const auto& [plain, encoded] : cases) {
        CAPTURE(plain);
        CHECK(to_base64(bytes(plain)) == encoded);
        const auto back = from_base64(encoded);
        REQUIRE(back.has_value());
        CHECK(*back == bytes(plain));
    }
}

TEST_CASE("base64 refuses anything else") {
    for (const char* bad : {"Zg", "Zg=", "Zg===", "Z===", "Zm9v Yg==", "Zm9vYg=a", "Zm=vYg==",
                            "Zh==", "Zm9=", "Zm9vYmFy\n", "-_-_", "Zm9vYg==Zm9v"}) {
        CAPTURE(bad);
        CHECK_FALSE(from_base64(bad).has_value());
    }
}

TEST_CASE("hex both ways, strict") {
    const std::vector<std::uint8_t> data = {0x00, 0x7f, 0x80, 0xff, 0x0a};
    CHECK(to_hex(data) == "007f80ff0a");
    CHECK(from_hex("007F80ff0A") == data);
    CHECK(from_hex("")->empty());
    CHECK_FALSE(from_hex("0").has_value());
    CHECK_FALSE(from_hex("0g").has_value());
    CHECK_FALSE(from_hex(" 00").has_value());
}

#include "floserver/core/version.h"

#include <doctest/doctest.h>

using floserver::core::best_match;
using floserver::core::Version;

namespace {
Version v(const char* text) {
    auto parsed = Version::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}
}  // namespace

TEST_CASE("version parsing accepts one to three numbers") {
    CHECK(v("8").to_string() == "8");
    CHECK(v("8.2").to_string() == "8.2");
    CHECK(v("8.2.27").to_string() == "8.2.27");
    CHECK(v("011.4").to_string() == "11.4");
}

TEST_CASE("version parsing rejects anything else") {
    for (const char* bad : {"", ".", "8.", ".8", "8..2", "8.2.27.1", "v8", "8.2a", " 8.2", "8.2 ",
                            "1234567", "8.-1", "8,2"}) {
        CAPTURE(bad);
        CHECK_FALSE(Version::parse(bad).has_value());
    }
}

TEST_CASE("short versions match installed versions with the same leading parts") {
    CHECK(v("8.2").matches(v("8.2.27")));
    CHECK(v("8").matches(v("8.4.1")));
    CHECK(v("8.2.27").matches(v("8.2.27")));
    CHECK_FALSE(v("8.2").matches(v("8.3.1")));
    CHECK_FALSE(v("8.2").matches(v("8.20.1")));
    CHECK_FALSE(v("8.2.27").matches(v("8.2")));
}

TEST_CASE("ordering treats missing parts as zero") {
    CHECK(v("8.2") == v("8.2.0"));
    CHECK(v("8.10") > v("8.9.99"));
    CHECK(v("11.4.2") < v("11.8"));
}

TEST_CASE("best_match picks the newest matching install") {
    const std::vector<Version> installed = {v("8.2.20"), v("8.3.1"), v("8.2.27"), v("8.2.9")};
    CHECK(best_match(v("8.2"), installed)->to_string() == "8.2.27");
    CHECK(best_match(v("8"), installed)->to_string() == "8.3.1");
    CHECK_FALSE(best_match(v("8.4"), installed).has_value());
    CHECK_FALSE(best_match(v("8.2"), {}).has_value());
}

#include "floserver/core/encoding.h"
#include "floserver/core/sha256.h"

#include <doctest/doctest.h>

#include <string>

using namespace floserver::core;

namespace {
std::string hex(std::string_view data) { return to_hex(sha256(data)); }
}  // namespace

TEST_CASE("SHA-256 matches the NIST examples") {
    CHECK(hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmn"
              "opqrlmnopqrsmnopqrstnopqrstu") ==
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    CHECK(hex(std::string(1'000'000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 gives the same answer however the data is split") {
    std::string data;
    for (int i = 0; i < 1000; ++i) {
        data += static_cast<char>(i * 7);
    }
    const auto whole = sha256(data);
    // Splits around the 55/56/64-byte padding edges and odd sizes.
    for (std::size_t chunk : {1U, 3U, 55U, 56U, 63U, 64U, 65U, 127U, 999U}) {
        CAPTURE(chunk);
        Sha256 h;
        for (std::size_t i = 0; i < data.size(); i += chunk) {
            h.update(std::string_view(data).substr(i, chunk));
        }
        CHECK(h.finish() == whole);
    }
    for (std::size_t len : {55U, 56U, 57U, 63U, 64U, 65U, 119U, 120U}) {
        CAPTURE(len);
        Sha256 a;
        a.update(std::string_view(data).substr(0, len));
        Sha256 b;
        b.update(std::string_view(data).substr(0, len / 2));
        b.update(std::string_view(data).substr(len / 2, len - len / 2));
        CHECK(a.finish() == b.finish());
    }
}

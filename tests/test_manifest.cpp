#include "floserver/core/encoding.h"
#include "floserver/core/manifest.h"

#include <doctest/doctest.h>
#include <monocypher-ed25519.h>
#include <nlohmann/json.hpp>

#include <limits>
#include <string>

using namespace floserver::core;
using namespace std::chrono;

namespace {

struct TestKey {
    std::array<std::uint8_t, 64> secret{};
    Ed25519PublicKey pub{};
    explicit TestKey(std::uint8_t fill) {
        std::array<std::uint8_t, 32> seed{};
        seed.fill(fill);
        crypto_ed25519_key_pair(secret.data(), pub.data(), seed.data());
    }
    [[nodiscard]] std::string sign(std::string_view bytes, const std::string& id) const {
        std::array<std::uint8_t, 64> sig{};
        crypto_ed25519_sign(sig.data(), secret.data(),
                            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        return nlohmann::json{{"key", id}, {"signature", to_base64(sig)}}.dump();
    }
};

const TestKey kKey(0x11);
const TestKey kOtherKey(0x22);
const std::string kHash(64, 'a');

sys_seconds at(const char* text) {
    const auto t = parse_utc_time(text);
    REQUIRE(t.has_value());
    return *t;
}

ManifestCheck default_check() {
    ManifestCheck c;
    c.trusted_keys = {{"hostkurd-2026", kKey.pub}};
    c.now = at("2026-10-27T12:00:00Z");
    c.last_sequences = {{"hostkurd-2026", 5}};
    return c;
}

nlohmann::json entry(const char* name, const char* version, const char* platform) {
    return {{"name", name},       {"version", version},
            {"platform", platform}, {"archive", "zip"},
            {"size", 1234},        {"sha256", kHash},
            {"license", "PHP-3.01"}, {"urls", {"https://mirror.hostkurd.net/php.zip"}}};
}

nlohmann::json good_doc() {
    return {{"schema", 1},
            {"sequence", 7},
            {"expires", "2026-12-01T00:00:00Z"},
            {"runtimes",
             {entry("php", "8.2.27", "windows-x64"), entry("php", "8.4.14", "windows-x64"),
              entry("php", "8.4.14", "linux-x64"), entry("mariadb", "11.4.4", "windows-x64")}}};
}

ManifestResult open_signed(const nlohmann::json& doc, const ManifestCheck& check = default_check()) {
    const std::string bytes = doc.dump(2);
    return open_manifest(bytes, kKey.sign(bytes, "hostkurd-2026"), check);
}

}  // namespace

TEST_CASE("Ed25519 matches RFC 8032 test 1") {
    const auto seed = from_hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    REQUIRE(seed.has_value());
    std::array<std::uint8_t, 64> secret{};
    std::array<std::uint8_t, 32> pub{};
    std::vector<std::uint8_t> s = *seed;
    crypto_ed25519_key_pair(secret.data(), pub.data(), s.data());
    CHECK(to_hex(pub) == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    std::array<std::uint8_t, 64> sig{};
    crypto_ed25519_sign(sig.data(), secret.data(), nullptr, 0);
    CHECK(to_hex(sig) ==
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e3970"
          "1cf9b46bd25bf5f0595bbe24655141438e7a100b");
}

TEST_CASE("a correctly signed manifest opens") {
    const auto r = open_signed(good_doc());
    REQUIRE(r.manifest.has_value());
    CHECK(r.error == ManifestError::None);
    CHECK(r.warnings.empty());
    CHECK(r.manifest->sequence == 7);
    CHECK(r.manifest->expires == at("2026-12-01T00:00:00Z"));
    REQUIRE(r.manifest->runtimes.size() == 4);
    const auto& php = r.manifest->runtimes[0];
    CHECK(php.name == "php");
    CHECK(php.size == 1234);
    CHECK(to_hex(php.sha256) == kHash);
    CHECK(php.urls == std::vector<std::string>{"https://mirror.hostkurd.net/php.zip"});
}

TEST_CASE("any change to the bytes breaks the signature") {
    const std::string bytes = good_doc().dump(2);
    const std::string sig = kKey.sign(bytes, "hostkurd-2026");
    for (std::size_t i : {std::size_t{0}, bytes.size() / 2, bytes.size() - 1}) {
        CAPTURE(i);
        std::string changed = bytes;
        changed[i] = static_cast<char>(changed[i] ^ 1);
        CHECK(open_manifest(changed, sig, default_check()).error == ManifestError::BadSignature);
    }
    // Same JSON, different bytes (reformatted): still refused.
    CHECK(open_manifest(good_doc().dump(), sig, default_check()).error ==
          ManifestError::BadSignature);
    CHECK(open_manifest(bytes + "\n", sig, default_check()).error == ManifestError::BadSignature);
}

TEST_CASE("keys: unknown id, wrong key, and two keys during a change") {
    const std::string bytes = good_doc().dump(2);
    CHECK(open_manifest(bytes, kKey.sign(bytes, "someone-else"), default_check()).error ==
          ManifestError::UnknownKey);
    // Signed by another key but claiming our id.
    CHECK(open_manifest(bytes, kOtherKey.sign(bytes, "hostkurd-2026"), default_check()).error ==
          ManifestError::BadSignature);

    ManifestCheck both = default_check();
    both.trusted_keys.push_back({"hostkurd-2027", kOtherKey.pub});
    CHECK(open_manifest(bytes, kOtherKey.sign(bytes, "hostkurd-2027"), both).manifest.has_value());
    CHECK(open_manifest(bytes, kKey.sign(bytes, "hostkurd-2026"), both).manifest.has_value());
}

TEST_CASE("a damaged signature file is refused") {
    const std::string bytes = good_doc().dump(2);
    for (const char* sig : {"", "{}", "[]", R"({"key":"hostkurd-2026"})",
                            R"({"key":"hostkurd-2026","signature":"bm90IGJhc2U2NA"})",
                            R"({"key":"hostkurd-2026","signature":"AAAA"})",
                            R"({"key":1,"signature":"AAAA"})"}) {
        CAPTURE(sig);
        CHECK(open_manifest(bytes, sig, default_check()).error == ManifestError::BadSignatureFile);
    }
}

TEST_CASE("expired and older manifests are refused even when signed") {
    auto doc = good_doc();
    ManifestCheck check = default_check();
    check.now = at("2026-12-01T00:00:00Z");  // the moment it expires
    CHECK(open_signed(doc, check).error == ManifestError::Expired);
    check.now = at("2026-11-30T23:59:59Z");
    CHECK(open_signed(doc, check).manifest.has_value());

    check = default_check();
    check.last_sequences["hostkurd-2026"] = 8;
    CHECK(open_signed(doc, check).error == ManifestError::Rollback);
    check.last_sequences["hostkurd-2026"] = 7;  // the same one again is fine
    const auto r = open_signed(doc, check);
    REQUIRE(r.manifest.has_value());
    CHECK(r.key_id == "hostkurd-2026");
}

TEST_CASE("sequence numbers are counted per key and can't jump too far") {
    // A stolen key signs a huge sequence: refused, it jumps too far.
    auto doc = good_doc();
    doc["sequence"] = 5 + kMaxSequenceJump + 1;
    CHECK(open_signed(doc).error == ManifestError::SequenceJump);
    doc["sequence"] = 5 + kMaxSequenceJump;
    CHECK(open_signed(doc).manifest.has_value());
    doc["sequence"] = std::numeric_limits<std::int64_t>::max();
    CHECK(open_signed(doc).error == ManifestError::SequenceJump);

    // After a key change the new key starts its own count, so a high count
    // under the old key can't lock users out.
    ManifestCheck check = default_check();
    check.trusted_keys.push_back({"hostkurd-2027", kOtherKey.pub});
    check.last_sequences["hostkurd-2026"] = 90'000;
    auto fresh = good_doc();
    fresh["sequence"] = 1;
    const std::string bytes = fresh.dump(2);
    const auto r = open_manifest(bytes, kOtherKey.sign(bytes, "hostkurd-2027"), check);
    REQUIRE(r.manifest.has_value());
    CHECK(r.key_id == "hostkurd-2027");
}

TEST_CASE("schema, sequence and expires are required") {
    auto doc = good_doc();
    doc["schema"] = 2;
    CHECK(open_signed(doc).error == ManifestError::UnsupportedSchema);
    for (const char* key : {"schema", "sequence", "expires"}) {
        CAPTURE(key);
        auto d = good_doc();
        d.erase(key);
        CHECK(open_signed(d).error == ManifestError::BadField);
    }
    doc = good_doc();
    doc["sequence"] = -1;
    CHECK(open_signed(doc).error == ManifestError::BadField);
    doc["sequence"] = 7.5;
    CHECK(open_signed(doc).error == ManifestError::BadField);
    doc["sequence"] = std::numeric_limits<std::uint64_t>::max();
    CHECK(open_signed(doc).error == ManifestError::BadField);
    doc = good_doc();
    doc["schema"] = 0;
    CHECK(open_signed(doc).error == ManifestError::BadField);
    doc["schema"] = std::numeric_limits<std::uint64_t>::max();
    CHECK(open_signed(doc).error == ManifestError::UnsupportedSchema);
    doc = good_doc();
    doc["expires"] = "2026-12-01";
    CHECK(open_signed(doc).error == ManifestError::BadField);

    for (const char* other : {"[1, 2]", "\"text\"", "42"}) {
        CAPTURE(other);
        CHECK(open_manifest(other, kKey.sign(other, "hostkurd-2026"), default_check()).error ==
              ManifestError::NotJson);
    }

    const std::string not_json = "this is signed but not JSON";
    CHECK(open_manifest(not_json, kKey.sign(not_json, "hostkurd-2026"), default_check()).error ==
          ManifestError::NotJson);
}

TEST_CASE("a bad entry is skipped with a warning, the rest still load") {
    const std::vector<std::pair<const char*, nlohmann::json>> bad = {
        {"http URL", {{"urls", {"http://example.com/php.zip"}}}},
        {"URL with a quote", {{"urls", {"https://example.com/a\"b"}}}},
        {"URL with a space", {{"urls", {"https://example.com/a b"}}}},
        {"URL with user info", {{"urls", {"https://evil@mirror.hostkurd.net/php.zip"}}}},
        {"URL with an empty host", {{"urls", {"https:///php.zip"}}}},
        {"URL with only user info", {{"urls", {"https://@x/php.zip"}}}},
        {"URL with a bad port", {{"urls", {"https://example.com:99999/a"}}}},
        {"URL with port 0", {{"urls", {"https://example.com:0/a"}}}},
        {"URL with a fragment", {{"urls", {"https://example.com/a#b"}}}},
        {"URL with an encoded NUL", {{"urls", {"https://example.com/a%00.zip"}}}},
        {"URL with an upper-case host", {{"urls", {"https://Example.com/a"}}}},
        {"URL with a dotted host end", {{"urls", {"https://example.com./a"}}}},
        {"http URL first", {{"urls", {"http://example.com/a", "https://example.com/a"}}}},
        {"size over 4 GiB", {{"size", 4ULL * 1024 * 1024 * 1024 + 1}}},
        {"no URLs", {{"urls", nlohmann::json::array()}}},
        {"short hash", {{"sha256", "abcd"}}},
        {"non-hex hash", {{"sha256", std::string(64, 'g')}}},
        {"zero size", {{"size", 0}}},
        {"negative size", {{"size", -5}}},
        {"bad version", {{"version", "latest"}}},
        {"bad name", {{"name", "PHP"}}},
        {"bad archive", {{"archive", "exe"}}},
        {"no licence", {{"license", ""}}},
    };
    for (const auto& [what, patch] : bad) {
        CAPTURE(what);
        auto doc = good_doc();
        doc["runtimes"][1].merge_patch(patch);
        const auto r = open_signed(doc);
        REQUIRE(r.manifest.has_value());
        CHECK(r.manifest->runtimes.size() == 3);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].find("entry 1") != std::string::npos);
    }
}

TEST_CASE("a single file such as composer.phar is allowed") {
    auto doc = good_doc();
    doc["runtimes"][0]["archive"] = "file";
    const auto r = open_signed(doc);
    REQUIRE(r.manifest.has_value());
    CHECK(r.warnings.empty());
    CHECK(r.manifest->runtimes[0].archive == "file");
}

TEST_CASE("well-formed URLs pass") {
    auto doc = good_doc();
    doc["runtimes"][0]["urls"] = {"https://mirror.hostkurd.net/php/php-8.2.27-nts-x64.zip",
                                  "https://windows.php.net:443/downloads/releases/php-8.2.27.zip",
                                  "https://a-b.c/x?y=1&z=%20"};
    const auto r = open_signed(doc);
    REQUIRE(r.manifest.has_value());
    CHECK(r.warnings.empty());
    CHECK(r.manifest->runtimes[0].urls.size() == 3);
}

TEST_CASE("signatures of the wrong length or non-canonical form are refused") {
    const std::string bytes = good_doc().dump(2);
    const auto sig_json = nlohmann::json::parse(kKey.sign(bytes, "hostkurd-2026"));
    auto sig = *from_base64(sig_json["signature"].get<std::string>());
    for (std::size_t len : {std::size_t{63}, std::size_t{65}}) {
        CAPTURE(len);
        auto wrong = sig;
        wrong.resize(len);
        const std::string file =
            nlohmann::json{{"key", "hostkurd-2026"}, {"signature", to_base64(wrong)}}.dump();
        CHECK(open_manifest(bytes, file, default_check()).error == ManifestError::BadSignatureFile);
    }
    // S + L (the group order) is the same signature in another spelling:
    // RFC 8032 requires it to be refused, so signatures can't be altered.
    constexpr std::array<std::uint8_t, 32> kL = {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
        0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    unsigned carry = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        const unsigned v = sig[32 + i] + kL[i] + carry;
        sig[32 + i] = static_cast<std::uint8_t>(v);
        carry = v >> 8U;
    }
    const std::string file =
        nlohmann::json{{"key", "hostkurd-2026"}, {"signature", to_base64(sig)}}.dump();
    CHECK(open_manifest(bytes, file, default_check()).error == ManifestError::BadSignature);
}

TEST_CASE("a list without runtimes opens with a warning") {
    auto doc = good_doc();
    doc.erase("runtimes");
    auto r = open_signed(doc);
    REQUIRE(r.manifest.has_value());
    CHECK(r.warnings.size() == 1);
    doc["runtimes"] = "php";
    r = open_signed(doc);
    REQUIRE(r.manifest.has_value());
    CHECK(r.warnings.size() == 1);
}

TEST_CASE("runtimes_for doesn't crash on a hand-built manifest") {
    Manifest m;
    RuntimeFile bad;
    bad.name = "php";
    bad.platform = "windows-x64";
    bad.version = "not a version";
    RuntimeFile good = bad;
    good.version = "8.4.1";
    m.runtimes = {bad, good};
    const auto list = runtimes_for(m, "php", "windows-x64");
    REQUIRE(list.size() == 2);
    CHECK(list[0].version == "8.4.1");
}

TEST_CASE("unknown keys are ignored for newer manifests") {
    auto doc = good_doc();
    doc["templates"] = nlohmann::json::array();
    doc["runtimes"][0]["checksum512"] = "x";
    const auto r = open_signed(doc);
    REQUIRE(r.manifest.has_value());
    CHECK(r.warnings.empty());
}

TEST_CASE("runtimes_for picks one platform, newest first") {
    const auto r = open_signed(good_doc());
    REQUIRE(r.manifest.has_value());
    const auto php = runtimes_for(*r.manifest, "php", "windows-x64");
    REQUIRE(php.size() == 2);
    CHECK(php[0].version == "8.4.14");
    CHECK(php[1].version == "8.2.27");
    CHECK(runtimes_for(*r.manifest, "node", "windows-x64").empty());
}

TEST_CASE("a download must match both size and hash") {
    const auto r = open_signed(good_doc());
    REQUIRE(r.manifest.has_value());
    const RuntimeFile& e = r.manifest->runtimes[0];
    CHECK(matches_entry(e, 1234, e.sha256));
    CHECK_FALSE(matches_entry(e, 1235, e.sha256));
    Sha256Digest other = e.sha256;
    other[31] ^= 1;
    CHECK_FALSE(matches_entry(e, 1234, other));
}

TEST_CASE("UTC times are read strictly") {
    CHECK(parse_utc_time("2028-02-29T23:59:59Z").has_value());
    for (const char* bad : {"2026-02-29T00:00:00Z", "2026-13-01T00:00:00Z", "2026-12-01T24:00:00Z",
                            "2026-12-01T00:60:00Z", "2026-12-01t00:00:00Z", "2026-12-01T00:00:00z",
                            "2026-12-01T00:00:00+03:00", "2026-12-01 00:00:00Z", "2026-12-1T00:00:00Z",
                            ""}) {
        CAPTURE(bad);
        CHECK_FALSE(parse_utc_time(bad).has_value());
    }
}

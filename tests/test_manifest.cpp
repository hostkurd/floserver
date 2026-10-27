#include "floserver/core/encoding.h"
#include "floserver/core/manifest.h"

#include <doctest/doctest.h>
#include <monocypher-ed25519.h>
#include <nlohmann/json.hpp>

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
    c.last_sequence = 5;
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
    check.last_sequence = 8;
    CHECK(open_signed(doc, check).error == ManifestError::Rollback);
    check.last_sequence = 7;  // the same one again is fine
    CHECK(open_signed(doc, check).manifest.has_value());
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
    doc = good_doc();
    doc["expires"] = "2026-12-01";
    CHECK(open_signed(doc).error == ManifestError::BadField);

    const std::string not_json = "this is signed but not JSON";
    CHECK(open_manifest(not_json, kKey.sign(not_json, "hostkurd-2026"), default_check()).error ==
          ManifestError::NotJson);
}

TEST_CASE("a bad entry is skipped with a warning, the rest still load") {
    const std::vector<std::pair<const char*, nlohmann::json>> bad = {
        {"http URL", {{"urls", {"http://example.com/php.zip"}}}},
        {"URL with a quote", {{"urls", {"https://example.com/a\"b"}}}},
        {"URL with a space", {{"urls", {"https://example.com/a b"}}}},
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

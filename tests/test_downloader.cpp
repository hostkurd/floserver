#include "floserver/core/downloader.h"
#include "floserver/core/http.h"

#include "test_util.h"

#include <doctest/doctest.h>

using namespace floserver::core;
using namespace test_util;
using namespace std::chrono;

namespace {

const SigningKey kKey(0x31);
const std::string kKeyId = "test-2026";
const std::string kListUrl = "https://cdn.example.org/manifest.json";
const std::string kSigUrl = "https://cdn.example.org/manifest.json.sig";

sys_seconds at(const char* text) {
    const auto t = parse_utc_time(text);
    REQUIRE(t.has_value());
    return *t;
}

std::string list_bytes(std::int64_t sequence, const char* expires = "2026-12-01T00:00:00Z") {
    const nlohmann::json doc = {
        {"schema", 1},
        {"sequence", sequence},
        {"expires", expires},
        {"runtimes",
         {{{"name", "php"},
           {"version", "8.5.12"},
           {"platform", "windows-x64"},
           {"archive", "zip"},
           {"size", 10},
           {"sha256", std::string(64, 'a')},
           {"license", "PHP-3.01"},
           {"urls", {"https://cdn.example.org/php.zip"}}}}}};
    return doc.dump(2);
}

ManifestFetchOptions options(int* sleeps = nullptr) {
    ManifestFetchOptions o;
    o.check.trusted_keys = {{kKeyId, kKey.pub}};
    o.check.now = at("2026-10-28T12:00:00Z");
    o.sleep = [sleeps](milliseconds) {
        if (sleeps != nullptr) {
            ++*sleeps;
        }
    };
    return o;
}

const ManifestSource kSource{kListUrl, kSigUrl};

RuntimeFile file_entry(const std::string& content, std::vector<std::string> urls) {
    RuntimeFile e;
    e.name = "php";
    e.version = "8.5.12";
    e.platform = "windows-x64";
    e.archive = "zip";
    e.size = content.size();
    e.sha256 = sha256(content);
    e.urls = std::move(urls);
    return e;
}

}  // namespace

TEST_CASE("HTTP dates are read in the one format servers must send") {
    const auto t = parse_http_date("Wed, 28 Oct 2026 19:12:28 GMT");
    REQUIRE(t.has_value());
    CHECK(*t == at("2026-10-28T19:12:28Z"));
    CHECK_FALSE(parse_http_date("Wed, 28 Oct 2026 19:12:28 UTC").has_value());
    CHECK_FALSE(parse_http_date("Wednesday, 28-Oct-26 19:12:28 GMT").has_value());
    CHECK_FALSE(parse_http_date("Wed, 31 Feb 2026 19:12:28 GMT").has_value());
    CHECK_FALSE(parse_http_date("Wed, 28 Okt 2026 19:12:28 GMT").has_value());
    CHECK_FALSE(parse_http_date("Wed, 28 Oct 2026 24:12:28 GMT").has_value());
    CHECK_FALSE(parse_http_date("").has_value());
}

TEST_CASE("a signed list is fetched and opened") {
    FakeTransport t;
    const std::string bytes = list_bytes(3);
    t.serve(kListUrl, {200, bytes});
    t.serve(kSigUrl, {200, kKey.sign(bytes, kKeyId)});
    const auto f = fetch_manifest(t, kSource, options());
    INFO(f.message);
    REQUIRE(f.manifest.has_value());
    CHECK(f.error == FetchError::None);
    CHECK(f.key_id == kKeyId);
    CHECK(f.manifest->sequence == 3);
    CHECK(f.manifest->runtimes.size() == 1);
}

TEST_CASE("a list and signature that don't match yet are retried once") {
    FakeTransport t;
    const std::string old_bytes = list_bytes(3);
    const std::string new_bytes = list_bytes(4);
    // The cache still has the old signature on the first visit.
    t.serve(kListUrl, {200, new_bytes});
    t.serve_sequence(kSigUrl, {{200, kKey.sign(old_bytes, kKeyId)}, {200, kKey.sign(new_bytes, kKeyId)}});
    int sleeps = 0;
    const auto f = fetch_manifest(t, kSource, options(&sleeps));
    INFO(f.message);
    REQUIRE(f.manifest.has_value());
    CHECK(f.manifest->sequence == 4);
    CHECK(sleeps == 1);
    CHECK(t.requests(kListUrl) == 2);
}

TEST_CASE("a signature that stays bad fails after one retry") {
    FakeTransport t;
    const std::string bytes = list_bytes(3);
    t.serve(kListUrl, {200, bytes});
    t.serve(kSigUrl, {200, kKey.sign(list_bytes(99), kKeyId)});
    int sleeps = 0;
    const auto f = fetch_manifest(t, kSource, options(&sleeps));
    CHECK_FALSE(f.manifest.has_value());
    CHECK(f.error == FetchError::Manifest);
    CHECK(f.manifest_error == ManifestError::BadSignature);
    CHECK(sleeps == 1);
    CHECK(t.requests(kSigUrl) == 2);
}

TEST_CASE("network and server failures are reported, without retries") {
    FakeTransport t;
    t.serve(kSigUrl, {0, "", "the server's name couldn't be found"});
    auto f = fetch_manifest(t, kSource, options());
    CHECK(f.error == FetchError::Network);
    CHECK(f.message.find("couldn't be found") != std::string::npos);

    t.serve(kSigUrl, {200, kKey.sign(list_bytes(3), kKeyId)});
    t.serve(kListUrl, {404, ""});
    f = fetch_manifest(t, kSource, options());
    CHECK(f.error == FetchError::Http);
    CHECK(f.message.find("404") != std::string::npos);
    CHECK(t.requests(kListUrl) == 1);
}

TEST_CASE("an oversized list or signature is stopped while it arrives") {
    FakeTransport t;
    t.serve(kSigUrl, {200, std::string(kMaxSignatureBytes + 1, 'x')});
    auto f = fetch_manifest(t, kSource, options());
    CHECK(f.error == FetchError::TooBig);
    CHECK(t.bytes_sent() <= kMaxSignatureBytes + 7);

    t.serve(kSigUrl, {200, kKey.sign("x", kKeyId)});
    t.serve(kListUrl, {200, std::string(kMaxManifestBytes + 100, ' ')});
    f = fetch_manifest(t, kSource, options());
    CHECK(f.error == FetchError::TooBig);
}

TEST_CASE("an expired list says whether the clock or the server is at fault") {
    FakeTransport t;
    const std::string bytes = list_bytes(3, "2026-10-01T00:00:00Z");
    t.serve(kSigUrl, {200, kKey.sign(bytes, kKeyId)});

    SUBCASE("the server agrees with our clock: the list is really stale") {
        t.serve(kListUrl, {200, bytes, "", at("2026-10-28T12:05:00Z")});
        const auto f = fetch_manifest(t, kSource, options());
        CHECK(f.manifest_error == ManifestError::Expired);
        CHECK_FALSE(f.clock_wrong);
        CHECK(f.message.find("our side") != std::string::npos);
    }
    SUBCASE("our clock is far ahead of the server's") {
        t.serve(kListUrl, {200, bytes, "", at("2026-09-20T08:00:00Z")});
        const auto f = fetch_manifest(t, kSource, options());
        CHECK(f.manifest_error == ManifestError::Expired);
        CHECK(f.clock_wrong);
        CHECK(f.message.find("2026-10-28 12:00 UTC") != std::string::npos);
        CHECK(f.message.find("2026-09-20 08:00 UTC") != std::string::npos);
    }
    SUBCASE("no Date header") {
        t.serve(kListUrl, {200, bytes});
        const auto f = fetch_manifest(t, kSource, options());
        CHECK_FALSE(f.clock_wrong);
    }
}

TEST_CASE("the last sequence is kept per key and never lowered") {
    const TempDir tmp;
    const auto file = tmp.path() / "state" / "manifest-state.json";
    CHECK(load_last_sequences(file).empty());

    std::string error;
    REQUIRE(store_last_sequence(file, "a", 5, error));
    REQUIRE(store_last_sequence(file, "a", 3, error));
    REQUIRE(store_last_sequence(file, "b", 1, error));
    const auto all = load_last_sequences(file);
    CHECK(all.size() == 2);
    CHECK(all.at("a") == 5);
    CHECK(all.at("b") == 1);
    REQUIRE(store_last_sequence(file, "a", 9, error));
    CHECK(load_last_sequences(file).at("a") == 9);

    write_file(file, "{not json");
    CHECK(load_last_sequences(file).empty());
    write_file(file, R"({"last_sequences": {"a": -4, "b": "7", "c": 2}})");
    const auto filtered = load_last_sequences(file);
    CHECK(filtered.size() == 1);
    CHECK(filtered.at("c") == 2);
}

TEST_CASE("a file downloads, matches its hash and lands in place") {
    const TempDir tmp;
    FakeTransport t;
    const std::string content = "the php zip, honest";
    t.serve("https://cdn.example.org/php.zip", {200, content});
    const auto dest = tmp.path() / "downloads" / "php.zip";
    std::uint64_t previous = 0;
    int calls = 0;
    DownloadOptions o;
    o.progress = [&](const DownloadProgress& p) {
        CHECK(p.received > previous);
        CHECK(p.total == content.size());
        previous = p.received;
        ++calls;
        return true;
    };
    const auto r = download_file(t, file_entry(content, {"https://cdn.example.org/php.zip"}), dest, o);
    INFO(r.message);
    REQUIRE(r.ok);
    CHECK(read_file(dest) == content);
    fs::path part = dest;
    part += ".part";
    CHECK_FALSE(fs::exists(part));
    CHECK(previous == content.size());
    CHECK(calls == 3);  // 7-byte chunks
}

TEST_CASE("a bad mirror is skipped for the next address") {
    const TempDir tmp;
    FakeTransport t;
    const std::string content = "the real file";
    const auto entry = file_entry(content, {"https://cdn.example.org/a.zip", "https://upstream.example.org/a.zip"});
    const auto dest = tmp.path() / "a.zip";

    SUBCASE("wrong content of the right size") {
        t.serve(entry.urls[0], {200, std::string(content.size(), 'x')});
    }
    SUBCASE("more bytes than the list says: stopped early") {
        t.serve(entry.urls[0], {200, content + std::string(10000, 'x')});
    }
    SUBCASE("cut short") {
        t.serve(entry.urls[0], {200, content.substr(0, 4)});
    }
    SUBCASE("not found") {
        t.serve(entry.urls[0], {404, ""});
    }
    SUBCASE("no connection") {
        t.serve(entry.urls[0], {0, "", "timed out"});
    }
    t.serve(entry.urls[1], {200, content});
    const auto r = download_file(t, entry, dest);
    INFO(r.message);
    REQUIRE(r.ok);
    CHECK(read_file(dest) == content);
    CHECK(r.attempts.size() == 2);
    CHECK(t.bytes_sent() < 2 * content.size() + 14);
}

TEST_CASE("when every address fails, nothing is left behind") {
    const TempDir tmp;
    FakeTransport t;
    const std::string content = "the real file";
    const auto entry = file_entry(content, {"https://a.example.org/f", "https://b.example.org/f"});
    t.serve(entry.urls[0], {200, "wrong wrong!!"});
    t.serve(entry.urls[1], {503, ""});
    const auto dest = tmp.path() / "f";
    const auto r = download_file(t, entry, dest);
    CHECK_FALSE(r.ok);
    CHECK(r.message.find("503") != std::string::npos);
    CHECK(r.attempts.size() == 2);
    CHECK(r.attempts[0].find("SHA-256") != std::string::npos);
    CHECK_FALSE(fs::exists(dest));
    CHECK(fs::is_empty(tmp.path()));
}

TEST_CASE("a cancelled download stops and deletes its part file") {
    const TempDir tmp;
    FakeTransport t;
    const std::string content(1000, 'z');
    const auto entry = file_entry(content, {"https://cdn.example.org/big"});
    t.serve(entry.urls[0], {200, content});
    DownloadOptions o;
    o.progress = [](const DownloadProgress& p) { return p.received < 50; };
    const auto r = download_file(t, entry, tmp.path() / "big", o);
    CHECK_FALSE(r.ok);
    CHECK(r.cancelled);
    CHECK(t.bytes_sent() < 100);
    CHECK(fs::is_empty(tmp.path()));
}

TEST_CASE("a file already downloaded is kept; a wrong one is replaced") {
    const TempDir tmp;
    FakeTransport t;
    const std::string content = "cached content";
    const auto entry = file_entry(content, {"https://cdn.example.org/c"});
    t.serve(entry.urls[0], {200, content});
    const auto dest = tmp.path() / "c";

    write_file(dest, content);
    REQUIRE(download_file(t, entry, dest).ok);
    CHECK(t.requests(entry.urls[0]) == 0);

    write_file(dest, "stale");
    REQUIRE(download_file(t, entry, dest).ok);
    CHECK(t.requests(entry.urls[0]) == 1);
    CHECK(read_file(dest) == content);
}

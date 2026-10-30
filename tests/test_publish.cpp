#include "build.h"
#include "publish.h"
#include "signing.h"
#include "test_util.h"

#include "floserver/core/encoding.h"
#include "floserver/core/manifest.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

using namespace floserver::manifest_tool;
using floserver::core::TrustedKey;
using test_util::FakeTransport;
using test_util::sha256_hex;

namespace {

const std::string kTestCdn = "https://cdn.floserver.org";

std::chrono::sys_seconds at(const char* text) { return *floserver::core::parse_utc_time(text); }

// A fake CDN with one file of each runtime the built-in templates need.
struct FakeCdn {
    FakeTransport http;
    test_util::SigningKey key{0x21};
    std::vector<PublishFile> files;

    FakeCdn() {
        add("php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip", "php 8.5.12 zip");
        add("mariadb/11.4.13/mariadb-11.4.13-winx64.zip", "mariadb zip");
        add("node/24.22.1/node-v24.22.1-win-x64.zip", "node zip");
        add("composer/2.10.3/composer.phar", "composer phar");
        add("nginx/1.30.6/nginx-1.30.6.zip", "nginx zip");
        add("apache/2.4.69/httpd-2.4.69-261002-Win64-VS18.zip", "apache zip");
        add("wordpress/7.1.4/wordpress-7.1.4.zip", "wordpress zip");
        add("python/3.13.18/cpython-3.13.18-20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz",
            "python tarball");
    }

    void add(const std::string& path, const std::string& body) {
        http.serve(kTestCdn + "/" + path, {200, body});
        PublishFile f;
        f.path = path;
        f.title = path;
        f.size = body.size();
        f.sha256 = sha256_hex(body);
        f.state = CheckState::Ok;
        files.push_back(f);
    }

    [[nodiscard]] std::vector<TrustedKey> keys() const { return {{"test-key", key.pub}}; }

    // Publishes a list with `sequence` made from `files`.
    void publish(std::int64_t sequence, std::chrono::sys_seconds now, int days = 90) {
        const BuiltList built = build_next_list(kTestCdn, files, sequence, now, days);
        REQUIRE_MESSAGE(built.manifest_json, (built.errors.empty() ? "" : built.errors.front()));
        http.serve(kTestCdn + "/manifest.json", {200, *built.manifest_json});
        http.serve(kTestCdn + "/manifest.json.sig", {200, key.sign(*built.manifest_json, "test-key")});
    }

    [[nodiscard]] std::string key_text() const {
        std::array<std::uint8_t, 32> seed{};
        seed.fill(0x21);
        return "test-key\n" + floserver::core::to_base64(seed) + "\n";
    }
};

}  // namespace

TEST_CASE("publisher: read the live list, add a new PHP, check, build and sign the next list") {
    FakeCdn cdn;
    const auto now = at("2026-10-30T12:00:00Z");
    cdn.publish(3, now - std::chrono::days{1});

    const LiveResult live = fetch_live_list(cdn.http, kTestCdn, cdn.keys(), now);
    REQUIRE_MESSAGE(live.list, live.error);
    CHECK(live.list->sequence == 3);
    CHECK_FALSE(live.list->expired);
    CHECK(live.list->runtimes.size() == 8);
    CHECK(live.list->template_count == 8);

    std::vector<std::string> notes;
    std::vector<PublishFile> files = files_from_live(*live.list, kTestCdn, notes);
    CHECK(notes.empty());
    REQUIRE(files.size() == 8);
    CHECK(files.front().published);
    CHECK(files.front().title == "PHP 8.5.12");

    // The user uploaded PHP 8.5.13 and pastes its address.
    const std::string php = "php/8.5.13/php-8.5.13-nts-Win32-vs17-x64.zip";
    const std::string body = "the new php zip";
    cdn.http.serve(kTestCdn + "/" + php, {200, body});
    cdn.http.serve("https://downloads.php.net/~windows/releases/sha256sum.txt",
                   {200, sha256_hex(body) + "  php-8.5.13-nts-Win32-vs17-x64.zip\n" +
                             std::string(64, 'a') + "  php-debug-pack-8.5.13-nts-Win32-vs17-x64.zip\n"});
    const AddResult added = new_file("  " + kTestCdn + "/" + php + "\r\n", kTestCdn, files);
    REQUIRE_MESSAGE(added.file, added.error);
    CHECK(added.file->path == php);
    CHECK(added.file->title == "PHP 8.5.13");
    CHECK_FALSE(added.file->published);
    files.push_back(*added.file);

    CheckContext ctx{cdn.http, kTestCdn};
    std::uint64_t seen = 0;
    ctx.progress = [&](std::uint64_t done) { seen = done; };
    for (PublishFile& f : files) {
        check_file(ctx, f);
        INFO(f.path << ": " << f.detail);
        CHECK(f.state == CheckState::Ok);
    }
    CHECK(seen == body.size());
    CHECK(files.back().size == body.size());
    CHECK(files.back().sha256 == sha256_hex(body));
    CHECK(files.back().detail == "Matches downloads.php.net");
    CHECK(files.front().detail == "Still served");
    // A published file is only asked for, not downloaded again.
    CHECK(cdn.http.requests(kTestCdn + "/" + files.front().path) == 1);

    const BuiltList next = build_next_list(kTestCdn, files, live.list->sequence + 1, now);
    REQUIRE_MESSAGE(next.manifest_json, (next.errors.empty() ? "" : next.errors.front()));
    CHECK(next.runtime_count == 9);
    CHECK(next.template_count == 8);
    CHECK(next.sequence == 4);
    CHECK(next.expires == now + std::chrono::days{90});

    std::string text = cdn.key_text();
    SecretKey key;
    REQUIRE(parse_secret_key(text, key));
    CHECK(text.empty());  // the key file's text is wiped
    const SignResult signed_list = sign_manifest(*next.manifest_json, key, now, cdn.keys());
    REQUIRE_MESSAGE(signed_list.signature_file, signed_list.error);
    CHECK(signed_list.sequence == 4);
    CHECK(signed_list.runtime_count == 9);

    floserver::core::ManifestCheck check;
    check.trusted_keys = cdn.keys();
    check.now = now;
    check.last_sequences = {{"test-key", 3}};
    const auto opened = floserver::core::open_manifest(*next.manifest_json, *signed_list.signature_file, check);
    REQUIRE(opened.manifest);
    CHECK(opened.manifest->sequence == 4);
}

TEST_CASE("publisher: a new file that differs, or has no official checksum") {
    FakeCdn cdn;
    const std::string node = "node/24.23.0/node-v24.23.0-win-x64.zip";
    cdn.http.serve(kTestCdn + "/" + node, {200, "tampered"});
    cdn.http.serve("https://nodejs.org/dist/v24.23.0/SHASUMS256.txt",
                   {200, sha256_hex("official") + "  node-v24.23.0-win-x64.zip\n"});
    CheckContext ctx{cdn.http, kTestCdn};

    PublishFile f;
    f.path = node;
    f.title = "Node.js 24.23.0";
    check_file(ctx, f);
    CHECK(f.state == CheckState::Failed);
    CHECK(f.detail.find("DIFFERENT") != std::string::npos);

    // Not in the checksum list: compare by hand.
    cdn.http.serve("https://nodejs.org/dist/v24.23.0/SHASUMS256.txt", {200, "nothing here\n"});
    ctx.sums_cache.clear();
    check_file(ctx, f);
    CHECK(f.state == CheckState::ByHand);
    CHECK(f.detail.find(sha256_hex("tampered")) != std::string::npos);

    // Not uploaded (S3 answers 403 through CloudFront).
    PublishFile missing;
    missing.path = "node/24.24.0/node-v24.24.0-win-x64.zip";
    cdn.http.serve(kTestCdn + "/" + missing.path, {403, ""});
    check_file(ctx, missing);
    CHECK(missing.state == CheckState::Failed);
    CHECK(missing.detail.find("HTTP 403") != std::string::npos);

    // Apache Lounge: always by hand, and the list needs it ticked.
    const std::string apache = "apache/2.4.70/httpd-2.4.70-261101-Win64-VS18.zip";
    cdn.http.serve(kTestCdn + "/" + apache, {200, "apache 2.4.70"});
    PublishFile a;
    a.path = apache;
    a.title = "Apache 2.4.70";
    check_file(ctx, a);
    CHECK(a.state == CheckState::ByHand);

    std::vector<PublishFile> files = cdn.files;
    std::erase_if(files, [](const PublishFile& p) { return p.path.starts_with("apache/"); });
    files.push_back(a);
    const auto now = at("2026-10-30T12:00:00Z");
    BuiltList built = build_next_list(kTestCdn, files, 5, now);
    CHECK_FALSE(built.manifest_json);
    REQUIRE(built.errors.size() == 1);
    CHECK(built.errors.front().find("by hand") != std::string::npos);
    files.back().accepted = true;
    built = build_next_list(kTestCdn, files, 5, now);
    CHECK(built.manifest_json);

    // A published file the CDN no longer serves at its size.
    PublishFile pub = cdn.files.front();
    pub.published = true;
    pub.size += 1;
    check_file(ctx, pub);
    CHECK(pub.state == CheckState::Failed);
    CHECK(pub.detail.find("different size") != std::string::npos);
}

TEST_CASE("publisher: nginx and WordPress are compared with their original downloads") {
    FakeCdn cdn;
    const std::string nginx = "nginx/1.30.7/nginx-1.30.7.zip";
    cdn.http.serve(kTestCdn + "/" + nginx, {200, "nginx 1.30.7"});
    cdn.http.serve("https://nginx.org/download/nginx-1.30.7.zip", {200, "nginx 1.30.7"});
    CheckContext ctx{cdn.http, kTestCdn};
    PublishFile f;
    f.path = nginx;
    check_file(ctx, f);
    CHECK(f.state == CheckState::Ok);
    CHECK(f.detail == "Matches the original from nginx.org");

    const OfficialSource wp = official_source("wordpress/7.1.5/wordpress-7.1.5.zip");
    CHECK(wp.kind == OfficialSource::Kind::Original);
    CHECK(wp.urls == std::vector<std::string>{"https://wordpress.org/wordpress-7.1.5.zip"});

    const OfficialSource py = official_source(
        "python/3.13.19/cpython-3.13.19-20261120-x86_64-pc-windows-msvc-install_only_stripped.tar.gz");
    CHECK(py.kind == OfficialSource::Kind::Sums);
    CHECK(py.name == "cpython-3.13.19+20261120-x86_64-pc-windows-msvc-install_only_stripped.tar.gz");
    CHECK(py.urls == std::vector<std::string>{
                         "https://github.com/astral-sh/python-build-standalone/releases/download/20261120/SHA256SUMS"});
    CHECK(official_source("apache/2.4.69/httpd-2.4.69-261002-Win64-VS18.zip").kind ==
          OfficialSource::Kind::ByHand);
}

TEST_CASE("publisher: official checksums are read from the usual list formats") {
    const std::string h = std::string(63, 'a') + "b";
    CHECK(find_official_hash(h + "  file.zip\n", "file.zip") == h);
    CHECK(find_official_hash("SHA256 (file.zip) = " + h + "\r\n", "file.zip") == h);
    CHECK(find_official_hash(std::string(64, 'C') + " *file.zip", "file.zip") == std::string(64, 'c'));
    // Only the whole name counts.
    CHECK_FALSE(find_official_hash(h + "  file.zip.asc\n" + h + "  debug-file.zip\n", "file.zip"));
    // composer.phar.sha256sum: just the hash.
    CHECK(find_official_hash(h + "\n", "composer.phar") == h);
    CHECK(find_official_hash(h + "  composer.phar\n", "composer.phar") == h);
    CHECK_FALSE(find_official_hash(h + "\n" + h + "\n", "composer.phar"));
    CHECK_FALSE(find_official_hash("", "file.zip"));
}

TEST_CASE("publisher: new paths are checked before anything is downloaded") {
    const FakeCdn cdn;
    std::vector<PublishFile> files = cdn.files;
    for (PublishFile& f : files) {
        const CdnPath p = describe_cdn_path(f.path);
        f.title = runtime_title(p.runtime, p.version);
    }
    CHECK(new_file("", kTestCdn, files).error.find("Type") != std::string::npos);
    CHECK(new_file("/php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip", kTestCdn, files).error ==
          "That file is already in the list.");
    CHECK(new_file("php/8.5.12/php-8.5.12-2-nts-Win32-vs17-x64.zip", kTestCdn, files)
              .error.find("Remove it first") != std::string::npos);
    CHECK(new_file("mariadb/11.4.14/mariadb-11.4.14.tar.gz", kTestCdn, files)
              .error.find("source archive") != std::string::npos);
    CHECK(new_file("ruby/3.4.0/ruby.zip", kTestCdn, files).error.find("isn't a runtime") != std::string::npos);
    CHECK(new_file("python/3.13.19/cpython-3.13.19+20261120-x86_64-pc-windows-msvc-install_only_stripped.tar.gz",
                   kTestCdn, files)
              .error.find("'+'") != std::string::npos);
    CHECK(new_file("php/8.5.13/php-8.5.13-Win32-vs17-x64.zip", kTestCdn, files)
              .error.find("another build") != std::string::npos);
    const AddResult ok = new_file("composer/2.11.0/composer.phar", kTestCdn, files);
    REQUIRE(ok.file);
    CHECK(ok.file->title == "Composer 2.11.0");
}

TEST_CASE("publisher: an expired live list is still read, a foreign one isn't") {
    FakeCdn cdn;
    cdn.publish(7, at("2026-07-01T00:00:00Z"), 30);
    const LiveResult live = fetch_live_list(cdn.http, kTestCdn, cdn.keys(), at("2026-10-30T12:00:00Z"));
    REQUIRE_MESSAGE(live.list, live.error);
    CHECK(live.list->expired);
    CHECK(live.list->sequence == 7);

    const test_util::SigningKey other(0x33);
    const LiveResult foreign =
        fetch_live_list(cdn.http, kTestCdn, {{"test-key", other.pub}}, at("2026-10-30T12:00:00Z"));
    CHECK_FALSE(foreign.list);
    CHECK(foreign.error.find("doesn't open") != std::string::npos);

    FakeTransport nothing;
    const LiveResult offline = fetch_live_list(nothing, kTestCdn, cdn.keys(), at("2026-10-30T12:00:00Z"));
    CHECK_FALSE(offline.list);
    CHECK(offline.error.find("Can't read the live list") != std::string::npos);
}

TEST_CASE("publisher: signing refuses a key FloServer doesn't trust and a damaged key file") {
    FakeCdn cdn;
    const auto now = at("2026-10-30T12:00:00Z");
    const BuiltList built = build_next_list(kTestCdn, cdn.files, 1, now);
    REQUIRE(built.manifest_json);

    std::string text = cdn.key_text();
    SecretKey key;
    REQUIRE(parse_secret_key(text, key));
    const test_util::SigningKey other(0x33);
    const SignResult wrong = sign_manifest(*built.manifest_json, key, now, {{"test-key", other.pub}});
    CHECK_FALSE(wrong.signature_file);
    CHECK(wrong.error.find("right key file") != std::string::npos);

    // Notepad's \r\n is fine; anything else isn't.
    std::string crlf = "test-key\r\n" + cdn.key_text().substr(9, 44) + "\r\n";
    SecretKey k2;
    CHECK(parse_secret_key(crlf, k2));
    CHECK(k2.public_key == cdn.key.pub);
    std::string damaged = "test-key\nnot base64\n";
    SecretKey k3;
    CHECK_FALSE(parse_secret_key(damaged, k3));
    CHECK(damaged.empty());
    std::string one_line = "test-key";
    CHECK_FALSE(parse_secret_key(one_line, k3));
}

TEST_CASE("publisher: the bucket must refuse direct reads") {
    FakeTransport http;
    const std::string s3(kDirectS3);
    http.serve(s3 + "/composer/2.10.3/composer.phar", {403, ""});
    CHECK(check_direct_s3(http, s3, "composer/2.10.3/composer.phar").empty());
    http.serve(s3 + "/composer/2.10.3/composer.phar", {200, "phar"});
    CHECK(check_direct_s3(http, s3, "composer/2.10.3/composer.phar").find("make it private") !=
          std::string::npos);
    CHECK(check_direct_s3(http, s3, "nothing").find("by hand") != std::string::npos);
}

TEST_CASE("publisher: sizes read like the window shows them") {
    CHECK(format_size(36310917) == "36.3 MB");
    CHECK(format_size(2773293) == "2.8 MB");
    CHECK(format_size(512000) == "512 KB");
}

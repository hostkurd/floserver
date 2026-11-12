#include "build.h"
#include "publish.h"
#include "signing.h"
#include "templates_json.h"
#include "test_util.h"

#include "floserver/core/encoding.h"
#include "floserver/core/manifest.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>

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

    // Not in the checksum list, or the list can't be read: it fails (never
    // "by hand"), and a list that couldn't be read is asked for again.
    cdn.http.serve("https://nodejs.org/dist/v24.23.0/SHASUMS256.txt", {200, "nothing here\n"});
    ctx.sums_cache.clear();
    check_file(ctx, f);
    CHECK(f.state == CheckState::Failed);
    CHECK(f.detail.find("doesn't name node-v24.23.0-win-x64.zip") != std::string::npos);
    ctx.sums_cache.clear();
    cdn.http.serve("https://nodejs.org/dist/v24.23.0/SHASUMS256.txt", {503, ""});
    check_file(ctx, f);
    CHECK(f.state == CheckState::Failed);
    CHECK(f.detail.find("Can't read the official checksum list") != std::string::npos);
    cdn.http.serve("https://nodejs.org/dist/v24.23.0/SHASUMS256.txt",
                   {200, sha256_hex("tampered") + "  node-v24.23.0-win-x64.zip\n"});
    check_file(ctx, f);
    CHECK(f.state == CheckState::Ok);

    // nginx's original can't be downloaded: fails too.
    PublishFile ng;
    ng.path = "nginx/1.30.8/nginx-1.30.8.zip";
    cdn.http.serve(kTestCdn + "/" + ng.path, {200, "nginx"});
    check_file(ctx, ng);
    CHECK(ng.state == CheckState::Failed);
    CHECK(ng.detail.find("Can't download the original") != std::string::npos);

    // Not uploaded (R2 answers 404; a private bucket would answer 403).
    PublishFile missing;
    missing.path = "node/24.24.0/node-v24.24.0-win-x64.zip";
    cdn.http.serve(kTestCdn + "/" + missing.path, {404, ""});
    check_file(ctx, missing);
    CHECK(missing.state == CheckState::Failed);
    CHECK(missing.detail.find("HTTP 404") != std::string::npos);
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
    CHECK(official_source("ruby/3.4.0/ruby.zip").kind == OfficialSource::Kind::Unknown);
}

TEST_CASE("publisher: phpMyAdmin is checked against its own .sha256 and listed for any platform") {
    FakeCdn cdn;
    const std::string path = "phpmyadmin/5.2.3/phpMyAdmin-5.2.3-all-languages.zip";
    const std::string body = "phpmyadmin zip";
    cdn.http.serve(kTestCdn + "/" + path, {200, body});

    const OfficialSource source = official_source(path);
    CHECK(source.kind == OfficialSource::Kind::Sums);
    CHECK(source.urls == std::vector<std::string>{
                             "https://files.phpmyadmin.net/phpMyAdmin/5.2.3/phpMyAdmin-5.2.3-all-languages.zip.sha256"});
    cdn.http.serve(source.urls.front(), {200, sha256_hex(body) + "  phpMyAdmin-5.2.3-all-languages.zip\n"});

    const AddResult added = new_file(kTestCdn + "/" + path, kTestCdn, cdn.files);
    REQUIRE_MESSAGE(added.file, added.error);
    CHECK(added.file->title == "phpMyAdmin 5.2.3");
    PublishFile f = *added.file;
    CheckContext ctx{cdn.http, kTestCdn};
    check_file(ctx, f);
    CHECK(f.state == CheckState::Ok);
    CHECK(f.detail == "Matches files.phpmyadmin.net");

    // Another file's checksum: refused.
    PublishFile bad = *added.file;
    cdn.http.serve(kTestCdn + "/" + path, {200, "something else"});
    check_file(ctx, bad);
    CHECK(bad.state == CheckState::Failed);
    cdn.http.serve(kTestCdn + "/" + path, {200, body});

    // Other downloads of the release aren't listed; a wrong name stops the build.
    CHECK(describe_cdn_path(path).listable);
    CHECK_FALSE(describe_cdn_path("phpmyadmin/5.2.3/phpMyAdmin-5.2.3-english.zip").listable);
    CHECK(describe_cdn_path("phpmyadmin/5.2.3/phpMyAdmin-5.2.3-english.zip").problem.find("another build") !=
          std::string::npos);
    CHECK_FALSE(describe_cdn_path("phpmyadmin/5.2.4/phpMyAdmin-5.2.3-all-languages.zip").listable);

    cdn.files.push_back(f);
    const auto now = at("2026-11-01T12:00:00Z");
    const BuiltList built = build_next_list(kTestCdn, cdn.files, 5, now);
    REQUIRE_MESSAGE(built.manifest_json, (built.errors.empty() ? "" : built.errors.front()));
    const auto doc = nlohmann::json::parse(*built.manifest_json);
    const auto entry = std::find_if(doc.at("runtimes").begin(), doc.at("runtimes").end(),
                                    [](const nlohmann::json& r) { return r.at("name").get<std::string>() == "phpmyadmin"; });
    REQUIRE(entry != doc.at("runtimes").end());
    CHECK(entry->at("version").get<std::string>() == "5.2.3");
    CHECK(entry->at("platform").get<std::string>() == "any");
    CHECK_FALSE(entry->contains("variant"));
    CHECK(entry->at("archive").get<std::string>() == "zip");
    CHECK(entry->at("license").get<std::string>() == "GPL-2.0-or-later");
    CHECK(entry->at("urls") == nlohmann::json::array({kTestCdn + "/" + path,
                                                      "https://files.phpmyadmin.net/phpMyAdmin/5.2.3/" +
                                                          std::string("phpMyAdmin-5.2.3-all-languages.zip")}));

    // The app opens the list with the entry in it, no warnings: an older
    // FloServer reads it the same way and only offers it where it knows the name.
    floserver::core::ManifestCheck check;
    check.trusted_keys = cdn.keys();
    check.now = now;
    const auto opened = floserver::core::open_manifest(*built.manifest_json, cdn.key.sign(*built.manifest_json, "test-key"),
                                                       check);
    REQUIRE(opened.manifest);
    CHECK(opened.warnings.empty());
    CHECK(floserver::core::runtimes_for(*opened.manifest, "phpmyadmin", "any").size() == 1);
    CHECK(floserver::core::runtimes_for(*opened.manifest, "phpmyadmin", "windows-x64").empty());
}

TEST_CASE("publisher: Mailpit is compared with its GitHub download and listed for Windows (D27)") {
    FakeCdn cdn;
    const std::string path = "mailpit/1.33.0/mailpit-windows-amd64.zip";
    const std::string body = "mailpit zip";
    cdn.http.serve(kTestCdn + "/" + path, {200, body});

    const OfficialSource source = official_source(path);
    CHECK(source.kind == OfficialSource::Kind::Original);
    CHECK(source.urls == std::vector<std::string>{
                             "https://github.com/axllent/mailpit/releases/download/v1.33.0/mailpit-windows-amd64.zip"});
    cdn.http.serve(source.urls.front(), {200, body});

    const AddResult added = new_file(kTestCdn + "/" + path, kTestCdn, cdn.files);
    REQUIRE_MESSAGE(added.file, added.error);
    CHECK(added.file->title == "Mailpit 1.33.0");
    PublishFile f = *added.file;
    CheckContext ctx{cdn.http, kTestCdn};
    check_file(ctx, f);
    CHECK(f.state == CheckState::Ok);

    // A CDN file that differs from the original is refused.
    PublishFile bad = *added.file;
    cdn.http.serve(kTestCdn + "/" + path, {200, "something else"});
    check_file(ctx, bad);
    CHECK(bad.state == CheckState::Failed);
    cdn.http.serve(kTestCdn + "/" + path, {200, body});

    // Other builds of the release aren't listed; a mismatched version is an error.
    CHECK(describe_cdn_path(path).listable);
    CHECK_FALSE(describe_cdn_path("mailpit/1.33.0/mailpit-windows-arm64.zip").listable);
    CHECK(describe_cdn_path("mailpit/1.33.0/mailpit-windows-arm64.zip").problem.find("another build") !=
          std::string::npos);
    CHECK(describe_cdn_path("mailpit/1.33.0/mailpit-linux-amd64.tar.gz").problem.find("not listed") !=
          std::string::npos);
    CHECK_FALSE(describe_cdn_path("mailpit/1.33.1/mailpit-windows-amd64.exe").listable);

    cdn.files.push_back(f);
    const auto now = at("2026-11-07T12:00:00Z");
    const BuiltList built = build_next_list(kTestCdn, cdn.files, 6, now);
    REQUIRE_MESSAGE(built.manifest_json, (built.errors.empty() ? "" : built.errors.front()));
    const auto doc = nlohmann::json::parse(*built.manifest_json);
    const auto entry = std::find_if(doc.at("runtimes").begin(), doc.at("runtimes").end(),
                                    [](const nlohmann::json& r) { return r.at("name").get<std::string>() == "mailpit"; });
    REQUIRE(entry != doc.at("runtimes").end());
    CHECK(entry->at("version").get<std::string>() == "1.33.0");
    CHECK(entry->at("platform").get<std::string>() == "windows-x64");
    CHECK(entry->at("archive").get<std::string>() == "zip");
    CHECK(entry->at("license").get<std::string>() == "MIT");
    CHECK(entry->at("urls") == nlohmann::json::array({kTestCdn + "/" + path, source.urls.front()}));

    floserver::core::ManifestCheck check;
    check.trusted_keys = cdn.keys();
    check.now = now;
    const auto opened = floserver::core::open_manifest(*built.manifest_json, cdn.key.sign(*built.manifest_json, "test-key"),
                                                       check);
    REQUIRE(opened.manifest);
    CHECK(opened.warnings.empty());
    CHECK(floserver::core::runtimes_for(*opened.manifest, "mailpit", "windows-x64").size() == 1);
}

TEST_CASE("publisher: MySQL 8.4 is compared with Oracle's download and listed for Windows (D28)") {
    FakeCdn cdn;
    const std::string path = "mysql/8.4.11/mysql-8.4.11-winx64.zip";
    const std::string body = "mysql zip";
    cdn.http.serve(kTestCdn + "/" + path, {200, body});

    // Oracle publishes MD5 sums and signatures, no SHA-256: the original is compared.
    const OfficialSource source = official_source(path);
    CHECK(source.kind == OfficialSource::Kind::Original);
    // Downloads/ keeps only the current release: the archive's address comes next.
    CHECK(source.urls == std::vector<std::string>{"https://cdn.mysql.com/Downloads/MySQL-8.4/mysql-8.4.11-winx64.zip",
                                                  "https://cdn.mysql.com/archives/mysql-8.4/mysql-8.4.11-winx64.zip"});
    const AddResult added = new_file(kTestCdn + "/" + path, kTestCdn, cdn.files);
    REQUIRE_MESSAGE(added.file, added.error);
    CHECK(added.file->title == "MySQL 8.4.11");
    CheckContext ctx{cdn.http, kTestCdn};
    // An older release: only the archive has it.
    cdn.http.serve(source.urls.back(), {200, body});
    PublishFile archived = *added.file;
    check_file(ctx, archived);
    CHECK(archived.state == CheckState::Ok);
    cdn.http.serve(source.urls.front(), {200, body});
    PublishFile f = *added.file;
    check_file(ctx, f);
    CHECK(f.state == CheckState::Ok);

    // 8.4 LTS only; the MSI and the debug build aren't listed.
    CHECK(describe_cdn_path(path).listable);
    CHECK_FALSE(describe_cdn_path("mysql/9.5.0/mysql-9.5.0-winx64.zip").listable);
    CHECK(describe_cdn_path("mysql/9.5.0/mysql-9.5.0-winx64.zip").problem.find("8.4 LTS only") != std::string::npos);
    CHECK_FALSE(describe_cdn_path("mysql/8.0.44/mysql-8.0.44-winx64.zip").listable);
    CHECK(describe_cdn_path("mysql/8.4.11/mysql-8.4.11-winx64.msi").problem.find("another build") != std::string::npos);
    CHECK(describe_cdn_path("mysql/8.4.11/mysql-8.4.11-winx64-debug-test.zip").problem.find("another build") !=
          std::string::npos);

    cdn.files.push_back(f);
    const auto now = at("2026-11-11T12:00:00Z");
    const BuiltList built = build_next_list(kTestCdn, cdn.files, 6, now);
    REQUIRE_MESSAGE(built.manifest_json, (built.errors.empty() ? "" : built.errors.front()));
    const auto doc = nlohmann::json::parse(*built.manifest_json);
    const auto entry = std::find_if(doc.at("runtimes").begin(), doc.at("runtimes").end(),
                                    [](const nlohmann::json& r) { return r.at("name").get<std::string>() == "mysql"; });
    REQUIRE(entry != doc.at("runtimes").end());
    CHECK(entry->at("version").get<std::string>() == "8.4.11");
    CHECK(entry->at("platform").get<std::string>() == "windows-x64");
    CHECK(entry->at("license").get<std::string>() == "GPL-2.0");
    CHECK(entry->at("urls") == nlohmann::json::array({kTestCdn + "/" + path, source.urls.front()}));
}

TEST_CASE("publisher: Bun's regular and baseline builds are both listed, checked against SHASUMS256.txt (D31)") {
    FakeCdn cdn;
    const std::string regular = "bun/1.3.14/bun-windows-x64.zip";
    const std::string baseline = "bun/1.3.14/bun-windows-x64-baseline.zip";
    const std::string regular_body = "bun zip";
    const std::string baseline_body = "bun baseline zip";
    cdn.http.serve(kTestCdn + "/" + regular, {200, regular_body});
    cdn.http.serve(kTestCdn + "/" + baseline, {200, baseline_body});

    const OfficialSource source = official_source(regular);
    CHECK(source.kind == OfficialSource::Kind::Sums);
    CHECK(source.name == "bun-windows-x64.zip");
    CHECK(source.urls ==
          std::vector<std::string>{"https://github.com/oven-sh/bun/releases/download/bun-v1.3.14/SHASUMS256.txt"});
    CHECK(official_source(baseline).name == "bun-windows-x64-baseline.zip");
    CHECK(official_source(baseline).urls == source.urls);
    // One list names every build; the regular file's name is not the baseline's.
    cdn.http.serve(source.urls.front(),
                   {200, std::string(64, 'a') + "  bun-linux-x64.zip\n" + sha256_hex(regular_body) +
                             "  bun-windows-x64.zip\n" + sha256_hex(baseline_body) + "  bun-windows-x64-baseline.zip\n" +
                             std::string(64, 'b') + "  bun-windows-x64-baseline-profile.zip\n"});

    // Both files can be added; the second isn't "already in the list".
    std::vector<PublishFile> files = cdn.files;
    const AddResult first = new_file(kTestCdn + "/" + regular, kTestCdn, files);
    REQUIRE_MESSAGE(first.file, first.error);
    CHECK(first.file->title == "Bun 1.3.14");
    files.push_back(*first.file);
    const AddResult second = new_file(kTestCdn + "/" + baseline, kTestCdn, files);
    REQUIRE_MESSAGE(second.file, second.error);
    CHECK(second.file->title == "Bun 1.3.14 baseline");
    files.push_back(*second.file);
    CHECK(new_file(kTestCdn + "/" + baseline, kTestCdn, files).error == "That file is already in the list.");

    CheckContext ctx{cdn.http, kTestCdn};
    PublishFile a = *first.file;
    PublishFile b = *second.file;
    check_file(ctx, a);
    check_file(ctx, b);
    CHECK_MESSAGE(a.state == CheckState::Ok, a.detail);
    CHECK_MESSAGE(b.state == CheckState::Ok, b.detail);
    // A CDN file that isn't the official one is refused.
    PublishFile bad = *second.file;
    cdn.http.serve(kTestCdn + "/" + baseline, {200, "something else"});
    check_file(ctx, bad);
    CHECK(bad.state == CheckState::Failed);
    cdn.http.serve(kTestCdn + "/" + baseline, {200, baseline_body});

    // Other builds of the release aren't listed.
    CHECK(describe_cdn_path(regular).listable);
    CHECK(describe_cdn_path(regular).variant.empty());
    CHECK(describe_cdn_path(baseline).listable);
    CHECK(describe_cdn_path(baseline).variant == "baseline");
    for (const char* other : {"bun-linux-x64.zip", "bun-darwin-aarch64.zip", "bun-windows-aarch64.zip",
                              "bun-windows-x64-profile.zip", "bun-windows-x64-baseline-profile.zip",
                              "bun-freebsd-x64.zip", "bun-linux-x64-android.zip"}) {
        const CdnPath p = describe_cdn_path(std::string("bun/1.3.14/") + other);
        CHECK_FALSE(p.listable);
        CHECK(p.problem.find("another build") != std::string::npos);
    }
    CHECK_FALSE(describe_cdn_path("bun/1.3.15/bun-windows-x64.zip.sha256").listable);

    cdn.files.push_back(a);
    cdn.files.push_back(b);
    const auto now = at("2026-11-12T12:00:00Z");
    cdn.publish(7, now);  // build_next_list: the app's own checks accept two Bun files
    const LiveResult live = fetch_live_list(cdn.http, kTestCdn, cdn.keys(), now);
    REQUIRE_MESSAGE(live.list, live.error);
    std::vector<std::string> bun;
    for (const floserver::core::RuntimeFile& r : live.list->runtimes) {
        if (r.name == "bun") {
            bun.push_back(r.version + "/" + r.variant + "/" + r.platform + "/" + r.license + "/" + r.archive + "/" +
                          r.urls.back());
        }
    }
    REQUIRE(bun.size() == 2);
    CHECK(bun[0] == "1.3.14//windows-x64/MIT/zip/https://github.com/oven-sh/bun/releases/download/bun-v1.3.14/bun-windows-x64.zip");
    CHECK(bun[1] == "1.3.14/baseline/windows-x64/MIT/zip/"
                    "https://github.com/oven-sh/bun/releases/download/bun-v1.3.14/bun-windows-x64-baseline.zip");

    // The next list carries both over, with their own titles.
    std::vector<std::string> notes;
    const auto carried = files_from_live(*live.list, kTestCdn, notes);
    CHECK(notes.empty());
    std::vector<std::string> titles;
    titles.reserve(carried.size());
    for (const PublishFile& f : carried) {
        titles.push_back(f.title);
    }
    CHECK(std::find(titles.begin(), titles.end(), "Bun 1.3.14") != titles.end());
    CHECK(std::find(titles.begin(), titles.end(), "Bun 1.3.14 baseline") != titles.end());
    CHECK(runtime_title("bun", "1.3.14", "baseline") == "Bun 1.3.14 baseline");
    CHECK(runtime_title("php", "8.5.12", "nts") == "PHP 8.5.12");
}

TEST_CASE("publisher: Deno is checked against its own .sha256sum, as PowerShell printed it (D31)") {
    FakeCdn cdn;
    const std::string path = "deno/2.9.8/deno-x86_64-pc-windows-msvc.zip";
    const std::string body = "deno zip";
    cdn.http.serve(kTestCdn + "/" + path, {200, body});

    const OfficialSource source = official_source(path);
    CHECK(source.kind == OfficialSource::Kind::Sums);
    CHECK(source.urls == std::vector<std::string>{"https://github.com/denoland/deno/releases/download/v2.9.8/"
                                                  "deno-x86_64-pc-windows-msvc.zip.sha256sum"});
    // The file Deno's release has: Get-FileHash's print, the hash in capitals.
    std::string upper = sha256_hex(body);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char c) { return (c >= 'a' && c <= 'f') ? static_cast<char>(c - 32) : c; });
    cdn.http.serve(source.urls.front(),
                   {200, "\nAlgorithm : SHA256\nHash      : " + upper +
                             "\nPath      : C:\\a\\deno\\deno\\target\\release\\deno-x86_64-pc-windows-msvc.zip\n\n"});

    const AddResult added = new_file(kTestCdn + "/" + path, kTestCdn, cdn.files);
    REQUIRE_MESSAGE(added.file, added.error);
    CHECK(added.file->title == "Deno 2.9.8");
    PublishFile f = *added.file;
    CheckContext ctx{cdn.http, kTestCdn};
    check_file(ctx, f);
    CHECK_MESSAGE(f.state == CheckState::Ok, f.detail);

    // Another hash in that file: refused.
    PublishFile bad = *added.file;
    cdn.http.serve(kTestCdn + "/" + path, {200, "something else"});
    check_file(ctx, bad);
    CHECK(bad.state == CheckState::Failed);
    cdn.http.serve(kTestCdn + "/" + path, {200, body});

    CHECK(describe_cdn_path(path).listable);
    for (const char* other : {"deno-aarch64-apple-darwin.zip", "deno-x86_64-unknown-linux-gnu.zip",
                              "denort-x86_64-pc-windows-msvc.zip"}) {
        const CdnPath p = describe_cdn_path(std::string("deno/2.9.8/") + other);
        CHECK_FALSE(p.listable);
        INFO(other << ": " << p.problem);
        CHECK(p.problem.find("another build") != std::string::npos);
    }
    CHECK(describe_cdn_path("deno/2.9.8/deno_src.tar.gz").problem.find("not listed") != std::string::npos);

    cdn.files.push_back(f);
    const auto now = at("2026-11-12T12:00:00Z");
    const BuiltList built = build_next_list(kTestCdn, cdn.files, 8, now);
    REQUIRE_MESSAGE(built.manifest_json, (built.errors.empty() ? "" : built.errors.front()));
    const auto doc = nlohmann::json::parse(*built.manifest_json);
    const auto entry = std::find_if(doc.at("runtimes").begin(), doc.at("runtimes").end(),
                                    [](const nlohmann::json& r) { return r.at("name").get<std::string>() == "deno"; });
    REQUIRE(entry != doc.at("runtimes").end());
    CHECK(entry->at("version").get<std::string>() == "2.9.8");
    CHECK(entry->at("platform").get<std::string>() == "windows-x64");
    CHECK_FALSE(entry->contains("variant"));
    CHECK(entry->at("license").get<std::string>() == "MIT");
    CHECK(entry->at("urls").back().get<std::string>() ==
          "https://github.com/denoland/deno/releases/download/v2.9.8/deno-x86_64-pc-windows-msvc.zip");
}

TEST_CASE("publisher: Deno's Get-FileHash print is read only for Deno, with a Hash line and one hash") {
    const std::string h = std::string(63, 'a') + "b";
    std::string upper = h;
    upper[0] = 'A';
    const std::string text = "Algorithm : SHA256\r\nHash      : " + upper + "\r\nPath      : C:\\x\\file.zip\r\n";
    CHECK(find_official_hash(text, "file.zip", true) == h);
    CHECK_FALSE(find_official_hash(text, "other.zip", true));  // it names another file
    CHECK_FALSE(find_official_hash(text, "file.zip"));         // the fallback is for Deno's lists alone
    CHECK(official_source("deno/2.9.8/deno-x86_64-pc-windows-msvc.zip").powershell_print);
    CHECK_FALSE(official_source("bun/1.3.14/bun-windows-x64.zip").powershell_print);
    CHECK_FALSE(official_source("node/24.22.1/node-v24.22.1-win-x64.zip").powershell_print);
    // No Hash line: not that format.
    CHECK_FALSE(find_official_hash("Path : C:\\x\\file.zip\n" + h + "\n", "file.zip", true));
    // Names the file but holds two different hashes (another one on the Path
    // line, or after the first on the Hash line): refused, not guessed.
    const std::string other(64, 'c');
    CHECK_FALSE(find_official_hash("Hash : " + h + "\nPath : C:\\x\\file.zip " + other + "\n", "file.zip", true));
    CHECK_FALSE(find_official_hash("Hash : " + h + " " + other + "\nPath : C:\\x\\file.zip\n", "file.zip", true));
    // The same hash twice is still one hash.
    CHECK(find_official_hash("Hash : " + h + "\nPath : C:\\x\\file.zip\nHash : " + h + "\n", "file.zip", true) == h);
    // Two different hashes and no line naming the file: not guessed.
    CHECK_FALSE(find_official_hash(h + "  a.zip\n" + other + "  b.zip\n", "file.zip", true));
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
    CHECK(find_official_hash(h + "  \r\n", "composer.phar") == h);
    // A single line naming another file isn't this file's hash.
    CHECK_FALSE(find_official_hash(h + "  composer-other.phar\n", "composer.phar"));
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

TEST_CASE("publisher: the clock is checked against the CDN's Date, on a reply no cache can answer") {
    FakeTransport http;
    const auto now = at("2026-10-30T12:00:00Z");
    const std::string url =
        kTestCdn + "/manifest.json.sig?clock=" + std::to_string(now.time_since_epoch().count());
    http.serve(url, {200, "sig", "", now + std::chrono::minutes{3}});
    const CdnClock clock = read_cdn_clock(http, kTestCdn, now);
    CHECK(clock.error.empty());
    CHECK(http.requests(url) == 1);
    CHECK(clock_problem(clock.server_date, now).empty());
    CHECK(clock_problem(now + std::chrono::minutes{11}, now).find("11 minutes off") != std::string::npos);
    CHECK(clock_problem(std::nullopt, now).find("Can't check") != std::string::npos);

    // No answer, or not a 200: no time to trust.
    const CdnClock missing = read_cdn_clock(http, kTestCdn, now + std::chrono::seconds{1});
    CHECK_FALSE(missing.server_date);
    CHECK(missing.error.find("Can't read the time") != std::string::npos);
    http.serve(url, {404, "", "", now});
    const CdnClock not_found = read_cdn_clock(http, kTestCdn, now);
    CHECK_FALSE(not_found.server_date);
    CHECK(not_found.error.find("HTTP 404") != std::string::npos);
}

TEST_CASE("publisher: an old live list, or different templates, need a yes or stop") {
    FakeCdn cdn;
    const auto now = at("2026-10-30T12:00:00Z");
    cdn.publish(4, now - std::chrono::days{1});
    LiveResult live = fetch_live_list(cdn.http, kTestCdn, cdn.keys(), now);
    REQUIRE(live.list);
    SuccessorCheck c = check_successor(*live.list, kBuiltInTemplates, now);
    CHECK(c.stop.empty());
    CHECK(c.confirm.empty());
    c = check_successor(*live.list, "[]", now);
    REQUIRE(c.confirm.size() == 1);
    CHECK(c.confirm.front().find("templates") != std::string::npos);

    live.list->expired = true;
    live.list->expires = now - std::chrono::days{3};
    c = check_successor(*live.list, kBuiltInTemplates, now);
    CHECK(c.stop.empty());
    REQUIRE(c.confirm.size() == 1);
    CHECK(c.confirm.front().find("expired on 2026-10-27") != std::string::npos);
    live.list->expires = now - std::chrono::days{91};
    c = check_successor(*live.list, kBuiltInTemplates, now);
    CHECK(c.stop.size() == 1);
}

TEST_CASE("publisher: a published file without a size from the CDN fails") {
    class NoLength final : public floserver::core::HttpTransport {
    public:
        floserver::core::HttpReply get(const std::string&, const floserver::core::HttpLimits&,
                                       const floserver::core::HttpBody& body) override {
            floserver::core::HttpReply r;
            r.status = 200;
            const std::string data = "abc";
            r.stopped = !body(std::span(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
            return r;
        }
    } http;
    CheckContext ctx(http, kTestCdn);
    PublishFile f;
    f.path = "composer/2.10.3/composer.phar";
    f.published = true;
    f.size = 3;
    check_file(ctx, f);
    CHECK(f.state == CheckState::Failed);
}

TEST_CASE("publisher: sizes read like the window shows them") {
    CHECK(format_size(36310917) == "36.3 MB");
    CHECK(format_size(2773293) == "2.8 MB");
    CHECK(format_size(512000) == "512 KB");
}

TEST_CASE("publisher: FloServer's installer is added with its notes, compared by hand and carried over") {
    FakeCdn cdn;
    const auto now = at("2026-11-05T12:00:00Z");
    const std::string msi = "app/1.2.0/FloServer-1.2.0-x64.msi";
    const std::string body = "the FloServer 1.2.0 installer";
    cdn.http.serve(kTestCdn + "/" + msi, {200, body});

    // Its What's new page after the path; only a plain https address there.
    CHECK(new_file(msi + " http://floserver.org/news", kTestCdn, cdn.files).error.find("plain https") !=
          std::string::npos);
    CHECK(new_file("php/8.5.13/php-8.5.13-nts-Win32-vs17-x64.zip https://floserver.org", kTestCdn, cdn.files)
              .error.find("Only FloServer's installer") != std::string::npos);
    const AddResult added = new_file("  " + msi + "   https://floserver.org/news/1.2.0 ", kTestCdn, cdn.files);
    REQUIRE_MESSAGE(added.file, added.error);
    CHECK(added.file->path == msi);
    CHECK(added.file->title == "FloServer 1.2.0");
    CHECK(added.file->notes_url == "https://floserver.org/news/1.2.0");
    PublishFile file = *added.file;

    // HostKurd's own file: nothing official to compare with.
    CHECK(official_source(msi).kind == OfficialSource::Kind::ByHand);
    CheckContext ctx{cdn.http, kTestCdn};
    check_file(ctx, file);
    CHECK(file.state == CheckState::ByHand);
    CHECK(file.sha256 == sha256_hex(body));
    CHECK(file.detail.find("CI build's artifact") != std::string::npos);
    CHECK(file.detail.find(file.sha256) != std::string::npos);

    std::vector<PublishFile> files = cdn.files;
    files.push_back(file);
    // Not ticked: not published.
    BuiltList built = build_next_list(kTestCdn, files, 4, now);
    CHECK_FALSE(built.manifest_json.has_value());
    files.back().accepted = true;
    built = build_next_list(kTestCdn, files, 4, now);
    REQUIRE_MESSAGE(built.manifest_json, (built.errors.empty() ? "" : built.errors.front()));
    const auto doc = nlohmann::json::parse(*built.manifest_json);
    REQUIRE(doc.contains("app"));
    CHECK(doc["app"]["version"] == "1.2.0");
    CHECK(doc["app"]["urls"] == nlohmann::json::array({kTestCdn + "/" + msi}));
    CHECK(doc["app"]["notes_url"] == "https://floserver.org/news/1.2.0");
    CHECK(doc["app"]["sha256"] == sha256_hex(body));

    // Live: read back, carried over with its notes, still served.
    cdn.http.serve(kTestCdn + "/manifest.json", {200, *built.manifest_json});
    cdn.http.serve(kTestCdn + "/manifest.json.sig", {200, cdn.key.sign(*built.manifest_json, "test-key")});
    const LiveResult live = fetch_live_list(cdn.http, kTestCdn, cdn.keys(), now);
    REQUIRE_MESSAGE(live.list, live.error);
    REQUIRE(live.list->app.has_value());
    std::vector<std::string> notes;
    std::vector<PublishFile> carried = files_from_live(*live.list, kTestCdn, notes);
    CHECK(notes.empty());
    REQUIRE(carried.size() == cdn.files.size() + 1);
    const PublishFile& again = carried.back();
    CHECK(again.path == msi);
    CHECK(again.title == "FloServer 1.2.0");
    CHECK(again.published);
    CHECK(again.notes_url == "https://floserver.org/news/1.2.0");
    for (PublishFile& f : carried) {
        check_file(ctx, f);
        INFO(f.path << ": " << f.detail);
        CHECK(f.state == CheckState::Ok);  // published: still served at its size
    }
    const BuiltList next = build_next_list(kTestCdn, carried, 5, now);
    REQUIRE_MESSAGE(next.manifest_json, (next.errors.empty() ? "" : next.errors.front()));
    CHECK(nlohmann::json::parse(*next.manifest_json)["app"]["notes_url"] == "https://floserver.org/news/1.2.0");

    // One installer at a time: the listed one goes first.
    const AddResult second = new_file("app/1.3.0/FloServer-1.3.0-x64.msi", kTestCdn, carried);
    CHECK_FALSE(second.file.has_value());
    CHECK(second.error.find("remove it first") != std::string::npos);
}

TEST_CASE("publisher: Garnet, .NET and phpredis are compared with their original downloads (D30)") {
    const OfficialSource g = official_source("garnet/2.2.4/win-x64-based-readytorun.zip");
    CHECK(g.kind == OfficialSource::Kind::Original);
    CHECK(g.urls == std::vector<std::string>{
                        "https://github.com/microsoft/garnet/releases/download/v2.2.4/win-x64-based-readytorun.zip"});
    const OfficialSource d = official_source("dotnet/10.0.3/dotnet-runtime-10.0.3-win-x64.zip");
    CHECK(d.kind == OfficialSource::Kind::Original);
    CHECK(d.urls == std::vector<std::string>{
                        "https://builds.dotnet.microsoft.com/dotnet/Runtime/10.0.3/dotnet-runtime-10.0.3-win-x64.zip"});
    const OfficialSource r = official_source("php-redis/6.3.0/php_redis-6.3.0-8.4-nts-vs17-x64.zip");
    CHECK(r.kind == OfficialSource::Kind::Original);
    CHECK(r.urls == std::vector<std::string>{
                        "https://downloads.php.net/~windows/pecl/releases/redis/6.3.0/php_redis-6.3.0-8.4-nts-vs17-x64.zip"});

    CHECK(runtime_title("garnet", "2.2.4") == "Garnet 2.2.4");
    CHECK(runtime_title("dotnet", "10.0.3") == ".NET runtime 10.0.3");
    CHECK(runtime_title("php-redis", "6.3.0", "php8-4") == "phpredis 6.3.0 for PHP 8.4");
    // One file per PHP series: the second series isn't "already in the list".
    std::vector<PublishFile> files;
    auto first = new_file("php-redis/6.3.0/php_redis-6.3.0-8.4-nts-vs17-x64.zip", kTestCdn, files);
    REQUIRE(first.file);
    files.push_back(*first.file);
    const auto second = new_file("php-redis/6.3.0/php_redis-6.3.0-8.3-nts-vs16-x64.zip", kTestCdn, files);
    CHECK(second.file);
    CHECK(second.error.empty());
    const auto again = new_file("php-redis/6.3.0/php_redis-6.3.0-8.4-nts-vs17-x64.zip", kTestCdn, files);
    CHECK_FALSE(again.file);
}

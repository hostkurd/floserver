// D36.3: the Publisher and FloServer's own Linux builds (hostkurd/floserver-runtimes):
// their CDN names, checksums, the list per platform, and that Windows'
// FloServer never sees them.

#include "../tools/manifest-tool/build.h"
#include "../tools/manifest-tool/fetch.h"
#include "../tools/manifest-tool/publish.h"
#include "../tools/manifest-tool/updates.h"
#include "floserver/core/manifest.h"
#include <nlohmann/json.hpp>
#include "floserver/core/sha256.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

using namespace floserver::manifest_tool;
using test_util::FakeTransport;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kReleases = "https://api.github.com/repos/hostkurd/floserver-runtimes/releases?per_page=100";

std::string asset_json(const std::string& tag, const std::string& name) {
    return R"({"name":")" + name + R"(","browser_download_url":"https://github.com/hostkurd/floserver-runtimes/releases/download/)" +
           tag + "/" + name + R"("})";
}

std::string release_json(const std::string& tag, const std::vector<std::string>& assets, bool prerelease = false) {
    std::string a;
    for (const std::string& n : assets) {
        a += (a.empty() ? "" : ",") + asset_json(tag, n);
    }
    return R"({"tag_name":")" + tag + R"(","draft":false,"prerelease":)" + (prerelease ? "true" : "false") +
           R"(,"assets":[)" + a + "]}";
}

// What GitHub answered on 27 Nov 2026, plus a pre-release and another runtime.
std::string releases_json() {
    return "[" + release_json("php-8.5.12-2", {"php-8.5.12-linux-x64.tar.gz", "php-8.5.12-linux-x64.tar.gz.sha256"}) + "," +
           release_json("php-8.4.27-2", {"php-8.4.27-linux-x64.tar.gz", "php-8.4.27-linux-x64.tar.gz.sha256"}) + "," +
           release_json("php-8.5.12-1", {"php-8.5.12-linux-x64.tar.gz", "php-8.5.12-linux-x64.tar.gz.sha256"}) + "," +
           release_json("php-8.5.13-1", {"php-8.5.13-linux-x64.tar.gz"}, /*prerelease=*/true) + "," +
           release_json("php-8.3.36-1", {"php-8.3.36-macos-arm64.tar.gz"}) + "," +
           release_json("nginx-1.30.6-1", {"nginx-1.30.6-linux-x64.tar.gz", "nginx-1.30.6-linux-x64.tar.gz.sha256"}) + "]";
}

std::vector<std::string> paths_of(const std::vector<PublishFile>& files) {
    std::vector<std::string> out;
    out.reserve(files.size());
    for (const PublishFile& f : files) {
        out.push_back(f.path);
    }
    return out;
}

PublishFile listed(const std::string& path) {
    PublishFile f;
    f.path = path;
    f.published = true;
    return f;
}

}  // namespace

TEST_CASE("publisher linux: our builds' CDN names, platform and release") {
    const CdnPath php = describe_cdn_path("php/8.5.12/php-8.5.12-2-linux-x64.tar.gz");
    CHECK(php.listable);
    CHECK(php.platform == "linux-x64");
    CHECK(php.variant == "nts");
    const auto ours = our_build(php);
    REQUIRE(ours);
    CHECK(ours->tag == "php-8.5.12-2");
    CHECK(ours->asset == "php-8.5.12-linux-x64.tar.gz");
    CHECK(ours->build == 2);
    CHECK(ours->url == "https://github.com/hostkurd/floserver-runtimes/releases/download/php-8.5.12-2/php-8.5.12-linux-x64.tar.gz");

    const CdnPath nginx = describe_cdn_path("nginx/1.30.6/nginx-1.30.6-1-linux-x64.tar.gz");
    CHECK(nginx.listable);
    CHECK(nginx.platform == "linux-x64");
    REQUIRE(our_build(nginx));

    // Windows files stay as they were, and aren't ours.
    const CdnPath win = describe_cdn_path("php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip");
    CHECK(win.platform == "windows-x64");
    CHECK_FALSE(our_build(win));
    CHECK(describe_cdn_path("nginx/1.30.6/nginx-1.30.6.zip").platform == "windows-x64");

    // Not ours: no build number, another version folder, another platform's name.
    CHECK_FALSE(describe_cdn_path("php/8.5.12/php-8.5.12-linux-x64.tar.gz").listable);
    CHECK_FALSE(describe_cdn_path("php/8.5.11/php-8.5.12-2-linux-x64.tar.gz").listable);
    CHECK_FALSE(describe_cdn_path("php/8.5.12/php-8.5.12-2-linux-arm64.tar.gz").listable);
    CHECK_FALSE(describe_cdn_path("mariadb/11.4.13/mariadb-11.4.13-2-linux-x64.tar.gz").listable);
}

TEST_CASE("publisher linux: our build is checked against its release's .sha256") {
    const OfficialSource s = official_source("php/8.5.12/php-8.5.12-2-linux-x64.tar.gz");
    CHECK(s.kind == OfficialSource::Kind::Sums);
    CHECK(s.name == "php-8.5.12-linux-x64.tar.gz");
    REQUIRE(s.urls.size() == 1);
    CHECK(s.urls[0] ==
          "https://github.com/hostkurd/floserver-runtimes/releases/download/php-8.5.12-2/php-8.5.12-linux-x64.tar.gz.sha256");
    // Windows' PHP still checks with php.net.
    CHECK(official_source("php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip").urls.front().starts_with("https://downloads.php.net/"));

    FakeTransport http;
    const test_util::TempDir dir;
    const std::string body = "php for linux";
    FetchPlan plan;
    plan.title = "PHP 8.5.12 (Linux)";
    plan.files = {{s.urls[0].substr(0, s.urls[0].size() - 7), "php/8.5.12/php-8.5.12-2-linux-x64.tar.gz"}};
    http.serve(plan.files[0].url, {200, body});
    SUBCASE("the same as its .sha256: kept") {
        http.serve(s.urls[0], {200, floserver::core::to_hex(floserver::core::sha256(body)) + "  php-8.5.12-linux-x64.tar.gz\n"});
        const FetchResult r = fetch_files(http, plan, dir.path());
        CHECK(r.problem.empty());
        CHECK(fs::exists(dir.path() / "php" / "8.5.12" / "php-8.5.12-2-linux-x64.tar.gz"));
    }
    SUBCASE("different: deleted") {
        http.serve(s.urls[0], {200, floserver::core::to_hex(floserver::core::sha256("x")) + "  php-8.5.12-linux-x64.tar.gz\n"});
        const FetchResult r = fetch_files(http, plan, dir.path());
        CHECK(r.problem.find("DIFFERENT") != std::string::npos);
        CHECK_FALSE(fs::exists(dir.path() / "php" / "8.5.12" / "php-8.5.12-2-linux-x64.tar.gz"));
    }
}

TEST_CASE("publisher linux: the list per runtime and platform") {
    CHECK(list_key("php", "windows-x64") == "php");
    CHECK(list_key("composer", "any") == "composer");
    CHECK(list_key("php", "linux-x64") == "php@linux-x64");
    CHECK(split_list_key("php@linux-x64").runtime == "php");
    CHECK(split_list_key("php@linux-x64").platform == "linux-x64");
    CHECK(split_list_key("php").platform.empty());
    CHECK(update_series("php@linux-x64", "8.5.12") == "8.5");

    SUBCASE("Windows only: PHP and nginx get an empty Linux key, others don't") {
        const auto l = listed_from_paths({"php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip", "nginx/1.30.6/nginx-1.30.6.zip",
                                          "node/24.22.1/node-v24.22.1-win-x64.zip", "composer/2.10.3/composer.phar",
                                          "app/0.9.1/FloServer-0.9.1-x64.msi"});
        CHECK(l.at("php") == std::vector<std::string>{"8.5.12"});
        CHECK(l.at("php@linux-x64").empty());
        CHECK(l.at("nginx@linux-x64").empty());
        CHECK_FALSE(l.contains("node@linux-x64"));
        CHECK(l.at("composer") == std::vector<std::string>{"2.10.3"});
        CHECK(l.at("app") == std::vector<std::string>{"0.9.1"});
    }
    SUBCASE("Linux files under their own key") {
        const auto l = listed_from_paths({"php/8.5.13/php-8.5.13-1-nts-Win32-vs17-x64.zip",
                                          "php/8.5.12/php-8.5.12-2-linux-x64.tar.gz"});
        CHECK(l.at("php") == std::vector<std::string>{"8.5.13"});
        CHECK(l.at("php@linux-x64") == std::vector<std::string>{"8.5.12"});
    }
}

TEST_CASE("publisher linux: versions from our releases, highest build, no pre-releases") {
    const std::string json = releases_json();
    const auto v = parse_our_releases(json, "php", "linux-x64");
    REQUIRE(v.size() == 2);
    std::map<std::string, std::string> by;
    for (const UpstreamVersion& u : v) {
        by[u.series] = u.version;
    }
    CHECK(by.at("8.5") == "8.5.12");  // 8.5.13 is a pre-release
    CHECK(by.at("8.4") == "8.4.27");
    CHECK(parse_our_releases(json, "php", "macos-arm64").size() == 1);
    CHECK(parse_our_releases(json, "nginx", "linux-x64").size() == 1);
    CHECK(parse_our_releases("not json", "php", "linux-x64").empty());

    const auto r = find_our_release(json, "php", "8.5.12", "linux-x64");
    REQUIRE(r);
    CHECK(r->tag == "php-8.5.12-2");
    CHECK(r->file == "php-8.5.12-2-linux-x64.tar.gz");
    CHECK_FALSE(find_our_release(json, "php", "8.5.13", "linux-x64"));
    // An asset whose address isn't this repository's release isn't taken.
    const std::string elsewhere = R"([{"tag_name":"php-8.2.35-1","assets":[{"name":"php-8.2.35-linux-x64.tar.gz",)"
                                  R"("browser_download_url":"https://example.com/php-8.2.35-linux-x64.tar.gz"}]}])";
    CHECK_FALSE(find_our_release(elsewhere, "php", "8.2.35", "linux-x64"));
}

TEST_CASE("publisher linux: rows of their own, a first version as a new series") {
    const std::map<std::string, std::vector<std::string>> l = {{"php", {"8.5.12"}}, {"php@linux-x64", {}}};
    std::map<std::string, FinderAnswer> found;
    found["php"].versions = {{"8.5", "8.5.13"}};
    found["php@linux-x64"].versions = parse_our_releases(releases_json(), "php", "linux-x64");
    const auto rows = compare_updates(l, found);
    std::map<std::string, const UpdateRow*> by;
    for (const UpdateRow& r : rows) {
        by[r.runtime + " " + r.series] = &r;
    }
    REQUIRE(by.contains("php 8.5"));
    CHECK(by["php 8.5"]->kind == UpdateKind::Update);
    REQUIRE(by.contains("php@linux-x64 8.5"));
    CHECK(by["php@linux-x64 8.5"]->kind == UpdateKind::NewSeries);
    CHECK(by["php@linux-x64 8.5"]->title == "PHP 8.5 (Linux)");
    CHECK_FALSE(by["php@linux-x64 8.5"]->ticked);
    REQUIRE(by.contains("php@linux-x64 8.4"));
}

TEST_CASE("publisher linux: plan_fetch takes the highest build from our releases") {
    FakeTransport http;
    http.serve(std::string(kReleases), {200, releases_json()});
    UpdateRow row;
    row.runtime = "php@linux-x64";
    row.latest = "8.5.12";
    const FetchPlan p = plan_fetch(http, row, {});
    CHECK(p.problem.empty());
    CHECK(p.title == "PHP 8.5.12 (Linux)");
    REQUIRE(p.files.size() == 1);
    CHECK(p.files[0].url == "https://github.com/hostkurd/floserver-runtimes/releases/download/php-8.5.12-2/php-8.5.12-linux-x64.tar.gz");
    CHECK(p.files[0].cdn_path == "php/8.5.12/php-8.5.12-2-linux-x64.tar.gz");

    row.latest = "8.5.13";
    CHECK(plan_fetch(http, row, {}).problem.find("run its workflow") != std::string::npos);
}

TEST_CASE("publisher linux: each platform's versions replace only their own") {
    const std::vector<PublishFile> files = {listed("php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip"),
                                            listed("php/8.5.12/php-8.5.12-2-linux-x64.tar.gz"),
                                            listed("nginx/1.30.6/nginx-1.30.6-1-linux-x64.tar.gz")};
    SUBCASE("a new Windows PHP leaves Linux's 8.5 as it is") {
        const ListChange c = add_to_list(files, {"php/8.5.13/php-8.5.13-1-nts-Win32-vs17-x64.zip"});
        CHECK(c.problems.empty());
        CHECK(c.left_out == std::vector<std::string>{"php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip"});
        CHECK(paths_of(c.files) == std::vector<std::string>{"php/8.5.12/php-8.5.12-2-linux-x64.tar.gz",
                                                            "nginx/1.30.6/nginx-1.30.6-1-linux-x64.tar.gz",
                                                            "php/8.5.13/php-8.5.13-1-nts-Win32-vs17-x64.zip"});
    }
    SUBCASE("a newer Linux patch, or a newer build of the same version, replaces Linux's") {
        const ListChange c = add_to_list(files, {"php/8.5.13/php-8.5.13-1-linux-x64.tar.gz",
                                                 "nginx/1.30.6/nginx-1.30.6-2-linux-x64.tar.gz"});
        CHECK(c.problems.empty());
        CHECK(c.left_out == std::vector<std::string>{"php/8.5.12/php-8.5.12-2-linux-x64.tar.gz",
                                                     "nginx/1.30.6/nginx-1.30.6-1-linux-x64.tar.gz"});
        CHECK(paths_of(c.files) == std::vector<std::string>{"php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip",
                                                            "php/8.5.13/php-8.5.13-1-linux-x64.tar.gz",
                                                            "nginx/1.30.6/nginx-1.30.6-2-linux-x64.tar.gz"});
    }
    SUBCASE("an older build never replaces a newer one") {
        const std::vector<PublishFile> newer = {listed("nginx/1.30.6/nginx-1.30.6-2-linux-x64.tar.gz")};
        const ListChange c = add_to_list(newer, {"nginx/1.30.6/nginx-1.30.6-1-linux-x64.tar.gz"});
        CHECK(c.left_out.empty());
        CHECK(c.added.empty());
        REQUIRE(c.problems.size() == 1);
        CHECK(c.problems[0].find("newer build") != std::string::npos);
        CHECK(paths_of(c.files) == std::vector<std::string>{"nginx/1.30.6/nginx-1.30.6-2-linux-x64.tar.gz"});
    }
}

TEST_CASE("publisher linux: the signed list carries our Linux builds beside Windows'") {
    using nlohmann::json;
    const auto file = [](const char* path, std::uint64_t size, const char* sha) {
        return json{{"path", path}, {"size", size}, {"sha256", sha}, {"result", "ok"}};
    };
    const json report = {
        {"cdn", "https://cdn.floserver.org"},
        {"files",
         {file("php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip", 36310917,
               "2a36a2edcb7a5b9252f638fd5e4cf84d7f6e066092654220dc0b63e5cf4e9723"),
          file("php/8.5.12/php-8.5.12-2-linux-x64.tar.gz", 56403771,
               "1111111111111111111111111111111111111111111111111111111111111111"),
          file("nginx/1.30.6/nginx-1.30.6-1-linux-x64.tar.gz", 3449997,
               "2222222222222222222222222222222222222222222222222222222222222222")}}};
    BuildOptions o;
    o.sequence = 20;
    o.days = 30;
    o.now = *floserver::core::parse_utc_time("2026-11-27T12:00:00Z");
    const auto r = build_manifest(report.dump(), o);
    const std::string first_error = r.errors.empty() ? std::string() : r.errors.front();
    INFO(first_error);
    REQUIRE(r.errors.empty());
    const auto doc = json::parse(*r.manifest_json);
    REQUIRE(doc["runtimes"].size() == 3);
    const auto& linux_php = doc["runtimes"][1];
    CHECK(linux_php["name"] == "php");
    CHECK(linux_php["version"] == "8.5.12");
    CHECK(linux_php["platform"] == "linux-x64");
    CHECK(linux_php["variant"] == "nts");
    CHECK(linux_php["archive"] == "tar.gz");
    CHECK(linux_php["urls"] == json::array({"https://cdn.floserver.org/php/8.5.12/php-8.5.12-2-linux-x64.tar.gz"}));
    CHECK(doc["runtimes"][0]["platform"] == "windows-x64");
    CHECK(doc["runtimes"][2]["platform"] == "linux-x64");
}

TEST_CASE("publisher linux: the same version for Linux is added beside Windows', titled apart") {
    const std::vector<PublishFile> files = {listed("php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip")};
    const AddResult linux_php = new_file("php/8.5.12/php-8.5.12-2-linux-x64.tar.gz", "https://cdn.floserver.org", files);
    REQUIRE_MESSAGE(linux_php.file, linux_php.error);
    CHECK(linux_php.file->title == "PHP 8.5.12 (Linux)");
    // A second build of it is the same title: one at a time, as for Windows.
    const std::vector<PublishFile> both = {files[0], *linux_php.file};
    CHECK_FALSE(new_file("php/8.5.12/php-8.5.12-3-linux-x64.tar.gz", "https://cdn.floserver.org", both).file);
}

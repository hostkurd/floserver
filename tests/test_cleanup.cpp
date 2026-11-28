// D37: which files on R2 the Publisher's Clean up offers, ticks, waits on
// or keeps; the paths of a list; purging in batches; the record.

#include "../tools/manifest-tool/cleanup.h"
#include "floserver/core/manifest.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <span>

using namespace floserver::manifest_tool;
using std::chrono::days;
using std::chrono::sys_seconds;

namespace {

sys_seconds at(const char* utc) { return *floserver::core::parse_utc_time(utc); }

R2Object obj(const char* key, std::uint64_t size = 100) { return {key, size, "2026-11-01T00:00:00.000Z"}; }

std::map<std::string, CleanupRow> by_path(const std::vector<CleanupRow>& rows) {
    std::map<std::string, CleanupRow> m;
    for (const CleanupRow& r : rows) {
        m[r.path] = r;
    }
    return m;
}

constexpr const char* kWin8512 = "php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip";
constexpr const char* kWin8513 = "php/8.5.13/php-8.5.13-1-nts-Win32-vs17-x64.zip";
constexpr const char* kLin8512 = "php/8.5.12/php-8.5.12-2-linux-x64.tar.gz";
constexpr const char* kLin8513 = "php/8.5.13/php-8.5.13-1-linux-x64.tar.gz";
constexpr const char* kMaria13 = "mariadb/11.4.13/mariadb-11.4.13-winx64.zip";
constexpr const char* kMaria13Src = "mariadb/11.4.13/mariadb-11.4.13.tar.gz";
constexpr const char* kMaria14 = "mariadb/11.4.14/mariadb-11.4.14-winx64.zip";
constexpr const char* kMaria14Src = "mariadb/11.4.14/mariadb-11.4.14.tar.gz";

}  // namespace

TEST_CASE("cleanup: list_paths takes each runtime's and the app's CDN address") {
    const std::string json = R"({"schema":1,"sequence":12,"runtimes":[
        {"name":"php","urls":["https://cdn.floserver.org/php/8.5.13/php-8.5.13-1-nts-Win32-vs17-x64.zip",
                              "https://downloads.php.net/~windows/releases/archives/php-8.5.13-1-nts-Win32-vs17-x64.zip"]},
        {"name":"x","urls":["https://elsewhere.example/x.zip"]},
        {"name":"bad"}],
        "app":{"version":"0.9.1","urls":["https://cdn.floserver.org/app/0.9.1/FloServer-0.9.1-x64.msi"]}})";
    const auto p = list_paths(json, "https://cdn.floserver.org");
    REQUIRE(p);
    CHECK(*p == std::vector<std::string>{kWin8513, "app/0.9.1/FloServer-0.9.1-x64.msi"});
    CHECK(list_paths(json, "https://cdn.floserver.org/") == p);
    CHECK_FALSE(list_paths("not json", "https://cdn.floserver.org"));
    CHECK_FALSE(list_paths(R"({"runtimes":3})", "https://cdn.floserver.org"));
}

TEST_CASE("cleanup: what's offered, ticked, waited on and kept (D37.1, D37.3)") {
    CleanupInput in;
    in.now = at("2026-11-28T12:00:00Z");
    in.objects = {obj("manifest.json"),        obj("manifest.json.sig"),     obj("notes/readme.txt"),
                  obj("website/logo.png"),     obj(kWin8512, 36310917),      obj(kWin8513),
                  obj(kLin8513),               obj(kMaria13, 95024126),      obj(kMaria13Src, 221118336),
                  obj(kMaria14),               obj(kMaria14Src),             obj("nginx/1.30.7/nginx-1.30.7.zip"),
                  obj("node/24.20.0/node-v24.20.0-win-x64.zip"),             obj("app/0.9.0/FloServer-0.9.0-x64.msi"),
                  obj("app/0.9.1/FloServer-0.9.1-x64.msi"),                  obj("php/9.9.9/odd-name.zip")};
    in.live_paths = {kWin8513, kMaria14, "app/0.9.1/FloServer-0.9.1-x64.msi"};
    in.next_paths = {kWin8513, kLin8513, kMaria14};
    // Saved lists: 10 had 8.5.12, MariaDB 11.4.13 and FloServer 0.9.0 (on
    // 1 Nov); 11 (12 Nov) moved PHP and MariaDB on; 12 (25 Nov) nginx 1.30.8
    // and FloServer 0.9.1.
    in.saved = {{11, at("2026-11-12T10:00:00Z"), {kWin8513, kMaria14, "nginx/1.30.7/nginx-1.30.7.zip", "app/0.9.0/FloServer-0.9.0-x64.msi"}},
                {10, at("2026-11-01T10:00:00Z"), {kWin8512, kMaria13, "nginx/1.30.7/nginx-1.30.7.zip", "app/0.9.0/FloServer-0.9.0-x64.msi"}},
                {12, at("2026-11-25T10:00:00Z"), {kWin8513, kMaria14, "nginx/1.30.8/nginx-1.30.8.zip", "app/0.9.1/FloServer-0.9.1-x64.msi"}}};
    const auto rows = plan_cleanup(in);
    const auto m = by_path(rows);

    // Never offered: listed files, the list itself, anything the catalog doesn't know.
    for (const char* never : {"manifest.json", "manifest.json.sig", "notes/readme.txt", "website/logo.png", kWin8513, kLin8513,
                              kMaria14, "app/0.9.1/FloServer-0.9.1-x64.msi", "php/9.9.9/odd-name.zip"}) {
        INFO(never);
        CHECK_FALSE(m.contains(never));
    }
    // Left 16 days ago: ticked.
    REQUIRE(m.contains(kWin8512));
    CHECK(m.at(kWin8512).kind == CleanupKind::Ready);
    CHECK(m.at(kWin8512).ticked);
    CHECK(m.at(kWin8512).title == "PHP 8.5.12");
    CHECK(m.at(kWin8512).size == 36310917);
    CHECK(*m.at(kWin8512).left == at("2026-11-12T10:00:00Z"));
    CHECK(left_text(m.at(kWin8512), in.now) == "16 days ago");
    // MariaDB 11.4.13 is gone from every list: its source can go with it.
    CHECK(m.at(kMaria13).kind == CleanupKind::Ready);
    CHECK(m.at(kMaria13Src).kind == CleanupKind::Ready);
    CHECK(m.at(kMaria13Src).title == "MariaDB 11.4.13 source");
    // MariaDB 11.4.14 is listed: its source is kept, greyed, never ticked.
    CHECK(m.at(kMaria14Src).kind == CleanupKind::Kept);
    CHECK_FALSE(m.at(kMaria14Src).ticked);
    CHECK(left_text(m.at(kMaria14Src), in.now) == "—");
    // nginx 1.30.7 and FloServer 0.9.0 left 3 days ago: wait.
    CHECK(m.at("nginx/1.30.7/nginx-1.30.7.zip").kind == CleanupKind::Wait);
    CHECK_FALSE(m.at("nginx/1.30.7/nginx-1.30.7.zip").ticked);
    CHECK(m.at("nginx/1.30.7/nginx-1.30.7.zip").note == "wait: left less than 7 days ago");
    CHECK(left_text(m.at("nginx/1.30.7/nginx-1.30.7.zip"), in.now) == "3 days ago");
    CHECK(m.at("app/0.9.0/FloServer-0.9.0-x64.msi").kind == CleanupKind::Wait);
    CHECK(m.at("app/0.9.0/FloServer-0.9.0-x64.msi").title == "FloServer 0.9.0");
    // In no saved list: unknown, unticked.
    CHECK(m.at("node/24.20.0/node-v24.20.0-win-x64.zip").kind == CleanupKind::Unknown);
    CHECK_FALSE(m.at("node/24.20.0/node-v24.20.0-win-x64.zip").ticked);
    CHECK(left_text(m.at("node/24.20.0/node-v24.20.0-win-x64.zip"), in.now) == "unknown");

    // Order: ready, wait, unknown, kept; by path within each.
    std::vector<CleanupKind> kinds;
    kinds.reserve(rows.size());
    for (const CleanupRow& r : rows) {
        kinds.push_back(r.kind);
    }
    CHECK(std::is_sorted(kinds.begin(), kinds.end(), [](CleanupKind a, CleanupKind b) {
        return static_cast<int>(a) < static_cast<int>(b);
    }));
    CHECK(rows.front().path == kMaria13);
    CHECK(rows.back().path == kMaria14Src);
}

TEST_CASE("cleanup: a Linux build that left, the newest saved list, and the wait setting") {
    CleanupInput in;
    in.now = at("2026-11-28T12:00:00Z");
    in.objects = {obj(kLin8512), obj(kLin8513)};
    in.live_paths = {kLin8513};
    in.saved = {{20, at("2026-11-01T00:00:00Z"), {kLin8512}}, {21, at("2026-11-26T00:00:00Z"), {kLin8513}}};
    auto m = by_path(plan_cleanup(in));
    CHECK(m.at(kLin8512).title == "PHP 8.5.12 (Linux)");
    CHECK(m.at(kLin8512).kind == CleanupKind::Wait);  // left 2 days ago
    in.wait = days{1};
    m = by_path(plan_cleanup(in));
    CHECK(m.at(kLin8512).kind == CleanupKind::Ready);

    // In the newest saved list but neither live nor next: it waits.
    in.live_paths = {};
    in.saved.push_back({22, at("2026-11-27T00:00:00Z"), {kLin8512}});
    m = by_path(plan_cleanup(in));
    CHECK(m.at(kLin8512).kind == CleanupKind::Wait);
    CHECK(m.at(kLin8512).note == "wait: in the newest list signed on this PC");
}

namespace {

// Cloudflare's purge endpoint: counts calls and the files in each.
class FakePurge final : public floserver::core::HttpTransport {
public:
    std::vector<std::size_t> batches;
    floserver::core::HttpReply get(const std::string&, const floserver::core::HttpLimits&,
                                   const floserver::core::HttpBody&) override {
        return {};
    }
    floserver::core::HttpResponse send(const floserver::core::HttpRequest& req, const floserver::core::HttpLimits&) override {
        std::string body(req.body_size, '\0');
        std::size_t got = 0;
        while (got < body.size()) {
            const std::size_t n = req.source(std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(body.data()) + got,  // NOLINT
                                                                     body.size() - got));
            REQUIRE(n > 0);
            got += n;
        }
        const auto doc = nlohmann::json::parse(body);
        batches.push_back(doc.at("files").size());
        CHECK(doc.at("files").at(0).get<std::string>().starts_with("https://cdn.floserver.org/php/"));
        floserver::core::HttpResponse r;
        r.status = 200;
        r.body = R"({"success":true,"errors":[],"messages":[],"result":{"id":"x"}})";
        return r;
    }
};

}  // namespace

TEST_CASE("cleanup: purges in batches of 30, and the record") {
    FakePurge http;
    const PurgeToken token{std::string(32, 'a'), "tok"};
    std::vector<std::string> paths;
    paths.reserve(65);
    for (int i = 0; i < 65; ++i) {
        paths.push_back("php/8.5." + std::to_string(i) + "/x.zip");
    }
    CHECK(purge_deleted(http, token, paths, "https://cdn.floserver.org/").empty());
    CHECK(http.batches == std::vector<std::size_t>{30, 30, 5});

    const std::string rec = cleanup_record({{kWin8512, 36310917, ""}, {kMaria13, 95024126, "R2 didn't delete it: Access Denied"}},
                                           at("2026-11-28T12:40:00Z"), "");
    CHECK(rec.find("2026-11-28T12:40:00Z") != std::string::npos);
    CHECK(rec.find("1 of 2 files deleted, 36310917 bytes freed") != std::string::npos);
    CHECK(rec.find("deleted  " + std::string(kWin8512)) != std::string::npos);
    CHECK(rec.find("FAILED   " + std::string(kMaria13) + ": R2 didn't delete it: Access Denied") != std::string::npos);
}

TEST_CASE("cleanup: read_saved_lists reads the Publisher's saved folders") {
    const test_util::TempDir dir;
    const auto list = [](int seq, const char* path) {
        return R"({"schema":1,"sequence":)" + std::to_string(seq) + R"(,"runtimes":[{"name":"php","urls":["https://cdn.floserver.org/)" +
               std::string(path) + R"("]}]})";
    };
    test_util::write_file(dir.path() / "2026-11-25 sequence 12" / "manifest.json", list(12, kWin8513));
    test_util::write_file(dir.path() / "2026-11-25 sequence 12" / "upload-steps.txt",
                          "FloServer runtime list, sequence 12, made 2026-11-25T10:20:30Z\r\nmore");
    test_util::write_file(dir.path() / "2026-11-01 sequence 10 (2)" / "manifest.json", list(10, kWin8512));  // no steps file
    test_util::write_file(dir.path() / "2026-11-02 sequence 11" / "manifest.json", "not json");
    test_util::write_file(dir.path() / "Updates 2026-11-20 101010" / "manifest.json", list(99, kWin8512));  // not a list folder
    test_util::write_file(dir.path() / "cleanup 2026-11-28.txt", "x");
    auto lists = read_saved_lists(dir.path(), "https://cdn.floserver.org");
    std::sort(lists.begin(), lists.end(), [](const SavedList& a, const SavedList& b) { return a.sequence < b.sequence; });
    REQUIRE(lists.size() == 2);
    CHECK(lists[0].sequence == 10);
    CHECK(lists[0].made == at("2026-11-01T00:00:00Z"));
    CHECK(lists[0].paths == std::vector<std::string>{kWin8512});
    CHECK(lists[1].sequence == 12);
    CHECK(lists[1].made == at("2026-11-25T10:20:30Z"));
    CHECK(read_saved_lists(dir.path() / "missing", "https://cdn.floserver.org").empty());
}

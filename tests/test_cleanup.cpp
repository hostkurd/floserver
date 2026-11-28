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

R2Object obj(const char* key, std::uint64_t size = 100, const char* uploaded = "2026-11-01T00:00:00.000Z") {
    return {key, size, uploaded};
}

SavedList saved(std::int64_t seq, const char* made, std::vector<std::string> paths,
                std::optional<sys_seconds> live_at = std::nullopt) {
    SavedList s;
    s.sequence = seq;
    s.made = at(made);
    s.live_at = live_at;
    s.paths = std::move(paths);
    return s;
}

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

TEST_CASE("cleanup: list_paths takes every CDN address, and fails when an entry has none") {
    const std::string good = R"({"schema":1,"sequence":12,"runtimes":[
        {"name":"php","urls":["https://cdn.floserver.org/php/8.5.13/php-8.5.13-1-nts-Win32-vs17-x64.zip",
                              "https://downloads.php.net/~windows/releases/archives/php-8.5.13-1-nts-Win32-vs17-x64.zip",
                              "https://cdn.floserver.org/php/8.5.13/second.zip"]}],
        "app":{"version":"0.9.1","urls":["https://cdn.floserver.org/app/0.9.1/FloServer-0.9.1-x64.msi"]}})";
    const auto p = list_paths(good, "https://cdn.floserver.org");
    REQUIRE(p);
    CHECK(*p == std::vector<std::string>{kWin8513, "php/8.5.13/second.zip", "app/0.9.1/FloServer-0.9.1-x64.msi"});
    CHECK(list_paths(good, "https://cdn.floserver.org/") == p);
    // An entry whose files can't be told: nothing may be deleted.
    CHECK_FALSE(list_paths(R"({"runtimes":[{"name":"x","urls":["https://elsewhere.example/x.zip"]}]})",
                           "https://cdn.floserver.org"));
    CHECK_FALSE(list_paths(R"({"runtimes":[{"name":"bad"}]})", "https://cdn.floserver.org"));
    CHECK_FALSE(list_paths(R"({"runtimes":[],"app":{"urls":["https://cdn.example/app.msi"]}})", "https://cdn.floserver.org"));
    CHECK(list_paths(R"({"runtimes":[]})", "https://cdn.floserver.org") == std::vector<std::string>{});
    CHECK_FALSE(list_paths("not json", "https://cdn.floserver.org"));
    CHECK_FALSE(list_paths(R"({"runtimes":3})", "https://cdn.floserver.org"));

    // The live list as read.
    LiveList live;
    live.runtimes.resize(1);
    live.runtimes[0].urls = {"https://cdn.floserver.org/" + std::string(kWin8513), "https://downloads.php.net/x.zip"};
    live.app.emplace();
    live.app->urls = {"https://cdn.floserver.org/app/0.9.1/FloServer-0.9.1-x64.msi"};
    CHECK(list_paths(live, "https://cdn.floserver.org") ==
          std::vector<std::string>{kWin8513, "app/0.9.1/FloServer-0.9.1-x64.msi"});
    live.app->urls = {"https://github.com/x.msi"};
    CHECK_FALSE(list_paths(live, "https://cdn.floserver.org"));
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
    // The live list (12) went up at 10:30 on 25 Nov.
    in.objects[0].last_modified = "2026-11-25T10:30:00.000Z";
    in.objects[1].last_modified = "2026-11-25T10:30:01.000Z";
    // MySQL: an installer Clean up never offers stays, so its source does.
    in.objects.push_back(obj("mysql/8.4.6/mysql-8.4.6-winx64.msi"));
    in.objects.push_back(obj("mysql/8.4.6/mysql-8.4.6.tar.gz"));
    // A tarball that isn't "<name>-<v>.tar.gz" is no source: never offered.
    in.objects.push_back(obj("php/8.5.12/php-8.5.12-macos-arm64.tar.gz"));
    in.live_sequence = 12;
    in.live_paths = {kWin8513, kMaria14, "app/0.9.1/FloServer-0.9.1-x64.msi"};
    in.next_paths = {kWin8513, kLin8513, kMaria14};
    // Saved lists: 10 had 8.5.12, MariaDB 11.4.13 and FloServer 0.9.0 (on
    // 1 Nov); 11 (made 12 Nov, seen live at 11:00) moved PHP and MariaDB
    // on; 12 (25 Nov) nginx 1.30.8 and FloServer 0.9.1.
    in.saved = {saved(11, "2026-11-12T10:00:00Z", {kWin8513, kMaria14, "nginx/1.30.7/nginx-1.30.7.zip", "app/0.9.0/FloServer-0.9.0-x64.msi"},
                      at("2026-11-12T11:00:00Z")),
                saved(10, "2026-11-01T10:00:00Z", {kWin8512, kMaria13, "nginx/1.30.7/nginx-1.30.7.zip", "app/0.9.0/FloServer-0.9.0-x64.msi"}),
                saved(12, "2026-11-25T10:00:00Z", {kWin8513, kMaria14, "nginx/1.30.8/nginx-1.30.8.zip", "app/0.9.1/FloServer-0.9.1-x64.msi"})};
    in.saved[1].sha256[kWin8512] = "beef";
    const auto rows = plan_cleanup(in);
    const auto m = by_path(rows);
    CHECK(m.at(kWin8512).sha256 == "beef");
    CHECK(m.at(kMaria13).sha256.empty());

    // Never offered: listed files, the list itself, anything the catalog doesn't know.
    for (const char* never : {"manifest.json", "manifest.json.sig", "notes/readme.txt", "website/logo.png", kWin8513, kLin8513,
                              kMaria14, "app/0.9.1/FloServer-0.9.1-x64.msi", "php/9.9.9/odd-name.zip",
                              "mysql/8.4.6/mysql-8.4.6-winx64.msi", "php/8.5.12/php-8.5.12-macos-arm64.tar.gz"}) {
        INFO(never);
        CHECK_FALSE(m.contains(never));
    }
    // Left 16 days ago: ticked.
    REQUIRE(m.contains(kWin8512));
    CHECK(m.at(kWin8512).kind == CleanupKind::Ready);
    CHECK(m.at(kWin8512).ticked);
    CHECK(m.at(kWin8512).title == "PHP 8.5.12");
    CHECK(m.at(kWin8512).size == 36310917);
    CHECK(*m.at(kWin8512).left == at("2026-11-12T11:00:00Z"));  // seen live, not signed
    CHECK(left_text(m.at(kWin8512), in.now) == "16 days ago");
    // MariaDB 11.4.13 is gone from every list: its source can go with it.
    CHECK(m.at(kMaria13).kind == CleanupKind::Ready);
    CHECK(m.at(kMaria13Src).kind == CleanupKind::Ready);
    CHECK(m.at(kMaria13Src).ticked);
    CHECK(m.at(kMaria13Src).source);
    CHECK(m.at(kMaria13Src).title == "MariaDB 11.4.13 source");
    // MariaDB 11.4.14 is listed: its source is kept, greyed, never ticked.
    CHECK(m.at(kMaria14Src).kind == CleanupKind::Kept);
    CHECK_FALSE(m.at(kMaria14Src).ticked);
    CHECK(left_text(m.at(kMaria14Src), in.now) == "—");
    CHECK(m.at("mysql/8.4.6/mysql-8.4.6.tar.gz").kind == CleanupKind::Kept);
    // nginx 1.30.7 and FloServer 0.9.0 left when 12 went up, 3 days ago: wait.
    CHECK(m.at("nginx/1.30.7/nginx-1.30.7.zip").kind == CleanupKind::Wait);
    CHECK_FALSE(m.at("nginx/1.30.7/nginx-1.30.7.zip").ticked);
    CHECK(m.at("nginx/1.30.7/nginx-1.30.7.zip").note == "wait: left less than 7 days ago");
    CHECK(*m.at("nginx/1.30.7/nginx-1.30.7.zip").left == at("2026-11-25T10:30:01Z"));
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
    CHECK(rows.back().path == "mysql/8.4.6/mysql-8.4.6.tar.gz");

    // Without the live list: nothing.
    in.live_paths.clear();
    CHECK(plan_cleanup(in).empty());
    in.live_paths = {kWin8513};
    in.live_sequence = 0;
    CHECK(plan_cleanup(in).empty());
}

TEST_CASE("cleanup: when a file left: late uploads, abandoned lists, lists never live, the wait setting") {
    CleanupInput in;
    in.now = at("2026-11-28T12:00:00Z");
    in.live_sequence = 21;
    in.objects = {obj("manifest.json", 10, "2026-11-26T00:00:00.000Z"), obj(kLin8512), obj(kLin8513)};
    in.live_paths = {kLin8513};
    in.saved = {saved(20, "2026-11-01T00:00:00Z", {kLin8512}), saved(21, "2026-11-26T00:00:00Z", {kLin8513})};
    auto m = by_path(plan_cleanup(in));
    CHECK(m.at(kLin8512).title == "PHP 8.5.12 (Linux)");
    CHECK(m.at(kLin8512).kind == CleanupKind::Wait);  // left 2 days ago
    in.wait = days{1};
    m = by_path(plan_cleanup(in));
    CHECK(m.at(kLin8512).kind == CleanupKind::Ready);
    in.wait = kCleanupWait;

    SUBCASE("signed on 2 Nov, uploaded by hand on 26 Nov: counts from the upload") {
        in.saved[1].made = at("2026-11-02T00:00:00Z");
        m = by_path(plan_cleanup(in));
        CHECK(m.at(kLin8512).kind == CleanupKind::Wait);
        CHECK(*m.at(kLin8512).left == at("2026-11-26T00:00:00Z"));
    }
    SUBCASE("an older list seen live early bounds it, a later one never moves it back") {
        in.saved = {saved(19, "2026-10-01T00:00:00Z", {kLin8512}), saved(20, "2026-10-02T00:00:00Z", {kLin8513}, at("2026-11-10T00:00:00Z")),
                    saved(21, "2026-11-26T00:00:00Z", {kLin8513})};
        m = by_path(plan_cleanup(in));
        CHECK(*m.at(kLin8512).left == at("2026-11-10T00:00:00Z"));
        CHECK(m.at(kLin8512).kind == CleanupKind::Ready);
    }
    SUBCASE("an abandoned list of the live sequence that still has it: wait") {
        in.saved.push_back(saved(21, "2026-11-25T00:00:00Z", {kLin8512}));
        m = by_path(plan_cleanup(in));
        CHECK(m.at(kLin8512).kind == CleanupKind::Wait);
        CHECK(m.at(kLin8512).note == "wait: in the newest list signed on this PC");
    }
    SUBCASE("an abandoned twin of the list that dropped it: the later of the two counts") {
        in.saved = {saved(20, "2026-11-01T00:00:00Z", {kLin8512}), saved(21, "2026-11-10T00:00:00Z", {kLin8513}),
                    saved(21, "2026-11-26T00:00:00Z", {kLin8513}), saved(22, "2026-11-27T00:00:00Z", {kLin8512})};
        in.objects[0].last_modified = "2026-11-10T00:00:00.000Z";
        m = by_path(plan_cleanup(in));
        CHECK(*m.at(kLin8512).left == at("2026-11-26T00:00:00Z"));  // 22 never went live: ignored
        CHECK(m.at(kLin8512).kind == CleanupKind::Wait);
    }
    SUBCASE("a list missing here after the one that dropped it may have put it back: counts from the live upload") {
        // 10 had it; 11 dropped it (live 1 Nov); 12 (made elsewhere) put it
        // back; 13 dropped it again and went up on 27 Nov.
        in.live_sequence = 13;
        in.objects[0].last_modified = "2026-11-27T00:00:00.000Z";
        in.saved = {saved(10, "2026-10-20T00:00:00Z", {kLin8512}), saved(11, "2026-10-31T00:00:00Z", {kLin8513}, at("2026-11-01T00:00:00Z")),
                    saved(13, "2026-11-26T00:00:00Z", {kLin8513})};
        m = by_path(plan_cleanup(in));
        CHECK(*m.at(kLin8512).left == at("2026-11-27T00:00:00Z"));
        CHECK(m.at(kLin8512).kind == CleanupKind::Wait);
        CHECK_FALSE(m.at(kLin8512).ticked);
        // With 12 saved here too (without it), 11's time stands.
        in.saved.push_back(saved(12, "2026-11-10T00:00:00Z", {kLin8513}));
        m = by_path(plan_cleanup(in));
        CHECK(*m.at(kLin8512).left == at("2026-11-01T00:00:00Z"));
        CHECK(m.at(kLin8512).kind == CleanupKind::Ready);
        // A gap before the list that dropped it changes nothing.
        in.saved.erase(in.saved.begin());
        in.saved.push_back(saved(8, "2026-10-01T00:00:00Z", {kLin8512}));
        m = by_path(plan_cleanup(in));
        CHECK(*m.at(kLin8512).left == at("2026-11-01T00:00:00Z"));
    }
    SUBCASE("the live list was made elsewhere: counts from its upload") {
        in.live_sequence = 23;
        in.saved = {saved(20, "2026-11-01T00:00:00Z", {kLin8512})};
        m = by_path(plan_cleanup(in));
        CHECK(*m.at(kLin8512).left == at("2026-11-26T00:00:00Z"));
        in.objects.erase(in.objects.begin());  // and no time for the live list
        m = by_path(plan_cleanup(in));
        CHECK(m.at(kLin8512).kind == CleanupKind::Unknown);
        CHECK_FALSE(m.at(kLin8512).ticked);
    }
    SUBCASE("uploaded again since it left: counts from then") {
        in.wait = days{1};
        in.objects[1].last_modified = "2026-11-28T06:00:00.000Z";
        m = by_path(plan_cleanup(in));
        CHECK(m.at(kLin8512).kind == CleanupKind::Wait);
        CHECK(*m.at(kLin8512).left == at("2026-11-28T06:00:00Z"));
    }
}

TEST_CASE("cleanup: a source goes only with every binary in its folder; the last guard") {
    std::vector<CleanupRow> rows(3);
    rows[0].path = kMaria13;
    rows[0].ticked = true;
    rows[1].path = "mariadb/11.4.13/mariadb-11.4.13-winx64-debug.zip";
    rows[1].ticked = true;
    rows[2].path = kMaria13Src;
    rows[2].source = true;
    rows[2].ticked = true;
    tie_sources(rows);
    CHECK(rows[2].ticked);
    rows[1].ticked = false;  // unticked in the window
    tie_sources(rows);
    CHECK_FALSE(rows[2].ticked);
    CHECK(rows[2].note == "goes only with every binary in its folder");

    for (const char* yes : {kWin8512, kLin8512, kMaria13Src, "app/0.9.0/FloServer-0.9.0-x64.msi"}) {
        INFO(yes);
        CHECK(may_delete(yes));
    }
    for (const char* no : {"", "manifest.json", "manifest.json.sig", "/php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip",
                           "website/logo.png", "php/9.9.9/odd-name.zip", "mysql/8.4.6/mysql-8.4.6-winx64.msi",
                           "php/8.5.12/php-8.5.12-macos-arm64.tar.gz"}) {
        INFO(no);
        CHECK_FALSE(may_delete(no));
    }
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

    const std::string rec = cleanup_record({{kWin8512, 36310917, std::string(64, 'c'), "", false},
                                            {kMaria13, 95024126, "", "R2 didn't delete it: Access Denied", false},
                                            {kMaria14, 5, "", "now in the live or next list", true}},
                                           at("2026-11-28T12:40:00Z"), "purged");
    CHECK(rec.find("2026-11-28T12:40:00Z") != std::string::npos);
    CHECK(rec.find("1 of 3 files deleted, 36310917 bytes freed") != std::string::npos);
    CHECK(rec.find("Cloudflare's cache: purged") != std::string::npos);
    CHECK(rec.find("deleted  " + std::string(kWin8512) + "  (36310917 bytes, SHA-256 " + std::string(64, 'c') + ")") !=
          std::string::npos);
    CHECK(rec.find("SKIPPED  " + std::string(kMaria14) + ": now in the live or next list") != std::string::npos);
    CHECK(rec.find("FAILED   " + std::string(kMaria13) + ": R2 didn't delete it: Access Denied") != std::string::npos);
}

TEST_CASE("cleanup: read_saved_lists reads the Publisher's signed folders; mark_live") {
    const test_util::TempDir dir;
    const auto list = [](int seq, const char* path) {
        return R"({"schema":1,"sequence":)" + std::to_string(seq) + R"(,"runtimes":[{"name":"php","sha256":"ab12","urls":["https://cdn.floserver.org/)" +
               std::string(path) + R"("]}]})";
    };
    const auto put = [&](const char* folder, const std::string& json, bool sig = true) {
        test_util::write_file(dir.path() / folder / "manifest.json", json);
        if (sig) {
            test_util::write_file(dir.path() / folder / "manifest.json.sig", "sig");
        }
    };
    put("2026-11-25 sequence 12", list(12, kWin8513));
    test_util::write_file(dir.path() / "2026-11-25 sequence 12" / "upload-steps.txt",
                          "FloServer runtime list, sequence 12, made 2026-11-25T10:20:30Z\r\nmore");
    put("2026-11-01 sequence 10 (2)", list(10, kWin8512));  // no steps file
    put("2026-11-02 sequence 11", "not json");
    put("2026-11-03 sequence 11 (2)", list(11, kWin8512), false);  // never signed
    put("Updates 2026-11-20 101010", list(99, kWin8512));  // not a list folder
    test_util::write_file(dir.path() / "cleanup 2026-11-28.txt", "x");
    const auto sorted = [&] {
        auto lists = read_saved_lists(dir.path(), "https://cdn.floserver.org");
        std::sort(lists.begin(), lists.end(), [](const SavedList& a, const SavedList& b) { return a.sequence < b.sequence; });
        return lists;
    };
    auto lists = sorted();
    REQUIRE(lists.size() == 2);
    CHECK(lists[0].sequence == 10);
    CHECK(lists[0].made == at("2026-11-01T23:59:59Z"));  // the end of the folder's day
    CHECK(lists[0].paths == std::vector<std::string>{kWin8512});
    CHECK(lists[0].sha256.at(kWin8512) == "ab12");
    CHECK_FALSE(lists[0].live_at);
    CHECK(lists[1].sequence == 12);
    CHECK(lists[1].made == at("2026-11-25T10:20:30Z"));
    CHECK(read_saved_lists(dir.path() / "missing", "https://cdn.floserver.org").empty());

    // Seen live: the first time stays.
    CHECK(mark_live(dir.path() / "2026-11-01 sequence 10 (2)", at("2026-11-02T08:00:00Z")));
    CHECK(mark_live(dir.path() / "2026-11-01 sequence 10 (2)", at("2026-11-20T08:00:00Z")));
    lists = sorted();
    CHECK(lists[0].live_at == at("2026-11-02T08:00:00Z"));

    // Check live marks the saved list with the live sequence and files, not an abandoned twin.
    put("2026-11-25 sequence 12 (2)", list(12, kWin8512));
    LiveList live;
    live.sequence = 12;
    live.runtimes.resize(1);
    live.runtimes[0].urls = {"https://cdn.floserver.org/" + std::string(kWin8513)};
    mark_live_lists(dir.path(), live, "https://cdn.floserver.org", at("2026-11-26T09:00:00Z"));
    CHECK(std::filesystem::exists(dir.path() / "2026-11-25 sequence 12" / "live-at.txt"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / "2026-11-25 sequence 12 (2)" / "live-at.txt"));
    lists = sorted();
    REQUIRE(lists.size() == 3);
    const auto twelve = std::find_if(lists.begin(), lists.end(), [](const SavedList& l) { return l.live_at && l.sequence == 12; });
    REQUIRE(twelve != lists.end());
    CHECK(*twelve->live_at == at("2026-11-26T09:00:00Z"));
}

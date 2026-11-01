// Settings → Folders: the data folder's size and its two clean-ups, on a
// made-up data folder. What each removes, and above all what it never touches.

#include "floserver/core/data_folder.h"

#include "floserver/core/paths.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/project_service.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <string>

using namespace floserver::core;
namespace fs = std::filesystem;
using test_util::TempDir;
using test_util::write_file;
using Clock = std::chrono::steady_clock;

namespace {

Clock::time_point later() { return Clock::now() + std::chrono::seconds(30); }

bool listed(const std::vector<CleanItem>& items, const fs::path& p) {
    return std::any_of(items.begin(), items.end(), [&](const CleanItem& i) { return i.path == p; });
}

// A data folder as FloServer leaves it, with something in every place.
struct FakeData {
    TempDir dir;
    fs::path d = dir.path();
    FakeData() {
        write_file(d / "config.json", R"({"projects": [], "mine": 1})");
        write_file(d / "runtimes" / "php" / "8.5.1" / "php.exe", "installed php");
        write_file(d / "runtimes" / "php" / ".staging-8.5.2-ab12" / "php.exe", "half made");
        write_file(d / "runtimes" / "php" / ".removing-8.4.0-cd34" / "ext" / "a.dll", "half gone");
        write_file(d / "runtimes" / "nginx" / "1.27.0" / "nginx.exe", "installed nginx");
        write_file(d / "downloads" / "php-8.5.2.zip.part", "12345");
        write_file(d / "downloads" / "php-8.5.2.zip.download", "123");
        write_file(d / "downloads" / "notes.txt", "not ours to guess");
        write_file(d / "templates" / "job-7" / "step-1.log", "composer said");
        write_file(d / "templates" / "readme.txt", "kept");
        write_file(d / "web" / "logs" / "nginx-error.log", "error lines");
        write_file(d / "web" / "logs" / "sites" / "shop.log", "1 200 0.1 GET /");
        write_file(d / "web" / "logs" / "nginx.pid", "123");
        write_file(d / "web" / "apps" / "api.log", "uvicorn running");
        write_file(d / "web" / "nginx.conf", "events {}");
        write_file(d / "cli" / "php" / "php-errors.log", "warning");
        write_file(d / "cli" / "php" / "8.5.1.ini", "memory_limit=1G");
        write_file(d / "data" / "mariadb-11.8" / "mariadbd.err", "database log");
        write_file(d / "data" / "mariadb-11.8" / "shop" / "t.ibd", "rows");
        write_file(d / "certs" / "ca.key", "secret key");
        write_file(d / "secrets" / "root.txt", "password");
        write_file(d / "php" / "8.5" / "custom.ini", "; mine");
    }
};

}  // namespace

TEST_CASE("the data folder's size counts every file and stops at its deadline") {
    const FakeData f;
    const DiskUsage all = disk_usage(f.d, later());
    CHECK(all.complete);
    CHECK(all.files == 22);
    CHECK(all.bytes > 100);
    CHECK(disk_usage(f.d / "missing", later()).bytes == 0);

    // A deadline already gone: what's counted so far, marked as such.
    for (int i = 0; i < 200; ++i) {
        write_file(f.d / "many" / (std::to_string(i) + ".txt"), "x");
    }
    const DiskUsage cut = disk_usage(f.d, Clock::now() - std::chrono::seconds(1));
    CHECK_FALSE(cut.complete);
}

#ifndef _WIN32
TEST_CASE("the size walk and the clean-up never go through a link") {
    const FakeData f;
    const TempDir outside;
    write_file(outside.path() / "precious.txt", "the user's");
    fs::create_directory_symlink(outside.path(), f.d / "runtimes" / "php" / ".staging-8.5.2-ab12" / "link");
    fs::create_directory_symlink(outside.path(), f.d / "templates" / "job-8");
    const DiskUsage before = disk_usage(f.d, later());
    CHECK(before.files == 22);  // precious.txt isn't counted

    const auto items = leftovers(f.d, false, false, later());
    const CleanResult done = remove_leftovers(f.d, items);
    CHECK(done.skipped == 0);
    CHECK_FALSE(fs::exists(f.d / "runtimes" / "php" / ".staging-8.5.2-ab12"));
    CHECK_FALSE(fs::exists(fs::symlink_status(f.d / "templates" / "job-8")));
    CHECK(test_util::read_file(outside.path() / "precious.txt") == "the user's");
}
#endif

#ifdef _WIN32
TEST_CASE("on Windows a junction or folder link in a work folder goes as a link, never followed") {
    const FakeData f;
    const TempDir outside;
    write_file(outside.path() / "precious.txt", "the user's");
    const fs::path job = f.d / "templates" / "job-7";
    // A junction needs no rights: made with cmd's mklink.
    const fs::path junction = job / "junction";
    const std::wstring command = L"mklink /J \"" + junction.wstring() + L"\" \"" + outside.path().wstring() + L"\" >nul";
    REQUIRE(_wsystem(command.c_str()) == 0);
    REQUIRE(fs::is_regular_file(junction / "precious.txt"));
    // A folder symlink needs Developer Mode or an administrator: tried, skipped when refused.
    std::error_code ec;
    fs::create_directory_symlink(outside.path(), job / "symlink", ec);
    INFO("folder symlink made: " << !ec);

    // Neither is walked into: the count and the work folder's size are as without them.
    CHECK(disk_usage(f.d, later()).files == 22);
    const auto items = leftovers(f.d, false, false, later());
    const auto it = std::find_if(items.begin(), items.end(), [&](const CleanItem& i) { return i.path == job; });
    REQUIRE(it != items.end());
    CHECK(it->bytes == 13);

    const CleanResult done = remove_leftovers(f.d, items);
    CHECK(done.skipped == 0);
    CHECK_FALSE(fs::exists(fs::symlink_status(job)));
    CHECK(test_util::read_file(outside.path() / "precious.txt") == "the user's");
}
#endif

namespace {

// Every log has been quiet for two days.
CleanOptions quiet() {
    CleanOptions o;
    o.now = fs::file_time_type::clock::now() + std::chrono::hours(48);
    return o;
}

bool skipped_path(const std::vector<CleanSkip>& items, const std::string& path, const std::string& why = {}) {
    return std::any_of(items.begin(), items.end(),
                       [&](const CleanSkip& s) { return s.path == path && (why.empty() || s.why == why); });
}

}  // namespace

TEST_CASE("clear old logs: FloServer's quiet logs are emptied, never a project's request log") {
    const FakeData f;
    const CleanOptions o = quiet();
    const auto logs = log_files(f.d, o);
    CHECK(logs.size() == 3);
    CHECK(listed(logs, f.d / "web" / "logs" / "nginx-error.log"));
    CHECK(listed(logs, f.d / "web" / "apps" / "api.log"));
    CHECK(listed(logs, f.d / "cli" / "php" / "php-errors.log"));
    // Never a project's request log (the panel's Log tab), the database's own
    // log, a pid file, a template's step log (a leftover instead).
    CHECK_FALSE(listed(logs, f.d / "web" / "logs" / "sites" / "shop.log"));
    CHECK_FALSE(listed(logs, f.d / "data" / "mariadb-11.8" / "mariadbd.err"));
    CHECK_FALSE(listed(logs, f.d / "web" / "logs" / "nginx.pid"));
    CHECK(total_bytes(logs) == 11 + 15 + 7);

    const CleanResult done = empty_logs(f.d, logs, o);
    CHECK(done.done == 3);
    CHECK(done.freed == 33);
    CHECK(done.skipped == 0);
    // Emptied, not deleted: a server's open handle writes on.
    CHECK(fs::is_regular_file(f.d / "web" / "logs" / "nginx-error.log"));
    CHECK(fs::file_size(f.d / "web" / "logs" / "nginx-error.log") == 0);
    CHECK(log_files(f.d, o).empty());  // empty logs aren't listed again
    CHECK(test_util::read_file(f.d / "web" / "logs" / "sites" / "shop.log") == "1 200 0.1 GET /");
    CHECK(test_util::read_file(f.d / "web" / "logs" / "nginx.pid") == "123");
    CHECK(test_util::read_file(f.d / "data" / "mariadb-11.8" / "mariadbd.err") == "database log");

    // A list that names anything else is refused item by item, and named.
    const std::vector<CleanItem> forged{{f.d / "config.json", 10, false, false},
                                        {f.d / "web" / "logs" / ".." / ".." / "certs" / "ca.log", 1, false, true},
                                        {f.d / "data" / "mariadb-11.8" / "mariadbd.err", 12, false, true},
                                        {f.d / "web" / "logs" / "sites" / "shop.log", 15, false, false}};
    const CleanResult refused = empty_logs(f.d, forged, o);
    CHECK(refused.done == 0);
    CHECK(refused.skipped == 4);
    CHECK(refused.skipped_items.size() == 4);
    CHECK(skipped_path(refused.skipped_items, "config.json", "not_allowed"));
    CHECK(test_util::read_file(f.d / "config.json") == R"({"projects": [], "mine": 1})");
    CHECK(test_util::read_file(f.d / "web" / "logs" / "sites" / "shop.log") == "1 200 0.1 GET /");
}

TEST_CASE("clear old logs: older copies go, a log written within the day stays, the sleep markers are never touched") {
    const FakeData f;
    write_file(f.d / "web" / "logs" / "nginx-error.log.1", "12345");
    write_file(f.d / "web" / "logs" / "apache-error.log.old", "123");
    write_file(f.d / "web" / "logs" / "php.log.bak", "12");
    write_file(f.d / "web" / "logs" / "nginx-error.log.2.gz", "1");
    write_file(f.d / "web" / "logs" / "nginx-access.log.gz", "1");
    write_file(f.d / "web" / "apps" / "api.log.20261031", "1234");
    write_file(f.d / "web" / "logs" / "8.5-a1b2.access.log", "1730000000.1");    // a PHP pool's idle log
    write_file(f.d / "web" / "logs" / "8.5-a1b2.access.log.1", "1730000000.1");  // not a copy of ours either
    write_file(f.d / "web" / "logs" / "sites" / "shop.log.1", "old requests");    // the Log tab's: not a place
    write_file(f.d / "web" / "logs" / "deep" / "x.log.1", "a sub-folder");
    write_file(f.d / "data" / "mariadb-11.8" / "mariadbd.err.1", "database");
    write_file(f.d / "data" / "mariadb-11.8" / "slow.log.1", "database");
    write_file(f.d / "web" / "logs" / "notes.log.txt", "not a log");

    // Just written: only the older copies go.
    const CleanOptions now;
    const auto fresh = log_files(f.d, now);
    CHECK(fresh.size() == 6);
    for (const CleanItem& item : fresh) {
        CHECK(item.remove);
    }
    const CleanResult done = empty_logs(f.d, fresh, now);
    CHECK(done.done == 6);
    CHECK(done.freed == 5 + 3 + 2 + 1 + 1 + 4);
    for (const char* gone : {"nginx-error.log.1", "apache-error.log.old", "php.log.bak", "nginx-error.log.2.gz",
                             "nginx-access.log.gz"}) {
        CHECK_FALSE(fs::exists(f.d / "web" / "logs" / gone));
    }
    CHECK_FALSE(fs::exists(f.d / "web" / "apps" / "api.log.20261031"));
    // The current logs, written just now, are as they were.
    CHECK(test_util::read_file(f.d / "web" / "logs" / "nginx-error.log") == "error lines");
    CHECK(test_util::read_file(f.d / "web" / "apps" / "api.log") == "uvicorn running");

    // A day later they're emptied; the rest never.
    const CleanOptions later_on = quiet();
    (void)empty_logs(f.d, log_files(f.d, later_on), later_on);
    CHECK(fs::file_size(f.d / "web" / "logs" / "nginx-error.log") == 0);
    CHECK(test_util::read_file(f.d / "web" / "logs" / "8.5-a1b2.access.log") == "1730000000.1");
    CHECK(test_util::read_file(f.d / "web" / "logs" / "8.5-a1b2.access.log.1") == "1730000000.1");
    CHECK(test_util::read_file(f.d / "web" / "logs" / "sites" / "shop.log.1") == "old requests");
    CHECK(test_util::read_file(f.d / "web" / "logs" / "sites" / "shop.log") == "1 200 0.1 GET /");
    CHECK(test_util::read_file(f.d / "web" / "logs" / "deep" / "x.log.1") == "a sub-folder");
    CHECK(test_util::read_file(f.d / "data" / "mariadb-11.8" / "mariadbd.err.1") == "database");
    CHECK(test_util::read_file(f.d / "data" / "mariadb-11.8" / "slow.log.1") == "database");
    CHECK(test_util::read_file(f.d / "data" / "mariadb-11.8" / "mariadbd.err") == "database log");
    CHECK(test_util::read_file(f.d / "web" / "logs" / "notes.log.txt") == "not a log");

    // A current log the list says is due, but written to since: left.
    write_file(f.d / "web" / "logs" / "nginx-error.log", "new line");
    const CleanResult recheck = empty_logs(f.d, {{f.d / "web" / "logs" / "nginx-error.log", 8, false, false}}, now);
    CHECK(recheck.done == 0);
    CHECK(test_util::read_file(f.d / "web" / "logs" / "nginx-error.log") == "new line");

    CHECK(is_rotated_log_name("x.log.1"));
    CHECK(is_rotated_log_name("X.LOG.OLD"));
    CHECK(is_rotated_log_name("x.log.2026-10-31"));
    CHECK_FALSE(is_rotated_log_name("x.log"));
    CHECK_FALSE(is_rotated_log_name("x.log.txt"));
    CHECK_FALSE(is_rotated_log_name(".log.1"));
    CHECK_FALSE(is_log_file(f.d, f.d / "web" / "logs" / "pool.access.log"));
}

TEST_CASE("a file another program holds is skipped and named; the rest goes on") {
    const FakeData f;
    write_file(f.d / "web" / "logs" / "nginx-error.log.1", "12345");
    write_file(f.d / "web" / "logs" / "php.log.old", "123");
    CleanOptions o = quiet();
    const auto locked = [](const fs::path& p) {
        const std::string name = path_to_utf8(p.filename());
        return name == "nginx-error.log" || name == "nginx-error.log.1" || name == "php-8.5.2.zip.part" ||
               name == "php.exe";
    };
    o.remove_file = [&](const fs::path& p, std::error_code& ec) {
        if (locked(p)) {
            ec = std::make_error_code(std::errc::permission_denied);  // what Windows says for a held file
            return;
        }
        fs::remove(p, ec);
    };
    o.empty_file = [&](const fs::path& p, std::error_code& ec) {
        if (locked(p)) {
            ec = std::make_error_code(std::errc::device_or_resource_busy);
            return;
        }
        fs::resize_file(p, 0, ec);
    };
    const CleanResult logs = empty_logs(f.d, log_files(f.d, o), o);
    CHECK(logs.freed == 3 + 15 + 7);  // php.log.old, api.log, php-errors.log
    CHECK(logs.skipped == 2);
    REQUIRE(logs.skipped_items.size() == 2);
    CHECK(skipped_path(logs.skipped_items, "web/logs/nginx-error.log", "in_use"));
    CHECK(skipped_path(logs.skipped_items, "web/logs/nginx-error.log.1", "in_use"));
    CHECK(logs.skipped_items[0].reason == "in use by another program");
    CHECK(test_util::read_file(f.d / "web" / "logs" / "nginx-error.log") == "error lines");
    CHECK_FALSE(fs::exists(f.d / "web" / "logs" / "php.log.old"));

    const CleanResult left = remove_leftovers(f.d, leftovers(f.d, false, false, later(), o), o);
    CHECK(left.skipped == 2);
    CHECK(skipped_path(left.skipped_items, "downloads/php-8.5.2.zip.part", "in_use"));
    CHECK(skipped_path(left.skipped_items, "runtimes/php/.staging-8.5.2-ab12", "in_use"));
    CHECK(fs::exists(f.d / "runtimes" / "php" / ".staging-8.5.2-ab12" / "php.exe"));
    CHECK_FALSE(fs::exists(f.d / "runtimes" / "php" / ".removing-8.4.0-cd34"));
    CHECK_FALSE(fs::exists(f.d / "templates" / "job-7"));
    CHECK_FALSE(fs::exists(f.d / "downloads" / "php-8.5.2.zip.download"));
}

TEST_CASE("projects, the projects folder and the www folder in a clean-up place are never touched") {
    const FakeData f;
    // Odd, but possible by hand: a project in web/apps, the www folder a work
    // folder, the projects folder inside the downloads.
    write_file(f.d / "web" / "apps" / "old.log.1", "a project's file");
    write_file(f.d / "templates" / "job-www" / "index.php", "a www site");
    write_file(f.d / "downloads" / "mine" / "keep.txt", "the user's");
    CleanOptions o = quiet();
    o.protected_dirs = {f.d / "web" / "apps", f.d / "templates" / "job-www", f.d / "downloads" / "mine"};
    std::vector<CleanSkip> places;
    const auto logs = log_files(f.d, o, &places);
    CHECK_FALSE(listed(logs, f.d / "web" / "apps" / "api.log"));
    CHECK_FALSE(listed(logs, f.d / "web" / "apps" / "old.log.1"));
    CHECK(skipped_path(places, "web/apps", "protected"));
    (void)empty_logs(f.d, logs, o);
    CHECK(test_util::read_file(f.d / "web" / "apps" / "old.log.1") == "a project's file");
    CHECK(test_util::read_file(f.d / "web" / "apps" / "api.log") == "uvicorn running");

    const auto items = leftovers(f.d, false, false, later(), o, &places);
    CHECK_FALSE(listed(items, f.d / "templates" / "job-www"));
    CHECK(skipped_path(places, "templates/job-www", "protected"));
    // The downloads folder holds the projects folder: its leftovers still go,
    // what's in the protected folder is never looked at.
    CHECK(listed(items, f.d / "downloads" / "php-8.5.2.zip.part"));
    write_file(f.d / "downloads" / "mine.part", "x");
    CleanOptions mine = o;
    mine.protected_dirs.push_back(f.d / "downloads" / "mine.part");
    std::vector<CleanSkip> mine_places;
    CHECK_FALSE(listed(leftovers(f.d, false, false, later(), mine, &mine_places), f.d / "downloads" / "mine.part"));
    CHECK(skipped_path(mine_places, "downloads/mine.part", "protected"));
    fs::remove(f.d / "downloads" / "mine.part");
    (void)remove_leftovers(f.d, items, o);
    CHECK(test_util::read_file(f.d / "templates" / "job-www" / "index.php") == "a www site");
    CHECK(test_util::read_file(f.d / "downloads" / "mine" / "keep.txt") == "the user's");
    // The rest still went.
    CHECK_FALSE(fs::exists(f.d / "templates" / "job-7"));
    CHECK_FALSE(fs::exists(f.d / "runtimes" / "php" / ".staging-8.5.2-ab12"));
    CHECK(fs::file_size(f.d / "web" / "logs" / "nginx-error.log") == 0);

    // A list naming a protected leftover is refused too.
    const CleanResult refused = remove_leftovers(f.d, {{f.d / "templates" / "job-www", 0, true, true}}, o);
    CHECK(refused.done == 0);
    CHECK(skipped_path(refused.skipped_items, "templates/job-www", "protected"));

    // A protected folder that holds the data folder: nothing is touched.
    const FakeData g;
    CleanOptions all = quiet();
    all.protected_dirs = {g.d};
    CHECK(log_files(g.d, all).empty());
    CHECK(leftovers(g.d, false, false, later(), all).empty());
}

#ifndef _WIN32
TEST_CASE("a linked top-level folder is never gone into; it's named") {
    const FakeData f;
    const TempDir outside;
    write_file(outside.path() / "logs" / "a.log.1", "keep");
    write_file(outside.path() / "logs" / "b.log", "keep");
    write_file(outside.path() / "dl" / "a.part", "keep");
    write_file(outside.path() / "rt" / "php" / ".staging-x" / "f", "keep");
    write_file(outside.path() / "tpl" / "job-1" / "f", "keep");
    write_file(outside.path() / "cli" / "php-errors.log", "keep");
    for (const char* top : {"web", "downloads", "runtimes", "templates", "cli"}) {
        fs::remove_all(f.d / top);
    }
    fs::create_directory_symlink(outside.path(), f.d / "web");  // web/logs is then outside/logs
    fs::create_directory_symlink(outside.path() / "dl", f.d / "downloads");
    fs::create_directory_symlink(outside.path() / "rt", f.d / "runtimes");
    fs::create_directory_symlink(outside.path() / "tpl", f.d / "templates");
    fs::create_directories(f.d / "cli");
    fs::create_directory_symlink(outside.path() / "cli", f.d / "cli" / "php");

    const CleanOptions o = quiet();
    std::vector<CleanSkip> places;
    CHECK(log_files(f.d, o, &places).empty());
    CHECK(leftovers(f.d, false, false, later(), o, &places).empty());
    CHECK(skipped_path(places, "web", "link"));
    CHECK(skipped_path(places, "downloads", "link"));
    CHECK(skipped_path(places, "runtimes", "link"));
    CHECK(skipped_path(places, "templates", "link"));
    CHECK(skipped_path(places, "cli/php", "link"));
    CHECK(places[0].reason == "it's a link; FloServer doesn't follow links");

    // A list naming files through the links is refused too.
    const CleanResult logs = empty_logs(f.d, {{f.d / "web" / "logs" / "a.log.1", 4, false, true},
                                              {f.d / "web" / "logs" / "b.log", 4, false, false}}, o);
    CHECK(logs.done == 0);
    CHECK(skipped_path(logs.skipped_items, "web/logs/a.log.1", "link"));
    const CleanResult left = remove_leftovers(f.d, {{f.d / "downloads" / "a.part", 4, false, true},
                                                    {f.d / "runtimes" / "php" / ".staging-x", 4, true, true},
                                                    {f.d / "templates" / "job-1", 4, true, true}}, o);
    CHECK(left.done == 0);
    CHECK(left.skipped == 3);
    CHECK(skipped_path(left.skipped_items, "downloads", "link"));
    for (const fs::path& kept : {outside.path() / "logs" / "a.log.1", outside.path() / "logs" / "b.log",
                                 outside.path() / "dl" / "a.part", outside.path() / "rt" / "php" / ".staging-x" / "f",
                                 outside.path() / "tpl" / "job-1" / "f", outside.path() / "cli" / "php-errors.log"}) {
        INFO(path_to_utf8(kept));
        CHECK(test_util::read_file(kept) == "keep");
    }

    // A linked runtime's own folder (runtimes/<name>) isn't gone into either.
    const FakeData g;
    write_file(outside.path() / "node" / ".staging-1" / "f", "keep");
    fs::create_directory_symlink(outside.path() / "node", g.d / "runtimes" / "node");
    std::vector<CleanSkip> g_places;
    const auto items = leftovers(g.d, false, false, later(), o, &g_places);
    CHECK(skipped_path(g_places, "runtimes/node", "link"));
    (void)remove_leftovers(g.d, items, o);
    CHECK(test_util::read_file(outside.path() / "node" / ".staging-1" / "f") == "keep");
    // A log that's a link to a file is left and named, not emptied through.
    write_file(outside.path() / "linked-target.log", "keep");
    fs::create_symlink(outside.path() / "linked-target.log", g.d / "web" / "logs" / "linked.log");
    std::vector<CleanSkip> log_places;
    (void)empty_logs(g.d, log_files(g.d, o, &log_places), o);
    CHECK(skipped_path(log_places, "web/logs/linked.log", "link"));
    CHECK(test_util::read_file(outside.path() / "linked-target.log") == "keep");
}
#endif

TEST_CASE("leftovers are the cut-off downloads, half-made installs and work folders only") {
    const FakeData f;
    const auto items = leftovers(f.d, false, false, later());
    CHECK(items.size() == 5);
    CHECK(listed(items, f.d / "downloads" / "php-8.5.2.zip.part"));
    CHECK(listed(items, f.d / "downloads" / "php-8.5.2.zip.download"));
    CHECK(listed(items, f.d / "runtimes" / "php" / ".staging-8.5.2-ab12"));
    CHECK(listed(items, f.d / "runtimes" / "php" / ".removing-8.4.0-cd34"));
    CHECK(listed(items, f.d / "templates" / "job-7"));
    CHECK(total_bytes(items) == 5 + 3 + 9 + 9 + 13);

    // While an install runs its download and folder are left alone; while a
    // project is being made, its work folder.
    const auto installing = leftovers(f.d, true, false, later());
    CHECK(installing.size() == 1);
    CHECK(listed(installing, f.d / "templates" / "job-7"));
    CHECK(leftovers(f.d, false, true, later()).size() == 4);
    CHECK(leftovers(f.d, true, true, later()).empty());

    const CleanResult done = remove_leftovers(f.d, items);
    CHECK(done.done == 5);
    CHECK(done.freed == 39);
    CHECK(leftovers(f.d, false, false, later()).empty());
    // Everything else is where it was.
    for (const fs::path& kept :
         {f.d / "config.json", f.d / "runtimes" / "php" / "8.5.1" / "php.exe", f.d / "runtimes" / "nginx" / "1.27.0" / "nginx.exe",
          f.d / "downloads" / "notes.txt", f.d / "templates" / "readme.txt", f.d / "data" / "mariadb-11.8" / "shop" / "t.ibd",
          f.d / "certs" / "ca.key", f.d / "secrets" / "root.txt", f.d / "php" / "8.5" / "custom.ini",
          f.d / "web" / "logs" / "nginx-error.log"}) {
        INFO(path_to_utf8(kept));
        CHECK(fs::is_regular_file(kept));
    }
}

TEST_CASE("remove_leftovers refuses anything that isn't a leftover, whatever the list says") {
    const FakeData f;
    const std::vector<CleanItem> forged{
        {f.d / "runtimes" / "php" / "8.5.1", 13, true, true},
        {f.d / "runtimes" / ".staging-x", 0, true, true},  // not inside a runtime's folder
        {f.d / "data", 0, true, true},
        {f.d / "downloads" / ".." / "certs" / "ca.key.part", 0, false, true},
        {f.d / "templates" / "job-7" / ".." / ".." / "secrets", 0, true, true},
        {f.d.parent_path() / "downloads" / "x.part", 0, false, true},
    };
    const CleanResult refused = remove_leftovers(f.d, forged);
    CHECK(refused.done == 0);
    CHECK(refused.skipped == forged.size());
    CHECK(fs::is_regular_file(f.d / "runtimes" / "php" / "8.5.1" / "php.exe"));
    CHECK(fs::is_regular_file(f.d / "secrets" / "root.txt"));
    CHECK(fs::is_regular_file(f.d / "data" / "mariadb-11.8" / "shop" / "t.ibd"));

    CHECK(is_leftover(f.d, f.d / "runtimes" / "node" / ".removing-24.1.0-x"));
    CHECK_FALSE(is_leftover(f.d, f.d / "runtimes" / "node" / "staging-24.1.0"));
    CHECK(is_log_file(f.d, f.d / "web" / "logs" / "nginx-error.log"));
    CHECK_FALSE(is_log_file(f.d, f.d / "web" / "logs" / "sites" / "blog.log"));  // the Log tab's
    CHECK_FALSE(is_log_file(f.d, f.d / "web" / "logs" / "sites" / "deeper" / "blog.log"));
    CHECK_FALSE(is_log_file(f.d, f.d / "data" / "mariadb-11.8" / "x.log"));
}

TEST_CASE("the projects folder is kept in config.json, with the other keys") {
    const TempDir data;
    const TempDir chosen;
    write_file(app_config_file(data.path()),
               R"({"projects": [], "web": {"http_port": 8080}, "from_a_newer_window": {"x": [1, 2]}})");
    ProjectService service(data.path());
    ServiceRefusal refusal;

    const auto start = service.projects_folder(refusal);
    REQUIRE(start);
    CHECK((*start)["custom"] == false);
    CHECK((*start)["projects_dir"] == (*start)["default_projects_dir"]);

    const auto set = service.set_projects_folder(chosen.path(), refusal);
    INFO(refusal.message);
    REQUIRE(set);
    CHECK((*set)["custom"] == true);
    // New project (the wizard) gets it from projects.list.
    const auto list = service.list(refusal);
    REQUIRE(list);
    std::string error;
    const auto real = real_folder(chosen.path(), error);  // long names, as the service keeps it
    REQUIRE(real);
    CHECK(path_from_utf8((*list)["projects_dir"].get<std::string>()) == *real);
    const auto saved = load_app_config(app_config_file(data.path()), error);
    REQUIRE(saved);
    REQUIRE(saved->projects_dir);
    CHECK(saved->other["web"]["http_port"] == 8080);
    CHECK(saved->other["from_a_newer_window"]["x"].size() == 2);

    // Refused: not a full path, a file, inside FloServer's data folder (made
    // or not yet), a network share.
    CHECK_FALSE(service.set_projects_folder(fs::path("projects"), refusal));
    CHECK(refusal.code == "bad_params");
    write_file(chosen.path() / "a-file", "x");
    CHECK_FALSE(service.set_projects_folder(chosen.path() / "a-file", refusal));
    CHECK(refusal.code == "bad_folder");
    CHECK_FALSE(service.set_projects_folder(chosen.path() / "a-file" / "below", refusal));
    fs::create_directories(data.path() / "runtimes");
    CHECK_FALSE(service.set_projects_folder(data.path() / "runtimes", refusal));
    CHECK_FALSE(service.set_projects_folder(data.path(), refusal));
    CHECK_FALSE(service.set_projects_folder(data.path() / "not-yet" / "made", refusal));
    CHECK(refusal.message.find("data folder") != std::string::npos);
#ifdef _WIN32
    CHECK_FALSE(service.set_projects_folder(fs::path(LR"(\\server\share\projects)"), refusal));
#else
    CHECK_FALSE(service.set_projects_folder(fs::path("//server/share/projects"), refusal));
#endif
    CHECK(refusal.message.find("network share") != std::string::npos);
    CHECK(load_app_config(app_config_file(data.path()), error)->projects_dir.has_value());  // unchanged

    // A folder that isn't there yet: kept (made when the first project goes
    // there, or Open is pressed), the part that's there in its real names.
    const fs::path later_dir = chosen.path() / "not yet" / path_from_utf8("پڕۆژەکان");
    const auto pending = service.set_projects_folder(later_dir, refusal);
    INFO(refusal.message);
    REQUIRE(pending);
    CHECK((*pending)["custom"] == true);
    CHECK_FALSE(fs::exists(later_dir));
    CHECK(path_from_utf8((*pending)["projects_dir"].get<std::string>()) ==
          *real / "not yet" / path_from_utf8("پڕۆژەکان"));

    // Back to the default: the key goes, the others stay.
    const auto reset = service.set_projects_folder(std::nullopt, refusal);
    REQUIRE(reset);
    CHECK((*reset)["custom"] == false);
    const auto after = load_app_config(app_config_file(data.path()), error);
    REQUIRE(after);
    CHECK_FALSE(after->projects_dir.has_value());
    CHECK(after->other["from_a_newer_window"]["x"].size() == 2);
}

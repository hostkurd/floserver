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

TEST_CASE("clear old logs lists FloServer's logs only and empties them") {
    const FakeData f;
    const auto logs = log_files(f.d);
    CHECK(logs.size() == 4);
    CHECK(listed(logs, f.d / "web" / "logs" / "nginx-error.log"));
    CHECK(listed(logs, f.d / "web" / "logs" / "sites" / "shop.log"));
    CHECK(listed(logs, f.d / "web" / "apps" / "api.log"));
    CHECK(listed(logs, f.d / "cli" / "php" / "php-errors.log"));
    // Never the database's own log, a pid file, a template's step log (a leftover instead).
    CHECK_FALSE(listed(logs, f.d / "data" / "mariadb-11.8" / "mariadbd.err"));
    CHECK_FALSE(listed(logs, f.d / "web" / "logs" / "nginx.pid"));
    CHECK(total_bytes(logs) == 11 + 15 + 15 + 7);

    const CleanResult done = empty_logs(f.d, logs);
    CHECK(done.done == 4);
    CHECK(done.freed == 48);
    // Emptied, not deleted: a server's open handle writes on.
    CHECK(fs::is_regular_file(f.d / "web" / "logs" / "nginx-error.log"));
    CHECK(fs::file_size(f.d / "web" / "logs" / "nginx-error.log") == 0);
    CHECK(log_files(f.d).empty());  // empty logs aren't listed again
    CHECK(test_util::read_file(f.d / "web" / "logs" / "nginx.pid") == "123");
    CHECK(test_util::read_file(f.d / "data" / "mariadb-11.8" / "mariadbd.err") == "database log");

    // A list that names anything else is refused item by item.
    const std::vector<CleanItem> forged{{f.d / "config.json", 10, false},
                                        {f.d / "web" / "logs" / ".." / ".." / "certs" / "ca.log", 1, false},
                                        {f.d / "data" / "mariadb-11.8" / "mariadbd.err", 12, false}};
    const CleanResult refused = empty_logs(f.d, forged);
    CHECK(refused.done == 0);
    CHECK(refused.skipped == 3);
    CHECK(test_util::read_file(f.d / "config.json") == R"({"projects": [], "mine": 1})");
}

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
        {f.d / "runtimes" / "php" / "8.5.1", 13, true},
        {f.d / "runtimes" / ".staging-x", 0, true},  // not inside a runtime's folder
        {f.d / "data", 0, true},
        {f.d / "downloads" / ".." / "certs" / "ca.key.part", 0, false},
        {f.d / "templates" / "job-7" / ".." / ".." / "secrets", 0, true},
        {f.d.parent_path() / "downloads" / "x.part", 0, false},
    };
    const CleanResult refused = remove_leftovers(f.d, forged);
    CHECK(refused.done == 0);
    CHECK(refused.skipped == forged.size());
    CHECK(fs::is_regular_file(f.d / "runtimes" / "php" / "8.5.1" / "php.exe"));
    CHECK(fs::is_regular_file(f.d / "secrets" / "root.txt"));
    CHECK(fs::is_regular_file(f.d / "data" / "mariadb-11.8" / "shop" / "t.ibd"));

    CHECK(is_leftover(f.d, f.d / "runtimes" / "node" / ".removing-24.1.0-x"));
    CHECK_FALSE(is_leftover(f.d, f.d / "runtimes" / "node" / "staging-24.1.0"));
    CHECK(is_log_file(f.d, f.d / "web" / "logs" / "sites" / "blog.log"));
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

    // Refused: not a full path, missing, inside FloServer's data folder.
    CHECK_FALSE(service.set_projects_folder(fs::path("projects"), refusal));
    CHECK(refusal.code == "bad_params");
    CHECK_FALSE(service.set_projects_folder(chosen.path() / "missing", refusal));
    fs::create_directories(data.path() / "runtimes");
    CHECK_FALSE(service.set_projects_folder(data.path() / "runtimes", refusal));
    CHECK_FALSE(service.set_projects_folder(data.path(), refusal));
    CHECK(load_app_config(app_config_file(data.path()), error)->projects_dir.has_value());  // unchanged

    // Back to the default: the key goes, the others stay.
    const auto reset = service.set_projects_folder(std::nullopt, refusal);
    REQUIRE(reset);
    CHECK((*reset)["custom"] == false);
    const auto after = load_app_config(app_config_file(data.path()), error);
    REQUIRE(after);
    CHECK_FALSE(after->projects_dir.has_value());
    CHECK(after->other["from_a_newer_window"]["x"].size() == 2);
}

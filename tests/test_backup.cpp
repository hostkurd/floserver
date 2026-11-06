// Backups (D22): what a backup leaves out and holds, its floserver-backup.json,
// unpacking one by the zip rules, the SQL made ready to load, .env and
// wp-config.php pointed at the new database, the names, and the jobs with
// the service's other parts as fakes. test_database_service.cpp dumps and
// loads a real MariaDB.

#include "floserver/core/backup.h"
#include "floserver/core/backup_service.h"
#include "floserver/core/paths.h"
#include "floserver/core/zip_extract.h"
#include "test_util.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <map>
#include <set>
#include <thread>

using namespace floserver::core;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// A project folder with the three folders that can be left out.
fs::path make_project(const fs::path& root) {
    const fs::path dir = root / "shop";
    test_util::write_file(dir / "index.php", "<?php echo 'hi';");
    test_util::write_file(dir / "floserver.json", R"({"php": "8.5", "database": "mariadb-11.4", "web": "nginx"})");
    test_util::write_file(dir / "composer.json", "{}");
    test_util::write_file(dir / "package.json", "{}");
    test_util::write_file(dir / ".env", "APP_NAME=shop\nDB_DATABASE=old\nDB_PASSWORD=old\n");
    test_util::write_file(dir / "vendor" / "autoload.php", "<?php");
    test_util::write_file(dir / "node_modules" / "x" / "index.js", "1");
    test_util::write_file(dir / ".venv" / "pyvenv.cfg", "home = x");
    test_util::write_file(dir / "src" / "vendor" / "kept.php", "<?php // a vendor folder deeper down stays");
    test_util::write_file(dir / "floserver-backup.json", "{\"an old one\": true}");
    // Arabic and Kurdish file names.
    test_util::write_file(dir / fs::path(u8"صور") / fs::path(u8"وێنە.txt"), "picture");
    return dir;
}

std::set<std::string> zip_names(const fs::path& zip) {
    mz_zip_archive a{};
    const std::string bytes = test_util::read_file(zip);
    std::set<std::string> out;
    REQUIRE(mz_zip_reader_init_mem(&a, bytes.data(), bytes.size(), 0));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&a); ++i) {
        mz_zip_archive_file_stat st{};
        REQUIRE(mz_zip_reader_file_stat(&a, i, &st));
        out.insert(st.m_filename);
    }
    mz_zip_reader_end(&a);
    return out;
}

template <class F>
bool eventually(F&& f, std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (f()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return f();
}

}  // namespace

TEST_CASE("a backup can leave out vendor, node_modules and .venv, only at the folder's top") {
    const test_util::TempDir tmp;
    const fs::path dir = make_project(tmp.path());
    CHECK(leavable_folders(dir) == std::vector<std::string>{"vendor", "node_modules", ".venv"});
#ifndef _WIN32
    // A link isn't offered (it isn't backed up anyway).
    fs::remove_all(dir / ".venv");
    fs::create_directory_symlink(tmp.path(), dir / ".venv");
    CHECK(leavable_folders(dir) == std::vector<std::string>{"vendor", "node_modules"});
#endif

    std::string error;
    const auto all = backup_contents(dir, {}, error);
    REQUIRE_MESSAGE(all, error);
    const auto some = backup_contents(dir, {"vendor", "node_modules"}, error);
    REQUIRE_MESSAGE(some, error);
    std::set<std::string> names;
    for (const BackupFile& f : some->files) {
        names.insert(f.name);
    }
    CHECK(names.contains("index.php"));
    CHECK(names.contains("src/vendor/kept.php"));
    CHECK(names.contains(reinterpret_cast<const char*>(u8"صور/وێنە.txt")));
    CHECK_FALSE(names.contains("vendor/autoload.php"));
    CHECK_FALSE(names.contains("node_modules/x/index.js"));
    // The project's own floserver-backup.json would take the backup's place.
    CHECK_FALSE(names.contains("floserver-backup.json"));
    CHECK(some->bytes < all->bytes);
    CHECK(some->database_file == "database.sql");
#ifndef _WIN32
    // Links are passed over, never followed.
    CHECK(some->links == 1);
    CHECK_FALSE(names.contains(".venv/pyvenv.cfg"));
#endif
    // A project with a database.sql of its own keeps it; the dump gets the other name.
    test_util::write_file(dir / "database.sql", "-- the project's own");
    CHECK(backup_contents(dir, {}, error)->database_file == "floserver-database.sql");
}

TEST_CASE("a backup zip holds the files, database.sql and floserver-backup.json, and unpacks by the safe rules") {
    const test_util::TempDir tmp;
    const fs::path dir = make_project(tmp.path());
    std::string error;
    const auto contents = backup_contents(dir, {"vendor", "node_modules", ".venv"}, error);
    REQUIRE_MESSAGE(contents, error);
    test_util::write_file(tmp.path() / "dump.sql", "CREATE TABLE t (a int);\n");
    BackupInfo info;
    info.name = "shop";
    info.date = "2026-11-05 14:30";
    info.made_by = "0.1.0";
    info.versions = backup_versions(dir);
    info.left_out = {"vendor", "node_modules", ".venv"};
    info.database = true;
    info.database_file = contents->database_file;
    const fs::path zip = tmp.path() / "shop 2026-11-05 1430.zip";
    bool cancelled = false;
    std::uint64_t seen = 0;
    REQUIRE_MESSAGE(write_backup_zip(zip, *contents, info, tmp.path() / "dump.sql",
                                     [&](std::uint64_t done) {
                                         seen = done;
                                         return true;
                                     },
                                     cancelled, error),
                    error);
    CHECK(seen == contents->files.size());
    const auto names = zip_names(zip);
    CHECK(names.contains("database.sql"));
    CHECK(names.contains("floserver-backup.json"));
    CHECK(names.contains("index.php"));
    CHECK_FALSE(names.contains("vendor/autoload.php"));

    // floserver-backup.json says what it is.
    const auto read = read_backup_info(zip, error);
    REQUIRE_MESSAGE(read, error);
    CHECK(read->name == "shop");
    CHECK(read->date == "2026-11-05 14:30");
    CHECK(read->versions == nlohmann::json{{"php", "8.5"}, {"database", "mariadb-11.4"}, {"web", "nginx"}});
    CHECK(read->left_out == std::vector<std::string>{"vendor", "node_modules", ".venv"});
    CHECK(read->database);
    CHECK(read->database_file == "database.sql");
    const auto json = nlohmann::json::parse(*read_zip_entry(zip, "floserver-backup.json", 4096, error));
    CHECK(json.at("format") == 1);
    CHECK(json.at("made_by") == "FloServer 0.1.0");

    // Unpacked as it was, its own two files left in the zip, Arabic names and all.
    ZipOptions options;
    options.drop_top_folder = false;
    options.unicode_names = true;
    options.skip = {"floserver-backup.json", "database.sql"};
    const auto r = extract_zip(zip, tmp.path() / "out", {}, options);
    REQUIRE_MESSAGE(r.ok, r.message);
    CHECK(test_util::read_file(tmp.path() / "out" / "index.php") == "<?php echo 'hi';");
    CHECK(test_util::read_file(tmp.path() / "out" / fs::path(u8"صور") / fs::path(u8"وێنە.txt")) == "picture");
    CHECK_FALSE(fs::exists(tmp.path() / "out" / "database.sql"));
    CHECK_FALSE(fs::exists(tmp.path() / "out" / "floserver-backup.json"));
    CHECK(fs::exists(tmp.path() / "out" / "src" / "vendor" / "kept.php"));

    // The dump comes out on its own.
    REQUIRE(extract_zip_entry(zip, "database.sql", tmp.path() / "db.sql", error));
    CHECK(test_util::read_file(tmp.path() / "db.sql") == "CREATE TABLE t (a int);\n");

    // Stopped part way: says so.
    ZipOptions stop = options;
    stop.progress = [](std::uint64_t done, std::uint64_t) { return done < 2; };
    const auto stopped = extract_zip(zip, tmp.path() / "out2", {}, stop);
    CHECK_FALSE(stopped.ok);
    CHECK(stopped.cancelled);

    // Cancelled while packing.
    REQUIRE_FALSE(write_backup_zip(tmp.path() / "x.zip", *contents, info, std::nullopt,
                                   [](std::uint64_t done) { return done < 2; }, cancelled, error));
    CHECK(cancelled);
}

TEST_CASE("a backup's names in any language, but never unsafe ones") {
    CHECK(safe_entry_path(reinterpret_cast<const char*>(u8"صور/a.txt"), true) == fs::path(u8"صور") / "a.txt");
    CHECK(safe_entry_path(reinterpret_cast<const char*>(u8"صور/a.txt"), false).empty());  // runtimes: ASCII only
    for (const char* bad : {"../x", "a/../../x", "/etc/passwd", "C:/x", "a\\..\\x", "nul.txt", "a:b", "x.", "a\x01",
                            "\xC0\xAF", "\xED\xA0\x80", "a\xE2\x80\xAE.txt", "\xC2\x85"}) {
        CAPTURE(bad);
        CHECK(safe_entry_path(bad, true).empty());
    }
}

TEST_CASE("floserver-backup.json that FloServer can't use is refused") {
    std::string error;
    CHECK_FALSE(parse_backup_info("not json", error));
    CHECK_FALSE(parse_backup_info(R"({"name": "shop"})", error));
    CHECK(error.find("format") != std::string::npos);
    CHECK_FALSE(parse_backup_info(R"({"format": 9, "name": "shop"})", error));
    CHECK(error.find("newer FloServer") != std::string::npos);
    CHECK_FALSE(parse_backup_info(R"({"format": 1, "name": "../etc"})", error));
    // The database must be one of the two names FloServer writes, never any file of the zip.
    CHECK_FALSE(parse_backup_info(R"({"format": 1, "name": "shop", "database": true, "database_file": "index.php"})",
                                  error));
    const auto ok = parse_backup_info(
        R"({"format": 1, "name": "shop", "database": true, "left_out": ["vendor", "../x", 3], "versions": {"php": "8.5", "x": 1}})",
        error);
    REQUIRE(ok);
    CHECK(ok->database_file == "database.sql");
    CHECK(ok->left_out == std::vector<std::string>{"vendor"});
    CHECK(ok->versions == nlohmann::json{{"php", "8.5"}});

    // A zip without one isn't a backup.
    const test_util::TempDir tmp;
    mz_zip_archive a{};
    REQUIRE(mz_zip_writer_init_heap(&a, 0, 0));
    REQUIRE(mz_zip_writer_add_mem(&a, "index.php", "x", 1, 0));
    void* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&a, &buffer, &size));
    test_util::write_file(tmp.path() / "plain.zip", std::string(static_cast<const char*>(buffer), size));
    mz_free(buffer);
    mz_zip_writer_end(&a);
    CHECK_FALSE(read_backup_info(tmp.path() / "plain.zip", error));
    CHECK(error == "plain.zip isn't a FloServer backup (it has no floserver-backup.json)");
}

TEST_CASE("backup names: the file, the date, and a new project's default name") {
    std::tm t{};
    t.tm_year = 126;
    t.tm_mon = 10;
    t.tm_mday = 5;
    t.tm_hour = 9;
    t.tm_min = 7;
    CHECK(backup_file_name("shop", t) == "shop 2026-11-05 0907.zip");
    CHECK(backup_date(t) == "2026-11-05 09:07");
    CHECK(backup_stamp(t) == "2026-11-05 0907");

    std::set<std::string> taken;
    const auto is_taken = [&](const std::string& n) { return taken.contains(n); };
    CHECK(copy_name("shop", is_taken) == "shop-copy");
    taken = {"shop-copy", "shop-copy-2"};
    CHECK(copy_name("shop", is_taken) == "shop-copy-3");
    const std::string longest(63, 'a');
    const std::string copy = copy_name(longest, is_taken);
    CHECK(copy.size() == 63);
    CHECK(copy.ends_with("-copy"));
    CHECK(copy_name(std::string(58, 'a') + "-bcde", is_taken) == std::string(58, 'a') + "-copy");
}

TEST_CASE("a backup's SQL is made ready to load into another database") {
    SqlLoadFilter f;
    std::string error;
    std::string line = "USE `shop`;";
    REQUIRE(f.line(line, error));
    CHECK(line.empty());
    line = "CREATE DATABASE /*!32312 IF NOT EXISTS*/ `shop`;";
    REQUIRE(f.line(line, error));
    CHECK(line.empty());
    line = "/*!50003 CREATE*/ /*!50017 DEFINER=`shop`@`localhost`*/ /*!50003 TRIGGER t BEFORE INSERT ON x FOR EACH ROW SET NEW.a = 1 */;;";
    REQUIRE(f.line(line, error));
    CHECK(line == "/*!50003 CREATE*/ /*!50017 */ /*!50003 TRIGGER t BEFORE INSERT ON x FOR EACH ROW SET NEW.a = 1 */;;");
    line = "/*!50013 DEFINER=`root`@`localhost` SQL SECURITY DEFINER */";
    REQUIRE(f.line(line, error));
    CHECK(line == "/*!50013 SQL SECURITY DEFINER */");
    // A new mariadb-dump's first line turns the sandbox on: let through.
    line = "/*M!999999\\- enable the sandbox mode */ ";
    CHECK(f.line(line, error));
    // Backslashes in strings are the data's own; a row's text isn't changed.
    line = R"(INSERT INTO t VALUES ('it\'s \\! fine','USE x; DEFINER=`a`@`b`'),("\"\\"),(`a\b`);)";
    const std::string row = line;
    REQUIRE(f.line(line, error));
    CHECK(line == row);
    // A string over two lines: the second starts inside it.
    line = "INSERT INTO t VALUES ('line one";
    REQUIRE(f.line(line, error));
    line = "USE x; \\! not a command here');";
    REQUIRE(f.line(line, error));
    CHECK(line == "USE x; \\! not a command here');");
    // The mariadb program's own commands are refused.
    for (const char* bad : {"\\! rm -rf /", "SELECT 1; \\! calc", "SELECT 1 /*! \\. other.sql */;", "\\T /tmp/log"}) {
        SqlLoadFilter fresh;
        line = bad;
        CAPTURE(bad);
        CHECK_FALSE(fresh.line(line, error));
        CHECK(error.find("mariadb program") != std::string::npos);
    }
    // ...but not inside comments.
    SqlLoadFilter comments;
    line = "-- a \\! comment";
    CHECK(comments.line(line, error));
    line = "/* \\! a comment";
    CHECK(comments.line(line, error));
    line = "still \\! one */ SELECT 1;";
    CHECK(comments.line(line, error));

    const test_util::TempDir tmp;
    test_util::write_file(tmp.path() / "in.sql", "USE `shop`;\nCREATE TABLE a (b int);\nINSERT INTO a VALUES (1);\n");
    REQUIRE(prepare_sql_for_load(tmp.path() / "in.sql", tmp.path() / "out.sql", "shop_copy", error));
    CHECK(test_util::read_file(tmp.path() / "out.sql") ==
          "DROP DATABASE IF EXISTS `shop_copy`;\n"
          "CREATE DATABASE `shop_copy` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;\n"
          "USE `shop_copy`;\n\nCREATE TABLE a (b int);\nINSERT INTO a VALUES (1);\n");
    test_util::write_file(tmp.path() / "bad.sql", "SELECT 1;\n\\! whoami\n");
    CHECK_FALSE(prepare_sql_for_load(tmp.path() / "bad.sql", tmp.path() / "out.sql", "shop_copy", error));
}

TEST_CASE("wp-config.php is pointed at the new database") {
    const DatabaseLogin login{3307, "shop_copy", "shop_copy", "pa'ss"};
    const std::string text = "<?php\ndefine( 'DB_NAME', 'shop' );\ndefine( 'DB_USER', 'shop' );\n"
                             "define('DB_PASSWORD', 'old');\ndefine( \"DB_HOST\", '127.0.0.1:3306' );\n$x = 1;\n";
    const auto out = update_wp_config_text(text, login);
    REQUIRE(out);
    CHECK(*out == "<?php\ndefine( 'DB_NAME', 'shop_copy' );\ndefine( 'DB_USER', 'shop_copy' );\n"
                  "define('DB_PASSWORD', 'pa\\'ss');\ndefine( \"DB_HOST\", '127.0.0.1:3307' );\n$x = 1;\n");
    CHECK_FALSE(update_wp_config_text("<?php // no database lines", login));
}

TEST_CASE("what brings back the left-out folders") {
    const test_util::TempDir tmp;
    const fs::path dir = tmp.path() / "p";
    test_util::write_file(dir / "composer.json", "{}");
    test_util::write_file(dir / "package.json", "{}");
    test_util::write_file(dir / "requirements.txt", "flask\n");
    std::vector<std::string> shown;
    for (const ReinstallStep& s : reinstall_steps(dir, {"vendor", "node_modules", ".venv"})) {
        shown.push_back(s.shown);
    }
    CHECK(shown == std::vector<std::string>{"composer install", "npm install", "python -m venv .venv",
                                            "pip install -r requirements.txt"});
    // Only what was left out, isn't there now, and has its list.
    fs::create_directories(dir / "node_modules");
    fs::remove(dir / "requirements.txt");
    CHECK(reinstall_steps(dir, {"vendor", "node_modules", ".venv"}).size() == 1);
    CHECK(reinstall_steps(dir, {}).empty());
}

namespace {

// The service's other parts, faked: projects in a map, a database that's a file.
struct Fakes {
    std::map<std::string, BackupProject> projects;
    std::vector<std::string> loaded;  // the prepared SQL each load ran
    std::vector<std::pair<std::string, bool>> switched;
    std::string dump = "CREATE TABLE t (a int);\nINSERT INTO t VALUES (1);\n";

    BackupHooks hooks() {
        BackupHooks h;
        h.project = [this](const std::string& name, std::string& error) -> std::optional<BackupProject> {
            const auto it = projects.find(name);
            if (it == projects.end()) {
                error = "there's no project called " + name;
                return std::nullopt;
            }
            return it->second;
        };
        h.dump_database = [this](const std::string&, const fs::path& work, const std::atomic<bool>&, std::string&) {
            test_util::write_file(work / "database.sql", dump);
            return true;
        };
        h.load_database = [this](const std::string&, const fs::path& sql, const std::atomic<bool>&, std::string&) {
            loaded.push_back(test_util::read_file(sql));
            return true;
        };
        h.database_login = [](const std::string& project, std::string&) -> std::optional<DatabaseLogin> {
            const std::string db = project_database_name(project);
            return DatabaseLogin{3306, db, db, "secret-" + db};
        };
        h.add_project = [this](const fs::path& dir, const std::string& name,
                               std::string&) -> std::optional<std::string> {
            projects[name] = BackupProject{name, dir, true, true};
            return name;
        };
        h.set_on = [this](const std::string& name, bool on, std::string&) {
            switched.emplace_back(name, on);
            projects[name].on = on;
            return true;
        };
        return h;
    }
};

nlohmann::json wait_for(const BackupService& s, std::int64_t id) {
    nlohmann::json j;
    REQUIRE(eventually([&] {
        j = *s.job(id);
        return j.at("state") != "running";
    }));
    return j;
}

}  // namespace

TEST_CASE("backup, new project from the backup, and restore, as jobs") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    const fs::path dir = make_project(tmp.path());
    Fakes fakes;
    fakes.projects["shop"] = BackupProject{"shop", dir, true, true};
    BackupService service(data, fakes.hooks());
    ServiceRefusal refusal;

    // Refused before anything starts.
    CHECK_FALSE(service.backup({"shop", "relative.zip", true, std::nullopt}, refusal));
    CHECK_FALSE(service.backup({"nope", tmp.path() / "x.zip", true, std::nullopt}, refusal));
    CHECK(refusal.code == "unknown_project");
    CHECK_FALSE(service.backup({"shop", dir / "inside.zip", true, std::nullopt}, refusal));
    CHECK_FALSE(service.backup({"shop", tmp.path() / "x.zip", true, std::vector<std::string>{"src"}}, refusal));

    // Back up: every folder it can leave out is left out.
    const fs::path zip = tmp.path() / "backups" / "shop 2026-11-05 1430.zip";
    const auto id = service.backup({"shop", zip, true, std::nullopt}, refusal);
    REQUIRE_MESSAGE(id, refusal.message);
    auto j = wait_for(service, *id);
    REQUIRE_MESSAGE(j.at("state") == "done", j.dump());
    CHECK(j.at("database") == "saved");
    CHECK(j.at("files") == j.at("total_files"));
    CHECK(j.at("size") == fs::file_size(zip));
    CHECK_FALSE(fs::exists(zip.string() + ".part"));
    CHECK_FALSE(fs::exists(data / "backups" / ("job-" + std::to_string(*id))));
    std::string error;
    const auto info = read_backup_info(zip, error);
    REQUIRE(info);
    CHECK(info->left_out == std::vector<std::string>{"vendor", "node_modules", ".venv"});
    CHECK(info->database);
    CHECK(zip_names(zip).contains("database.sql"));
    // The project kept running.
    CHECK(fakes.switched.empty());

    // A new project from it: "shop-copy", its database loaded and .env pointed at it.
    const fs::path projects = tmp.path() / "projects";
    const auto cid = service.create({zip, "shop-copy", projects, false}, refusal);
    REQUIRE_MESSAGE(cid, refusal.message);
    j = wait_for(service, *cid);
    REQUIRE_MESSAGE(j.at("state") == "done", j.dump());
    const fs::path copy = projects / "shop-copy";
    CHECK(test_util::read_file(copy / "index.php") == "<?php echo 'hi';");
    CHECK_FALSE(fs::exists(copy / "database.sql"));
    CHECK_FALSE(fs::exists(copy / "vendor"));
    CHECK(fakes.projects.contains("shop-copy"));
    REQUIRE(fakes.loaded.size() == 1);
    CHECK(fakes.loaded[0].starts_with("DROP DATABASE IF EXISTS `shop_copy`;"));
    CHECK(fakes.loaded[0].find("INSERT INTO t VALUES (1);") != std::string::npos);
    const std::string env = test_util::read_file(copy / ".env");
    CHECK(env.find("APP_NAME=shop\n") != std::string::npos);
    CHECK(env.find("DB_DATABASE=shop_copy") != std::string::npos);
    CHECK(env.find("DB_PASSWORD=secret-shop_copy") != std::string::npos);
    CHECK(env.find("DB_PASSWORD=old") == std::string::npos);
    // ...and the name rules.
    CHECK_FALSE(service.create({zip, "shop-copy", projects, false}, refusal));
    CHECK(refusal.code == "folder_exists");
    CHECK_FALSE(service.create({zip, "Bad_Name", projects, false}, refusal));
    CHECK(refusal.code == "bad_name");
    CHECK_FALSE(service.create({zip, "shop", tmp.path() / "elsewhere", false}, refusal));
    CHECK(refusal.code == "name_taken");

    // Restore: today's database kept next to the backup, today's files
    // renamed aside (the window moves them to the Recycle Bin first), the
    // backup's put back, and the project started again.
    test_util::write_file(dir / "index.php", "<?php echo 'changed';");
    test_util::write_file(dir / "new-today.txt", "x");
    fakes.dump = "-- today's\n";
    const auto rid = service.restore({"shop", zip, true, true, false}, refusal);
    REQUIRE_MESSAGE(rid, refusal.message);
    j = wait_for(service, *rid);
    REQUIRE_MESSAGE(j.at("state") == "done", j.dump());
    CHECK(test_util::read_file(dir / "index.php") == "<?php echo 'hi';");
    CHECK_FALSE(fs::exists(dir / "new-today.txt"));
    const fs::path aside = j.at("set_aside").get<std::string>();
    CHECK(test_util::read_file(aside / "new-today.txt") == "x");
    const fs::path saved = j.at("saved_database").get<std::string>();
    CHECK(saved.parent_path() == zip.parent_path());
    CHECK(saved.filename().string().starts_with("shop before restore "));
    CHECK(test_util::read_file(saved) == "-- today's\n");
    REQUIRE(fakes.loaded.size() == 2);
    CHECK(fakes.loaded[1].starts_with("DROP DATABASE IF EXISTS `shop`;"));
    CHECK(fakes.switched == std::vector<std::pair<std::string, bool>>{{"shop", false}, {"shop", true}});

    // A folder already gone (the Recycle Bin): put back with nothing set aside.
    fs::remove_all(dir);
    const auto rid2 = service.restore({"shop", zip, true, false, false}, refusal);
    REQUIRE(rid2);
    j = wait_for(service, *rid2);
    REQUIRE_MESSAGE(j.at("state") == "done", j.dump());
    CHECK_FALSE(j.contains("set_aside"));
    CHECK(fs::exists(dir / "index.php"));
    CHECK(fakes.loaded.size() == 2);  // its database wasn't asked for

    // Something that isn't a backup.
    test_util::write_file(tmp.path() / "not.zip", "PK nothing");
    CHECK_FALSE(service.restore({"shop", tmp.path() / "not.zip", true, true, true}, refusal));
    CHECK(refusal.code == "not_a_backup");
}

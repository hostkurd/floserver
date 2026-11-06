// Backups (D22): what a backup leaves out and holds, its floserver-backup.json,
// unpacking one by the zip rules, the SQL made ready to load, .env and
// wp-config.php pointed at the new database, the names, and the jobs with
// the service's other parts as fakes. test_database_service.cpp dumps and
// loads a real MariaDB.

#include "floserver/core/backup.h"
#include "floserver/core/backup_service.h"
#include "floserver/core/database_service.h"
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
#ifndef _WIN32
    // Links are passed over, never followed.
    CHECK(some->links == 1);
    CHECK_FALSE(names.contains(".venv/pyvenv.cfg"));
#endif
    // A project's own database.sql is its own file; only a .floserver/database.sql
    // of its own is left out (the dump goes there).
    test_util::write_file(dir / "database.sql", "-- the project's own");
    test_util::write_file(dir / ".floserver" / "database.sql", "-- would take the dump's place");
    names.clear();
    const auto with_own = backup_contents(dir, {}, error);
    REQUIRE(with_own);
    for (const BackupFile& f : with_own->files) {
        names.insert(f.name);
    }
    CHECK(names.contains("database.sql"));
    CHECK_FALSE(names.contains(".floserver/database.sql"));
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
    info.computer = std::string(32, 'a');
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
    CHECK(names.contains(".floserver/database.sql"));
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
    CHECK(read->database_file == ".floserver/database.sql");
    CHECK(read->computer == std::string(32, 'a'));
    const auto json = nlohmann::json::parse(*read_zip_entry(zip, "floserver-backup.json", 4096, error));
    CHECK(json.at("format") == 1);
    CHECK(json.at("made_by") == "FloServer 0.1.0");

    // Unpacked as it was, its own two files left in the zip, Arabic names and all.
    ZipOptions options;
    options.drop_top_folder = false;
    options.unicode_names = true;
    options.skip = {"floserver-backup.json", ".floserver/database.sql"};
    const auto r = extract_zip(zip, tmp.path() / "out", {}, options);
    REQUIRE_MESSAGE(r.ok, r.message);
    CHECK(test_util::read_file(tmp.path() / "out" / "index.php") == "<?php echo 'hi';");
    CHECK(test_util::read_file(tmp.path() / "out" / fs::path(u8"صور") / fs::path(u8"وێنە.txt")) == "picture");
    CHECK_FALSE(fs::exists(tmp.path() / "out" / ".floserver"));
    CHECK_FALSE(fs::exists(tmp.path() / "out" / "floserver-backup.json"));
    CHECK(fs::exists(tmp.path() / "out" / "src" / "vendor" / "kept.php"));

    // The dump comes out on its own.
    REQUIRE(extract_zip_entry(zip, ".floserver/database.sql", tmp.path() / "db.sql", error));
    CHECK(test_util::read_file(tmp.path() / "db.sql") == "CREATE TABLE t (a int);\n");
    std::string streamed;
    std::uint64_t size = 0;
    REQUIRE(stream_zip_entry(zip, ".floserver/database.sql", [&](std::string_view piece) {
        streamed.append(piece);
        return true;
    }, error, &size));
    CHECK(streamed == "CREATE TABLE t (a int);\n");
    CHECK(size == streamed.size());
    CHECK(zip_entry_size(zip, ".floserver/database.sql", error) == size);

    // Not enough room for it all and a tenth more: refused before anything is written.
    ZipOptions full = options;
    full.free_space = [](const fs::path&) { return std::optional<std::uint64_t>(10); };
    const auto no_room = extract_zip(zip, tmp.path() / "out3", {}, full);
    CHECK_FALSE(no_room.ok);
    CHECK(no_room.no_space);
    CHECK(no_room.message.starts_with("there isn't enough free space on the drive"));
    CHECK_FALSE(fs::exists(tmp.path() / "out3"));

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
                            "\xC0\xAF", "\xED\xA0\x80", "a\xE2\x80\xAE.txt", "\xC2\x85", "COM\xC2\xB9", "lpt\xC2\xB3.txt",
                            "Com\xC2\xB2.log"}) {
        CAPTURE(bad);
        CHECK(safe_entry_path(bad, true).empty());
    }
    CHECK_FALSE(safe_entry_path("COM10", true).empty());
}

namespace {

std::string zip_of(const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive a{};
    REQUIRE(mz_zip_writer_init_heap(&a, 0, 0));
    for (const auto& [name, content] : entries) {
        REQUIRE(mz_zip_writer_add_mem(&a, name.c_str(), content.data(), content.size(), 0));
    }
    void* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&a, &buffer, &size));
    std::string out(static_cast<const char*>(buffer), size);
    mz_free(buffer);
    mz_zip_writer_end(&a);
    return out;
}

}  // namespace

TEST_CASE("a backup with the same name twice is refused") {
    const test_util::TempDir tmp;
    test_util::write_file(tmp.path() / "twice.zip", zip_of({{"a/index.php", "1"}, {"a/INDEX.php", "2"}}));
    ZipOptions options;
    options.drop_top_folder = false;
    const auto r = extract_zip(tmp.path() / "twice.zip", tmp.path() / "out", {}, options);
    CHECK_FALSE(r.ok);
    CHECK(r.message == "the archive has the same file twice: a/INDEX.php");
    CHECK_FALSE(fs::exists(tmp.path() / "out"));
}

TEST_CASE("this computer's id, and text from a backup made safe to show") {
    const test_util::TempDir tmp;
    CHECK_FALSE(read_computer_id(tmp.path()));
    std::string error;
    const auto id = computer_id(tmp.path(), error);
    REQUIRE(id);
    CHECK(id->size() == 32);
    CHECK(computer_id(tmp.path(), error) == id);  // made once
    CHECK(read_computer_id(tmp.path()) == id);
    BackupInfo info;
    info.computer = *id;
    CHECK(made_here(info, tmp.path()));
    info.computer = std::string(32, '0');
    CHECK_FALSE(made_here(info, tmp.path()));
    info.computer.clear();
    CHECK_FALSE(made_here(info, tmp.path()));

    // A terminal's escapes, control characters and direction marks go.
    CHECK(shown_text("ok\x1b[31m red\x07\r\nnext\tline") == "ok[31m red  next line");
    CHECK(shown_text("a\xE2\x80\xAE" "b\xE2\x81\xA6" "c\xC2\x9B" "d\xEF\xBB\xBF") == "abcd");
    CHECK(shown_text(reinterpret_cast<const char*>(u8"صور")) == reinterpret_cast<const char*>(u8"صور"));
    const auto parsed =
        parse_backup_info("{\"format\": 1, \"name\": \"shop\", \"date\": \"2026\\u001b[2J\", \"versions\": {\"php\": \"8.5\\u202e\"}}", error);
    REQUIRE_MESSAGE(parsed, error);
    CHECK(parsed->date == "2026[2J");
    CHECK(parsed->versions.at("php") == "8.5");
}

TEST_CASE("floserver-backup.json that FloServer can't use is refused") {
    std::string error;
    CHECK_FALSE(parse_backup_info("not json", error));
    CHECK_FALSE(parse_backup_info(R"({"name": "shop"})", error));
    CHECK(error.find("format") != std::string::npos);
    CHECK_FALSE(parse_backup_info(R"({"format": 9, "name": "shop"})", error));
    CHECK(error.find("newer FloServer") != std::string::npos);
    CHECK_FALSE(parse_backup_info(R"({"format": 1, "name": "../etc"})", error));
    // The database must be the name FloServer writes, never any file of the zip.
    CHECK_FALSE(parse_backup_info(R"({"format": 1, "name": "shop", "database": true, "database_file": "index.php"})",
                                  error));
    const auto ok = parse_backup_info(
        R"({"format": 1, "name": "shop", "database": true, "left_out": ["vendor", "../x", 3], "versions": {"php": "8.5", "x": 1}})",
        error);
    REQUIRE(ok);
    CHECK(ok->database_file == ".floserver/database.sql");
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
    // ...and their long forms at a statement's start.
    for (const char* bad : {"source /etc/passwd", "SYSTEM ls", "tee /tmp/x", "connect other", "  pager less"}) {
        SqlLoadFilter fresh;
        line = bad;
        CAPTURE(bad);
        CHECK_FALSE(fresh.line(line, error));
        CHECK(error.find("mariadb program") != std::string::npos);
    }
    {
        // Inside a statement they're its words.
        SqlLoadFilter inside;
        line = "SELECT 1,";
        REQUIRE(inside.line(line, error));
        line = "source FROM t;";
        CHECK(inside.line(line, error));
        line = "system ls";
        CHECK_FALSE(inside.line(line, error));  // the statement ended: a command again
        // DELIMITER is the dump's own and is let through.
        SqlLoadFilter delimiter;
        line = "DELIMITER ;;";
        REQUIRE(delimiter.line(line, error));
        line = "CREATE TRIGGER t BEFORE INSERT ON x FOR EACH ROW SET NEW.a = 1;";
        REQUIRE(delimiter.line(line, error));
        line = "tee is a word here ;;";
        CHECK(delimiter.line(line, error));
        line = "tee /tmp/x";
        CHECK_FALSE(delimiter.line(line, error));
    }
    // ...but not inside comments, whatever space follows the --.
    SqlLoadFilter comments;
    line = "-- a \\! comment";
    CHECK(comments.line(line, error));
    for (const char* comment : {"--\v\\! x", "--\f\\! x", "--\r\\! x", "--\t\\! x", "--"}) {
        line = comment;
        CAPTURE(comment);
        CHECK(comments.line(line, error));
    }
    // "--x" isn't a comment, and /*M without ! is a plain comment.
    {
        SqlLoadFilter dash;
        line = "SELECT 1--\\! x";
        CHECK_FALSE(dash.line(line, error));
        SqlLoadFilter m;
        line = "/*M \\! still a comment */ SELECT 1;";
        CHECK(m.line(line, error));
        line = "/*M!100000 \\! run */";
        CHECK_FALSE(m.line(line, error));
    }
    line = "/* \\! a comment";
    CHECK(comments.line(line, error));
    line = "still \\! one */ SELECT 1;";
    CHECK(comments.line(line, error));

    const test_util::TempDir tmp;
    test_util::write_file(tmp.path() / "in.sql",
                          "USE `shop`;\nCREATE TABLE a (b int);\nINSERT INTO a VALUES (1);\n--\n"
                          "-- Dumping routines for database 'shop'\n--\nDELIMITER ;;\n"
                          "CREATE DEFINER=`shop`@`localhost` PROCEDURE p() SELECT 1 ;;\nDELIMITER ;\n");
    bool extras = false;
    REQUIRE(prepare_sql_for_load(tmp.path() / "in.sql", tmp.path() / "out.sql", tmp.path() / "extras.sql", "shop_copy",
                                 extras, error));
    CHECK(extras);
    CHECK(test_util::read_file(tmp.path() / "out.sql") ==
          "DROP DATABASE IF EXISTS `shop_copy`;\n"
          "CREATE DATABASE `shop_copy` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;\n"
          "USE `shop_copy`;\n\nCREATE TABLE a (b int);\nINSERT INTO a VALUES (1);\n--\n");
    // Routines on their own, with no other user's DEFINER.
    CHECK(test_util::read_file(tmp.path() / "extras.sql") ==
          "USE `shop_copy`;\n-- Dumping routines for database 'shop'\n--\nDELIMITER ;;\n"
          "CREATE PROCEDURE p() SELECT 1 ;;\nDELIMITER ;\n");
    test_util::write_file(tmp.path() / "bad.sql", "SELECT 1;\n\\! whoami\n");
    CHECK_FALSE(prepare_sql_for_load(tmp.path() / "bad.sql", tmp.path() / "out.sql", tmp.path() / "extras.sql",
                                     "shop_copy", extras, error));
    // A piece at a time, split anywhere.
    SqlLoadWriter writer;
    REQUIRE(writer.open(tmp.path() / "w.sql", tmp.path() / "we.sql", "x", error));
    for (const char* piece : {"CREATE TA", "BLE a (b int);\nINS", "ERT INTO a VALUES (1);"}) {
        REQUIRE(writer.feed(piece, error));
    }
    REQUIRE(writer.finish(error));
    CHECK_FALSE(writer.has_extras());
    CHECK(test_util::read_file(tmp.path() / "w.sql").ends_with("USE `x`;\nCREATE TABLE a (b int);\nINSERT INTO a VALUES (1);\n"));
}

TEST_CASE("the mariadb program loads a backup with no client commands and no local files") {
    const auto args = mariadb_load_args(false);
    CHECK(std::find(args.begin(), args.end(), "--binary-mode") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--local-infile=0") != args.end());
    CHECK(std::find(args.begin(), args.end(), "--force") == args.end());
    const auto extras = mariadb_load_args(true);
    CHECK(std::find(extras.begin(), extras.end(), "--force") != extras.end());
    CHECK(std::find(extras.begin(), extras.end(), "--local-infile=0") != extras.end());
    const auto dump = mariadb_dump_args("shop", "database.sql");
    for (const char* a : {"--single-transaction", "--routines", "--events", "--triggers", "--result-file=database.sql"}) {
        CAPTURE(a);
        CHECK(std::find(dump.begin(), dump.end(), a) != dump.end());
    }
    CHECK(dump.back() == "shop");
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
    // A define() in a comment or a string isn't the one.
    const std::string commented = "<?php\n// define( 'DB_NAME', 'old' );\n/* define('DB_USER', 'x'); */\n"
                                  "$s = \"define('DB_PASSWORD', 'no')\";\n"
                                  "define( 'DB_NAME', 'a' ); define( 'DB_USER', 'b' ); define( 'DB_PASSWORD', 'c' );"
                                  " define( 'DB_HOST', 'd' );\n";
    const auto skipped = update_wp_config_text(commented, login);
    REQUIRE(skipped);
    CHECK(skipped->starts_with("<?php\n// define( 'DB_NAME', 'old' );\n/* define('DB_USER', 'x'); */\n"
                               "$s = \"define('DB_PASSWORD', 'no')\";\n"));
    CHECK(skipped->find("define( 'DB_NAME', 'shop_copy' )") != std::string::npos);
    CHECK(skipped->find("define( 'DB_HOST', '127.0.0.1:3307' )") != std::string::npos);
    CHECK_FALSE(update_wp_config_text("<?php // define('DB_NAME','a'); define('DB_USER','b');", login));
    // A large file is fine.
    CHECK(update_wp_config_text(std::string(2'000'000, ' ') + text, login));
}

TEST_CASE("a restore keeps today's database next to the backup, else in FloServer's folder") {
    const test_util::TempDir tmp;
    test_util::write_file(tmp.path() / "work" / "database.sql", "-- today");
    std::string error;
    const auto kept = keep_file(tmp.path() / "work" / "database.sql", {tmp.path() / "backups"}, "shop before restore",
                                "sql", error);
    REQUIRE_MESSAGE(kept, error);
    CHECK(*kept == tmp.path() / "backups" / "shop before restore.sql");
    // Taken: the next name. Not a folder that can be written (a file in its
    // place): the next folder.
    test_util::write_file(tmp.path() / "work" / "database.sql", "-- again");
    test_util::write_file(tmp.path() / "blocked", "a file, not a folder");
    const auto second = keep_file(tmp.path() / "work" / "database.sql",
                                  {tmp.path() / "blocked", tmp.path() / "backups"}, "shop before restore", "sql", error);
    REQUIRE_MESSAGE(second, error);
    CHECK(*second == tmp.path() / "backups" / "shop before restore 2.sql");
    CHECK(test_util::read_file(*second) == "-- again");
    CHECK_FALSE(fs::exists(tmp.path() / "work" / "database.sql"));
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
    bool dump_fails = false;
    int load_fails = 0;           // this many loads fail, from the next one
    std::atomic<bool> load_waits{false};  // a load runs until it's cancelled
    std::atomic<bool> loading{false};
    std::optional<std::uint64_t> free;  // the free space every folder has; nothing: the real one

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
        h.dump_database = [this](const std::string&, const fs::path& work, const std::atomic<bool>&,
                                 std::string& error) {
            if (dump_fails) {
                error = "mariadb-dump: Access denied";
                return false;
            }
            test_util::write_file(work / "database.sql", dump);
            return true;
        };
        h.load_database = [this](const std::string&, const fs::path& sql, const fs::path&,
                                 const std::atomic<bool>& cancel, std::string&, std::string& error) {
            loaded.push_back(test_util::read_file(sql));
            if (load_waits) {
                loading = true;
                while (!cancel) {
                    std::this_thread::sleep_for(5ms);
                }
                error = "cancelled";
                return false;
            }
            if (load_fails > 0) {
                --load_fails;
                error = "ERROR 1064: syntax";
                return false;
            }
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
        h.free_space = [this](const fs::path&) { return free; };
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

// A project with a backup of it, made by `service`.
struct Restorable {
    test_util::TempDir tmp;
    fs::path data = tmp.path() / "data";
    fs::path dir;
    fs::path zip = tmp.path() / "backups" / "shop.zip";
    Fakes fakes;
    std::unique_ptr<BackupService> service;

    Restorable() {
        dir = make_project(tmp.path());
        fakes.projects["shop"] = BackupProject{"shop", dir, true, true};
        service = std::make_unique<BackupService>(data, fakes.hooks());
        ServiceRefusal refusal;
        const auto id = service->backup({"shop", zip, true, std::nullopt}, refusal);
        REQUIRE_MESSAGE(id, refusal.message);
        REQUIRE(wait_for(*service, *id).at("state") == "done");
        // Today's, different from the backup's.
        test_util::write_file(dir / "index.php", "<?php echo 'today';");
        fakes.dump = "-- today's\n";
    }
    nlohmann::json restore(bool files, bool database) {
        ServiceRefusal refusal;
        const auto id = service->restore({"shop", zip, files, database, false}, refusal);
        REQUIRE_MESSAGE(id, refusal.message);
        return wait_for(*service, *id);
    }
    [[nodiscard]] std::string today() const { return test_util::read_file(dir / "index.php"); }
};

}  // namespace

TEST_CASE("backup and new project from the backup, as jobs") {
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

    // Back up: every folder it can leave out is left out; an old zip there
    // is replaced only once the new one is whole.
    const fs::path zip = tmp.path() / "backups" / "shop 2026-11-05 1430.zip";
    test_util::write_file(zip, "an older backup");
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
    CHECK(info->computer == read_computer_id(data));  // made here
    CHECK(made_here(*info, data));
    CHECK(zip_names(zip).contains(".floserver/database.sql"));
    CHECK(fakes.switched.empty());  // the project kept running

    // A new project from it: "shop-copy", its database loaded and .env pointed at it.
    const fs::path projects = tmp.path() / "projects";
    const auto cid = service.create({zip, "shop-copy", projects, false}, refusal);
    REQUIRE_MESSAGE(cid, refusal.message);
    j = wait_for(service, *cid);
    REQUIRE_MESSAGE(j.at("state") == "done", j.dump());
    const fs::path copy = projects / "shop-copy";
    CHECK(test_util::read_file(copy / "index.php") == "<?php echo 'hi';");
    CHECK_FALSE(fs::exists(copy / ".floserver"));
    CHECK_FALSE(fs::exists(copy / "vendor"));
    CHECK_FALSE(fs::exists(projects / ".flo-r2"));  // the short unpack folder is gone
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

    // No room for it: nothing is made.
    fakes.free = 10;
    const auto full = service.create({zip, "shop-full", projects, false}, refusal);
    REQUIRE(full);
    j = wait_for(service, *full);
    CHECK(j.at("state") == "failed");
    CHECK(j.at("error").get<std::string>().find("there isn't enough free space") != std::string::npos);
    CHECK_FALSE(fs::exists(projects / "shop-full"));
    CHECK_FALSE(fakes.projects.contains("shop-full"));
}

TEST_CASE("a backup made on another computer installs nothing unless asked") {
    const test_util::TempDir tmp;
    const fs::path dir = make_project(tmp.path());
    Fakes fakes;
    fakes.projects["shop"] = BackupProject{"shop", dir, false, false};
    const fs::path zip = tmp.path() / "shop.zip";
    {
        BackupService elsewhere(tmp.path() / "other-data", fakes.hooks());
        ServiceRefusal refusal;
        const auto id = elsewhere.backup({"shop", zip, false, std::nullopt}, refusal);
        REQUIRE(id);
        REQUIRE(wait_for(elsewhere, *id).at("state") == "done");
    }
    std::string error;
    const auto info = read_backup_info(zip, error);
    REQUIRE(info);
    const fs::path data = tmp.path() / "data";
    REQUIRE(computer_id(data, error));
    CHECK_FALSE(made_here(*info, data));
    // composer install would run (composer.json is there, vendor left out) if it weren't foreign.
    BackupService here(data, fakes.hooks());
    ServiceRefusal refusal;
    const auto id = here.create({zip, "shop-copy", tmp.path() / "projects", std::nullopt}, refusal);
    REQUIRE(id);
    const auto j = wait_for(here, *id);
    REQUIRE_MESSAGE(j.at("state") == "done", j.dump());
    bool said = false;
    for (const auto& line : j.at("log")) {
        said = said || line.get<std::string>().starts_with("Made on another computer");
        CHECK_FALSE(line.get<std::string>().starts_with("$ "));
    }
    CHECK(said);
}

TEST_CASE("restore: today's database kept, today's files moved aside, the backup's put back") {
    Restorable r;
    const auto j = r.restore(true, true);
    REQUIRE_MESSAGE(j.at("state") == "done", j.dump());
    CHECK(r.today() == "<?php echo 'hi';");
    const fs::path aside = j.at("set_aside").get<std::string>();
    CHECK(aside.filename().string().starts_with("shop before restore "));
    CHECK(test_util::read_file(aside / "index.php") == "<?php echo 'today';");
    const fs::path saved = j.at("saved_database").get<std::string>();
    CHECK(saved.parent_path() == r.zip.parent_path());
    CHECK(saved.filename().string().starts_with("shop before restore "));
    CHECK(test_util::read_file(saved) == "-- today's\n");
    REQUIRE(r.fakes.loaded.size() == 1);
    CHECK(r.fakes.loaded[0].starts_with("DROP DATABASE IF EXISTS `shop`;"));
    CHECK(r.fakes.switched == std::vector<std::pair<std::string, bool>>{{"shop", false}, {"shop", true}});
    CHECK_FALSE(fs::exists(r.dir.parent_path() / ".flo-r2"));
}

TEST_CASE("restore: a backup that can't be used changes nothing") {
    Restorable r;
    // An unsafe name in it: refused before the project is touched.
    test_util::write_file(r.tmp.path() / "bad.zip",
                          zip_of({{"floserver-backup.json", R"({"format": 1, "name": "shop"})"}, {"nul.txt", "x"}}));
    ServiceRefusal refusal;
    const auto id = r.service->restore({"shop", r.tmp.path() / "bad.zip", true, false, false}, refusal);
    REQUIRE(id);
    auto j = wait_for(*r.service, *id);
    CHECK(j.at("state") == "failed");
    CHECK(j.at("error") == "the backup can't be unpacked: the archive has an unsafe file name: nul.txt; "
                           "nothing was changed. Today's files are where they were.");
    CHECK(r.today() == "<?php echo 'today';");
    CHECK(r.fakes.switched.empty());

    // No room for it.
    r.fakes.free = 10;
    j = r.restore(true, true);
    CHECK(j.at("state") == "failed");
    CHECK(j.at("error").get<std::string>().find("there isn't enough free space") != std::string::npos);
    CHECK(j.at("error").get<std::string>().ends_with(
        "nothing was changed. Today's files are where they were. Its database wasn't changed."));
    CHECK(r.today() == "<?php echo 'today';");
    CHECK(r.fakes.switched.empty());
    CHECK(r.fakes.loaded.empty());
}

TEST_CASE("restore: today's database that can't be saved changes nothing, and it starts again") {
    Restorable r;
    r.fakes.dump_fails = true;
    const auto j = r.restore(true, true);
    CHECK(j.at("state") == "failed");
    CHECK(j.at("error") == "today's database couldn't be saved, so nothing was changed: mariadb-dump: Access denied. "
                           "Today's files are where they were. Its database wasn't changed.");
    CHECK(r.today() == "<?php echo 'today';");
    CHECK(r.fakes.loaded.empty());
    CHECK(r.fakes.switched == std::vector<std::pair<std::string, bool>>{{"shop", false}, {"shop", true}});
    // Nothing unpacked is left beside the folder.
    for (const auto& e : fs::directory_iterator(r.dir.parent_path())) {
        CHECK_FALSE(e.path().filename().string().starts_with(".flo-"));
    }
}

TEST_CASE("restore: a database that won't load puts today's files and database back") {
    Restorable r;
    r.fakes.load_fails = 1;  // the backup's; today's loads back
    const auto j = r.restore(true, true);
    CHECK(j.at("state") == "failed");
    const std::string saved = j.at("saved_database").get<std::string>();
    CHECK(j.at("error") == "the database couldn't be loaded from the backup: ERROR 1064: syntax. Today's files are "
                           "back in place. Today's database was loaded back. Today's files are where they were. "
                           "Today's database is kept in " + saved + ", and is still the project's.");
    CHECK(r.today() == "<?php echo 'today';");
    REQUIRE(r.fakes.loaded.size() == 2);
    CHECK(r.fakes.loaded[1].find("-- today's") != std::string::npos);
    CHECK(r.fakes.switched.back() == std::pair<std::string, bool>{"shop", true});
    CHECK_FALSE(j.contains("set_aside"));

    // Today's can't be loaded back either: it says where it is.
    r.fakes.load_fails = 2;
    const auto k = r.restore(true, true);
    CHECK(k.at("state") == "failed");
    const std::string saved2 = k.at("saved_database").get<std::string>();
    CHECK(k.at("error").get<std::string>().find("Today's database couldn't be loaded back (ERROR 1064: syntax).") !=
          std::string::npos);
    CHECK(k.at("error").get<std::string>().ends_with("Today's database is kept in " + saved2 + "."));
}

TEST_CASE("restore: Cancel while the database loads puts today's back") {
    Restorable r;
    r.fakes.load_waits = true;
    ServiceRefusal refusal;
    const auto id = r.service->restore({"shop", r.zip, true, true, false}, refusal);
    REQUIRE(id);
    REQUIRE(eventually([&] { return r.fakes.loading.load(); }));
    r.fakes.load_waits = false;  // loading today's back isn't cancelled
    CHECK(r.service->cancel(*id));
    const auto j = wait_for(*r.service, *id);
    CHECK(j.at("state") == "cancelled");
    CHECK(j.at("error").get<std::string>().starts_with(
        "cancelled while the database was loaded. Today's files are back in place. Today's database was loaded back."));
    CHECK(r.today() == "<?php echo 'today';");
}

TEST_CASE("restore: Cancel while unpacking changes nothing") {
    Restorable r;
    // Many files, so there's time to cancel.
    for (int i = 0; i < 400; ++i) {
        test_util::write_file(r.dir / "many" / (std::to_string(i) + ".txt"), std::string(1000, 'x'));
    }
    ServiceRefusal refusal;
    const fs::path big = r.tmp.path() / "backups" / "big.zip";
    const auto bid = r.service->backup({"shop", big, false, std::nullopt}, refusal);
    REQUIRE(bid);
    REQUIRE(wait_for(*r.service, *bid).at("state") == "done");
    const auto id = r.service->restore({"shop", big, true, false, false}, refusal);
    REQUIRE(id);
    (void)r.service->cancel(*id);
    const auto j = wait_for(*r.service, *id);
    if (j.at("state") == "cancelled") {
        CHECK(j.at("error") == "cancelled; nothing was changed. Today's files are where they were.");
        CHECK(r.today() == "<?php echo 'today';");
        const bool switched_back = r.fakes.switched.empty() ||
            r.fakes.switched == std::vector<std::pair<std::string, bool>>{{"shop", false}, {"shop", true}};
        CHECK(switched_back);
    } else {
        CHECK(j.at("state") == "done");  // it was quicker than the cancel
    }
}

TEST_CASE("restore: a project whose folder is gone isn't turned on again") {
    Restorable r;
    fs::remove_all(r.dir);
    const auto j = r.restore(false, true);  // its database only
    REQUIRE_MESSAGE(j.at("state") == "done", j.dump());
    CHECK(r.fakes.switched == std::vector<std::pair<std::string, bool>>{{"shop", false}});
    // With its files the folder is back, and so is the project.
    const auto k = r.restore(true, false);
    REQUIRE_MESSAGE(k.at("state") == "done", k.dump());
    CHECK_FALSE(k.contains("set_aside"));
    CHECK(fs::exists(r.dir / "index.php"));
}

TEST_CASE("restore: something that isn't a backup is refused") {
    Restorable r;
    test_util::write_file(r.tmp.path() / "not.zip", "PK nothing");
    ServiceRefusal refusal;
    CHECK_FALSE(r.service->restore({"shop", r.tmp.path() / "not.zip", true, true, true}, refusal));
    CHECK(refusal.code == "not_a_backup");
}

TEST_CASE("stop() ends a load that's running") {
    Restorable r;
    r.fakes.load_waits = true;
    ServiceRefusal refusal;
    const auto id = r.service->restore({"shop", r.zip, true, true, false}, refusal);
    REQUIRE(id);
    REQUIRE(eventually([&] { return r.fakes.loading.load(); }));
    r.fakes.load_waits = false;
    const auto started = std::chrono::steady_clock::now();
    r.service->stop();
    CHECK(std::chrono::steady_clock::now() - started < 5s);
    CHECK(r.today() == "<?php echo 'today';");
}

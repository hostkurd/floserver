// The backup dialogs' words and sums (src/ui-win32/model/backup_dialogs.h,
// mockups approved 5 Nov).

#include "model/backup_dialogs.h"

#include <doctest/doctest.h>

using namespace floserver::ui;
using nlohmann::json;

namespace {

const std::string kLri{'\xE2', '\x80', '\xAA'};
const std::string kPdi{'\xE2', '\x80', '\xAC'};
constexpr std::uint64_t kMb = std::uint64_t{1024} * 1024;

json plan_reply() {
    return {{"name", "my-shop"},
            {"path", "C:\\sites\\my-shop"},
            {"bytes", 100 * kMb},
            {"complete", true},
            {"folders",
             json::array({{{"name", "vendor"}, {"bytes", 41 * kMb}, {"tool", "composer"}},
                          {{"name", "node_modules"}, {"bytes", 50 * kMb}, {"tool", "npm"}}})},
            {"database", {{"name", "my_shop"}, {"bytes", 2 * kMb}}},
            {"default_file", "C:\\Users\\me\\Documents\\FloServer Backups\\my-shop 2026-11-05 1430.zip"}};
}

}  // namespace

TEST_CASE("Back up: the dialog's words, only what the project has") {
    const auto plan = parse_backup_plan(plan_reply());
    REQUIRE(plan);
    const BackupForm f = backup_form(*plan, Lang::En);
    CHECK(f.title == "Back up my-shop");
    CHECK(f.save_as == "Save as");
    CHECK(f.change == "Change…");
    CHECK(f.include == "Include");
    CHECK(f.database == "The database my_shop");
    CHECK(f.database_detail == "As database.sql in the zip · 2 MB");
    CHECK(f.leave_out == "Leave out (they can be installed again)");
    REQUIRE(f.folders.size() == 2);
    CHECK(f.folders[0] == std::pair<std::string, std::string>{"vendor", "Composer's packages · 41 MB · composer install brings them back"});
    CHECK(f.folders[1].second == "npm's packages · 50 MB · npm install brings them back");
    CHECK(f.back_up == "Back up");
    CHECK(f.cancel == "Cancel");

    // No database, no folders to leave out: nothing offered.
    json bare = plan_reply();
    bare["database"] = nullptr;
    bare["folders"] = json::array();
    const BackupForm b = backup_form(*parse_backup_plan(bare), Lang::En);
    CHECK(b.database.empty());
    CHECK(b.leave_out.empty());
    CHECK(b.folders.empty());
    CHECK_FALSE(parse_backup_plan(json::object()));

    // Arabic: the folder names stay left to right.
    const BackupForm ar = backup_form(*plan, Lang::Ar);
    CHECK(ar.folders[0].first == kLri + "vendor" + kPdi);
    CHECK(ar.title == "نسخ " + kLri + "my-shop" + kPdi + " احتياطيًا");
}

TEST_CASE("Back up: the total follows the ticks") {
    const auto plan = parse_backup_plan(plan_reply());
    REQUIRE(plan);
    // Both left out, the database in: 100 - 41 - 50 + 2.
    CHECK(backup_bytes(*plan, true, {true, true}) == 11 * kMb);
    CHECK(backup_total_text(*plan, true, {true, true}, Lang::En) == "About 11 MB in all.");
    CHECK(backup_bytes(*plan, false, {true, false}) == 59 * kMb);
    CHECK(backup_bytes(*plan, false, {false, false}) == 100 * kMb);
    json partial = plan_reply();
    partial["complete"] = false;
    CHECK(backup_total_text(*parse_backup_plan(partial), false, {false, false}, Lang::En) == "At least 100 MB in all.");

    const json params = backup_start_params(*plan, "D:\\b.zip", true, {false, true});
    CHECK(params == json{{"name", "my-shop"}, {"to", "D:\\b.zip"}, {"database", true}, {"leave_out", {"node_modules"}}});
    CHECK(with_zip_extension("D:\\b") == "D:\\b.zip");
    CHECK(with_zip_extension("D:\\b.ZIP") == "D:\\b.ZIP");
}

TEST_CASE("a backup described from its floserver-backup.json") {
    BackupSummary b;
    b.name = "my-shop";
    b.date = "2026-11-05 14:30";
    b.versions = {{"php", "8.5"}, {"database", "mariadb-11.4"}, {"web", "nginx"}};
    b.database = true;
    CHECK(describe_backup(b, Lang::En) == "my-shop, backed up 2026-11-05 14:30 · PHP 8.5 · MariaDB 11.4 · with its database");
    CHECK(restore_backed_up(b, Lang::En) == "Backed up 2026-11-05 14:30 · with its database");
    b.database = false;
    b.versions = {{"php", "none"}, {"node", "22"}};
    CHECK(describe_backup(b, Lang::En) == "my-shop, backed up 2026-11-05 14:30 · Node.js 22");
    CHECK(restore_backed_up(b, Lang::En) == "Backed up 2026-11-05 14:30");
    CHECK(backup_versions_text({{"python", "3.13"}, {"php", "newest"}}) == "PHP · Python 3.13");
}

TEST_CASE("New project from a backup and Restore: the words") {
    const FromBackupForm f = from_backup_form(Lang::En);
    CHECK(f.title == "New project from a backup");
    CHECK(f.create == "Create project");
    CHECK(f.install == "Install the packages that were left out");
    CHECK(from_backup_where("C:\\Users\\me\\FloServer\\projects", "my-shop-copy", Lang::En) ==
          "It goes in C:\\Users\\me\\FloServer\\projects\\my-shop-copy and opens at my-shop-copy.localhost.");

    const RestoreForm r = restore_form("my-shop", true, Lang::En);
    CHECK(r.title == "Restore my-shop from a backup?");
    CHECK(r.files == "Replace the project's files");
    CHECK(r.database == "Replace its database my_shop");
    CHECK(r.note == "The project stops while it's restored, then starts again.");
    CHECK(r.restore == "Restore");
    CHECK(r.files_note == "Today's files are moved aside first, to a folder beside it.");
    CHECK(r.foreign == "Made on another computer: installing runs the packages' own scripts as you.");
    CHECK(restore_form("my-shop", false, Lang::En).database.empty());
    for (const Lang lang : {Lang::En, Lang::Ar, Lang::Ku}) {
        const RestoreForm any = restore_form("my-shop", true, lang);
        CHECK_FALSE(any.files_note.empty());
        CHECK_FALSE(any.database_note.empty());
        CHECK_FALSE(from_backup_form(lang).database_note.empty());
        CHECK_FALSE(from_backup_form(lang).foreign.empty());
        CHECK_FALSE(any.foreign.empty());
    }
}

TEST_CASE("a backup job's progress line and how it ends") {
    json job = {{"kind", "backup"}, {"state", "running"}, {"files", 0}, {"total_files", 340}, {"database", "saving"},
                {"step", "Saving the database…"}};
    CHECK(backup_job_title("backup", "my-shop", Lang::En) == "Backing up my-shop…");
    CHECK(backup_progress_text(job, Lang::En) == "0 of 340 files · saving the database… · the project keeps running");
    job["files"] = 120;
    job["database"] = "saved";
    CHECK(backup_progress_text(job, Lang::En) == "120 of 340 files · the database is saved · the project keeps running");
    CHECK(backup_progress_percent(job) == 35);
    job["total_files"] = 0;
    CHECK_FALSE(backup_progress_percent(job));

    json restore = {{"kind", "restore"}, {"files", 3}, {"total_files", 4}, {"database", "loading"}, {"step", "x"}};
    CHECK(backup_job_title("restore", "my-shop", Lang::En) == "Restoring my-shop…");
    CHECK(backup_progress_text(restore, Lang::En) == "3 of 4 files · loading the database… · the project is stopped meanwhile");
    restore["step"] = "$ composer install --no-interaction";
    CHECK(backup_progress_text(restore, Lang::En).find("installing the packages…") != std::string::npos);
    CHECK_FALSE(backup_progress_percent(restore));

    // Done: the file and its size under the headline, and the file to show.
    const json done = {{"kind", "backup"}, {"state", "done"}, {"size", 12 * kMb},
                       {"file", "C:\\Users\\me\\Documents\\FloServer Backups\\my-shop 2026-11-05 1430.zip"}};
    BackupOutcome o = backup_outcome("backup", "my-shop", done, {}, Lang::En);
    CHECK(o.text == "Backed up my-shop\nmy-shop 2026-11-05 1430.zip · 12 MB");
    CHECK_FALSE(o.error);
    CHECK(o.show_file == done.at("file").get<std::string>());
    o = backup_outcome("backup", "my-shop", {{"state", "failed"}, {"error", "the disk is full"}}, {}, Lang::En);
    CHECK(o.text == "Couldn't back up my-shop: the disk is full");
    CHECK(o.error);
    o = backup_outcome("backup", "my-shop", {{"state", "cancelled"}}, {}, Lang::En);
    CHECK(o.text == "The backup of my-shop was cancelled; nothing was saved.");
    CHECK_FALSE(o.error);
    o = backup_outcome("restore", "my-shop", nullptr, "FloServer stopped answering", Lang::En);
    CHECK(o.text == "Couldn't restore my-shop: FloServer stopped answering");
    o = backup_outcome("restore", "my-shop", {{"state", "done"}, {"saved_database", "D:\\my-shop before restore.sql"}}, {},
                    Lang::En);
    CHECK(o.text == "my-shop is restored from the backup. Today's database is in D:\\my-shop before restore.sql");
    o = backup_outcome("create", "my-shop-copy", {{"state", "done"}}, {}, Lang::En);
    CHECK(o.text == "my-shop-copy is made from the backup.");
}

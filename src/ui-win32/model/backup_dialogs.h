#pragma once

// Backups in the window (D22, mockups approved 5 Nov): the Back up, New
// project from a backup and Restore dialogs' words, what each tick adds or
// takes away, the progress line while a job runs and what's said when it
// ends. Plain C++ so it's tested on every platform; backup_dialogs.cpp in
// the window shows it.

#include "model/ui_strings.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace floserver::ui {

// backup.plan's answer.
struct BackupPlan {
    struct Folder {
        std::string name;  // "vendor"
        std::string tool;  // "composer"
        std::uint64_t bytes = 0;
    };
    std::string name;
    std::string path;
    std::uint64_t bytes = 0;  // the whole folder
    bool complete = true;     // false: the count stopped early ("at least")
    std::vector<Folder> folders;
    std::optional<std::string> database;  // its name, when it has one
    std::uint64_t database_bytes = 0;
    std::string default_file;
};
[[nodiscard]] std::optional<BackupPlan> parse_backup_plan(const nlohmann::json& reply);

// The Back up dialog: "Back up shop", Save as with Change…, Include (the
// database, ticked), Leave out (each folder it has, ticked), the total,
// Back up and Cancel.
struct BackupForm {
    std::string title;
    std::string save_as;
    std::string change;
    std::string include;
    std::string database;         // empty: no database to offer
    std::string database_detail;  // "As database.sql in the zip · 2.1 MB"
    std::string leave_out;        // empty: no folder to leave out
    std::vector<std::pair<std::string, std::string>> folders;  // "vendor", "Composer's packages · 41 MB · ..."
    std::string back_up;
    std::string cancel;
};
[[nodiscard]] BackupForm backup_form(const BackupPlan& plan, Lang lang);
// About how much goes into the zip: the folder without the folders left out
// (`leave_out[i]` for plan.folders[i]), and the database when it's in.
[[nodiscard]] std::uint64_t backup_bytes(const BackupPlan& plan, bool database, const std::vector<bool>& leave_out);
// "About 14 MB in all."
[[nodiscard]] std::string backup_total_text(const BackupPlan& plan, bool database, const std::vector<bool>& leave_out,
                                            Lang lang);
// backup.start's parameters for those choices.
[[nodiscard]] nlohmann::json backup_start_params(const BackupPlan& plan, const std::string& file, bool database,
                                                 const std::vector<bool>& leave_out);
// "shop.zip" or "C:\...\shop.zip": a .zip at the end, whatever was typed.
[[nodiscard]] std::string with_zip_extension(const std::string& file);

// A backup as floserver-backup.json describes it (read in the window when it's chosen).
struct BackupSummary {
    std::string name;
    std::string date;
    nlohmann::json versions = nlohmann::json::object();
    bool database = false;
    std::vector<std::string> left_out;
};
// "PHP 8.5 · MariaDB 11.4": the versions it was made with, as the panel names them.
[[nodiscard]] std::string backup_versions_text(const nlohmann::json& versions);
// "shop, backed up 2026-11-05 14:30 · PHP 8.5 · MariaDB 11.4 · with its database"
[[nodiscard]] std::string describe_backup(const BackupSummary& backup, Lang lang);

// New project from a backup.
struct FromBackupForm {
    std::string title;
    std::string backup;
    std::string choose;
    std::string name;
    std::string database_note;
    std::string install;
    std::string create;
    std::string cancel;
};
[[nodiscard]] FromBackupForm from_backup_form(Lang lang);
// "It goes in C:\...\shop-copy and opens at shop-copy.localhost."
[[nodiscard]] std::string from_backup_where(const std::string& projects_dir, const std::string& name, Lang lang);

// Restore my-shop from a backup?
struct RestoreForm {
    std::string title;
    std::string backup;
    std::string choose;
    std::string files;
    std::string files_note;
    std::string database;  // empty: the project has no database
    std::string database_note;
    std::string install;
    std::string note;
    std::string restore;
    std::string cancel;
};
[[nodiscard]] RestoreForm restore_form(const std::string& project, bool has_database, Lang lang);
// "Backed up 2026-11-05 14:30 · with its database"
[[nodiscard]] std::string restore_backed_up(const BackupSummary& backup, Lang lang);

// While a job runs (backup.get's answer): the progress dialog's title, its
// line ("120 of 340 files · the database is saved · the project keeps
// running") and how far along it is (0 to 100; nothing while there's no
// count yet: the bar moves on its own).
[[nodiscard]] std::string backup_job_title(const std::string& kind, const std::string& project, Lang lang);
[[nodiscard]] std::string backup_progress_text(const nlohmann::json& job, Lang lang);
[[nodiscard]] std::optional<int> backup_progress_percent(const nlohmann::json& job);

// What's said when it ends: a toast (an error stays until it's closed).
struct BackupOutcome {
    std::string text;
    bool error = false;
    std::string show_file;  // a backup made: "Show in folder" selects it
};
// `job` is backup.get's last answer, or null when it couldn't be followed
// (`error` then says why, from the service or the connection).
[[nodiscard]] BackupOutcome backup_outcome(const std::string& kind, const std::string& project, const nlohmann::json& job,
                                     const std::string& error, Lang lang);

}  // namespace floserver::ui

#pragma once

// The backup dialogs (D22, mockups approved 5 Nov), as the remove question
// is made (form_parts.cpp): small dialogs of standard controls in the
// window's language, laid out right to left in Arabic and Kurdish, so Tab,
// Enter, Esc and screen readers work as in any Windows dialog. Their words
// and sums are in model/backup_dialogs.h.
//
//   Back up <name>              Save as + Change…, the database, the folders
//                               to leave out, the total; Back up (default), Cancel
//   New project from a backup   Backup + Choose…, what it is, Name, where it
//                               goes, Install the packages; Create project, Cancel
//   Restore <name>?             Backup + Choose…, Replace the files, Replace
//                               the database, Install; Restore (red), Cancel
//                               (the default, with the keys on it)
//   the job                     "Backing up <name>…", the line, a bar and
//                               Cancel, while the service runs it
//
// A chosen backup is read here (floserver-backup.json, from the zip on
// disk) to say what it is; the service reads it again for itself.

#include "model/backup_dialogs.h"
#include "model/ui_strings.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace floserver::ui {

struct BackupChoice {
    std::string file;
    bool database = false;
    std::vector<bool> leave_out;  // for plan.folders, in order
};
// Nothing for Cancel (or no dialog at all).
[[nodiscard]] std::optional<BackupChoice> ask_backup(HWND owner, Lang lang, const BackupPlan& plan);

struct FromBackupSetup {
    std::string file;          // already chosen (a .zip opened with FloServer); empty: chosen in the dialog
    std::string projects_dir;  // where it goes
    std::function<bool(const std::string&)> name_taken;  // a project or a folder there has it
};
struct FromBackupChoice {
    std::string file;
    std::string name;
    bool install = true;
};
[[nodiscard]] std::optional<FromBackupChoice> ask_from_backup(HWND owner, Lang lang, const FromBackupSetup& setup);

struct RestoreChoice {
    std::string file;
    bool files = true;
    bool database = true;
    bool install = true;
};
// `has_database`: the project asks for one. `start` is the folder the
// picker opens in (Documents\FloServer Backups).
[[nodiscard]] std::optional<RestoreChoice> ask_restore(HWND owner, Lang lang, const std::string& project,
                                                       bool has_database, const std::string& start);

// Starts a job with `method` and `params` on a connection of its own and
// shows it until it ends; Cancel asks the service to stop it (once it's
// replacing things it can't be, and the button says so). The job's last
// answer (backup.get's), or null with `error`.
struct JobResult {
    nlohmann::json job;
    std::string error;
};
[[nodiscard]] JobResult run_backup_job(HWND owner, Lang lang, const std::string& kind, const std::string& project,
                                       const std::string& method, const nlohmann::json& params);

// The folder backups go to: Documents\FloServer Backups.
[[nodiscard]] std::string backups_folder();

// Opens Explorer at `file`'s folder with the file selected.
void show_file_in_folder(const std::string& file);

}  // namespace floserver::ui

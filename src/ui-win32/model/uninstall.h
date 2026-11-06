#pragma once

// Remove FloServer… in Settings > General (D21, approved with its mockup):
// the question's words, which folders it touches, and what's said after.
// Plain C++ so it's tested on every platform; the window asks the question,
// removes the certificate, stops the service and starts Windows' uninstall.
//
// What each box deletes in the data folder is core's (data_folder.h,
// uninstall_items). The projects folder goes to the Recycle Bin from the
// window (recycle_bin.h), never deleted from here, and only the folder set
// in Settings > Folders: a project added from anywhere else is never touched.

#include "model/folder_removal.h"
#include "model/folders_page.h"
#include "model/project_menu.h"
#include "model/ui_strings.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// The question's "Also delete" boxes, as the mockup has them by default.
struct UninstallTicks {
    bool runtimes = true;
    bool databases = false;
    bool projects = false;
    bool operator==(const UninstallTicks&) const = default;
};
// As Settings posts them to FloServer's window (an lparam), and back.
[[nodiscard]] unsigned uninstall_ticks_bits(const UninstallTicks& ticks);
[[nodiscard]] UninstallTicks uninstall_ticks_from(unsigned bits);

// The folders the question and the clean-up go by, read from config.json
// (the service may not be running): the projects folder set in Settings >
// Folders (else the default), every registered project's folder, the www
// folder. Empty where there's none.
struct UninstallFolders {
    bool readable = true;  // false: config.json is there but can't be read
    std::string projects_dir;
    std::vector<std::string> project_dirs;
    std::string shared_dir;
};
[[nodiscard]] UninstallFolders uninstall_folders(const std::filesystem::path& data);

// The guard the projects folder is checked against before it goes to the
// Recycle Bin: `base` (Windows' folders and the data folder, as for a
// removed project's folder, but not the projects folder itself), the www
// folder kept, and the projects added from other folders as "other
// projects" (so a projects folder inside one, or holding one, stays). The
// projects inside it are its own and go with it. A projects folder that is
// one of Windows' own (Documents, the profile) is refused by `base`.
[[nodiscard]] FolderGuard projects_folder_guard(FolderGuard base, const UninstallFolders& folders);

// The sizes beside the boxes; nothing while they're counted ("…").
struct UninstallSizes {
    std::optional<std::uint64_t> runtimes;
    std::optional<std::uint64_t> databases;
};

// The question's words. `projects` and `projects_note` are empty when
// there's no projects folder to offer.
struct UninstallQuestion {
    std::string window_title;
    std::string title;
    std::string text;
    std::string also_delete;
    std::string runtimes;
    std::string runtimes_note;
    std::string databases;
    std::string databases_note;
    std::string projects;
    std::string projects_note;
    std::string remove;
    std::string cancel;
};
[[nodiscard]] UninstallQuestion uninstall_question(Lang lang, const std::string& projects_dir,
                                                   const UninstallSizes& sizes);

// What the clean-up left, said after it: what stayed in the data folder,
// and what became of the projects folder (NotAsked when its box wasn't
// ticked or it wasn't there). Empty when everything chosen went.
struct UninstallReport {
    std::vector<SkippedItem> skipped;
    FolderOutcome projects = FolderOutcome::NotAsked;
    std::string projects_reason;  // Stayed: why, in Windows' or FloServer's words
};
[[nodiscard]] std::vector<std::string> uninstall_problems(const UninstallReport& report, Lang lang);

// The message after the clean-up: what stayed (each on its own line), then
// that Windows' uninstall opens next (`installed`), or that the program's
// files were left because FloServer wasn't installed by its installer.
[[nodiscard]] std::string uninstall_done_text(const UninstallReport& report, bool installed, Lang lang);

// --- the Uninstall window (D25, in floserver-setup.dll) ---

// The "Also delete" boxes: each one's bit (uninstall_ticks_bits), its words
// and its note, from uninstall_question()'s.
struct UninstallChoiceText {
    unsigned bit = 0;
    std::string label;
    std::string note;
    bool checked = false;
};
[[nodiscard]] std::vector<UninstallChoiceText> uninstall_choices(const UninstallQuestion& q,
                                                                 const UninstallTicks& ticks);

// What FloServer.exe's copy in %TEMP% is told to do once the clean-up is
// done: remove the MSI with this product code, at the window's place, then
// say what was deleted, kept and left. Written as JSON beside the copy and
// read back there (FloServer.exe --remove-program <file>).
struct RemoveProgramPlan {
    std::string product;
    std::uint32_t parent = 0;           // the FloServer.exe that wrote it: the copy waits until it has gone
    int x = 0;
    int y = 0;
    std::string deleted;                // one sentence
    std::vector<std::string> kept;      // "Databases: C:\..."
    std::vector<std::string> problems;  // what couldn't be done
    std::string data_dir;               // "Open the data folder"; empty: no link
    bool operator==(const RemoveProgramPlan&) const = default;
};
[[nodiscard]] std::string remove_program_json(const RemoveProgramPlan& plan);
// Nothing when it isn't one, or its product isn't a product code.
[[nodiscard]] std::optional<RemoveProgramPlan> parse_remove_program_json(std::string_view text);

// The Done page's words: what was deleted too (as ticked), and what stayed
// where (the boxes not ticked, with their folders).
[[nodiscard]] std::string uninstall_deleted_text(const UninstallTicks& ticks, bool projects_offered);
[[nodiscard]] std::vector<std::string> uninstall_kept_lines(const UninstallTicks& ticks, const std::string& data_dir,
                                                            const std::string& runtimes_dir,
                                                            const std::string& projects_dir);

// FloServer.exe --remove-program <file>: the plan's file; nothing for other arguments.
inline constexpr std::string_view kRemoveProgramFlag = "--remove-program";
[[nodiscard]] std::optional<std::string> parse_remove_program_args(const std::vector<std::string>& args);
// The copy's command line; empty when a path can't be quoted.
[[nodiscard]] std::string remove_program_command(std::string_view exe, std::string_view plan_file);

// FloServer's MSI, found through its upgrade code (the same for every version).
inline constexpr std::string_view kUpgradeCode = "{3AC2E747-DDE1-465C-87CC-277C97C81D0F}";

// "{12345678-ABCD-...}": a product code as Windows Installer gives it (and
// nothing else), so only that is ever put on msiexec's command line.
[[nodiscard]] bool is_product_code(std::string_view code);

// msiexec's arguments for Windows' own uninstall (its usual window): "/x {...}".
// Empty when `product_code` isn't one.
[[nodiscard]] std::string msiexec_uninstall_args(std::string_view product_code);

}  // namespace floserver::ui

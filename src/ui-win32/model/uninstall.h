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

// FloServer's MSI, found through its upgrade code (the same for every version).
inline constexpr std::string_view kUpgradeCode = "{3AC2E747-DDE1-465C-87CC-277C97C81D0F}";

// "{12345678-ABCD-...}": a product code as Windows Installer gives it (and
// nothing else), so only that is ever put on msiexec's command line.
[[nodiscard]] bool is_product_code(std::string_view code);

// msiexec's arguments for Windows' own uninstall (its usual window): "/x {...}".
// Empty when `product_code` isn't one.
[[nodiscard]] std::string msiexec_uninstall_args(std::string_view product_code);

}  // namespace floserver::ui

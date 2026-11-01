#pragma once

// Settings > Folders and Settings > Shared sites: what the service said and
// the words shown. Plain C++ so it's tested on every platform; the window
// only draws it.

#include "model/ui_strings.h"
#include "model/view_model.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace floserver::ui {

// folders.get's answer (folders.set and folders.clean answer parts of it).
struct FoldersInfo {
    std::string projects_dir;          // where New project makes projects; empty: no home folder
    std::string default_projects_dir;
    bool custom = false;               // set in Settings, not the default
    std::string data_dir;
    std::uint64_t data_bytes = 0;
    bool data_complete = true;         // false: counting stopped early ("more than")
    std::uint64_t logs_bytes = 0;      // what Clear old logs would free
    std::uint64_t leftovers_bytes = 0; // what Remove leftover downloads would free
    bool installing = false;           // a download or New project is running: its files are left alone
};

// folders.get's result; nothing when it isn't one.
[[nodiscard]] std::optional<FoldersInfo> folders_from_reply(const nlohmann::json& result);

// Takes in folders.set's or folders.clean's answer, keeping the rest. A
// clean-up's "freed" comes off the data folder's size (not counted again).
void merge_folders_reply(FoldersInfo& info, const nlohmann::json& result);

// "512 KB", "12.5 MB", "1.4 GB".
[[nodiscard]] std::string disk_size_text(std::uint64_t bytes);

// "Uses 1.4 GB", "Uses more than 1.4 GB (...)".
[[nodiscard]] std::string data_usage_text(const FoldersInfo& info, Lang lang);

// Beside a clean-up button: "Frees 12 MB", "Nothing to free", or (for the
// leftovers while something installs) to wait.
[[nodiscard]] std::string clean_size_text(std::uint64_t bytes, bool installing, Lang lang);

// After a clean-up: "Freed 12 MB." and how many were left because they're in use.
[[nodiscard]] std::string clean_done_text(const nlohmann::json& result, Lang lang);

// Settings > Shared sites: the hint under the title (what they do, or
// what's wrong), the pickers, and the www database login when there is one.
struct SharedPage {
    std::string hint;
    bool problem = false;  // the hint says what's wrong
    std::vector<VersionField> fields;
    bool login = false;    // the www database's login is shown (only while on)
    std::string login_title;
    std::string login_detail;
};
[[nodiscard]] SharedPage shared_page(const SharedSites& shared, const Installed& installed,
                                     const nlohmann::json& databases, Lang lang);

}  // namespace floserver::ui

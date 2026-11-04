#pragma once

// Removing a project can also move its folder to the Recycle Bin (approved
// 4 Nov). Which folders may never go there is decided here, in plain C++ on
// Windows path text, so it's tested on every platform; the window gives it
// the folders Windows knows (the profile, Desktop, Documents, Downloads),
// FloServer's own, the other projects', and the folder's real path.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// Windows path text made comparable: "\\?\" (and "\\?\UNC\") taken off,
// '/' as '\', repeated separators and "." folded, ".." applied, no trailing
// separator (except a drive's "C:\"), ASCII letters lower case. Empty when
// it isn't a full path (a drive and a separator, or a "\\server\share").
[[nodiscard]] std::string comparable_path(std::string_view path);

// `inner` is `outer` or inside it (both comparable_path()s, not empty).
[[nodiscard]] bool same_or_inside(std::string_view inner, std::string_view outer);

// The folders a project's folder is checked against.
struct FolderGuard {
    // Never moved themselves, nor a folder holding one: the user's profile,
    // Desktop, Documents, Downloads, the projects folder, the Shared sites
    // (www) folder.
    std::vector<std::string> kept;
    // Never moved, nor anything inside or holding one: FloServer's data folder.
    std::vector<std::string> kept_with_contents;
    // The same for Windows' own: Windows, Program Files (and x86, and their
    // Common Files), ProgramData, AppData (Roaming, Local, LocalLow).
    std::vector<std::string> system_trees;
    // The other projects' folders: not this one, nor inside, nor holding one.
    std::vector<std::string> other_projects;
};

enum class FolderRefusal : std::uint8_t {
    None,          // it may go to the Recycle Bin
    NotFullPath,   // empty or relative
    DriveRoot,     // "C:\" or "\\server\share"
    KeptFolder,    // one of FolderGuard::kept, or holds one
    DataFolder,    // FloServer's own files
    SystemFolder,  // Windows' own (system_trees), or a Recycle Bin's ($Recycle.Bin)
    OtherProject,  // is, holds or is inside another project's folder
    Link,          // the folder is a junction or a link (the window says so)
};

// Why `folder` may not go to the Recycle Bin, or None. `real` is where it
// really is (the window resolves junctions above it); empty: the same.
// Both are checked.
[[nodiscard]] FolderRefusal folder_removal_refusal(std::string_view folder, std::string_view real,
                                                   const FolderGuard& guard);

}  // namespace floserver::ui

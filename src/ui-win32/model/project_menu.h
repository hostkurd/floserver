#pragma once

// A project row's right-click menu, and the question asked before a project
// is removed from FloServer (approved 2 Nov). What each item says, whether
// it's greyed out or ticked, and the question's words are here, in plain
// C++, so they're tested on every platform; the window only shows them and
// runs the actions the panel's buttons and switches already run.

#include "model/glyphs.h"
#include "model/ui_strings.h"
#include "model/view_model.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

enum class MenuCommand : std::uint8_t {
    None,  // a separator
    OpenBrowser,
    OpenFolder,
    OpenTerminal,
    OpenEditor,   // Open in VS Code (the code editor), or Choose a code editor… when there's none
    CopyAddress,
    Toggle,       // Turn on / Turn off
    KeepRunning,
    Settings,     // Settings → Shared sites, from the Shared sites row
    BackUp,       // Back up as zip… (D22)
    Restore,      // Restore from backup…
    Remove,
};

struct MenuItem {
    MenuCommand command = MenuCommand::None;
    Str label = Str::Count_;
    bool enabled = true;
    bool checked = false;
};

// What the window knows besides the row.
struct ProjectMenuState {
    bool on = false;         // the switch as shown (the user's choice while it's on its way)
    bool keep = false;       // Keep running, likewise
    bool connected = false;  // the service answers
    bool removing = false;   // its projects.remove is on its way
};

// Open in browser · Open folder · Open terminal · Open in <editor> · Copy
// address | Turn on (or off) · Keep running | Back up as zip… · Restore
// from backup… (D22, approved 5 Nov) | Remove from FloServer…
// (no Project settings…: a click on the row opens its panel).
// The address items are greyed out when there's nothing to open (off, a
// problem, needs a download); the changes while the service isn't there or
// the project is being removed. `editor` is the code editor's name ("VS
// Code"); empty when there's none: the item is Choose a code editor… then.
[[nodiscard]] std::vector<MenuItem> project_menu(const ProjectRow& row, const ProjectMenuState& state,
                                                 std::string_view editor = {});

// The small icon beside an item, the panel's own where there's one (approved
// 4 Nov): Keep running keeps its tick instead.
[[nodiscard]] std::optional<Glyph> menu_glyph(MenuCommand command);

// An item's words: "Open in VS Code" with the editor's name filled in.
[[nodiscard]] std::string menu_item_text(const MenuItem& item, Lang lang, std::string_view editor);

// The pinned Shared sites row: only what it already does. Open in browser ·
// Open folder | Settings… (no Remove).
[[nodiscard]] std::vector<MenuItem> shared_menu(const SharedSites& shared);

// The project has a MariaDB database (floserver.json asks for one, or the
// service says what it uses): the question offers to delete it too.
[[nodiscard]] bool row_has_database(const ProjectRow& row);

// The question before removing: the window's title, "Remove my-shop from
// FloServer?", what happens to the folder, the buttons, and two boxes, both
// unticked: the database (empty when it has none) and moving the folder to
// the Recycle Bin (empty when it isn't offered: model/folder_removal.h), with
// the folder shown under it.
struct RemoveQuestion {
    std::string window_title;
    std::string title;
    std::string text;
    std::string remove;
    std::string cancel;
    std::string drop;
    std::string trash;
    std::string folder;      // "Folder: C:\...", with `trash`
    std::string trash_note;  // why there's no box: its drive has no Recycle Bin
};
// Whether the folder may go to the Recycle Bin: Offer, not at all (the
// rules, said nothing), or its drive has no Recycle Bin (said in the dialog).
enum class FolderOffer : std::uint8_t { None, Offer, NoRecycleBin };
[[nodiscard]] RemoveQuestion remove_question(const ProjectRow& row, Lang lang,
                                             FolderOffer offer = FolderOffer::None);

// What became of the folder after a removal.
enum class FolderOutcome : std::uint8_t {
    NotAsked,   // the box wasn't ticked
    Recycled,   // in the Recycle Bin (Windows said where it put it)
    Deleted,    // Windows deleted it for good (the user said yes to its question)
    Gone,       // gone from its place, Windows didn't say where
    Stayed,     // it couldn't be moved (files open, refused...): `reason` says why
    Cancelled,  // the user said no in Windows' own question
};
struct RemoveOutcome {
    std::string text;
    bool error = false;  // shown until it's closed
};
// "my-shop is removed from FloServer…" with what happened to its database and folder.
[[nodiscard]] RemoveOutcome remove_outcome(const std::string& project, bool database_dropped, FolderOutcome folder,
                                           const std::string& reason, Lang lang);

}  // namespace floserver::ui

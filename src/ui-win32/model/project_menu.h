#pragma once

// A project row's right-click menu, and the question asked before a project
// is removed from FloServer (approved 2 Nov). What each item says, whether
// it's greyed out or ticked, and the question's words are here, in plain
// C++, so they're tested on every platform; the window only shows them and
// runs the actions the panel's buttons and switches already run.

#include "model/ui_strings.h"
#include "model/view_model.h"

#include <cstdint>
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
    Settings,     // the project's panel; Settings → Shared sites for the shared row
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
// address | Turn on (or off) · Keep running | Project settings… | Remove from
// FloServer…
// The address items are greyed out when there's nothing to open (off, a
// problem, needs a download); the changes while the service isn't there or
// the project is being removed. `editor` is the code editor's name ("VS
// Code"); empty when there's none: the item is Choose a code editor… then.
[[nodiscard]] std::vector<MenuItem> project_menu(const ProjectRow& row, const ProjectMenuState& state,
                                                 std::string_view editor = {});

// An item's words: "Open in VS Code" with the editor's name filled in.
[[nodiscard]] std::string menu_item_text(const MenuItem& item, Lang lang, std::string_view editor);

// The pinned Shared sites row: only what it already does. Open in browser ·
// Open folder | Settings… (no Remove).
[[nodiscard]] std::vector<MenuItem> shared_menu(const SharedSites& shared);

// The project has a MariaDB database (floserver.json asks for one, or the
// service says what it uses): the question offers to delete it too.
[[nodiscard]] bool row_has_database(const ProjectRow& row);

// The question before removing (a Windows task dialog): the window's title,
// "Remove my-shop from FloServer?", what happens to the folder, the
// buttons, and the box for the database (empty when it has none; unticked).
struct RemoveQuestion {
    std::string window_title;
    std::string title;
    std::string text;
    std::string remove;
    std::string cancel;
    std::string drop;
};
[[nodiscard]] RemoveQuestion remove_question(const ProjectRow& row, Lang lang);

}  // namespace floserver::ui

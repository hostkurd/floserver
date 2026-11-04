// A project row's right-click menu and the question before removing a
// project (src/ui-win32/model/project_menu.h, approved 2 Nov).

#include "model/project_menu.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace floserver::ui;

namespace {

const std::string kLri{'\xE2', '\x80', '\xAA'};
const std::string kPdi{'\xE2', '\x80', '\xAC'};

ProjectRow served() {
    ProjectRow row;
    row.name = "my-shop";
    row.on = true;
    row.state = RowState::On;
    row.address = "https://my-shop.localhost";
    row.path = "C:/sites/my-shop";
    return row;
}

const MenuItem& item(const std::vector<MenuItem>& menu, MenuCommand command) {
    for (const MenuItem& i : menu) {
        if (i.command == command) {
            return i;
        }
    }
    static const MenuItem none;
    FAIL("no such item");
    return none;
}

std::vector<MenuCommand> commands(const std::vector<MenuItem>& menu) {
    std::vector<MenuCommand> out;
    out.reserve(menu.size());
    for (const MenuItem& i : menu) {
        out.push_back(i.command);
    }
    return out;
}

}  // namespace

TEST_CASE("project menu: the items, in the approved order") {
    const auto menu = project_menu(served(), {true, false, true, false});
    using enum MenuCommand;
    CHECK(commands(menu) == std::vector<MenuCommand>{OpenBrowser, OpenFolder, OpenTerminal, OpenEditor, CopyAddress,
                                                     None, Toggle, KeepRunning, None, Settings, None, Remove});
    for (const MenuItem& i : menu) {
        CAPTURE(static_cast<int>(i.command));
        CHECK(i.enabled);
        if (i.command != None) {
            CHECK_FALSE(text(Lang::En, i.label).empty());
        }
    }
    // The panel's and tray's words where they exist already.
    CHECK(item(menu, OpenBrowser).label == Str::OpenInBrowser);
    CHECK(item(menu, KeepRunning).label == Str::KeepRunning);
    CHECK(text(Lang::En, item(menu, Remove).label) == "Remove from FloServer…");
    CHECK(text(Lang::En, item(menu, Settings).label) == "Project settings…");
}

TEST_CASE("project menu: the code editor's item, after Open terminal") {
    const ProjectMenuState state{true, false, true, false};
    auto menu = project_menu(served(), state, "VS Code");
    const MenuItem& open = item(menu, MenuCommand::OpenEditor);
    CHECK(open.enabled);
    CHECK(menu_item_text(open, Lang::En, "VS Code") == "Open in VS Code");
    // The editor's Latin name keeps its direction in Arabic and Kurdish.
    CHECK(menu_item_text(open, Lang::Ar, "VS Code") == "فتح في " + kLri + "VS Code" + kPdi);
    CHECK(menu_item_text(open, Lang::Ku, "VS Code") == "کردنەوە لە " + kLri + "VS Code" + kPdi);
    CHECK(menu_item_text(item(menu, MenuCommand::OpenTerminal), Lang::En, "VS Code") == "Open terminal");
    // None found: the item asks for one, even without a folder.
    menu = project_menu(served(), state);
    CHECK(menu_item_text(item(menu, MenuCommand::OpenEditor), Lang::En, {}) == "Choose a code editor…");
    ProjectRow row = served();
    row.path.clear();
    CHECK(item(project_menu(row, state), MenuCommand::OpenEditor).enabled);
    CHECK_FALSE(item(project_menu(row, state, "Cursor"), MenuCommand::OpenEditor).enabled);
}

TEST_CASE("project menu: Turn on or off and Keep running follow the switches") {
    ProjectMenuState state{true, true, true, false};
    auto menu = project_menu(served(), state);
    CHECK(text(Lang::En, item(menu, MenuCommand::Toggle).label) == "Turn off");
    CHECK(item(menu, MenuCommand::KeepRunning).checked);
    state.on = false;
    state.keep = false;
    menu = project_menu(served(), state);
    CHECK(text(Lang::En, item(menu, MenuCommand::Toggle).label) == "Turn on");
    CHECK_FALSE(item(menu, MenuCommand::KeepRunning).checked);
}

TEST_CASE("project menu: what doesn't apply is greyed out") {
    ProjectRow row = served();
    // Nothing to open while it's off, a problem or needs a download.
    for (const RowState s : {RowState::Off, RowState::Problem, RowState::NeedsDownload}) {
        row.state = s;
        row.address = s == RowState::Off ? "" : "https://my-shop.localhost";
        const auto menu = project_menu(row, {true, false, true, false});
        CHECK_FALSE(item(menu, MenuCommand::OpenBrowser).enabled);
        CHECK_FALSE(item(menu, MenuCommand::CopyAddress).enabled);
        CHECK(item(menu, MenuCommand::OpenFolder).enabled);
        CHECK(item(menu, MenuCommand::Toggle).enabled);
    }
    // Only a project's own local address.
    row = served();
    row.address = "https://example.com";
    CHECK_FALSE(item(project_menu(row, {true, false, true, false}), MenuCommand::OpenBrowser).enabled);
    // No folder from the service.
    row = served();
    row.path.clear();
    const auto no_folder = project_menu(row, {true, false, true, false});
    CHECK_FALSE(item(no_folder, MenuCommand::OpenFolder).enabled);
    CHECK_FALSE(item(no_folder, MenuCommand::OpenTerminal).enabled);
    // The service isn't there: nothing can be changed; the panel still opens.
    const auto offline = project_menu(served(), {true, false, false, false});
    CHECK_FALSE(item(offline, MenuCommand::Toggle).enabled);
    CHECK_FALSE(item(offline, MenuCommand::KeepRunning).enabled);
    CHECK_FALSE(item(offline, MenuCommand::Remove).enabled);
    CHECK(item(offline, MenuCommand::Settings).enabled);
    CHECK(item(offline, MenuCommand::OpenBrowser).enabled);
    // Being removed: not again, nor switched.
    const auto removing = project_menu(served(), {true, false, true, true});
    CHECK_FALSE(item(removing, MenuCommand::Remove).enabled);
    CHECK_FALSE(item(removing, MenuCommand::Toggle).enabled);
}

TEST_CASE("project menu: the Shared sites row has only what it already does") {
    SharedSites shared;
    shared.known = true;
    shared.on = true;
    shared.dir = "C:/Users/me/www";
    shared.address = "http://localhost:8080";
    auto menu = shared_menu(shared);
    using enum MenuCommand;
    CHECK(commands(menu) == std::vector<MenuCommand>{OpenBrowser, OpenFolder, None, Settings});
    CHECK(item(menu, OpenBrowser).enabled);
    CHECK(text(Lang::En, item(menu, Settings).label) == "Settings…");
    shared.state = "needs_download";
    CHECK_FALSE(item(shared_menu(shared), OpenBrowser).enabled);
    shared.state.clear();
    shared.address = "http://evil.example";
    CHECK_FALSE(item(shared_menu(shared), OpenBrowser).enabled);
    shared.dir.clear();
    CHECK_FALSE(item(shared_menu(shared), OpenFolder).enabled);
}

TEST_CASE("remove outcome: what's said about the database and the folder") {
    CHECK(remove_outcome("my-shop", false, FolderOutcome::NotAsked, {}, Lang::En).text ==
          "my-shop is removed from FloServer. Its folder is where it was.");
    CHECK(remove_outcome("my-shop", false, FolderOutcome::Recycled, {}, Lang::En).text ==
          "my-shop is removed from FloServer, and its folder is in the Recycle Bin.");
    CHECK(remove_outcome("my-shop", true, FolderOutcome::Recycled, {}, Lang::En).text ==
          "my-shop is removed from FloServer, its database is deleted and its folder is in the Recycle Bin.");
    const RemoveOutcome stayed = remove_outcome("my-shop", true, FolderOutcome::Stayed, "a file is open", Lang::En);
    CHECK(stayed.error);
    CHECK(stayed.text == "my-shop is removed from FloServer, but its folder stayed where it was: a file is open");
    const RemoveOutcome cancelled = remove_outcome("my-shop", false, FolderOutcome::Cancelled, {}, Lang::En);
    CHECK_FALSE(cancelled.error);
    CHECK(cancelled.text == "my-shop is removed from FloServer. Its folder stayed where it was.");
    CHECK_FALSE(remove_outcome("my-shop", false, FolderOutcome::NotAsked, {}, Lang::Ar).text.empty());
    // Never "in the Recycle Bin" without Windows saying so.
    CHECK(remove_outcome("my-shop", true, FolderOutcome::Deleted, {}, Lang::En).text ==
          "my-shop is removed from FloServer, and its folder is deleted (Windows couldn't put it in the Recycle Bin).");
    CHECK(remove_outcome("my-shop", false, FolderOutcome::Gone, {}, Lang::En).text ==
          "my-shop is removed from FloServer, and its folder is gone from where it was.");
}

TEST_CASE("remove question: its words, and the database box only with a database") {
    ProjectRow row = served();
    RemoveQuestion q = remove_question(row, Lang::En);
    CHECK(q.window_title == "FloServer");
    CHECK(q.title == "Remove my-shop from FloServer?");
    CHECK(q.text == "Its folder stays where it is; you can add it back any time.");
    CHECK(q.remove == "Remove");
    CHECK(q.cancel == "Cancel");
    CHECK(q.drop.empty());
    CHECK(q.trash.empty());  // the folder only when it's offered
    CHECK(q.folder.empty());
    CHECK_FALSE(row_has_database(row));
    q = remove_question(row, Lang::En, FolderOffer::Offer);
    CHECK(q.trash == "Also move the project folder to the Recycle Bin");
    CHECK(q.folder == "Folder: C:/sites/my-shop");
    CHECK(q.trash_note.empty());
    CHECK(remove_question(row, Lang::Ar, FolderOffer::Offer).folder.find(kLri + "C:/sites/my-shop" + kPdi) !=
          std::string::npos);
    // A drive with no Recycle Bin: no box, and the dialog says why.
    q = remove_question(row, Lang::En, FolderOffer::NoRecycleBin);
    CHECK(q.trash.empty());
    CHECK(q.trash_note == "This drive has no Recycle Bin, so FloServer won't delete the folder.");

    // floserver.json asks for one (the project may be off), or the service says it uses one.
    row.wants.database = "mariadb-11.4";
    CHECK(row_has_database(row));
    q = remove_question(row, Lang::En);
    CHECK(q.drop == "Also delete its database my_shop and its user (this can't be undone)");
    row.wants.database.clear();
    row.database = "mariadb-11.8";
    CHECK(row_has_database(row));

    // In Arabic and Kurdish the Latin names read left to right.
    q = remove_question(row, Lang::Ar);
    CHECK(q.title.find(kLri + "my-shop" + kPdi) != std::string::npos);
    CHECK(q.drop.find(kLri + "my_shop" + kPdi) != std::string::npos);
    CHECK(q.cancel == text(Lang::Ar, Str::WizardCancel));
    q = remove_question(row, Lang::Ku);
    CHECK(q.title.find(kLri + "my-shop" + kPdi) != std::string::npos);
    CHECK(q.title.find(kLri + "FloServer" + kPdi) != std::string::npos);
}

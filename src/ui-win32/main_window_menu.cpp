// A project row's right-click menu and removing a project from FloServer
// (approved 2 Nov). The menu opens on a right-click (the row is selected
// first), the menu key and Shift+F10; its items run what the panel's
// buttons and the switches already do (model/project_menu.h says which
// apply). Removing asks first with a task dialog in the window's language;
// projects.remove then runs off the polling path (dropping a database may
// start MariaDB), and the folder is never touched.

#include "main_window.h"

#include "main_window_layout.h"
#include "model/folder_removal.h"
#include "model/view_model.h"

#include "floserver/core/paths.h"
#include "floserver/core/process.h"

#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

namespace floserver::ui {

using namespace layout;

namespace {

std::wstring w(std::string_view s) { return win::widen(s); }

constexpr std::string_view kRemovePurpose = "remove\n";

// A command's menu id: never 0, which TrackPopupMenuEx returns for none.
UINT menu_id(MenuCommand command) { return static_cast<UINT>(command) + 1U; }

// The menu as Windows shows it; `labels` keeps the words alive meanwhile.
// `editor` is the code editor's name for its item.
win::UniqueMenu make_menu(const std::vector<MenuItem>& items, Lang lang, std::vector<std::wstring>& labels,
                          std::string_view editor = {}) {
    win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return menu;
    }
    labels.reserve(items.size());
    for (const MenuItem& item : items) {
        if (item.command == MenuCommand::None) {
            AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
            continue;
        }
        // A program's own name may hold '&': shown as it is, not as a shortcut key.
        std::wstring label;
        for (const wchar_t ch : w(menu_item_text(item, lang, editor))) {
            label += ch;
            if (ch == L'&') {
                label += L'&';
            }
        }
        labels.push_back(std::move(label));
        UINT flags = MF_STRING;
        flags |= item.enabled ? 0U : UINT{MF_GRAYED};
        flags |= item.checked ? UINT{MF_CHECKED} : 0U;
        AppendMenuW(menu.get(), flags, menu_id(item.command), labels.back().c_str());
    }
    return menu;
}

// The command picked, or None. `at` in screen coordinates.
MenuCommand track(HWND owner, HMENU menu, POINT at, bool rtl, const std::vector<MenuItem>& items) {
    // As the panel's pickers: the menu reads from the reading start.
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_TOPALIGN;
    flags |= rtl ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto id = static_cast<UINT>(TrackPopupMenuEx(menu, flags, at.x, at.y, owner, nullptr));
    for (const MenuItem& item : items) {
        if (item.command != MenuCommand::None && item.enabled && menu_id(item.command) == id) {
            return item.command;
        }
    }
    return MenuCommand::None;
}

// Plain text on the clipboard (an address: nothing secret).
bool put_text_on_clipboard(HWND owner, const std::string& value) {
    const std::wstring wide = w(value);
    if (OpenClipboard(owner) == FALSE) {
        return false;
    }
    bool done = false;
    EmptyClipboard();
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL text = GlobalAlloc(GMEM_MOVEABLE, bytes); text != nullptr) {
        if (void* to = GlobalLock(text); to != nullptr) {
            std::memcpy(to, wide.c_str(), bytes);
            GlobalUnlock(text);
            done = SetClipboardData(CF_UNICODETEXT, text) != nullptr;
        }
        if (!done) {
            GlobalFree(text);
        }
    }
    CloseClipboard();
    return done;
}

}  // namespace

void MainWindow::on_context_menu(LPARAM lparam) {
    // Only over the list: not the first-run or New project page, nor while
    // a question, a picker or another menu is open.
    if (first_run_open() || wizard_open() || asking_ || picking_folder_ || quitting_ || !shows_rows()) {
        return;
    }
    const int sx = GET_X_LPARAM(lparam);
    const int sy = GET_Y_LPARAM(lparam);
    if (sx != -1 || sy != -1) {
        // A right-click: the row under the mouse is selected first.
        POINT client{sx, sy};
        ScreenToClient(hwnd_, &client);
        const Hit hit = hit_test(client.x, client.y);
        switch (hit.part) {
            case Part::SharedRow:
            case Part::SharedSwitch:
            case Part::SharedEnd:
            case Part::SharedOpen:
            case Part::SharedGear:
                focus_ = -1;
                panel_focus_ = -1;
                shared_focused_ = true;
                shared_part_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
                show_shared_menu({sx, sy});
                return;
            case Part::Row:
            case Part::Switch:
            case Part::Address:
            case Part::Download: {
                const int row = visible_index(hit.project);
                if (row < 0) {
                    return;
                }
                // Selected, without a focus frame: it was the mouse (Windows' rule).
                focus_ = row;
                shared_focused_ = false;
                panel_focus_ = -1;
                show_focus_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
                show_project_menu(hit.project, {sx, sy});
                return;
            }
            default:
                return;
        }
    }
    // The menu key or Shift+F10: where the keys are, under it at its reading start.
    const auto under = [this](const RECT& r, int inset) {
        POINT at{r.left + inset, r.bottom};
        ClientToScreen(hwnd_, &at);  // the window is mirrored in right-to-left languages: that's the start there
        return at;
    };
    if (panel_open() && (panel_focus_ >= 0 || !panel_beside_list())) {
        const std::string project = selected_;
        show_project_menu(project, under(panel_geometry_.line2, 0));
        return;
    }
    if (shared_focused_ && width_of(shared_geometry_.row) > 0) {
        show_shared_menu(under(shared_geometry_.row, px(kRowPad)));
        return;
    }
    if (focus_ < 0 || focus_ >= static_cast<int>(visible_.size())) {
        return;
    }
    show_focus_ = true;
    ensure_visible(focus_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);  // the row where it's painted now
    const auto row = static_cast<std::size_t>(focus_);
    if (row >= geometry_.size()) {
        return;
    }
    const std::string project = geometry_[row].project;
    const RECT r = geometry_[row].row;
    show_project_menu(project, under(r, px(kRowPad)));
}

void MainWindow::show_project_menu(const std::string& project, POINT at) {
    const auto found = std::find_if(snap_.rows.begin(), snap_.rows.end(),
                                    [&](const ProjectRow& r) { return r.name == project; });
    if (found == snap_.rows.end()) {
        return;
    }
    // Copies: snapshots keep arriving while the menu is open.
    const ProjectRow row = *found;
    const ProjectMenuState state{switch_value(row), keep_value(row), snap_.link == Link::Connected,
                                 leaving(project)};
    ensure_editor();
    const auto& target = editor();
    const std::string editor_name = target ? target->name : std::string();
    const std::vector<MenuItem> items = project_menu(row, state, editor_name);
    std::vector<std::wstring> labels;
    const win::UniqueMenu menu = make_menu(items, lang_, labels, editor_name);
    if (!menu) {
        return;
    }
    hover_ = {};
    const MenuCommand command = track(hwnd_, menu.get(), at, rtl_, items);
    if (command != MenuCommand::None) {
        run_project_command(project, command);
    }
}

void MainWindow::show_shared_menu(POINT at) {
    const SharedSites shared = snap_.shared;
    const std::vector<MenuItem> items = shared_menu(shared);
    std::vector<std::wstring> labels;
    const win::UniqueMenu menu = make_menu(items, lang_, labels);
    if (!menu) {
        return;
    }
    hover_ = {};
    switch (track(hwnd_, menu.get(), at, rtl_, items)) {
        case MenuCommand::OpenBrowser:
            open_shared_address();
            break;
        case MenuCommand::OpenFolder:
            open_folder(snap_.shared.dir);
            break;
        case MenuCommand::Settings:
            open_settings_shared();
            break;
        default:
            break;
    }
}

void MainWindow::run_project_command(const std::string& project, MenuCommand command) {
    // Found again: the list may have changed while the menu was open.
    const auto found = std::find_if(snap_.rows.begin(), snap_.rows.end(),
                                    [&](const ProjectRow& r) { return r.name == project; });
    if (found == snap_.rows.end()) {
        return;
    }
    const ProjectRow row = *found;
    switch (command) {
        case MenuCommand::OpenBrowser:
            // Only a project's own local address ever reaches the shell.
            if (is_openable_address(row.address)) {
                ShellExecuteW(hwnd_, L"open", w(row.address).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        case MenuCommand::OpenFolder:
            open_folder(row.path);
            break;
        case MenuCommand::OpenTerminal:
            open_terminal(row.path);
            break;
        case MenuCommand::OpenEditor:
            open_editor(row.path);
            break;
        case MenuCommand::CopyAddress:
            if (is_openable_address(row.address)) {
                copy_address(row.address);
            }
            break;
        case MenuCommand::Toggle:
            toggle_project(project);  // by name: the search may hide it meanwhile
            break;
        case MenuCommand::KeepRunning:
            toggle_keep_running_for(project);
            break;
        case MenuCommand::Settings:
            if (const int at = visible_index(project); at >= 0) {
                focus_ = at;
            }
            shared_focused_ = false;
            select(project);
            break;
        case MenuCommand::Remove:
            ask_remove(project);
            break;
        case MenuCommand::None:
            break;
    }
}

void MainWindow::copy_address(const std::string& address) {
    const bool done = put_text_on_clipboard(hwnd_, address);
    show_notice(done ? std::string(text(lang_, Str::AddressCopied)) : fill(lang_, Str::OpenFailed, {"clipboard"}), !done);
}

void MainWindow::ask_remove(const std::string& project) {
    if (!worker_ || snap_.link != Link::Connected || leaving(project) || asking_ || quitting_) {
        return;
    }
    const auto found = std::find_if(snap_.rows.begin(), snap_.rows.end(),
                                    [&](const ProjectRow& r) { return r.name == project; });
    if (found == snap_.rows.end()) {
        return;
    }
    if (!remove_checking_.empty()) {
        return;  // one question at a time
    }
    if (found->path.empty()) {
        ask_remove_now(project, {}, FolderOffer::None);
        return;
    }
    // Whether its folder may be offered is worked out off this thread (real
    // paths, the drive's Recycle Bin: an offline share would freeze the
    // window); the question opens with the answer (on_recycled()).
    remove_checking_ = project;
    recycle_bin_.check(project, found->path, folder_guard(project));
    SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
}

void MainWindow::ask_remove_now(const std::string& project, const std::string& folder, FolderOffer offer) {
    if (!worker_ || snap_.link != Link::Connected || leaving(project) || asking_ || quitting_) {
        return;
    }
    const auto found = std::find_if(snap_.rows.begin(), snap_.rows.end(),
                                    [&](const ProjectRow& r) { return r.name == project; });
    if (found == snap_.rows.end()) {
        return;
    }
    // The Recycle Bin box only for the folder that was checked (checked again before it moves).
    const ProjectRow row = *found;
    if (row.path != folder) {
        offer = FolderOffer::None;
    }
    const bool offer_folder = offer == FolderOffer::Offer;
    const RemoveQuestion question = remove_question(row, lang_, offer);
    // The dialog runs its own message loop: as while a save question is
    // open, a quit or a new language waits for it.
    asking_ = true;
    pressed_ = {};
    hover_ = {};
    InvalidateRect(hwnd_, nullptr, FALSE);
    bool drop = false;
    bool trash = false;
    const bool remove = ask_remove_project(hwnd_, lang_, question, drop, trash);
    asking_ = false;
    first_run_after_ask();
    if (quit_after_ask_) {
        quit_after_ask_ = false;
        quit();
        return;
    }
    if (!remove || !worker_ || snap_.link != Link::Connected || leaving(project)) {
        return;
    }
    removing_[project] = Removing{drop, trash && offer_folder, row.path};
    dismiss_error();
    worker_->database_call("projects.remove", {{"name", project}, {"drop_database", drop}},
                           std::string(kRemovePurpose) + project);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::vector<MainWindow::Removed> MainWindow::on_remove_answers(std::vector<Snapshot::DbAnswer>& answers) {
    std::vector<Removed> removed;
    for (auto it = answers.begin(); it != answers.end();) {
        if (it->method != "projects.remove" || !it->purpose.starts_with(kRemovePurpose)) {
            ++it;
            continue;
        }
        const std::string project = it->purpose.substr(kRemovePurpose.size());
        const auto asked = removing_.find(project);
        const Removing job = asked != removing_.end() ? asked->second : Removing{};
        removing_.erase(project);
        // Said deleted only when the service dropped one it made.
        const bool drop = it->reply && it->reply->is_object() && it->reply->value("database_dropped", false);
        if (!it->error.empty() || !it->reply) {
            // Nothing was removed: the user can try again (on_snapshot shows it).
            show_notice(fill(lang_, Str::RemoveFailed, {project, it->error.empty() ? "no answer" : it->error}), true);
        } else {
            Removed done{project, drop, false, {}, false};
            if (job.trash) {
                // Only the folder the question showed, and only if it's still the
                // project's as the service had it when it removed it.
                const std::string had =
                    it->reply->is_object() ? it->reply->value("path", std::string()) : std::string();
                const std::string want = comparable_path(job.path);
                done.trash = !want.empty() && comparable_path(had) == want;
                done.refused = !done.trash;
                done.path = job.path;
            }
            removed.push_back(std::move(done));
        }
        it = answers.erase(it);
    }
    return removed;
}

FolderGuard MainWindow::folder_guard(const std::string& project) const {
    FolderGuard guard;
    const auto known = [](const KNOWNFOLDERID& id, std::vector<std::string>& into) {
        PWSTR found = nullptr;
        // KF_FLAG_DONT_VERIFY: a folder that isn't there (3D Objects) is still named, never made.
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &found)) && found != nullptr) {
            into.push_back(win::narrow(found));
        }
        CoTaskMemFree(found);
    };
    // The user's own folders: never moved themselves (nor one holding them);
    // what's inside them is where projects usually live.
    for (const KNOWNFOLDERID& id :
         {FOLDERID_Profile, FOLDERID_UserProfiles, FOLDERID_Public, FOLDERID_Desktop, FOLDERID_Documents,
          FOLDERID_Downloads, FOLDERID_Pictures, FOLDERID_Music, FOLDERID_Videos, FOLDERID_SavedGames, FOLDERID_Links,
          FOLDERID_Favorites, FOLDERID_Contacts, FOLDERID_SavedSearches, FOLDERID_Objects3D, FOLDERID_SkyDrive}) {
        known(id, guard.kept);
    }
    for (const char* name : {"OneDrive", "OneDriveConsumer", "OneDriveCommercial"}) {
        if (const auto dir = core::get_env(name); dir && !dir->empty()) {
            guard.kept.push_back(*dir);
        }
    }
    // Windows' own: never anything inside them either.
    for (const KNOWNFOLDERID& id :
         {FOLDERID_Windows, FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_ProgramFilesCommon,
          FOLDERID_ProgramFilesCommonX86, FOLDERID_ProgramData, FOLDERID_RoamingAppData, FOLDERID_LocalAppData,
          FOLDERID_LocalAppDataLow}) {
        known(id, guard.system_trees);
    }
    if (!snap_.projects_dir.empty()) {
        guard.kept.push_back(snap_.projects_dir);
    }
    if (!snap_.shared.dir.empty()) {
        guard.kept.push_back(snap_.shared.dir);
    }
    if (const auto data = core::data_dir()) {
        guard.kept_with_contents.push_back(core::path_to_utf8(*data));
    }
    for (const ProjectRow& r : snap_.rows) {
        if (r.name != project && !r.path.empty()) {
            guard.other_projects.push_back(r.path);
        }
    }
    return guard;
}

void MainWindow::on_recycled() {
    for (const RecycleBin::Result& r : recycle_bin_.take()) {
        const RemoveOutcome said = remove_outcome(r.project, r.database_dropped, r.outcome, r.reason, lang_);
        show_notice(said.text, said.error);
    }
    // A Quit that waited for the move.
    if (quit_after_folder_ && !recycle_bin_.busy()) {
        quit_after_folder_ = false;
        quit();
        return;
    }
    // The remove question, once it's known whether the folder can be offered.
    for (const RecycleBin::Check& c : recycle_bin_.take_checks()) {
        if (c.project == remove_checking_) {
            remove_checking_.clear();
            ask_remove_now(c.project, c.folder, c.offer);
        }
    }
}

}  // namespace floserver::ui

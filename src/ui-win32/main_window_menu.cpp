// A project row's right-click menu and removing a project from FloServer
// (approved 2 Nov). The menu opens on a right-click (the row is selected
// first), the menu key and Shift+F10; its items run what the panel's
// buttons and the switches already do (model/project_menu.h says which
// apply). Removing asks first with a task dialog in the window's language;
// projects.remove then runs off the polling path (dropping a database may
// start MariaDB), and the folder is never touched.

#include "main_window.h"

#include "main_window_layout.h"
#include "model/view_model.h"

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
    const RemoveQuestion question = remove_question(*found, lang_);
    // The dialog runs its own message loop: as while a save question is
    // open, a quit or a new language waits for it.
    asking_ = true;
    pressed_ = {};
    hover_ = {};
    InvalidateRect(hwnd_, nullptr, FALSE);
    bool drop = false;
    const bool remove = ask_remove_project(hwnd_, lang_, question, drop);
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
    removing_[project] = drop;
    change_error_.clear();
    worker_->database_call("projects.remove", {{"name", project}, {"drop_database", drop}},
                           std::string(kRemovePurpose) + project);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::vector<std::pair<std::string, bool>> MainWindow::on_remove_answers(std::vector<Snapshot::DbAnswer>& answers) {
    std::vector<std::pair<std::string, bool>> removed;
    for (auto it = answers.begin(); it != answers.end();) {
        if (it->method != "projects.remove" || !it->purpose.starts_with(kRemovePurpose)) {
            ++it;
            continue;
        }
        const std::string project = it->purpose.substr(kRemovePurpose.size());
        removing_.erase(project);
        // Said deleted only when the service dropped one it made.
        const bool drop = it->reply && it->reply->is_object() && it->reply->value("database_dropped", false);
        if (!it->error.empty() || !it->reply) {
            // Nothing was removed: the user can try again (on_snapshot shows it).
            change_error_ = fill(lang_, Str::RemoveFailed, {project, it->error.empty() ? "no answer" : it->error});
        } else {
            removed.emplace_back(project, drop);
        }
        it = answers.erase(it);
    }
    return removed;
}

}  // namespace floserver::ui

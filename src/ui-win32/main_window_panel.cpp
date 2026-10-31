// The project panel (step 5b): beside the list when the window is wide
// enough, in its place when it isn't. It shows the selected project's
// address, buttons to open it, its versions (saved in floserver.json through
// projects.use), its database with "Copy .env lines", Keep running and the
// last requests.

#include "main_window.h"

#include "main_window_layout.h"
#include "model/theme.h"
#include "model/view_model.h"

#include "floserver/core/mariadb.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_config.h"
#include "floserver/core/shim.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <string_view>
#include <utility>

namespace floserver::ui {

using namespace layout;
namespace core = floserver::core;

namespace {

constexpr int kFieldLabelH = 16;
constexpr int kFieldH = 32;
constexpr int kFieldGap = 10;
constexpr int kButtonH = 32;
constexpr int kButtonGap = 8;
constexpr int kLogLineH = 17;
constexpr UINT kFirstChoice = 100;
// The tab bar: Windows 11's underline tabs (the accent line under the shown one).
constexpr int kTabH = 36;
constexpr int kTabPadMost = 14;
constexpr int kTabPadLeast = 4;

std::wstring w(std::string_view s) { return win::widen(s); }

std::string_view tab_label(Lang lang, PanelTab tab) {
    switch (tab) {
        case PanelTab::Php:
            return text(lang, Str::FieldPhp);
        case PanelTab::Database:
            return text(lang, Str::FieldDatabase);
        case PanelTab::Log:
            return text(lang, Str::Log);
        case PanelTab::General:
            break;
    }
    return text(lang, Str::PanelTabGeneral);
}

Str action_label(int action) {
    switch (action) {
        case 0:
            return Str::OpenInBrowser;
        case 1:
            return Str::OpenFolder;
        case 2:
            return Str::OpenTerminal;
        default:
            break;
    }
    return Str::OpenInStudio;
}

// A folder the service gave, as Windows wants it; nothing when it isn't an
// existing folder (a file there would be run by the shell).
std::optional<std::wstring> existing_folder(const std::string& path) {
    if (path.empty()) {
        return std::nullopt;
    }
    std::wstring wide = win::widen(path);
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return std::nullopt;
    }
    return wide;
}

// This process's environment with `first` put at the front of Path, as a
// block for CreateProcessW.
std::wstring environment_with_path_first(const std::wstring& first) {
    std::wstring block;
    bool had_path = false;
    LPWCH env = GetEnvironmentStringsW();
    if (env != nullptr) {
        for (const wchar_t* entry = env; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
            std::wstring_view e(entry);
            if (e.size() >= 5 && _wcsnicmp(entry, L"PATH=", 5) == 0) {
                block += L"Path=" + first + L";" + std::wstring(e.substr(5));
                had_path = true;
            } else {
                block += e;
            }
            block += L'\0';
        }
        FreeEnvironmentStringsW(env);
    }
    if (!had_path) {
        block += L"Path=" + first;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

}  // namespace

// --- state -------------------------------------------------------------------

const ProjectRow* MainWindow::selected_row() const {
    if (selected_.empty()) {
        return nullptr;
    }
    for (const ProjectRow& row : snap_.rows) {
        if (row.name == selected_) {
            return &row;
        }
    }
    return nullptr;
}

bool MainWindow::panel_open() const { return shows_rows() && selected_row() != nullptr; }

bool MainWindow::panel_beside_list() const { return width_ >= px(kPanelBesideMinW); }

RECT MainWindow::panel_rect() const {
    const RECT b = banner_rect();
    const int top = b.bottom > 0 ? b.bottom + px(8) : px(kTopBarH);
    const int bottom = std::max(top, height_ - px(kFooterH) - px(kGap));
    const int left = panel_beside_list() ? width_ - px(kPad) - px(kPanelW) : px(kPad);
    return {left, top, width_ - px(kPad), bottom};
}

void MainWindow::select(const std::string& project) {
    if (project == selected_ || !leave_php_part(true, {PanelAct::Kind::Project, PanelTab::General, project})) {
        return;
    }
    forget_php_info();
    selected_ = project;
    panel_scroll_ = 0;
    // When the panel takes the list's place, the keys go to it: the hidden
    // list must never be switched by Space.
    panel_focus_ = panel_beside_list() ? -1 : 0;
    if (worker_) {
        worker_->select(project);
    }
    update_fields();
    tab_ask_declined_ = false;
    panel_shown_ = wanted_tab();
    if (panel_shown_ == PanelTab::Php) {
        open_php_part();  // the remembered tab: its php.get is asked now it's shown
    }
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::close_panel() {
    if (selected_.empty() || !leave_php_part(true, {PanelAct::Kind::Close, PanelTab::General, {}})) {
        return;
    }
    // Keyboard focus goes back to the project it came from.
    if (const int row = visible_index(selected_); row >= 0) {
        focus_ = row;
    }
    forget_php_info();
    selected_.clear();
    fields_.clear();
    panel_shown_ = PanelTab::General;
    tab_ask_declined_ = false;
    panel_focus_ = -1;
    panel_scroll_ = 0;
    if (worker_) {
        worker_->select({});
    }
    scroll_to(scroll_);
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

PanelTab MainWindow::shown_tab() const { return selected_row() != nullptr ? panel_shown_ : PanelTab::General; }

PanelTab MainWindow::wanted_tab() const {
    const ProjectRow* row = selected_row();
    return row != nullptr ? shown_panel_tab(panel_tabs(*row), panel_tab_) : PanelTab::General;
}

void MainWindow::select_tab(PanelTab tab) {
    const ProjectRow* row = selected_row();
    if (row == nullptr) {
        return;
    }
    const std::vector<PanelTab> tabs = panel_tabs(*row);
    if (std::find(tabs.begin(), tabs.end(), tab) == tabs.end()) {
        return;
    }
    if (tab == panel_shown_) {
        panel_tab_ = tab;  // the user's choice, kept for the next project
        return;
    }
    if (!leave_php_part(true, {PanelAct::Kind::Tab, tab, {}})) {
        return;
    }
    panel_tab_ = tab;
    if (panel_focus_ >= 0) {
        panel_focus_ = 0;  // the keys stay on the tab bar
    }
    sync_panel_tab();
}

void MainWindow::sync_panel_tab() {
    if (!panel_open()) {
        return;
    }
    const PanelTab want = wanted_tab();
    if (want == panel_shown_) {
        tab_ask_declined_ = false;
        if (want == PanelTab::Php && !php_open_ && !php_saving_) {
            open_php_part();  // left after a Save, and still the tab to show
        }
        return;
    }
    if (panel_shown_ == PanelTab::Php && php_open_) {
        // The PHP tab went from under the user (the project stopped using
        // PHP): asked once about changes not saved; a Save leaves once done.
        if (tab_ask_declined_ || !leave_php_part(true)) {
            tab_ask_declined_ = tab_ask_declined_ || (!php_saving_ && !asking_);
            return;
        }
    }
    tab_ask_declined_ = false;
    panel_shown_ = want;
    panel_scroll_ = 0;
    if (want == PanelTab::Php) {
        open_php_part();
    }
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::step_panel_tab(int by) {
    const ProjectRow* row = selected_row();
    if (row == nullptr) {
        return;
    }
    const std::vector<PanelTab> tabs = panel_tabs(*row);
    const auto it = std::find(tabs.begin(), tabs.end(), shown_tab());
    const int count = static_cast<int>(tabs.size());
    const int at = it == tabs.end() ? 0 : static_cast<int>(it - tabs.begin());
    select_tab(tabs[static_cast<std::size_t>(((at + by) % count + count) % count)]);
    panel_focus_ = 0;  // the tab bar
    show_focus_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// What a Save chosen on the way out of the PHP tab was for, once it's saved.
void MainWindow::run_panel_act(const PanelAct& act) {
    switch (act.kind) {
        case PanelAct::Kind::Tab:
            select_tab(act.tab);
            break;
        case PanelAct::Kind::Project:
            select(act.project);
            break;
        case PanelAct::Kind::Close:
            close_panel();
            break;
        case PanelAct::Kind::None:
            break;
    }
}

void MainWindow::update_fields() {
    const ProjectRow* row = selected_row();
    fields_ = row != nullptr ? version_fields(*row, snap_.installed, lang_) : std::vector<VersionField>{};
}

bool MainWindow::keep_value(const ProjectRow& row) const {
    const auto it = pending_keep_.find(row.name);
    return it != pending_keep_.end() ? it->second : row.keep_running;
}

// The choice shown: the user's pick while it's on its way, else the file's.
std::size_t MainWindow::field_value(const VersionField& field) const {
    const auto it = pending_use_.find(ServiceWorker::use_key(selected_, field.key));
    if (it != pending_use_.end()) {
        for (std::size_t i = 0; i < field.choices.size(); ++i) {
            if (field.choices[i].value == it->second) {
                return i;
            }
        }
    }
    return field.current;
}

std::string MainWindow::panel_button_text(PanelAction action) const {
    if (action == PanelAction::Download) {
        const ProjectRow* row = selected_row();
        return row != nullptr ? download_button_text(downloads_for(*row, snap_.installed), lang_) : std::string();
    }
    return std::string(text(lang_, action_label(static_cast<int>(action))));
}

// --- geometry ----------------------------------------------------------------

MainWindow::PanelGeometry MainWindow::panel_geometry(HDC dc) const {
    PanelGeometry g;
    const ProjectRow* row = selected_row();
    if (row == nullptr) {
        return g;
    }
    g.project = row->name;
    g.panel = panel_rect();
    const int x0 = g.panel.left + px(kPanelPad);
    const int x1 = g.panel.right - px(kPanelPad);
    int y = g.panel.top + px(kPanelPad);

    // The header: the name, the address (or the folder) and Close.
    g.close = {x1 - px(28), y - px(2), x1, y + px(26)};
    g.title = {x0, y, g.close.left - px(8), y + px(24)};
    y += px(24);
    g.line2 = {x0, y, g.close.left - px(8), y + px(20)};
    if (!row->address.empty() && is_openable_address(row->address)) {
        const int aw = Canvas::measure(dc, w(row->address), fonts_.button.get(), false);
        g.address = {x0, g.line2.top, std::min(static_cast<int>(g.line2.right), x0 + aw), g.line2.bottom};
    }
    y = g.line2.bottom + px(10);

    // The tabs, under the header: their labels, with the padding shrunk
    // before a label is cut when they don't all fit (Arabic, Kurdish).
    std::vector<PanelTab> tabs = panel_tabs(*row);
    if (std::find(tabs.begin(), tabs.end(), panel_shown_) == tabs.end()) {
        // Gone from the project while its unsaved changes are still shown.
        tabs.insert(tabs.end() - 1, panel_shown_);
    }
    std::vector<int> widths;
    widths.reserve(tabs.size());
    for (const PanelTab tab : tabs) {
        widths.push_back(Canvas::measure(dc, w(tab_label(lang_, tab)), fonts_.button_bold.get(), rtl_) + px(2));
    }
    const int pad = tab_padding(widths, x1 - x0, px(kTabPadMost), px(kTabPadLeast));
    g.tab_bar = {x0, y, x1, y + px(kTabH)};
    int x = x0;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const int tw = std::min(widths[i] + 2 * pad, x1 - x);
        g.tabs.emplace_back(RECT{x, y, x + tw, y + px(kTabH)}, tabs[i]);
        x += tw;
    }
    y = g.tab_bar.bottom;
    g.content = {g.panel.left + std::max(1, px(1)), y + std::max(1, px(1)), g.panel.right - std::max(1, px(1)),
                 std::max(y + std::max(1, px(1)), static_cast<int>(g.panel.bottom) - px(6))};

    // The shown tab's content, from its top less what's scrolled.
    const PanelTab tab = shown_tab();
    const int top = g.content.top + px(kPanelGap);
    y = top - panel_scroll_;
    if (tab == PanelTab::General) {
        // The buttons, wrapping to a new line when they don't fit.
        x = x0;
        const auto add_button = [&](PanelAction action) {
            const int bw = Canvas::measure(dc, w(panel_button_text(action)), fonts_.button.get(), rtl_) + px(24);
            if (x > x0 && x + bw > x1) {
                x = x0;
                y += px(kButtonH) + px(kButtonGap);
            }
            g.buttons.emplace_back(RECT{x, y, std::min(x1, x + bw), y + px(kButtonH)}, action);
            x += bw + px(kButtonGap);
        };
        // The row's Download button again, where the keyboard reaches it.
        std::string ignored;
        if (row_end(*row, ignored) == RowGeometry::End::Download) {
            add_button(PanelAction::Download);
        }
        if (is_openable_address(row->address)) {
            add_button(PanelAction::Browser);
        }
        add_button(PanelAction::Folder);
        add_button(PanelAction::Terminal);
        if (snap_.panel.project == row->name && snap_.panel.studio) {
            add_button(PanelAction::Studio);
        }
        y += px(kButtonH) + px(kPanelGap);

        // The project folder (the header shows the address when there's one).
        g.folder_label = {x0, y, x1, y + px(18)};
        y += px(18) + px(2);
        g.folder = {x0, y, x1, y + px(18)};
        y = g.folder.bottom + px(kPanelGap);

        g.versions = {x0, y, x1, y + px(18)};
        y += px(18) + px(4);
        const int col_w = (x1 - x0 - px(kFieldGap)) / 2;
        const int cell_h = px(kFieldLabelH) + px(4) + px(kFieldH);
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            const int col = static_cast<int>(i % 2);
            const int line = static_cast<int>(i / 2);
            const int cx = x0 + col * (col_w + px(kFieldGap));
            const int cy = y + line * (cell_h + px(kFieldGap));
            g.fields.emplace_back(RECT{cx, cy, cx + col_w, cy + px(kFieldLabelH)},
                                  RECT{cx, cy + px(kFieldLabelH) + px(4), cx + col_w, cy + cell_h});
        }
        const int lines = static_cast<int>((fields_.size() + 1) / 2);
        y += lines * cell_h + std::max(0, lines - 1) * px(kFieldGap);
        if (!php_off_note(*row, lang_).empty()) {
            g.php_note = {x0, y + px(6), x1, y + px(6) + px(32)};  // two lines at most
            y = g.php_note.bottom;
        }
        g.saved_in = {x0, y + px(6), x1, y + px(6) + px(16)};
        y = g.saved_in.bottom + px(kPanelGap);

        const int keep_mid = y + px(18);
        g.keep_switch = {x1 - px(kSwitchW), keep_mid - px(kSwitchH) / 2, x1,
                         keep_mid - px(kSwitchH) / 2 + px(kSwitchH)};
        g.keep_title = {x0, y, g.keep_switch.left - px(12), y + px(18)};
        g.keep_detail = {x0, y + px(18), g.keep_switch.left - px(12), y + px(36)};
        y += px(36) + px(kPanelGap);
    } else if (tab == PanelTab::Database) {
        g.db_box = {x0, y, x1, y + px(56)};
        const int mid = y + px(28);
        const int cw = Canvas::measure(dc, w(text(lang_, Str::CopyEnv)), fonts_.caption.get(), rtl_) + px(24);
        g.copy_env = {x1 - px(12) - cw, mid - px(14), x1 - px(12), mid + px(14)};
        g.db_title = {x0 + px(12), y + px(9), g.copy_env.left - px(12), y + px(28)};
        g.db_detail = {x0 + px(12), y + px(28), g.copy_env.left - px(12), y + px(46)};
        y = g.db_box.bottom + px(kPanelGap);
    } else if (tab == PanelTab::Log) {
        // The log fills the tab: the newest lines that fit, nothing to scroll.
        g.log_box = {x0, y, x1, std::max(y, static_cast<int>(g.content.bottom) - px(10))};
        g.content_h = height_of(g.content);
        return g;
    }
    g.content_h = y + panel_scroll_ - static_cast<int>(g.content.top);
    return g;
}

int MainWindow::panel_content_h() const {
    return shown_tab() == PanelTab::Php ? php_geometry_.content_h : panel_geometry_.content_h;
}

void MainWindow::update_panel_geometry(HDC dc) {
    if (!panel_open()) {
        panel_geometry_ = {};
        php_geometry_ = {};
        return;
    }
    for (int pass = 0; pass < 2; ++pass) {
        panel_geometry_ = panel_geometry(dc);
        php_geometry_ = php_open_ && shown_tab() == PanelTab::Php ? php_panel_geometry(dc) : PhpPanelGeometry{};
        // Kept inside the content when it got shorter (a window grown, a box gone).
        const int most = std::max(0, panel_content_h() - height_of(panel_geometry_.content));
        if (panel_scroll_ <= most) {
            break;
        }
        panel_scroll_ = most;
    }
}

void MainWindow::panel_scroll_to(int target) {
    const int most = std::max(0, panel_content_h() - height_of(panel_geometry_.content));
    const int value = std::clamp(target, 0, most);
    if (value == panel_scroll_) {
        return;
    }
    panel_scroll_ = value;
    layout();  // the PHP boxes follow
    InvalidateRect(hwnd_, nullptr, FALSE);
}

RECT MainWindow::panel_rect_of(const Hit& hit) const {
    const PanelGeometry& g = panel_geometry_;
    const PhpPanelGeometry& p = php_geometry_;
    const auto at = [&](const auto& list, int i) -> RECT {
        return i >= 0 && std::cmp_less(i, list.size()) ? list[static_cast<std::size_t>(i)] : RECT{};
    };
    switch (hit.part) {
        case Part::PanelButton:
            return hit.row >= 0 && std::cmp_less(hit.row, g.buttons.size()) ? g.buttons[static_cast<std::size_t>(hit.row)].first
                                                                           : RECT{};
        case Part::PanelField:
            if (hit.row >= 0 && std::cmp_less(hit.row, g.fields.size())) {
                const auto& [label, box] = g.fields[static_cast<std::size_t>(hit.row)];
                return {box.left, label.top, box.right, box.bottom};
            }
            return {};
        case Part::PanelCopyEnv:
            return g.db_box;
        case Part::PanelKeep:
            return {g.keep_title.left, g.keep_title.top, g.keep_switch.right, g.keep_detail.bottom};
        case Part::PanelPhpSettings:
            return p.open_settings;
        case Part::PanelPhpChange:
            return p.change;
        case Part::PanelPhpField: {
            const RECT box = at(p.boxes, hit.row);
            const RECT label = at(p.labels, hit.row);
            return {box.left, std::min(label.top, box.top), box.right, box.bottom};
        }
        case Part::PanelPhpReset:
            return p.reset;
        case Part::PanelPhpDiscard:
            return p.discard;
        case Part::PanelPhpSave:
            return p.save;
        default:
            break;
    }
    return {};
}

void MainWindow::ensure_panel_visible(const Hit& hit) {
    const RECT r = panel_rect_of(hit);
    const RECT& content = panel_geometry_.content;
    if (r.bottom <= r.top || height_of(content) <= 0) {
        return;  // the header and tabs don't scroll
    }
    const int margin = px(8);
    if (r.top < content.top + margin) {
        panel_scroll_to(panel_scroll_ - (static_cast<int>(content.top) + margin - static_cast<int>(r.top)));
    } else if (r.bottom > content.bottom - margin) {
        panel_scroll_to(panel_scroll_ + std::min(static_cast<int>(r.bottom) - static_cast<int>(content.bottom) + margin,
                                                 static_cast<int>(r.top) - static_cast<int>(content.top) - margin));
    }
}

MainWindow::Hit MainWindow::panel_hit_test(int x, int y) const {
    const PanelGeometry& g = panel_geometry_;
    const auto hit = [&](Part part, int index) { return Hit{part, index, g.project}; };
    if (contains(g.close, x, y)) {
        return hit(Part::PanelClose, -1);
    }
    if (contains(g.address, x, y)) {
        return hit(Part::PanelAddress, -1);
    }
    for (std::size_t i = 0; i < g.tabs.size(); ++i) {
        if (contains(g.tabs[i].first, x, y)) {
            return hit(Part::PanelTab, static_cast<int>(i));
        }
    }
    // What's scrolled out of the content can't be clicked.
    if (!contains(g.content, x, y)) {
        return {};
    }
    if (shown_tab() == PanelTab::Php) {
        return php_open_ ? php_panel_hit_test(x, y) : Hit{};
    }
    for (std::size_t i = 0; i < g.buttons.size(); ++i) {
        if (contains(g.buttons[i].first, x, y)) {
            return hit(Part::PanelButton, static_cast<int>(i));
        }
    }
    for (std::size_t i = 0; i < g.fields.size(); ++i) {
        if (contains(g.fields[i].second, x, y)) {
            return hit(Part::PanelField, static_cast<int>(i));
        }
    }
    if (contains(g.copy_env, x, y)) {
        return hit(Part::PanelCopyEnv, -1);
    }
    if (contains(g.keep_switch, x, y)) {
        return hit(Part::PanelKeep, -1);
    }
    return {};
}

// What Tab visits in the panel, in reading order: the tab bar (one stop; the
// arrows move between its tabs), the shown tab's controls, then Close.
std::vector<MainWindow::Hit> MainWindow::panel_focus_order() const {
    std::vector<Hit> order;
    const PanelGeometry& g = panel_geometry_;
    if (g.project.empty()) {
        return order;
    }
    for (std::size_t i = 0; i < g.tabs.size(); ++i) {
        if (g.tabs[i].second == shown_tab()) {
            order.push_back({Part::PanelTab, static_cast<int>(i), g.project});
        }
    }
    if (shown_tab() == PanelTab::Php) {
        add_php_focus_order(order);
    }
    for (std::size_t i = 0; i < g.buttons.size(); ++i) {
        order.push_back({Part::PanelButton, static_cast<int>(i), g.project});
    }
    for (std::size_t i = 0; i < g.fields.size(); ++i) {
        order.push_back({Part::PanelField, static_cast<int>(i), g.project});
    }
    if (g.copy_env.right > g.copy_env.left) {
        order.push_back({Part::PanelCopyEnv, -1, g.project});
    }
    if (g.keep_switch.right > g.keep_switch.left) {
        order.push_back({Part::PanelKeep, -1, g.project});
    }
    order.push_back({Part::PanelClose, -1, g.project});
    return order;
}

// --- input -------------------------------------------------------------------

void MainWindow::on_panel_click(const Hit& hit) {
    if (hit.project != selected_ || selected_row() == nullptr) {
        return;  // the panel changed under the click
    }
    if (is_php_part(hit.part)) {
        on_php_panel_click(hit);
        return;
    }
    const PanelGeometry& g = panel_geometry_;
    switch (hit.part) {
        case Part::PanelClose:
            close_panel();
            break;
        case Part::PanelAddress:
            run_panel_action(PanelAction::Browser);
            break;
        case Part::PanelTab:
            if (hit.row >= 0 && std::cmp_less(hit.row, g.tabs.size())) {
                select_tab(g.tabs[static_cast<std::size_t>(hit.row)].second);
            }
            break;
        case Part::PanelButton:
            if (hit.row >= 0 && std::cmp_less(hit.row, g.buttons.size())) {
                run_panel_action(g.buttons[static_cast<std::size_t>(hit.row)].second);
            }
            break;
        case Part::PanelField:
            if (hit.row >= 0) {
                show_field_menu(static_cast<std::size_t>(hit.row));
            }
            break;
        case Part::PanelCopyEnv:
            if (worker_ && snap_.link == Link::Connected) {
                change_error_.clear();
                worker_->copy_env(selected_);
            }
            break;
        case Part::PanelKeep:
            toggle_keep_running();
            break;
        default:
            break;
    }
}

void MainWindow::on_panel_key(WPARAM key) {
    // The panel was just opened or changed tab and not painted yet: lay it out first.
    if (panel_geometry_.project != selected_ || (shown_tab() == PanelTab::Php && php_geometry_.project != selected_)) {
        UpdateWindow(hwnd_);
    }
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const ProjectRow* row = selected_row();
    const auto step_tab = [this](int by) { step_panel_tab(by); };
    // Ctrl+Tab and Ctrl+Shift+Tab: the next or previous tab, from anywhere in the panel.
    if (key == VK_TAB && ctrl) {
        step_tab(shift ? -1 : 1);
        return;
    }
    const std::vector<Hit> order = panel_focus_order();
    const int count = static_cast<int>(order.size());
    if (count == 0) {
        panel_focus_ = -1;
        return;
    }
    panel_focus_ = std::clamp(panel_focus_, 0, count - 1);
    const Hit& current = order[static_cast<std::size_t>(panel_focus_)];
    switch (key) {
        case VK_TAB:
            if (shift && panel_focus_ == 0) {
                panel_focus_ = -1;  // back to the list, or the search box when the list is hidden
                if (!panel_beside_list()) {
                    SetFocus(search_);
                    return;
                }
            } else if (!shift && panel_focus_ == count - 1) {
                panel_focus_ = -1;
                SetFocus(search_);
                return;
            } else {
                panel_focus_ += shift ? -1 : 1;
                const Hit& next = order[static_cast<std::size_t>(panel_focus_)];
                ensure_panel_visible(next);
                // A box takes the keys itself.
                if (next.part == Part::PanelPhpField && next.row >= 0) {
                    if (FieldEdit* edit = php_edit_of(static_cast<std::size_t>(next.row));
                        edit != nullptr && IsWindowVisible(edit->hwnd()) != FALSE) {
                        SetFocus(edit->hwnd());
                        SendMessageW(edit->hwnd(), EM_SETSEL, 0, -1);
                    }
                }
            }
            break;
        case VK_LEFT:
        case VK_RIGHT:
            // On the tab bar the arrows move between tabs, the way they look
            // (the window is mirrored in right-to-left languages).
            if (current.part == Part::PanelTab) {
                const bool forward = (key == VK_RIGHT) != rtl_;
                step_tab(forward ? 1 : -1);
            }
            return;
        case VK_HOME:
        case VK_END:
            if (current.part == Part::PanelTab && row != nullptr) {
                const std::vector<PanelTab> tabs = panel_tabs(*row);
                select_tab(key == VK_HOME ? tabs.front() : tabs.back());
                panel_focus_ = 0;
                show_focus_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                panel_scroll_to(key == VK_HOME ? 0 : panel_content_h());
            }
            return;
        case VK_NEXT:
        case VK_PRIOR: {
            const int page = std::max(px(40), height_of(panel_geometry_.content) - px(40));
            panel_scroll_to(panel_scroll_ + (key == VK_NEXT ? page : -page));
            return;
        }
        case VK_RETURN:
        case VK_SPACE:
            on_panel_click(current);
            return;
        case VK_ESCAPE:
            close_panel();
            return;
        case VK_F5:
            if (worker_) {
                worker_->refresh();
            }
            return;
        default:
            return;
    }
    show_focus_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::run_panel_action(PanelAction action) {
    const ProjectRow* row = selected_row();
    if (row == nullptr) {
        return;
    }
    switch (action) {
        case PanelAction::Browser:
            // Only a project's own local address ever reaches the shell.
            if (is_openable_address(row->address)) {
                ShellExecuteW(hwnd_, L"open", w(row->address).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        case PanelAction::Folder:
            open_folder(row->path);
            break;
        case PanelAction::Terminal:
            open_terminal(row->path);
            break;
        case PanelAction::Studio:
            if (worker_) {
                worker_->open_in_studio(row->name);
            }
            break;
        case PanelAction::Download:
            start_downloads_for(row->name);
            break;
    }
}

void MainWindow::open_folder(const std::string& path) {
    const auto folder = existing_folder(path);
    // "explore" opens folders only, never runs a file.
    const auto result = folder ? reinterpret_cast<INT_PTR>(
                                     ShellExecuteW(hwnd_, L"explore", folder->c_str(), nullptr, nullptr, SW_SHOWNORMAL))
                               : 0;
    if (result <= 32) {
        show_notice(fill(lang_, Str::OpenFailed, {path}), true);
    }
}

// Windows Terminal when it's there, else Command Prompt, in the project's
// folder, with FloServer's shims first on Path (D10) so php, node, composer
// and python are the project's versions.
void MainWindow::open_terminal(const std::string& path) {
    const auto folder = existing_folder(path);
    if (!folder) {
        show_notice(fill(lang_, Str::OpenFailed, {path}), true);
        return;
    }
    std::wstring environment;
    if (const auto data = core::data_dir()) {
        const std::filesystem::path shims = core::shims_dir(*data);
        std::error_code ec;
        if (std::filesystem::is_directory(shims, ec)) {
            environment = environment_with_path_first(shims.wstring());
        }
    }
    std::wstring program;
    std::wstring command;
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    // Windows Terminal's own alias only, never a wt.exe found on the search
    // path (the current folder and Path come first there).
    std::wstring terminal;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local != nullptr) {
        terminal = std::wstring(local) + L"\\Microsoft\\WindowsApps\\wt.exe";
    }
    CoTaskMemFree(local);
    if (!terminal.empty() && GetFileAttributesW(terminal.c_str()) != INVALID_FILE_ATTRIBUTES) {
        program = terminal;
        std::wstring dir;
        for (const wchar_t ch : *folder) {
            // wt reads ';' as the start of another command, even inside quotes.
            if (ch == L';') {
                dir += L'\\';
            }
            dir += ch;
        }
        // A trailing backslash would escape the closing quote.
        if (!dir.empty() && dir.back() == L'\\') {
            dir += L'.';
        }
        // A new window, so it starts with this environment (the shims first).
        command = L"wt.exe -w new -d \"" + dir + L"\"";
    } else {
        wchar_t system[MAX_PATH] = {};
        const UINT n = GetSystemDirectoryW(system, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            show_notice(fill(lang_, Str::OpenFailed, {path}), true);
            return;
        }
        program = std::wstring(system) + L"\\cmd.exe";
        command = L"cmd.exe";
        flags |= CREATE_NEW_CONSOLE;
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, flags,
                       environment.empty() ? nullptr : environment.data(), folder->c_str(), &si, &pi) == FALSE) {
        show_notice(fill(lang_, Str::OpenFailed, {path}), true);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

// New project, for now: Windows' folder picker, then projects.add. The
// templates screen comes later in step 5c; "Add an existing folder…" is a
// button on it in the mockup.
void MainWindow::add_existing_folder() {
    if (!worker_ || snap_.link != Link::Connected || picking_folder_) {
        return;
    }
    // The dialog runs its own message loop: the button mustn't stay pressed
    // under it, and a second picker mustn't open.
    picking_folder_ = true;
    pressed_ = {};
    hover_ = {};
    InvalidateRect(hwnd_, nullptr, FALSE);
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
        dialog == nullptr) {
        picking_folder_ = false;
        show_notice(fill(lang_, Str::OpenFailed, {"folder picker"}), true);
        return;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    const std::wstring title = w(text(lang_, Str::AddExistingFolder));
    dialog->SetTitle(title.c_str());
    std::string path;
    bool failed = false;
    const HRESULT shown = dialog->Show(hwnd_);
    if (SUCCEEDED(shown)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR chosen = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) && chosen != nullptr) {
                path = win::narrow(chosen);
                CoTaskMemFree(chosen);
            }
            item->Release();
        }
        failed = path.empty();
    } else {
        failed = shown != HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    dialog->Release();
    picking_folder_ = false;
    if (failed) {
        show_notice(fill(lang_, Str::OpenFailed, {"folder picker"}), true);
    } else if (!path.empty() && worker_) {
        change_error_.clear();
        worker_->add_folder(path);
    }
}

void MainWindow::show_field_menu(std::size_t index) {
    if (index >= fields_.size() || index >= panel_geometry_.fields.size() || !worker_ ||
        snap_.link != Link::Connected) {
        return;
    }
    // Copies: snapshots keep arriving while the menu is open, and each one
    // rebuilds fields_ (or closes the panel).
    const VersionField field = fields_[index];
    const std::string project = selected_;
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    const std::size_t shown = field_value(field);
    std::vector<std::wstring> labels;
    labels.reserve(field.choices.size());
    for (std::size_t i = 0; i < field.choices.size(); ++i) {
        labels.push_back(w(field.choices[i].label));
        AppendMenuW(menu.get(), MF_STRING | (i == shown ? UINT{MF_CHECKED} : 0U), kFirstChoice + static_cast<UINT>(i),
                    labels.back().c_str());
    }
    // Under the box, at its reading start. The window is mirrored in
    // right-to-left languages, so its client x is already the logical x and
    // ClientToScreen lands on the box's right edge there.
    const RECT box = panel_geometry_.fields[index].second;
    POINT at{box.left, box.bottom};
    ClientToScreen(hwnd_, &at);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
    flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    if (command < kFirstChoice || command - kFirstChoice >= field.choices.size()) {
        return;
    }
    const std::size_t picked = command - kFirstChoice;
    if (picked == shown || selected_ != project || !worker_) {
        return;
    }
    const std::string key = ServiceWorker::use_key(project, field.key);
    pending_use_[key] = field.choices[picked].value;
    ++outstanding_[key];
    change_error_.clear();
    worker_->use(project, field.key, field.choices[picked].value);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::toggle_keep_running() {
    const ProjectRow* row = selected_row();
    if (row == nullptr || !worker_ || snap_.link != Link::Connected) {
        return;
    }
    const bool value = !keep_value(*row);
    pending_keep_[row->name] = value;
    ++outstanding_[ServiceWorker::keep_key(row->name)];
    change_error_.clear();
    worker_->set_keep_running(row->name, value);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- painting ----------------------------------------------------------------

void MainWindow::paint_switch(Canvas& c, const RECT& r, bool on, bool hover) const {
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    // 40 x 20, a 10 px knob 4 px from the edge (Fluent).
    const float radius = static_cast<float>(height_of(r)) / 2.0F;
    const float knob = pxf(hover ? 6.0F : 5.0F);
    const float cy = static_cast<float>(r.top) + radius;
    if (on) {
        c.round_rect(r, radius, p.accent);
        c.ellipse(static_cast<float>(r.right) - pxf(9), cy, knob, p.switch_on_knob);
    } else {
        c.round_rect(r, radius, p.switch_off_fill, p.switch_off_border, border);
        c.ellipse(static_cast<float>(r.left) + pxf(9), cy, knob, p.switch_off_knob);
    }
}

void MainWindow::paint_button(Canvas& c, const RECT& r, bool hover, bool focused) const {
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    c.round_rect(r, pxf(4), hover ? p.row_hover : p.button, p.button_border, border);
    if (focused) {
        RECT ring = r;
        InflateRect(&ring, px(2), px(2));
        c.round_outline(ring, pxf(6), p.focus, pxf(2));
    }
}

void MainWindow::paint_panel(Canvas& c, HDC dc, bool text_pass) const {
    const PanelGeometry& g = panel_geometry_;
    const ProjectRow* row = selected_row();
    if (row == nullptr || g.project != row->name || height_of(g.panel) <= 0) {
        return;
    }
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    const bool has_focus = GetFocus() == hwnd_ && show_focus_;
    const std::vector<Hit> order = panel_focus_order();
    const auto focused = [&](Part part, int index) {
        return has_focus && panel_focus_ >= 0 && std::cmp_less(panel_focus_, order.size()) &&
               order[static_cast<std::size_t>(panel_focus_)].part == part &&
               order[static_cast<std::size_t>(panel_focus_)].row == index;
    };
    const auto hovered = [&](Part part, int index) { return hover_.part == part && hover_.row == index; };
    const PanelTab shown = shown_tab();

    if (!text_pass) {
        c.round_rect(g.panel, pxf(8), p.card, p.card_border, border);
        c.clip(g.panel);
        if (hovered(Part::PanelClose, -1) || focused(Part::PanelClose, -1)) {
            c.round_rect(g.close, pxf(4), p.row_hover);
        }
        if (focused(Part::PanelClose, -1)) {
            c.round_outline(g.close, pxf(4), p.focus, pxf(2));
        }
        // The close mark: an X, 10 px.
        const float cx = static_cast<float>(g.close.left + g.close.right) / 2.0F;
        const float cy = static_cast<float>(g.close.top + g.close.bottom) / 2.0F;
        const float k = pxf(5);
        c.line(cx - k, cy - k, cx + k, cy + k, p.text, pxf(1.3F));
        c.line(cx - k, cy + k, cx + k, cy - k, p.text, pxf(1.3F));

        // The tabs: a hover fill, and the shown one's accent line under its
        // label (Windows 11's tabs); a line under them all.
        c.fill({g.panel.left + std::max(1, px(1)), g.tab_bar.bottom, g.panel.right - std::max(1, px(1)),
                g.tab_bar.bottom + std::max(1, px(1))},
               p.divider);
        for (std::size_t i = 0; i < g.tabs.size(); ++i) {
            const auto& [r, tab] = g.tabs[i];
            const int index = static_cast<int>(i);
            RECT inner = r;
            InflateRect(&inner, 0, -px(4));
            if (tab != shown && hovered(Part::PanelTab, index)) {
                c.round_rect(inner, pxf(4), p.row_hover);
            }
            if (tab == shown) {
                const int line_w = std::max(px(16), width_of(r) - 2 * px(10));
                const int lx = (r.left + r.right - line_w) / 2;
                c.round_rect({lx, r.bottom - px(3), lx + line_w, r.bottom}, pxf(1.5F), p.accent);
            }
            if (focused(Part::PanelTab, index)) {
                c.round_outline(inner, pxf(4), p.focus, pxf(2));
            }
        }

        // The content scrolls under the tabs.
        c.clip(g.content);
        if (shown == PanelTab::Php) {
            paint_php_panel(c, false);
        }
        for (std::size_t i = 0; i < g.buttons.size(); ++i) {
            const int index = static_cast<int>(i);
            paint_button(c, g.buttons[i].first, hovered(Part::PanelButton, index), focused(Part::PanelButton, index));
        }
        for (std::size_t i = 0; i < g.fields.size(); ++i) {
            const int index = static_cast<int>(i);
            const RECT& box = g.fields[i].second;
            c.round_rect(box, pxf(4), hovered(Part::PanelField, index) ? p.row_hover : p.input, p.input_border, border);
            if (focused(Part::PanelField, index)) {
                RECT ring = box;
                InflateRect(&ring, px(2), px(2));
                c.round_outline(ring, pxf(6), p.focus, pxf(2));
            }
            // The chevron, at the box's end.
            const float vx = static_cast<float>(box.right) - pxf(14);
            const float vy = static_cast<float>(box.top + box.bottom) / 2.0F;
            c.line(vx - pxf(4), vy - pxf(2), vx, vy + pxf(2), p.text_subtle, pxf(1.3F));
            c.line(vx, vy + pxf(2), vx + pxf(4), vy - pxf(2), p.text_subtle, pxf(1.3F));
        }
        if (g.db_box.right > g.db_box.left) {
            c.round_rect(g.db_box, pxf(6), p.panel_box);
            paint_button(c, g.copy_env, hovered(Part::PanelCopyEnv, -1), focused(Part::PanelCopyEnv, -1));
        }
        if (g.keep_switch.right > g.keep_switch.left) {
            paint_switch(c, g.keep_switch, keep_value(*row), hovered(Part::PanelKeep, -1));
            if (focused(Part::PanelKeep, -1)) {
                RECT ring = g.keep_switch;
                InflateRect(&ring, px(3), px(3));
                c.round_outline(ring, static_cast<float>(height_of(ring)) / 2.0F, p.focus, pxf(2));
            }
        }
        if (height_of(g.log_box) > 0) {
            c.round_rect(g.log_box, pxf(6), p.log);
        }
        // A thin scroll indicator, as in Settings, when the tab is taller than its room.
        const int content_h = panel_content_h();
        const int view = height_of(g.content);
        if (content_h > view && view > 0) {
            const int thumb = std::max(px(24), view * view / content_h);
            const int top = g.content.top + (view - thumb) * panel_scroll_ / (content_h - view);
            c.round_rect({g.content.right - px(6), top + px(2), g.content.right - px(2), top + thumb - px(2)}, pxf(2),
                         p.switch_off_border);
        }
        c.unclip();
        return;
    }

    c.clip_text(g.panel);
    c.text(g.title, w(row->name), fonts_.panel_title.get(), p.text, Align::Start, true);
    if (g.address.right > g.address.left) {
        c.text(g.line2, w(row->address), fonts_.button.get(), p.link, Align::Start, true);
    } else {
        c.text(g.line2, w(row->path), fonts_.button.get(), p.text_subtle, Align::Start, true);
    }
    for (const auto& [r, tab] : g.tabs) {
        RECT label = r;
        label.bottom -= px(2);
        c.text(label, w(tab_label(lang_, tab)), tab == shown ? fonts_.button_bold.get() : fonts_.button.get(),
               tab == shown ? p.text : p.text_subtle, Align::Center, false);
    }

    c.clip_text(g.content);
    if (shown == PanelTab::Php) {
        paint_php_panel(c, true);
    }
    for (const auto& [rect, action] : g.buttons) {
        c.text(rect, w(panel_button_text(action)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (g.folder.right > g.folder.left) {
        c.text(g.folder_label, w(text(lang_, Str::PanelFolder)), fonts_.button_bold.get(), p.text, Align::Start, false);
        c.text(g.folder, w(row->path), fonts_.caption.get(), p.text_subtle, Align::Start, true);
    }
    if (g.versions.right > g.versions.left) {
        c.text(g.versions, w(text(lang_, Str::Versions)), fonts_.button_bold.get(), p.text, Align::Start, false);
    }
    for (std::size_t i = 0; i < g.fields.size() && i < fields_.size(); ++i) {
        const VersionField& field = fields_[i];
        c.text(g.fields[i].first, w(field.label), fonts_.caption.get(), p.text_subtle, Align::Start, false);
        RECT inner = g.fields[i].second;
        inner.left += px(8);
        inner.right -= px(26);
        const std::size_t value = field_value(field);
        if (value < field.choices.size()) {
            c.text(inner, w(field.choices[value].label), fonts_.button.get(), p.text, Align::Start, false);
        }
    }
    if (g.php_note.right > g.php_note.left) {
        c.paragraph(g.php_note, w(php_off_note(*row, lang_)), fonts_.caption.get(), p.text_subtle);
    }
    if (g.saved_in.right > g.saved_in.left) {
        c.text(g.saved_in, w(fill(lang_, Str::SavedIn, {core::kProjectConfigFileName})), fonts_.caption.get(),
               p.text_subtle, Align::Start, false);
    }

    if (g.db_box.right > g.db_box.left) {
        const std::string database = !row->database.empty() ? row->database : row->wants.database;
        const std::string name = core::project_database_name(row->name);
        const std::uint16_t port = database_port(snap_.databases, database);
        const std::string host = port != 0 ? "127.0.0.1:" + std::to_string(port) : std::string("127.0.0.1");
        c.text(g.db_title, w(fill(lang_, Str::DatabaseTitle, {name})), fonts_.button_bold.get(), p.text, Align::Start,
               false);
        c.text(g.db_detail, w(fill(lang_, Str::DatabaseDetail, {host, name})), fonts_.caption.get(), p.text_subtle,
               Align::Start, false);
        c.text(g.copy_env, w(text(lang_, Str::CopyEnv)), fonts_.caption.get(), p.text, Align::Center, false);
    }

    if (g.keep_switch.right > g.keep_switch.left) {
        const bool keep = keep_value(*row);
        c.text(g.keep_title, w(text(lang_, Str::KeepRunning)), fonts_.button_bold.get(), p.text, Align::Start, false);
        c.text(g.keep_detail,
               w(keep ? std::string(text(lang_, Str::KeepRunningOn))
                      : fill(lang_, Str::KeepRunningOff,
                             {std::to_string(snap_.footer.idle_minutes != 0 ? snap_.footer.idle_minutes : 10)})),
               fonts_.caption.get(), p.text_subtle, Align::Start, false);
    }

    if (height_of(g.log_box) > px(kLogLineH)) {
        RECT inner = g.log_box;
        InflateRect(&inner, -px(10), -px(8));
        const bool ours = snap_.panel.project == row->name && snap_.panel.has_log;
        const std::vector<LogLine> empty;
        const std::vector<LogLine>& lines = ours ? snap_.panel.log : empty;
        if (lines.empty()) {
            if (ours) {
                c.text({inner.left, inner.top, inner.right, inner.top + px(kLogLineH)}, w(text(lang_, Str::LogEmpty)),
                       fonts_.caption.get(), p.log_subtle, Align::Start, false);
            }
        } else {
            // The newest at the bottom; as many as fit.
            const int fit = std::max(1, height_of(inner) / px(kLogLineH));
            const std::size_t first =
                lines.size() > static_cast<std::size_t>(fit) ? lines.size() - static_cast<std::size_t>(fit) : 0;
            c.clip_text(inner);
            int ly = inner.top;
            for (std::size_t i = first; i < lines.size(); ++i) {
                // Log lines read left to right in every language.
                const RECT line{inner.left, ly, inner.right, ly + px(kLogLineH)};
                c.text(line, w(log_text(lines[i])), fonts_.mono.get(),
                       lines[i].status >= 500 ? palette(true).problem.text : p.log_text,
                       rtl_ ? Align::End : Align::Start, true);
                ly += px(kLogLineH);
            }
        }
    }
    c.unclip_text();
    (void)dc;
}

}  // namespace floserver::ui

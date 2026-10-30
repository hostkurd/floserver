// Shared sites (D19): one row pinned under the project list (the list
// scrolls above it) and its settings page, shown over the list the way New
// project is. The window asks shared.get without the folders, so nothing
// reads the www folder in the background; there's no folder list here, the
// list is the page at localhost/.

#include "main_window.h"

#include "main_window_layout.h"
#include "model/theme.h"
#include "model/view_model.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <utility>

namespace floserver::ui {

using namespace layout;

namespace {

constexpr int kSharedButtonH = 28;
constexpr int kPagePad = 28;
constexpr int kPageFooterH = 64;
constexpr int kPageButtonH = 32;
constexpr UINT kFirstChoice = 100;

std::wstring w(std::string_view s) { return win::widen(s); }

std::string shared_key() { return ServiceWorker::kSharedKey; }

}  // namespace

// --- state -------------------------------------------------------------------

bool MainWindow::shows_shared() const { return shows_rows() && snap_.shared.known; }

bool MainWindow::shared_switch_value() const {
    const auto it = pending_.find(shared_key());
    return it != pending_.end() ? it->second : snap_.shared.on;
}

void MainWindow::toggle_shared() {
    if (!shows_shared() || snap_.link != Link::Connected || !worker_) {
        return;
    }
    const bool value = !shared_switch_value();
    const std::string key = ServiceWorker::on_key(shared_key());
    pending_[shared_key()] = value;
    ++outstanding_[key];
    change_error_.clear();
    worker_->set_shared({{"on", value}}, key);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::start_shared_downloads() {
    if (snap_.link != Link::Connected || !worker_ || installing_.contains(shared_key())) {
        return;
    }
    const std::vector<Download> downloads = shared_downloads(snap_.shared, snap_.installed);
    if (downloads.empty()) {
        return;
    }
    installing_[shared_key()] = true;
    ++outstanding_[ServiceWorker::install_key(shared_key())];
    change_error_.clear();
    worker_->install(shared_key(), downloads);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::open_shared_address() {
    // Only shared sites' own address ever reaches the shell.
    std::string address = snap_.shared.address;
    if (!is_shared_address(address)) {
        return;
    }
    if (!address.ends_with("/")) {
        address += '/';
    }
    ShellExecuteW(hwnd_, L"open", w(address).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::run_shared_part(Part part) {
    switch (part) {
        case Part::SharedSwitch:
            toggle_shared();
            break;
        case Part::SharedEnd:
            if (shared_geometry_.end == SharedGeometry::End::Download) {
                start_shared_downloads();
            } else if (shared_geometry_.end == SharedGeometry::End::Address) {
                open_shared_address();
            }
            break;
        case Part::SharedOpen:
            open_folder(snap_.shared.dir);
            break;
        case Part::SharedGear:
            open_www();
            break;
        default:
            break;
    }
}

// --- the row: geometry ------------------------------------------------------------

MainWindow::SharedGeometry MainWindow::shared_geometry(HDC dc) const {
    SharedGeometry g;
    if (!shows_shared()) {
        return g;
    }
    RECT inner = card_rect();
    const int border = std::max(1, px(1));
    InflateRect(&inner, -border, -border);
    if (width_of(inner) <= 0 || height_of(inner) < px(kRowH)) {
        return g;  // the panel has the list's place, or the window is too short
    }
    const SharedRow row = shared_row(snap_.shared, snap_.installed, lang_);
    g.row = {inner.left, inner.bottom - px(kRowH), inner.right, inner.bottom};
    const int mid = (g.row.top + g.row.bottom) / 2;
    g.toggle = {g.row.left + px(kRowPad), mid - px(kSwitchH) / 2, g.row.left + px(kRowPad) + px(kSwitchW),
                mid - px(kSwitchH) / 2 + px(kSwitchH)};
    g.icon = {g.toggle.right + px(kRowPad), mid - px(8), g.toggle.right + px(kRowPad) + px(20), mid + px(8)};
    const int text_start = g.icon.right + px(12);

    // From the end: the gear, Open folder, then the address or a Download button.
    const auto button_at = [&](int right, const std::string& label) {
        const int bw = Canvas::measure(dc, w(label), fonts_.button.get(), rtl_) + px(24);
        return RECT{right - bw, mid - px(kSharedButtonH) / 2, right, mid - px(kSharedButtonH) / 2 + px(kSharedButtonH)};
    };
    const int end = g.row.right - px(kRowPad);
    g.gear = {end - px(kSharedButtonH), mid - px(kSharedButtonH) / 2, end, mid - px(kSharedButtonH) / 2 + px(kSharedButtonH)};
    g.open = button_at(g.gear.left - px(8), std::string(text(lang_, Str::SharedOpenFolder)));
    int right = g.open.left - px(8);

    const std::string key = shared_key();
    if (const auto it = snap_.downloads.find(key); it != snap_.downloads.end() && !it->second.empty()) {
        g.end = SharedGeometry::End::Progress;
        g.end_text = it->second;
    } else if (installing_.contains(key)) {
        const std::vector<Download> downloads = shared_downloads(snap_.shared, snap_.installed);
        const std::string first =
            downloads.empty() ? std::string()
                              : runtime_label(downloads.front().name) +
                                    (downloads.front().version.empty() ? "" : " " + downloads.front().version);
        g.end = SharedGeometry::End::Progress;
        g.end_text = fill(lang_, Str::Installing, {first});
    } else if (row.state == RowState::NeedsDownload && snap_.link == Link::Connected && snap_.has_installed &&
               !shared_downloads(snap_.shared, snap_.installed).empty()) {
        // Not before the first runtimes.list answer: nginx would be counted as missing.
        g.end = SharedGeometry::End::Download;
        g.end_text = download_button_text(shared_downloads(snap_.shared, snap_.installed), lang_);
    } else if (!row.address.empty()) {
        g.end = SharedGeometry::End::Address;
        g.end_text = row.address;
    }
    if (g.end != SharedGeometry::End::None) {
        g.end_rect = button_at(right, g.end_text);
        right = g.end_rect.left - px(kRowPad);
    }

    // The chips, while the text keeps room (as a project row's).
    std::vector<int> chip_w;
    chip_w.reserve(row.chips.size());
    for (const std::string& chip : row.chips) {
        chip_w.push_back(Canvas::measure(dc, w(chip), fonts_.caption.get(), false) + px(16));
    }
    const auto chips_width = [&](std::size_t n) {
        int total = 0;
        for (std::size_t k = 0; k < n; ++k) {
            total += chip_w[k] + px(8);
        }
        return total;
    };
    std::size_t shown = chip_w.size();
    while (shown > 0 && right - chips_width(shown) - text_start < px(kMinTextW)) {
        --shown;
    }
    int x = right - chips_width(shown) + px(8);
    for (std::size_t k = 0; k < shown; ++k) {
        g.chips.push_back({x, mid - px(kChipH) / 2, x + chip_w[k], mid - px(kChipH) / 2 + px(kChipH)});
        x += chip_w[k] + px(8);
    }
    const int text_end = std::max(text_start, (shown > 0 ? static_cast<int>(g.chips.front().left) : right) - px(kRowPad));

    // The title, then its state pill.
    const int name_top = mid - px(19);
    const int title_w = Canvas::measure(dc, w(text(lang_, Str::SharedSites)), fonts_.body_bold.get(), rtl_) + px(2);
    g.name = {text_start, name_top, std::min(text_end, text_start + title_w), name_top + px(20)};
    const std::string pill(text(lang_, state_label(row.state)));
    const int pill_w = Canvas::measure(dc, w(pill), fonts_.small_bold.get(), rtl_) + px(16);
    const int pill_left = g.name.right + px(8);
    if (pill_left + pill_w <= text_end) {
        g.state = {pill_left, name_top + px(1), pill_left + pill_w, name_top + px(19)};
    }
    g.line2 = {text_start, name_top + px(20), text_end, name_top + px(38)};
    return g;
}

MainWindow::Hit MainWindow::shared_hit_test(int x, int y) const {
    const SharedGeometry& g = shared_geometry_;
    if (!contains(g.row, x, y)) {
        return {};
    }
    if (contains(g.toggle, x, y)) {
        return {Part::SharedSwitch, -1, {}};
    }
    if (g.end != SharedGeometry::End::None && g.end != SharedGeometry::End::Progress && contains(g.end_rect, x, y)) {
        return {Part::SharedEnd, -1, {}};
    }
    if (contains(g.open, x, y)) {
        return {Part::SharedOpen, -1, {}};
    }
    if (contains(g.gear, x, y)) {
        return {Part::SharedGear, -1, {}};
    }
    return {Part::SharedRow, -1, {}};
}

// What the arrow keys and Tab visit in the row, in reading order.
std::vector<MainWindow::Part> MainWindow::shared_focus_order() const {
    std::vector<Part> order{Part::SharedSwitch};
    if (shared_geometry_.end == SharedGeometry::End::Address || shared_geometry_.end == SharedGeometry::End::Download) {
        order.push_back(Part::SharedEnd);
    }
    order.push_back(Part::SharedOpen);
    order.push_back(Part::SharedGear);
    return order;
}

void MainWindow::on_shared_key(WPARAM key) {
    const std::vector<Part> order = shared_focus_order();
    const int count = static_cast<int>(order.size());
    shared_part_ = std::clamp(shared_part_, 0, count - 1);
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (key) {
        case VK_UP:
            // Back to the last project.
            shared_focused_ = false;
            focus_ = static_cast<int>(visible_.size()) - 1;
            ensure_visible(focus_);
            break;
        case VK_LEFT:
        case VK_RIGHT: {
            // Right moves on in left-to-right reading, back in right-to-left.
            const int step = (key == VK_RIGHT) != rtl_ ? 1 : -1;
            shared_part_ = std::clamp(shared_part_ + step, 0, count - 1);
            break;
        }
        case VK_TAB:
            if (!shift && shared_part_ == count - 1) {
                shared_focused_ = false;
                SetFocus(search_);
                return;
            }
            shared_part_ = std::clamp(shared_part_ + (shift ? -1 : 1), 0, count - 1);
            break;
        case VK_SPACE:
            toggle_shared();
            break;
        case VK_RETURN:
            run_shared_part(order[static_cast<std::size_t>(shared_part_)]);
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

// --- the row: painting -----------------------------------------------------------

void MainWindow::paint_shared_row(Canvas& c, HDC /*dc*/, bool text_pass) const {
    const SharedGeometry& g = shared_geometry_;
    if (width_of(g.row) <= 0) {
        return;
    }
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    const SharedRow row = shared_row(snap_.shared, snap_.installed, lang_);
    const StateColors& sc = state_colors(p, row.state);
    const bool keys = show_focus_ && GetFocus() == hwnd_ && shared_focused_;
    const std::vector<Part> order = shared_focus_order();
    const auto focused = [&](Part part) {
        return keys && shared_part_ >= 0 && std::cmp_less(shared_part_, order.size()) &&
               order[static_cast<std::size_t>(shared_part_)] == part;
    };
    const auto hovered = [&](Part part) { return hover_.part == part; };

    if (!text_pass) {
        c.clip(g.row);
        const bool row_hover = hover_.part == Part::SharedRow || hover_.part == Part::SharedSwitch ||
                               hover_.part == Part::SharedEnd || hover_.part == Part::SharedOpen ||
                               hover_.part == Part::SharedGear;
        if (row_hover) {
            c.fill(g.row, p.row_hover);
        }
        // The line that separates it from the list scrolling above.
        c.fill({g.row.left, g.row.top, g.row.right, g.row.top + static_cast<int>(border)}, p.card_border);
        paint_switch(c, g.toggle, shared_switch_value(), hovered(Part::SharedSwitch));
        if (focused(Part::SharedSwitch)) {
            RECT ring = g.toggle;
            InflateRect(&ring, px(3), px(3));
            c.round_outline(ring, static_cast<float>(height_of(ring)) / 2.0F, p.focus, pxf(2));
        }
        // A folder, 20 x 16: the tab, then the body.
        const auto ix = static_cast<float>(g.icon.left);
        const auto iy = static_cast<float>(g.icon.top);
        const float k = pxf(1.0F);
        const Rgb ink = p.text_subtle;
        c.line(ix + 1 * k, iy + 1 * k, ix + 7 * k, iy + 1 * k, ink, 1.3F * k);
        c.line(ix + 7 * k, iy + 1 * k, ix + 9 * k, iy + 3.5F * k, ink, 1.3F * k);
        c.round_outline({g.icon.left, g.icon.top + px(3), g.icon.right, g.icon.bottom}, pxf(2), ink, 1.3F * k);
        if (g.state.right > g.state.left) {
            if (sc.border != sc.fill) {
                c.round_rect(g.state, pxf(9), sc.fill, sc.border, border);
            } else {
                c.round_rect(g.state, pxf(9), sc.fill);
            }
        }
        for (const RECT& chip : g.chips) {
            c.round_rect(chip, pxf(10), p.chip);
        }
        if (g.end == SharedGeometry::End::Download) {
            // In the Needs download colours, as a project row's.
            c.round_rect(g.end_rect, pxf(4), hovered(Part::SharedEnd) ? p.row_hover : p.needs_download.fill,
                         p.needs_download.border, border);
            if (focused(Part::SharedEnd)) {
                RECT ring = g.end_rect;
                InflateRect(&ring, px(2), px(2));
                c.round_outline(ring, pxf(6), p.focus, pxf(2));
            }
        } else if (g.end == SharedGeometry::End::Progress) {
            c.round_rect(g.end_rect, pxf(11), p.waking.fill);
        } else if (g.end == SharedGeometry::End::Address) {
            paint_button(c, g.end_rect, hovered(Part::SharedEnd), focused(Part::SharedEnd));
        }
        paint_button(c, g.open, hovered(Part::SharedOpen), focused(Part::SharedOpen));
        paint_button(c, g.gear, hovered(Part::SharedGear), focused(Part::SharedGear));
        paint_gear(c, g.gear, p.text);
        c.unclip();
        return;
    }

    c.clip_text(g.row);
    c.text(g.name, w(text(lang_, Str::SharedSites)), fonts_.body_bold.get(), p.text, Align::Start, false);
    if (g.state.right > g.state.left) {
        c.text(g.state, w(text(lang_, state_label(row.state))), fonts_.small_bold.get(), sc.text, Align::Center, false);
    }
    c.text(g.line2, w(row.detail), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    for (std::size_t k = 0; k < g.chips.size() && k < row.chips.size(); ++k) {
        c.text(g.chips[k], w(row.chips[k]), fonts_.caption.get(), p.chip_text, Align::Center, true);
    }
    if (g.end != SharedGeometry::End::None) {
        const Rgb color = g.end == SharedGeometry::End::Download   ? p.needs_download.text
                          : g.end == SharedGeometry::End::Progress ? p.waking.text
                                                                   : p.link;
        c.text(g.end_rect, w(g.end_text), fonts_.button.get(), color, Align::Center,
               g.end == SharedGeometry::End::Address);
    }
    c.text(g.open, w(text(lang_, Str::SharedOpenFolder)), fonts_.button.get(), p.text, Align::Center, false);
    c.unclip_text();
}

// --- the settings page: state ----------------------------------------------------

void MainWindow::open_www() {
    if (!shows_shared() || wizard_open()) {
        return;
    }
    www_open_ = true;
    www_focus_ = -1;
    www_fields_ = shared_fields(snap_.shared, snap_.installed, lang_);
    hover_ = {};
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::close_www() {
    if (!www_open_) {
        return;
    }
    www_open_ = false;
    www_focus_ = -1;
    www_fields_.clear();
    hover_ = {};
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// The choice shown: the user's pick while it's on its way, else the setting.
std::size_t MainWindow::www_field_value(const VersionField& field) const {
    const auto it = pending_use_.find(ServiceWorker::use_key(shared_key(), field.key));
    if (it != pending_use_.end()) {
        for (std::size_t i = 0; i < field.choices.size(); ++i) {
            if (field.choices[i].value == it->second) {
                return i;
            }
        }
    }
    return field.current;
}

void MainWindow::www_change_folder() {
    if (!worker_ || snap_.link != Link::Connected || picking_folder_) {
        return;
    }
    // The dialog runs its own message loop: no second picker under it.
    picking_folder_ = true;
    pressed_ = {};
    hover_ = {};
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
    const std::wstring title = w(text(lang_, Str::SharedWwwFolder));
    dialog->SetTitle(title.c_str());
    std::string path;
    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR chosen = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) && chosen != nullptr) {
                path = win::narrow(chosen);
                CoTaskMemFree(chosen);
            }
            item->Release();
        }
    }
    dialog->Release();
    picking_folder_ = false;
    if (path.empty() || path == snap_.shared.dir || !worker_) {
        return;
    }
    const std::string key = ServiceWorker::use_key(shared_key(), "dir");
    pending_use_[key] = path;
    ++outstanding_[key];
    change_error_.clear();
    worker_->set_shared({{"dir", path}}, key);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::www_field_menu(std::size_t index) {
    if (index >= www_fields_.size() || index >= www_geometry_.fields.size() || !worker_ ||
        snap_.link != Link::Connected) {
        return;
    }
    // A copy: snapshots keep arriving while the menu is open.
    const VersionField field = www_fields_[index];
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    const std::size_t shown = www_field_value(field);
    std::vector<std::wstring> labels;
    labels.reserve(field.choices.size());
    for (std::size_t i = 0; i < field.choices.size(); ++i) {
        labels.push_back(w(field.choices[i].label));
        AppendMenuW(menu.get(), MF_STRING | (i == shown ? UINT{MF_CHECKED} : 0U), kFirstChoice + static_cast<UINT>(i),
                    labels.back().c_str());
    }
    const RECT box = www_geometry_.fields[index].second;
    POINT at{box.left, box.bottom};
    ClientToScreen(hwnd_, &at);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
    flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    if (command < kFirstChoice || command - kFirstChoice >= field.choices.size()) {
        return;
    }
    const std::size_t picked = command - kFirstChoice;
    if (picked == shown || !www_open_ || !worker_) {
        return;
    }
    // Sent at once: there's no Save button.
    const std::optional<std::string>& value = field.choices[picked].value;
    const std::string key = ServiceWorker::use_key(shared_key(), field.key);
    pending_use_[key] = value;
    ++outstanding_[key];
    change_error_.clear();
    worker_->set_shared({{field.key, value ? nlohmann::json(*value) : nlohmann::json(nullptr)}}, key);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- the settings page: geometry and input ------------------------------------------

MainWindow::WwwGeometry MainWindow::www_geometry(HDC dc) const {
    WwwGeometry g;
    g.area = wizard_rect();
    const int x0 = g.area.left + px(kPagePad);
    const int x1 = g.area.right - px(kPagePad);
    int y = g.area.top + px(20);
    const int switch_top = y + (px(30) - px(kSwitchH)) / 2;
    g.toggle = {x1 - px(kSwitchW), switch_top, x1, switch_top + px(kSwitchH)};
    g.title = {x0, y, g.toggle.left - px(12), y + px(30)};
    y += px(30) + px(6);
    g.hint = {x0, y, x1, y + px(36)};
    y += px(36) + px(16);

    const auto button = [&](Str label, int right, int top) {
        const int bw = Canvas::measure(dc, w(text(lang_, label)), fonts_.button.get(), rtl_) + px(28);
        return RECT{right - bw, top, right, top + px(kPageButtonH)};
    };
    g.folder_label = {x0, y, x1, y + px(18)};
    y += px(22);
    g.open = button(Str::SharedOpen, x1, y);
    g.change = button(Str::SharedChange, g.open.left - px(8), y);
    g.folder_box = {x0, y, std::max(x0, static_cast<int>(g.change.left) - px(8)), y + px(kPageButtonH)};
    y += px(kPageButtonH) + px(20);

    const int col_w = std::min(px(300), (x1 - x0 - px(16)) / 2);
    const int cell_h = px(16) + px(4) + px(kPageButtonH) + px(14);
    for (std::size_t i = 0; i < www_fields_.size(); ++i) {
        const int cx = x0 + static_cast<int>(i % 2) * (col_w + px(16));
        const int cy = y + static_cast<int>(i / 2) * cell_h;
        g.fields.emplace_back(RECT{cx, cy, cx + col_w, cy + px(16)},
                              RECT{cx, cy + px(20), cx + col_w, cy + px(20) + px(kPageButtonH)});
    }
    y += static_cast<int>((www_fields_.size() + 1) / 2) * cell_h + px(6);

    if (!snap_.shared.database.empty()) {
        const int right = std::min(x1, x0 + 2 * col_w + px(16));
        g.db_box = {x0, y, right, y + px(56)};
        const int mid = y + px(28);
        const int cw = Canvas::measure(dc, w(text(lang_, Str::CopyEnv)), fonts_.caption.get(), rtl_) + px(24);
        g.copy_env = {right - px(12) - cw, mid - px(14), right - px(12), mid + px(14)};
        g.db_title = {x0 + px(12), y + px(9), g.copy_env.left - px(12), y + px(28)};
        g.db_detail = {x0 + px(12), y + px(28), g.copy_env.left - px(12), y + px(46)};
    }

    g.footer = {g.area.left + 1, g.area.bottom - px(kPageFooterH), g.area.right - 1, g.area.bottom - 1};
    const int footer_mid = (g.footer.top + g.footer.bottom) / 2;
    const int cw = Canvas::measure(dc, w(text(lang_, Str::WizardClose)), fonts_.button_bold.get(), rtl_) + px(28);
    g.close = {x1 - cw, footer_mid - px(kPageButtonH) / 2, x1, footer_mid + px(kPageButtonH) / 2};
    return g;
}

MainWindow::Hit MainWindow::www_hit_test(int x, int y) const {
    const WwwGeometry& g = www_geometry_;
    // Nothing under the footer's line is the page's content.
    const bool above_footer = y < g.footer.top;
    if (above_footer) {
        for (std::size_t i = 0; i < g.fields.size(); ++i) {
            if (contains(g.fields[i].second, x, y)) {
                return {Part::WwwField, static_cast<int>(i), {}};
            }
        }
        const std::pair<const RECT*, Part> parts[] = {{&g.toggle, Part::WwwSwitch},
                                                      {&g.change, Part::WwwChange},
                                                      {&g.open, Part::WwwOpen},
                                                      {&g.copy_env, Part::WwwCopyEnv}};
        for (const auto& [rect, part] : parts) {
            if (contains(*rect, x, y)) {
                return {part, -1, {}};
            }
        }
    }
    if (contains(g.close, x, y)) {
        return {Part::WwwClose, -1, {}};
    }
    return {};
}

// What Tab visits on the page, in reading order.
std::vector<MainWindow::Hit> MainWindow::www_focus_order() const {
    const WwwGeometry& g = www_geometry_;
    std::vector<Hit> order{{Part::WwwSwitch, -1, {}}, {Part::WwwChange, -1, {}}, {Part::WwwOpen, -1, {}}};
    for (std::size_t i = 0; i < g.fields.size(); ++i) {
        order.push_back({Part::WwwField, static_cast<int>(i), {}});
    }
    if (g.copy_env.right > g.copy_env.left) {
        order.push_back({Part::WwwCopyEnv, -1, {}});
    }
    order.push_back({Part::WwwClose, -1, {}});
    return order;
}

void MainWindow::on_www_click(const Hit& hit) {
    switch (hit.part) {
        case Part::WwwSwitch:
            toggle_shared();
            break;
        case Part::WwwChange:
            www_change_folder();
            break;
        case Part::WwwOpen:
            open_folder(snap_.shared.dir);
            break;
        case Part::WwwField:
            if (hit.row >= 0) {
                www_field_menu(static_cast<std::size_t>(hit.row));
            }
            break;
        case Part::WwwCopyEnv:
            if (worker_ && snap_.link == Link::Connected) {
                change_error_.clear();
                worker_->copy_shared_env();
            }
            break;
        case Part::WwwClose:
            close_www();
            break;
        default:
            break;
    }
}

void MainWindow::on_www_key(WPARAM key) {
    const std::vector<Hit> order = www_focus_order();
    const int count = static_cast<int>(order.size());
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (key) {
        case VK_TAB:
            // Round the page; the first Tab lands on the switch.
            if (www_focus_ < 0) {
                www_focus_ = shift ? count - 1 : 0;
            } else {
                www_focus_ = (www_focus_ + (shift ? count - 1 : 1)) % count;
            }
            break;
        case VK_RETURN:
        case VK_SPACE:
            if (www_focus_ >= 0 && www_focus_ < count) {
                on_www_click(order[static_cast<std::size_t>(www_focus_)]);
            }
            return;
        case VK_ESCAPE:
            close_www();
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

// --- the settings page: painting -----------------------------------------------------

void MainWindow::paint_www(Canvas& c, HDC /*dc*/, bool text_pass) const {
    const WwwGeometry& g = www_geometry_;
    if (height_of(g.area) <= 0) {
        return;
    }
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    const bool keys = show_focus_ && GetFocus() == hwnd_;
    const std::vector<Hit> order = www_focus_order();
    const auto focused = [&](Part part, int index) {
        return keys && www_focus_ >= 0 && std::cmp_less(www_focus_, order.size()) &&
               order[static_cast<std::size_t>(www_focus_)].part == part &&
               order[static_cast<std::size_t>(www_focus_)].row == index;
    };
    const auto hovered = [&](Part part, int index) { return hover_.part == part && hover_.row == index; };
    // The content stops at the footer's line when the window is short.
    const RECT content{g.area.left, g.area.top, g.area.right, g.footer.top};

    if (!text_pass) {
        c.round_rect(g.area, pxf(8), p.card, p.card_border, border);
        c.fill({g.footer.left, g.footer.top, g.footer.right, g.footer.top + static_cast<int>(border)}, p.divider);
        c.round_rect(g.close, pxf(4), p.accent);
        if (focused(Part::WwwClose, -1)) {
            RECT ring = g.close;
            InflateRect(&ring, px(2), px(2));
            c.round_outline(ring, pxf(6), p.focus, pxf(2));
        }
        c.clip(content);
        paint_switch(c, g.toggle, shared_switch_value(), hovered(Part::WwwSwitch, -1));
        if (focused(Part::WwwSwitch, -1)) {
            RECT ring = g.toggle;
            InflateRect(&ring, px(3), px(3));
            c.round_outline(ring, static_cast<float>(height_of(ring)) / 2.0F, p.focus, pxf(2));
        }
        c.round_rect(g.folder_box, pxf(4), p.window, p.input_border, border);
        paint_button(c, g.change, hovered(Part::WwwChange, -1), focused(Part::WwwChange, -1));
        paint_button(c, g.open, hovered(Part::WwwOpen, -1), focused(Part::WwwOpen, -1));
        for (std::size_t i = 0; i < g.fields.size(); ++i) {
            const int index = static_cast<int>(i);
            const RECT& box = g.fields[i].second;
            c.round_rect(box, pxf(4), hovered(Part::WwwField, index) ? p.row_hover : p.input, p.input_border, border);
            if (focused(Part::WwwField, index)) {
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
            paint_button(c, g.copy_env, hovered(Part::WwwCopyEnv, -1), focused(Part::WwwCopyEnv, -1));
        }
        c.unclip();
        return;
    }

    c.clip_text(content);
    c.text(g.title, w(text(lang_, Str::SharedSites)), fonts_.title.get(), p.text, Align::Start, false);
    // What they do (with the address when they're served), or what's wrong.
    const SharedRow row = shared_row(snap_.shared, snap_.installed, lang_);
    std::string hint = row.detail;
    if (row.state != RowState::NeedsDownload && row.state != RowState::Problem) {
        const std::string host = row.address.empty() ? std::string("localhost") : row.address;
        hint = fill(lang_, Str::SharedHint, {snap_.shared.dir, host + "/<folder>/"});
    }
    c.paragraph(g.hint, w(hint), fonts_.button.get(),
                row.state == RowState::Problem ? p.problem.text : p.text_subtle);

    c.text(g.folder_label, w(text(lang_, Str::SharedWwwFolder)), fonts_.button_bold.get(), p.text, Align::Start, false);
    std::string dir = snap_.shared.dir;
    if (const auto it = pending_use_.find(ServiceWorker::use_key(shared_key(), "dir"));
        it != pending_use_.end() && it->second) {
        dir = *it->second;
    }
    RECT inner = g.folder_box;
    InflateRect(&inner, -px(10), 0);
    c.text(inner, w(dir), fonts_.button.get(), p.text, Align::Start, true);
    c.text(g.change, w(text(lang_, Str::SharedChange)), fonts_.button.get(), p.text, Align::Center, false);
    c.text(g.open, w(text(lang_, Str::SharedOpen)), fonts_.button.get(), p.text, Align::Center, false);

    for (std::size_t i = 0; i < g.fields.size() && i < www_fields_.size(); ++i) {
        const VersionField& f = www_fields_[i];
        c.text(g.fields[i].first, w(f.label), fonts_.caption.get(), p.text_subtle, Align::Start, false);
        RECT box = g.fields[i].second;
        box.left += px(8);
        box.right -= px(26);
        const std::size_t shown = www_field_value(f);
        if (shown < f.choices.size()) {
            c.text(box, w(f.choices[shown].label), fonts_.button.get(), p.text, Align::Start, false);
        }
    }

    if (g.db_box.right > g.db_box.left) {
        // User www: the www database and any www_... it makes (D19).
        const std::uint16_t port = database_port(snap_.databases, snap_.shared.database);
        const std::string host = port != 0 ? "127.0.0.1:" + std::to_string(port) : std::string("127.0.0.1");
        c.text(g.db_title, w(fill(lang_, Str::DatabaseTitle, {"www"})), fonts_.button_bold.get(), p.text, Align::Start,
               false);
        c.text(g.db_detail, w(fill(lang_, Str::SharedDatabaseDetail, {host, "www", "www_"})), fonts_.caption.get(),
               p.text_subtle, Align::Start, false);
        c.text(g.copy_env, w(text(lang_, Str::CopyEnv)), fonts_.caption.get(), p.text, Align::Center, false);
    }
    c.unclip_text();
    c.text(g.close, w(text(lang_, Str::WizardClose)), fonts_.button_bold.get(), p.accent_text, Align::Center, false);
}

}  // namespace floserver::ui

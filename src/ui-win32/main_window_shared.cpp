// Shared sites (D19): one row pinned under the project list (the list
// scrolls above it). Its gear opens Settings > Shared sites
// (settings_shared.cpp). The window asks shared.get without the folders, so
// nothing reads the www folder in the background; there's no folder list
// here, the list is the page at localhost/.

#include "main_window.h"

#include "main_window_layout.h"
#include "model/theme.h"
#include "model/view_model.h"

#include <algorithm>
#include <utility>

namespace floserver::ui {

using namespace layout;

namespace {

constexpr int kSharedButtonH = 28;

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
            open_settings_shared();
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
        case VK_RETURN:
            // The focused part (on the switch, that toggles it).
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
        paint_gear(c, g.gear, p.text, false);
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
    paint_gear(c, g.gear, p.text, true);
    c.unclip_text();
}

}  // namespace floserver::ui

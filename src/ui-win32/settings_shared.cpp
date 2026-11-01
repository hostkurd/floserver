// Settings > Shared sites (D19): the settings page that was shown over the
// main window's list, moved here unchanged in what it does: the on/off
// switch, the www folder (Change…, Open), PHP, database and web server, and
// the www database login with Copy .env lines. The gear on the main
// window's Shared sites row opens it.
//
// It asks nothing when it opens: what it shows is the main window's
// shared.get snapshot (asked without the folders, so nothing reads the www
// folder). Each change is sent at once (shared.set) and shown as picked
// until the worker reports it done.

#include "settings_window.h"

#include "main_window_layout.h"
#include "model/theme.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace floserver::ui {

using nlohmann::json;
using layout::height_of;

namespace {

constexpr int kGap = 12;
constexpr int kCardPad = 16;
constexpr int kControlH = 32;
constexpr int kSwitchW = 40;
constexpr int kSwitchH = 20;
constexpr UINT kFirstChoice = 100;

std::wstring w(std::string_view s) { return win::widen(s); }
bool is_empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

int text_height(HDC dc, std::wstring_view s, HFONT font, int width, bool rtl) {
    if (s.empty() || width <= 0) {
        return 0;
    }
    const win::Selected selected(dc, font);
    RECT r{0, 0, width, 0};
    DrawTextW(dc, s.data(), static_cast<int>(s.size()), &r,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | (rtl ? UINT{DT_RTLREADING} : 0U));
    return static_cast<int>(r.bottom - r.top);
}

std::string shared_key() { return ServiceWorker::kSharedKey; }

}  // namespace

// --- state -------------------------------------------------------------------------

void SettingsWindow::show_shared() {
    if (hwnd_ == nullptr) {
        return;
    }
    bring_to_front();
    if (!asking_) {
        open_section(Section::Shared);
    }
}

void SettingsWindow::on_shared_answers(const Snapshot& snap) {
    const bool had_changes = !shared_waiting_.empty();
    shared_ = snap.shared;
    // A switch or picker shows the user's choice until its last command has finished.
    const std::string use_prefix = ServiceWorker::use_key(shared_key(), "");
    for (const std::string& key : snap.settled) {
        const auto it = shared_waiting_.find(key);
        if (it == shared_waiting_.end() || --it->second > 0) {
            continue;
        }
        shared_waiting_.erase(it);
        if (key == ServiceWorker::on_key(shared_key())) {
            pending_shared_on_.reset();
        } else if (key.starts_with(use_prefix)) {
            pending_shared_use_.erase(key.substr(use_prefix.size()));
        }
    }
    // The worker's words for a change that failed ("Couldn't change Shared sites: ...").
    if (!snap.change_error.empty() && (had_changes || copying_env_)) {
        copying_env_ = false;
        show_message(snap.change_error, true);
    }
    shared_page_ = shared_page(shared_, installed_, databases_, lang_);
}

void SettingsWindow::env_copied(bool done) {
    if (hwnd_ == nullptr || !copying_env_) {
        return;
    }
    copying_env_ = false;
    show_message(done ? std::string(text(lang_, Str::EnvCopied)) : fill(lang_, Str::OpenFailed, {"clipboard"}), !done);
}

bool SettingsWindow::shown_shared_on() const { return pending_shared_on_.value_or(shared_.on); }

std::string SettingsWindow::shown_www_dir() const {
    if (const auto it = pending_shared_use_.find("dir"); it != pending_shared_use_.end() && it->second) {
        return *it->second;
    }
    return shared_.dir;
}

// The choice shown: the user's pick while it's on its way, else the setting.
std::size_t SettingsWindow::shared_field_value(const VersionField& field) const {
    if (const auto it = pending_shared_use_.find(field.key); it != pending_shared_use_.end()) {
        for (std::size_t i = 0; i < field.choices.size(); ++i) {
            if (field.choices[i].value == it->second) {
                return i;
            }
        }
    }
    return field.current;
}

void SettingsWindow::send_shared(const json& changes, const std::string& key) {
    ++shared_waiting_[key];
    message_.clear();
    worker_.set_shared(changes, key);
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::toggle_shared() {
    if (!connected_ || !shared_.known) {
        return;
    }
    const bool value = !shown_shared_on();
    pending_shared_on_ = value;
    send_shared({{"on", value}}, ServiceWorker::on_key(shared_key()));
}

void SettingsWindow::change_www_folder() {
    if (!connected_ || !shared_.known || picking_folder_ || asking_) {
        return;
    }
    // The dialog runs its own message loop: no second picker under it, and
    // (as while asking to save) the owner mustn't close or destroy Settings
    // meanwhile; a Quit then waits for the picker.
    picking_folder_ = true;
    asking_ = true;
    pressed_ = {};
    hover_ = {};
    const auto picked = pick_folder(hwnd_, std::string(text(lang_, Str::SharedWwwFolder)), shown_www_dir());
    picking_folder_ = false;
    asking_ = false;
    if (quit_after_ask_) {
        PostMessageW(owner_, closed_message_, 1, 0);
        return;
    }
    if (hwnd_ == nullptr) {
        return;
    }
    if (!picked) {
        show_message(fill(lang_, Str::OpenFailed, {"folder picker"}), true);
        return;
    }
    if (picked->empty() || *picked == shown_www_dir() || !connected_) {
        return;
    }
    pending_shared_use_["dir"] = *picked;
    send_shared({{"dir", *picked}}, ServiceWorker::use_key(shared_key(), "dir"));
}

void SettingsWindow::shared_field_menu(std::size_t index) {
    if (index >= shared_page_.fields.size() || index >= geometry_.shared.fields.size() || !connected_) {
        return;
    }
    // A copy: snapshots keep arriving while the menu is open.
    const VersionField field = shared_page_.fields[index];
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    const std::size_t shown = shared_field_value(field);
    std::vector<std::wstring> labels;
    labels.reserve(field.choices.size());
    for (std::size_t i = 0; i < field.choices.size(); ++i) {
        labels.push_back(w(field.choices[i].label));
        AppendMenuW(menu.get(), MF_STRING | (i == shown ? UINT{MF_CHECKED} : 0U), kFirstChoice + static_cast<UINT>(i),
                    labels.back().c_str());
    }
    const RECT box = geometry_.shared.fields[index].second;
    POINT at{box.left, box.bottom};
    ClientToScreen(hwnd_, &at);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
    flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    if (command < kFirstChoice || command - kFirstChoice >= field.choices.size() || hwnd_ == nullptr) {
        return;
    }
    const std::size_t picked = command - kFirstChoice;
    if (picked == shown || section_ != Section::Shared || !connected_) {
        return;
    }
    // Sent at once: there's no Save button.
    const std::optional<std::string>& value = field.choices[picked].value;
    pending_shared_use_[field.key] = value;
    send_shared({{field.key, value ? json(*value) : json(nullptr)}}, ServiceWorker::use_key(shared_key(), field.key));
}

// --- geometry ------------------------------------------------------------------------

void SettingsWindow::layout_shared(HDC dc, Geometry& g, int x0, int x1, int y) const {
    SharedGeometry& sg = g.shared;
    const auto button_w = [&](Str label, HFONT font) {
        return Canvas::measure(dc, w(text(lang_, label)), font, rtl_) + px(28);
    };

    // The title with the switch at its end, then what they do (or what's wrong).
    const int switch_top = y + (px(28) - px(kSwitchH)) / 2;
    if (shared_.known) {
        sg.toggle = {x1 - px(kSwitchW), switch_top, x1, switch_top + px(kSwitchH)};
    }
    g.title = {x0, y, (shared_.known ? static_cast<int>(sg.toggle.left) : x1) - px(kGap), y + px(28)};
    const std::string& hint = shared_page_.hint;
    const int hint_h = shared_.known ? text_height(dc, w(hint), fonts_.caption.get(), x1 - x0, rtl_) : 0;
    g.hint = {x0, g.title.bottom + px(2), x1, g.title.bottom + px(2) + hint_h};
    y = static_cast<int>(g.hint.bottom) + px(kGap);
    if (!message_.empty()) {
        const int mh = std::max(px(32), text_height(dc, w(message_), fonts_.button.get(), x1 - x0 - px(24), rtl_) + px(14));
        g.message = {x0, y, x1, y + mh};
        y += mh + px(kGap);
    }
    if (!shared_.known) {
        sg.note = {x0, y, x1, y + px(24)};
        g.content_h = sg.note.bottom + scroll_ + px(8);
        return;
    }

    const int cx0 = x0 + px(kCardPad);
    const int cx1 = x1 - px(kCardPad);
    int yy = y + px(14);
    sg.card = {x0, y, x1, y};
    sg.folder_label = {cx0, yy, cx1, yy + px(18)};
    yy += px(22);
    const int open_w = button_w(Str::SharedOpen, fonts_.button.get());
    const int change_w = button_w(Str::SharedChange, fonts_.button.get());
    sg.open = {std::max(cx0, cx1 - open_w), yy, cx1, yy + px(kControlH)};
    sg.change = {std::max(cx0, static_cast<int>(sg.open.left) - px(8) - change_w), yy, static_cast<int>(sg.open.left) - px(8),
                 yy + px(kControlH)};
    sg.folder_box = {cx0, yy, std::max(cx0 + px(40), static_cast<int>(sg.change.left) - px(8)), yy + px(kControlH)};
    yy += px(kControlH) + px(20);

    // The pickers, two to a line.
    const int col_w = std::min(px(300), (cx1 - cx0 - px(16)) / 2);
    const int cell_h = px(16) + px(4) + px(kControlH) + px(14);
    const std::size_t count = shared_page_.fields.size();
    for (std::size_t i = 0; i < count; ++i) {
        const int cx = cx0 + static_cast<int>(i % 2) * (col_w + px(16));
        const int cy = yy + static_cast<int>(i / 2) * cell_h;
        sg.fields.emplace_back(RECT{cx, cy, cx + col_w, cy + px(16)},
                               RECT{cx, cy + px(20), cx + col_w, cy + px(20) + px(kControlH)});
    }
    yy += static_cast<int>((count + 1) / 2) * cell_h;

    // The www login, while they're on with a database.
    if (shared_page_.login) {
        yy += px(6);
        const int right = std::min(cx1, cx0 + 2 * col_w + px(16));
        sg.db_box = {cx0, yy, right, yy + px(56)};
        const int mid = yy + px(28);
        const int cw = button_w(Str::CopyEnv, fonts_.caption.get()) - px(4);
        sg.copy_env = {right - px(12) - cw, mid - px(14), right - px(12), mid + px(14)};
        sg.db_title = {cx0 + px(12), yy + px(9), static_cast<int>(sg.copy_env.left) - px(12), yy + px(28)};
        sg.db_detail = {cx0 + px(12), yy + px(28), static_cast<int>(sg.copy_env.left) - px(12), yy + px(46)};
        yy += px(56);
    }
    sg.card.bottom = yy + px(14);
    y = static_cast<int>(sg.card.bottom) + px(kGap);
    // In page terms (y is on screen), so the page shrinks back when it gets shorter.
    g.content_h = y + scroll_ + px(8);
}

const RECT* SettingsWindow::shared_rect_of(const Hit& hit) const {
    const SharedGeometry& g = geometry_.shared;
    switch (hit.part) {
        case Part::SharedSwitch:
            return &g.toggle;
        case Part::SharedChange:
            return &g.change;
        case Part::SharedOpen:
            return &g.open;
        case Part::SharedField:
            return hit.row >= 0 && std::cmp_less(hit.row, g.fields.size()) ? &g.fields[static_cast<std::size_t>(hit.row)].second
                                                                           : nullptr;
        case Part::SharedCopyEnv:
            return &g.copy_env;
        default:
            break;
    }
    return nullptr;
}

// What Tab visits, in reading order.
void SettingsWindow::shared_focus_order(std::vector<Hit>& order) const {
    const SharedGeometry& g = geometry_.shared;
    if (is_empty(g.toggle)) {
        return;
    }
    order.push_back({Part::SharedSwitch, -1, -1});
    order.push_back({Part::SharedChange, -1, -1});
    order.push_back({Part::SharedOpen, -1, -1});
    for (std::size_t i = 0; i < g.fields.size(); ++i) {
        order.push_back({Part::SharedField, -1, static_cast<int>(i)});
    }
    if (!is_empty(g.copy_env)) {
        order.push_back({Part::SharedCopyEnv, -1, -1});
    }
}

// --- input -----------------------------------------------------------------------------

void SettingsWindow::on_shared_click(const Hit& hit) {
    switch (hit.part) {
        case Part::SharedSwitch:
            toggle_shared();
            break;
        case Part::SharedChange:
            change_www_folder();
            break;
        case Part::SharedOpen:
            explore(shown_www_dir());
            break;
        case Part::SharedField:
            if (hit.row >= 0) {
                shared_field_menu(static_cast<std::size_t>(hit.row));
            }
            break;
        case Part::SharedCopyEnv:
            if (connected_ && !copying_env_) {
                copying_env_ = true;
                message_.clear();
                worker_.copy_shared_env();  // the main window puts it on the clipboard; env_copied() follows
                relayout();
            }
            break;
        default:
            break;
    }
}

// --- painting ----------------------------------------------------------------------

void SettingsWindow::paint_switch(Canvas& c, const RECT& r, bool on, bool hover) const {
    const Palette& p = palette(dark_);
    const float radius = static_cast<float>(height_of(r)) / 2.0F;
    const float knob = pxf(hover ? 6.0F : 5.0F);
    const float ky = static_cast<float>(r.top) + radius;
    if (on) {
        c.round_rect(r, radius, p.accent);
        c.ellipse(static_cast<float>(r.right) - pxf(9), ky, knob, p.switch_on_knob);
    } else {
        c.round_rect(r, radius, p.switch_off_fill, p.switch_off_border, std::max(1.0F, pxf(1.0F)));
        c.ellipse(static_cast<float>(r.left) + pxf(9), ky, knob, p.switch_off_knob);
    }
}

void SettingsWindow::paint_shared_shapes(Canvas& c) const {
    const Palette& p = palette(dark_);
    const SharedGeometry& sg = geometry_.shared;
    const float border = std::max(1.0F, pxf(1.0F));
    c.clip({main_left() - px(4), 0, width_, height_});
    if (!is_empty(geometry_.message)) {
        c.round_rect(geometry_.message, pxf(4), message_error_ ? p.banner_error.fill : p.banner_info.fill);
    }
    if (!is_empty(sg.toggle)) {
        const Hit toggle{Part::SharedSwitch, -1, -1};
        paint_switch(c, sg.toggle, shown_shared_on(), hover_ == toggle && connected_);
        if (is_focused(toggle)) {
            RECT ring = sg.toggle;
            InflateRect(&ring, px(3), px(3));
            c.round_outline(ring, static_cast<float>(height_of(ring)) / 2.0F, p.focus, pxf(2));
        }
        c.round_rect(sg.card, pxf(8), p.card, p.card_border, border);
        paint_folder_box(c, sg.folder_box, {}, false);
        const Hit change{Part::SharedChange, -1, -1};
        const bool can_change = connected_ && !picking_folder_;
        paint_button(c, sg.change, hover_ == change && can_change, is_focused(change), can_change);
        const Hit open{Part::SharedOpen, -1, -1};
        paint_button(c, sg.open, hover_ == open, is_focused(open), true);
        for (std::size_t i = 0; i < sg.fields.size(); ++i) {
            const Hit hit{Part::SharedField, -1, static_cast<int>(i)};
            const RECT& box = sg.fields[i].second;
            c.round_rect(box, pxf(4), hover_ == hit && connected_ ? p.row_hover : p.input, p.input_border, border);
            if (is_focused(hit)) {
                paint_ring(c, box);
            }
            // The chevron, at the box's end.
            const float vx = static_cast<float>(box.right) - pxf(14);
            const float vy = static_cast<float>(box.top + box.bottom) / 2.0F;
            c.line(vx - pxf(4), vy - pxf(2), vx, vy + pxf(2), p.text_subtle, pxf(1.3F));
            c.line(vx, vy + pxf(2), vx + pxf(4), vy - pxf(2), p.text_subtle, pxf(1.3F));
        }
        if (!is_empty(sg.db_box)) {
            c.round_rect(sg.db_box, pxf(6), p.panel_box);
            const Hit copy{Part::SharedCopyEnv, -1, -1};
            const bool can_copy = connected_ && !copying_env_;
            paint_button(c, sg.copy_env, hover_ == copy && can_copy, is_focused(copy), can_copy);
        }
    }
    // The scroll bar, when the page is taller than the window.
    scroll_bar_.paint(c, p);
    c.unclip();
}

void SettingsWindow::paint_shared_text(Canvas& c) const {
    const Palette& p = palette(dark_);
    const SharedGeometry& sg = geometry_.shared;
    c.clip_text({main_left() - px(4), 0, width_, height_});
    c.text(geometry_.title, w(text(lang_, Str::SharedSites)), fonts_.title.get(), p.text, Align::Start, false);
    if (!is_empty(geometry_.hint)) {
        c.paragraph(geometry_.hint, w(shared_page_.hint), fonts_.caption.get(),
                    shared_page_.problem ? p.problem.text : p.text_subtle);
    }
    if (!is_empty(geometry_.message)) {
        RECT inner = geometry_.message;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(message_), fonts_.button.get(), message_error_ ? p.banner_error.text : p.banner_info.text);
    }
    if (!is_empty(sg.note)) {
        c.text(sg.note, w(text(lang_, Str::SettingsLoading)), fonts_.button.get(), p.text_subtle, Align::Start, false);
    }
    if (is_empty(sg.toggle)) {
        c.unclip_text();
        return;
    }
    c.text(sg.folder_label, w(text(lang_, Str::SharedWwwFolder)), fonts_.button_bold.get(), p.text, Align::Start, false);
    paint_folder_box(c, sg.folder_box, shown_www_dir(), true);
    const bool can_change = connected_ && !picking_folder_;
    c.text(sg.change, w(text(lang_, Str::SharedChange)), fonts_.button.get(), can_change ? p.text : p.button_disabled_text,
           Align::Center, false);
    c.text(sg.open, w(text(lang_, Str::SharedOpen)), fonts_.button.get(), p.text, Align::Center, false);
    for (std::size_t i = 0; i < sg.fields.size() && i < shared_page_.fields.size(); ++i) {
        const VersionField& f = shared_page_.fields[i];
        c.text(sg.fields[i].first, w(f.label), fonts_.caption.get(), p.text_subtle, Align::Start, false);
        RECT box = sg.fields[i].second;
        box.left += px(8);
        box.right -= px(26);
        const std::size_t shown = shared_field_value(f);
        if (shown < f.choices.size()) {
            c.text(box, w(f.choices[shown].label), fonts_.button.get(), p.text, Align::Start, false);
        }
    }
    if (!is_empty(sg.db_box)) {
        // User www: the www database and any www_... it makes (D19).
        c.text(sg.db_title, w(shared_page_.login_title), fonts_.button_bold.get(), p.text, Align::Start, false);
        c.text(sg.db_detail, w(shared_page_.login_detail), fonts_.caption.get(), p.text_subtle, Align::Start, false);
        c.text(sg.copy_env, w(text(lang_, Str::CopyEnv)), fonts_.caption.get(),
               connected_ && !copying_env_ ? p.text : p.button_disabled_text, Align::Center, false);
    }
    c.unclip_text();
}

}  // namespace floserver::ui

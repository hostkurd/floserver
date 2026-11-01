// Settings > Folders: where new projects are made (New project starts
// there; projects already made stay where they are), the www folder for
// shared sites (the same setting as Settings > Shared sites), and
// FloServer's own data folder with how much it uses and two clean-ups.
//
// One folders.get when the section opens: the service counts the data
// folder then, on a thread of its own (never the 2 s poll's), giving up
// after a few seconds. Each change and clean-up is sent at once; nothing
// polls and nothing runs while Settings is closed.

#include "settings_window.h"

#include "main_window_layout.h"
#include "model/theme.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <string_view>
#include <utility>

namespace floserver::ui {

using nlohmann::json;

namespace {

constexpr int kGap = 12;
constexpr int kCardPad = 16;
constexpr int kControlH = 30;
constexpr int kTitleH = 22;

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

// "localhost:8080/<folder>/", or "localhost/<folder>/" while there's no address.
std::string www_host(const SharedSites& shared) {
    std::string host = "localhost";
    if (is_shared_address(shared.address)) {
        host = shared.address.substr(shared.address.find("//") + 2);
        if (host.ends_with("/")) {
            host.pop_back();
        }
    }
    return host + "/<folder>/";
}

}  // namespace

// --- what's asked, what comes back ------------------------------------------------

void SettingsWindow::load_folders() {
    folders_loading_ = true;
    // The one request the section makes; the service counts the data folder for it.
    worker_.database_call("folders.get", json::object());
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::on_folders_answers(const Snapshot& snap) {
    const auto service_words = [this](const std::string& s) { return rtl_ ? isolate_ltr(s) : s; };
    for (const Snapshot::DbAnswer& answer : snap.db) {
        if (!answer.method.starts_with("folders.")) {
            continue;
        }
        const bool failed = !answer.error.empty() || !answer.reply || !answer.reply->is_object();
        if (answer.method == "folders.get") {
            folders_loading_ = false;
            if (failed) {
                show_message(fill(lang_, Str::FoldersReadFailed, {service_words(answer.error)}), true);
            } else {
                folders_ = folders_from_reply(*answer.reply);
            }
        } else if (answer.method == "folders.set") {
            folders_setting_ = false;
            if (failed) {
                show_message(fill(lang_, Str::FoldersChangeFailed, {service_words(answer.error)}), true);
            } else if (folders_) {
                merge_folders_reply(*folders_, *answer.reply);
                show_message(fill(lang_, Str::FoldersProjectsChanged, {folders_->projects_dir}), false);
            }
        } else if (answer.method == "folders.clean") {
            cleaning_.clear();
            if (failed) {
                show_message(fill(lang_, Str::FoldersCleanFailed, {service_words(answer.error)}), true);
            } else {
                if (folders_) {
                    merge_folders_reply(*folders_, *answer.reply);
                }
                show_message(clean_done_text(*answer.reply, lang_), false);
            }
        }
    }
}

void SettingsWindow::change_projects_folder() {
    if (!connected_ || picking_folder_ || asking_ || folders_setting_ || !folders_) {
        return;
    }
    // The dialog runs its own message loop: no second picker under it, and
    // (as while asking to save) the owner mustn't close or destroy Settings
    // meanwhile; a Quit then waits for the picker.
    picking_folder_ = true;
    asking_ = true;
    pressed_ = {};
    hover_ = {};
    const std::string start = folders_->projects_dir;
    const auto picked = pick_folder(hwnd_, std::string(text(lang_, Str::FoldersPickProjects)), start);
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
    if (picked->empty() || !folders_ || *picked == folders_->projects_dir || !connected_) {
        return;
    }
    folders_setting_ = true;
    message_.clear();
    worker_.database_call("folders.set", {{"projects_dir", *picked}});
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::clean_up(const std::string& what) {
    if (!connected_ || !cleaning_.empty() || !folders_) {
        return;
    }
    const std::uint64_t bytes = what == "logs" ? folders_->logs_bytes : folders_->leftovers_bytes;
    if (bytes == 0) {
        return;
    }
    cleaning_ = what;
    message_.clear();
    // The service lists what to remove again itself: nothing named here is deleted.
    worker_.database_call("folders.clean", {{"what", what}});
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::explore(const std::string& folder) {
    if (!explore_folder(hwnd_, folder)) {
        show_message(fill(lang_, Str::OpenFailed, {folder}), true);
    }
}

// --- geometry ------------------------------------------------------------------------

void SettingsWindow::layout_folders(HDC dc, Geometry& g, int x0, int x1, int y) const {
    FoldersGeometry& fg = g.folders;
    const auto measure = [&](std::string_view s, HFONT font) { return Canvas::measure(dc, w(s), font, rtl_); };
    const auto button_w = [&](Str label) { return measure(text(lang_, label), fonts_.button.get()) + px(24); };

    const int hint_h = text_height(dc, w(text(lang_, Str::FoldersHint)), fonts_.caption.get(), x1 - x0, rtl_);
    g.title = {x0, y, x1, y + px(28)};
    g.hint = {x0, g.title.bottom, x1, g.title.bottom + hint_h};
    y = static_cast<int>(g.hint.bottom) + px(kGap);
    if (!message_.empty()) {
        const int mh = std::max(px(32), text_height(dc, w(message_), fonts_.button.get(), x1 - x0 - px(24), rtl_) + px(14));
        g.message = {x0, y, x1, y + mh};
        y += mh + px(kGap);
    }
    if (!folders_) {
        fg.note = {x0, y, x1, y + px(24)};
        g.content_h = fg.note.bottom + scroll_ + px(8);
        return;
    }

    const int cx0 = x0 + px(kCardPad);
    const int cx1 = x1 - px(kCardPad);
    const int avail = cx1 - cx0;
    // A card's title and the words under it; `yy` moves past them.
    const auto head = [&](RECT& title, RECT& note, const std::string& words, int& yy) {
        title = {cx0, yy, cx1, yy + px(kTitleH)};
        yy += px(kTitleH);
        const int h = text_height(dc, w(words), fonts_.caption.get(), avail, rtl_);
        note = {cx0, yy, cx1, yy + h};
        yy += h + px(8);
    };
    // The folder's box with its buttons at the line's end, in reading order.
    const auto folder_line = [&](RECT& box, std::initializer_list<std::pair<RECT*, Str>> buttons, int& yy) {
        int right = cx1;
        for (auto it = std::rbegin(buttons); it != std::rend(buttons); ++it) {
            const int bw = button_w(it->second);
            *it->first = {std::max(cx0, right - bw), yy, right, yy + px(kControlH)};
            right = static_cast<int>(it->first->left) - px(8);
        }
        box = {cx0, yy, std::max(cx0 + px(40), right), yy + px(kControlH)};
        yy += px(kControlH);
    };

    // Projects folder: Change…, Open, and back to the default when it's the user's.
    int yy = y + px(14);
    fg.projects_card = {x0, y, x1, y};
    head(fg.projects_title, fg.projects_note, std::string(text(lang_, Str::FoldersProjectsNote)), yy);
    if (folders_->custom) {
        folder_line(fg.projects_box,
                    {{&fg.projects_change, Str::SharedChange}, {&fg.projects_open, Str::SharedOpen},
                     {&fg.projects_default, Str::FoldersUseDefault}},
                    yy);
    } else {
        folder_line(fg.projects_box, {{&fg.projects_change, Str::SharedChange}, {&fg.projects_open, Str::SharedOpen}}, yy);
    }
    fg.projects_card.bottom = yy + px(14);
    y = static_cast<int>(fg.projects_card.bottom) + px(kGap);

    // The www folder, as Settings > Shared sites shows it.
    if (shared_.known) {
        yy = y + px(14);
        fg.www_card = {x0, y, x1, y};
        head(fg.www_title, fg.www_note, fill(lang_, Str::FoldersWwwNote, {www_host(shared_)}), yy);
        folder_line(fg.www_box, {{&fg.www_change, Str::SharedChange}, {&fg.www_open, Str::SharedOpen}}, yy);
        fg.www_card.bottom = yy + px(14);
        y = static_cast<int>(fg.www_card.bottom) + px(kGap);
    }

    // The data folder: read-only, with what it uses under it.
    yy = y + px(14);
    fg.data_card = {x0, y, x1, y};
    head(fg.data_title, fg.data_note, std::string(text(lang_, Str::FoldersDataNote)), yy);
    folder_line(fg.data_box, {{&fg.data_open, Str::SharedOpen}}, yy);
    yy += px(6);
    fg.data_usage = {cx0, yy, cx1, yy + px(20)};
    yy += px(20);
    fg.data_card.bottom = yy + px(14);
    y = static_cast<int>(fg.data_card.bottom) + px(kGap);

    // Clean up: each button at the line's end, what it frees and does before it.
    yy = y + px(14);
    fg.clean_card = {x0, y, x1, y};
    fg.clean_title = {cx0, yy, cx1, yy + px(kTitleH)};
    yy += px(kTitleH) + px(8);
    const int bw = std::max({button_w(Str::FoldersClearLogs), button_w(Str::FoldersLeftovers), button_w(Str::FoldersCleaning)});
    const int text_w = std::max(px(80), avail - bw - px(kGap));
    const auto clean_line = [&](RECT& button, RECT& size, RECT& note, Str words) {
        button = {cx1 - bw, yy, cx1, yy + px(kControlH)};
        size = {cx0, yy, cx0 + text_w, yy + px(20)};
        const int h = text_height(dc, w(text(lang_, words)), fonts_.caption.get(), text_w, rtl_);
        note = {cx0, yy + px(20), cx0 + text_w, yy + px(20) + h};
        yy = std::max(static_cast<int>(button.bottom), static_cast<int>(note.bottom)) + px(kGap);
    };
    clean_line(fg.logs, fg.logs_size, fg.logs_note, Str::FoldersClearLogsNote);
    clean_line(fg.leftovers, fg.leftovers_size, fg.leftovers_note, Str::FoldersLeftoversNote);
    const int never_h = text_height(dc, w(text(lang_, Str::FoldersNeverNote)), fonts_.caption.get(), avail, rtl_);
    fg.never_note = {cx0, yy, cx1, yy + never_h};
    yy += never_h;
    fg.clean_card.bottom = yy + px(14);
    y = static_cast<int>(fg.clean_card.bottom) + px(kGap);
    // In page terms (y is on screen), so the page shrinks back when it gets shorter.
    g.content_h = y + scroll_ + px(8);
}

const RECT* SettingsWindow::folders_rect_of(const Hit& hit) const {
    const FoldersGeometry& g = geometry_.folders;
    switch (hit.part) {
        case Part::FoldersProjectsChange:
            return &g.projects_change;
        case Part::FoldersProjectsOpen:
            return &g.projects_open;
        case Part::FoldersProjectsDefault:
            return &g.projects_default;
        case Part::FoldersWwwChange:
            return &g.www_change;
        case Part::FoldersWwwOpen:
            return &g.www_open;
        case Part::FoldersDataOpen:
            return &g.data_open;
        case Part::FoldersClearLogs:
            return &g.logs;
        case Part::FoldersLeftovers:
            return &g.leftovers;
        default:
            break;
    }
    return nullptr;
}

void SettingsWindow::folders_focus_order(std::vector<Hit>& order) const {
    const FoldersGeometry& g = geometry_.folders;
    for (const auto& [part, r] :
         {std::pair{Part::FoldersProjectsChange, &g.projects_change}, std::pair{Part::FoldersProjectsOpen, &g.projects_open},
          std::pair{Part::FoldersProjectsDefault, &g.projects_default}, std::pair{Part::FoldersWwwChange, &g.www_change},
          std::pair{Part::FoldersWwwOpen, &g.www_open}, std::pair{Part::FoldersDataOpen, &g.data_open},
          std::pair{Part::FoldersClearLogs, &g.logs}, std::pair{Part::FoldersLeftovers, &g.leftovers}}) {
        if (!is_empty(*r)) {
            order.push_back({part, -1, -1});
        }
    }
}

// --- input -----------------------------------------------------------------------------

void SettingsWindow::on_folders_click(const Hit& hit) {
    if (!folders_) {
        return;
    }
    switch (hit.part) {
        case Part::FoldersProjectsChange:
            change_projects_folder();
            break;
        case Part::FoldersProjectsOpen:
            explore(folders_->projects_dir);
            break;
        case Part::FoldersProjectsDefault:
            if (connected_ && !folders_setting_ && folders_->custom) {
                folders_setting_ = true;
                message_.clear();
                worker_.database_call("folders.set", {{"projects_dir", nullptr}});
                relayout();
            }
            break;
        case Part::FoldersWwwChange:
            change_www_folder();
            break;
        case Part::FoldersWwwOpen:
            explore(shown_www_dir());
            break;
        case Part::FoldersDataOpen:
            explore(folders_->data_dir);
            break;
        case Part::FoldersClearLogs:
            clean_up("logs");
            break;
        case Part::FoldersLeftovers:
            clean_up("leftovers");
            break;
        default:
            break;
    }
}

// --- painting ----------------------------------------------------------------------

void SettingsWindow::paint_folder_box(Canvas& c, const RECT& box, const std::string& path, bool text_pass) const {
    const Palette& p = palette(dark_);
    if (!text_pass) {
        c.round_rect(box, pxf(4), p.window, p.input_border, std::max(1.0F, pxf(1.0F)));
        return;
    }
    RECT inner = box;
    InflateRect(&inner, -px(10), 0);
    c.text(inner, w(path), fonts_.button.get(), p.text, Align::Start, true);
}

void SettingsWindow::paint_folders_shapes(Canvas& c) const {
    const Palette& p = palette(dark_);
    const FoldersGeometry& fg = geometry_.folders;
    const float border = std::max(1.0F, pxf(1.0F));
    c.clip({main_left() - px(4), 0, width_, height_});
    if (!is_empty(geometry_.message)) {
        c.round_rect(geometry_.message, pxf(4), message_error_ ? p.banner_error.fill : p.banner_info.fill);
    }
    if (folders_) {
        const bool can_change = connected_ && !picking_folder_;
        const auto button = [&](Part part, const RECT& r, bool enabled) {
            if (!is_empty(r)) {
                const Hit hit{part, -1, -1};
                paint_button(c, r, hover_ == hit && enabled, is_focused(hit), enabled);
            }
        };
        c.round_rect(fg.projects_card, pxf(8), p.card, p.card_border, border);
        paint_folder_box(c, fg.projects_box, {}, false);
        button(Part::FoldersProjectsChange, fg.projects_change, can_change && !folders_setting_);
        button(Part::FoldersProjectsOpen, fg.projects_open, true);
        button(Part::FoldersProjectsDefault, fg.projects_default, can_change && !folders_setting_);
        if (!is_empty(fg.www_card)) {
            c.round_rect(fg.www_card, pxf(8), p.card, p.card_border, border);
            paint_folder_box(c, fg.www_box, {}, false);
            button(Part::FoldersWwwChange, fg.www_change, can_change);
            button(Part::FoldersWwwOpen, fg.www_open, true);
        }
        c.round_rect(fg.data_card, pxf(8), p.card, p.card_border, border);
        paint_folder_box(c, fg.data_box, {}, false);
        button(Part::FoldersDataOpen, fg.data_open, true);
        c.round_rect(fg.clean_card, pxf(8), p.card, p.card_border, border);
        const bool idle = connected_ && cleaning_.empty();
        button(Part::FoldersClearLogs, fg.logs, idle && folders_->logs_bytes > 0);
        button(Part::FoldersLeftovers, fg.leftovers, idle && folders_->leftovers_bytes > 0);
    }
    // The scroll bar, when the page is taller than the window.
    scroll_bar_.paint(c, p);
    c.unclip();
}

void SettingsWindow::paint_folders_text(Canvas& c) const {
    const Palette& p = palette(dark_);
    const FoldersGeometry& fg = geometry_.folders;
    c.clip_text({main_left() - px(4), 0, width_, height_});
    c.text(geometry_.title, w(text(lang_, Str::SettingsFolders)), fonts_.title.get(), p.text, Align::Start, false);
    c.paragraph(geometry_.hint, w(text(lang_, Str::FoldersHint)), fonts_.caption.get(), p.text_subtle);
    if (!is_empty(geometry_.message)) {
        RECT inner = geometry_.message;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(message_), fonts_.button.get(), message_error_ ? p.banner_error.text : p.banner_info.text);
    }
    if (!folders_) {
        if (!is_empty(fg.note)) {
            c.text(fg.note, w(text(lang_, Str::SettingsLoading)), fonts_.button.get(), p.text_subtle, Align::Start, false);
        }
        c.unclip_text();
        return;
    }
    const bool can_change = connected_ && !picking_folder_;
    const auto button = [&](const RECT& r, Str label, bool enabled) {
        if (!is_empty(r)) {
            c.text(r, w(text(lang_, label)), fonts_.button.get(), enabled ? p.text : p.button_disabled_text, Align::Center,
                   false);
        }
    };
    const auto head = [&](const RECT& title, Str words, const RECT& note, const std::string& note_words) {
        c.text(title, w(text(lang_, words)), fonts_.body_bold.get(), p.text, Align::Start, false);
        c.paragraph(note, w(note_words), fonts_.caption.get(), p.text_subtle);
    };

    head(fg.projects_title, Str::FoldersProjects, fg.projects_note, std::string(text(lang_, Str::FoldersProjectsNote)));
    paint_folder_box(c, fg.projects_box, folders_->projects_dir, true);
    button(fg.projects_change, Str::SharedChange, can_change && !folders_setting_);
    button(fg.projects_open, Str::SharedOpen, true);
    button(fg.projects_default, Str::FoldersUseDefault, can_change && !folders_setting_);

    if (!is_empty(fg.www_card)) {
        head(fg.www_title, Str::FoldersWww, fg.www_note, fill(lang_, Str::FoldersWwwNote, {www_host(shared_)}));
        paint_folder_box(c, fg.www_box, shown_www_dir(), true);
        button(fg.www_change, Str::SharedChange, can_change);
        button(fg.www_open, Str::SharedOpen, true);
    }

    head(fg.data_title, Str::FoldersData, fg.data_note, std::string(text(lang_, Str::FoldersDataNote)));
    paint_folder_box(c, fg.data_box, folders_->data_dir, true);
    button(fg.data_open, Str::SharedOpen, true);
    c.text(fg.data_usage, w(folders_loading_ ? std::string(text(lang_, Str::FoldersCounting)) : data_usage_text(*folders_, lang_)),
           fonts_.button.get(), folders_loading_ ? p.text_subtle : p.text, Align::Start, false);

    c.text(fg.clean_title, w(text(lang_, Str::FoldersCleanUp)), fonts_.body_bold.get(), p.text, Align::Start, false);
    const bool idle = connected_ && cleaning_.empty();
    const auto clean_line = [&](const RECT& r, const RECT& size, const RECT& note, Str label, Str words,
                                std::uint64_t bytes, bool waits, const char* what) {
        button(r, cleaning_ == what ? Str::FoldersCleaning : label, idle && bytes > 0);
        c.text(size, w(clean_size_text(bytes, waits, lang_)), fonts_.button_bold.get(), bytes > 0 ? p.text : p.text_subtle,
               Align::Start, false);
        c.paragraph(note, w(text(lang_, words)), fonts_.caption.get(), p.text_subtle);
    };
    clean_line(fg.logs, fg.logs_size, fg.logs_note, Str::FoldersClearLogs, Str::FoldersClearLogsNote,
               folders_->logs_bytes, false, "logs");
    clean_line(fg.leftovers, fg.leftovers_size, fg.leftovers_note, Str::FoldersLeftovers, Str::FoldersLeftoversNote,
               folders_->leftovers_bytes, folders_->installing, "leftovers");
    c.paragraph(fg.never_note, w(text(lang_, Str::FoldersNeverNote)), fonts_.caption.get(), p.text_subtle);
    c.unclip_text();
}

}  // namespace floserver::ui

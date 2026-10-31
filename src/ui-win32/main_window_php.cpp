// The project panel's PHP tab (D20): this project's own extension changes
// (chips, and "Change…" for a checklist), its php.ini values over the
// series' (shown grey as placeholders), a warning under a name PHP doesn't
// know, Reset to defaults, Discard and Save (one projects.php). php.get is
// asked when the tab is first shown for a project, for the placeholders,
// the list and the names PHP knows; never while the panel or tab is closed.

#include "main_window.h"

#include "main_window_layout.h"
#include "model/theme.h"
#include "model/view_model.h"

#include "floserver/core/project_config.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace floserver::ui {

using namespace layout;
using nlohmann::json;

namespace {

constexpr int kLabelH = 16;
constexpr int kBoxH = 30;
constexpr int kPillH = 26;
constexpr int kButtonH = 30;
constexpr int kSection = 18;
constexpr UINT kFirstChoice = 100;
constexpr std::size_t kMenuColumn = 18;  // extensions per column of the Change… list

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

std::string trimmed(const std::string& s) {
    const auto first = s.find_first_not_of(" \t");
    return first == std::string::npos ? std::string() : s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

}  // namespace

// --- opening, leaving, saving ---------------------------------------------------------

void MainWindow::open_php_part() {
    const ProjectRow* row = selected_row();
    if (row == nullptr || php_open_) {
        return;
    }
    php_open_ = true;
    php_series_ = project_php_series(*row, snap_.installed).value_or(std::string());
    php_before_ = project_php_of(*row);
    php_after_ = php_before_;
    php_another_.clear();
    php_message_.clear();
    php_error_key_.clear();
    php_saving_ = false;
    // Asked once per project while it's selected: shown again, the tab
    // uses the answer it has (or the one on its way).
    if (php_info_project_ != row->name || php_info_series_ != php_series_) {
        forget_php_info();
        php_info_project_ = row->name;
        php_info_series_ = php_series_;
        ask_php_info();
    }
    make_project_php_edits();
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::forget_php_info() {
    php_info_.reset();
    php_info_project_.clear();
    php_info_series_.clear();
    php_info_wanted_ = false;
    php_loading_ = false;
}

// The one request the tab makes; when the service isn't there it's asked
// once it is (on_snapshot), never on a timer.
void MainWindow::ask_php_info() {
    php_info_wanted_ = false;
    if (php_info_series_.empty() || php_info_project_.empty() || !worker_) {
        return;
    }
    if (snap_.link != Link::Connected) {
        php_info_wanted_ = true;
        return;
    }
    php_loading_ = true;
    worker_->php_get(php_info_series_, php_info_project_);
}

bool MainWindow::php_dirty() const {
    return php_open_ && (php_after_ != php_before_ || !trimmed(php_another_).empty());
}

bool MainWindow::leave_php_part(bool ask) { return leave_php_part(ask, PanelAct{}); }

bool MainWindow::leave_php_part(bool ask, const PanelAct& then) {
    if (!php_open_) {
        return true;
    }
    if (ask && (php_saving_ || asking_)) {
        return false;  // a save's answer, or the question, comes first
    }
    if (ask && php_dirty()) {
        asking_ = true;
        const SaveChoice choice = ask_to_save(hwnd_, lang_, fill(lang_, Str::ProjectPhpAskSave, {selected_}));
        asking_ = false;
        if (quit_after_ask_) {
            quit_after_ask_ = false;
            quit();  // asked while the question was open
            return false;
        }
        if (!php_open_) {
            return true;  // closed meanwhile (the project went)
        }
        switch (choice) {
            case SaveChoice::Save:
                if (!save_project_php()) {
                    return false;  // not sent: "Another setting" can't be read, or not connected
                }
                if (php_saving_) {
                    // Done once it's answered without a refusal; a refusal shows here.
                    php_leave_after_save_ = true;
                    php_then_ = then;
                    return false;
                }
                break;
            case SaveChoice::DontSave:
                break;
            case SaveChoice::Cancel:
                return false;
        }
    }
    php_open_ = false;
    php_leave_after_save_ = false;
    php_then_ = {};
    for (const auto& edit : php_edits_) {
        if (edit->focused()) {
            SetFocus(hwnd_);  // before its box goes
        }
    }
    php_edits_.clear();
    php_field_edit_.clear();
    php_keys_.clear();
    php_geometry_ = {};
    panel_focus_ = panel_beside_list() ? -1 : 0;
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool MainWindow::save_project_php() {
    if (!php_open_ || php_saving_ || !worker_) {
        return false;
    }
    if (snap_.link != Link::Connected) {
        php_message_ = fill(lang_, Str::PhpSaveFailed, {text(lang_, Str::ConnectionLost)});
        php_message_error_ = true;
        layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }
    ProjectPhp after = php_after_;
    if (!trimmed(php_another_).empty()) {
        const auto line = parse_ini_line(php_another_);
        if (!line) {
            php_message_ = std::string(text(lang_, Str::ProjectPhpBadLine));
            php_message_error_ = true;
            layout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return false;
        }
        set_project_ini(after, line->first, line->second);  // an empty value removes it
    }
    const json params = project_php_params(selected_, php_before_, after);
    if (!params.contains("extensions") && !params.contains("ini")) {
        php_after_ = php_before_;
        php_another_.clear();
        make_project_php_edits();
        return true;
    }
    php_saving_ = true;
    set_project_php_read_only(true);  // what's typed now would be lost when the answer comes
    php_message_.clear();
    worker_->project_php(selected_, params);
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void MainWindow::set_project_php_read_only(bool read_only) {
    for (const auto& edit : php_edits_) {
        edit->set_read_only(read_only);
    }
}

void MainWindow::on_project_php_answers() {
    for (const Snapshot::PhpAnswer& answer : snap_.php) {
        if (answer.to == ServiceWorker::kPhpSettings) {
            continue;
        }
        if (!answer.save) {
            // php.get: kept for the project it was asked for, the tab shown or not.
            if (php_info_project_.empty() ||
                !php_answer_matches(answer.to, answer.series, php_info_project_, php_info_series_)) {
                continue;  // a late answer for a project the user has left
            }
            php_loading_ = false;
            if (!answer.error.empty() || !answer.reply) {
                const std::string series = php_info_series_;
                php_info_project_.clear();  // asked again when the tab is shown again
                if (php_open_) {
                    php_message_ = fill(lang_, Str::PhpReadFailed, {series, answer.error});
                    php_message_error_ = true;
                }
                continue;
            }
            if (auto info = php_series_from_reply(*answer.reply); info && info->series == php_info_series_) {
                php_info_ = std::move(info);
                if (php_open_) {
                    make_project_php_edits();  // the placeholders and warnings
                }
            }
            continue;
        }
        // A save's answer for a project the user has left is dropped, errors too.
        const bool ours = php_open_ && php_answer_matches(answer.to, answer.series, selected_, php_series_);
        if (!ours) {
            if (!answer.error.empty()) {
                show_notice(fill(lang_, Str::ChangeFailed, {answer.to, answer.error}), true);
            }
            continue;
        }
        php_saving_ = false;
        set_project_php_read_only(false);
        if (!answer.error.empty() || !answer.reply) {
            php_leave_after_save_ = false;  // refused: stay with the reason shown
            php_then_ = {};
            std::vector<std::string> keys = php_keys_;
            if (const auto line = parse_ini_line(php_another_)) {
                keys.push_back(line->first);
            }
            php_error_key_ = ini_error_key(answer.error, keys);
            if (!php_error_key_.empty() &&
                std::find(php_keys_.begin(), php_keys_.end(), php_error_key_) == php_keys_.end()) {
                php_error_key_ = " ";  // it was "Another setting": its box gets the mark
            }
            php_message_ = fill(lang_, Str::PhpSaveFailed, {answer.error});
            php_message_error_ = true;
            continue;
        }
        // The project as the service now describes it.
        const auto rows = rows_from_list(json{{"projects", json::array({*answer.reply})}}, lang_);
        if (!rows.empty()) {
            php_before_ = project_php_of(rows.front());
        }
        php_after_ = php_before_;
        php_another_.clear();
        php_error_key_.clear();
        php_message_ = std::string(text(lang_, Str::PhpSaved));
        php_message_error_ = false;
        make_project_php_edits();
        if (std::exchange(php_leave_after_save_, false)) {
            const PanelAct then = std::exchange(php_then_, {});
            (void)leave_php_part(false);
            run_panel_act(then);  // what the Save was chosen on the way to
            sync_panel_tab();     // or the tab that should be shown now
            return;               // the rest of the answers aren't this tab's any more
        }
    }
}

// floserver.json changed some other way (flo php, an editor) while nothing's being changed here.
void MainWindow::sync_project_php() {
    if (!php_open_ || php_saving_ || php_dirty()) {
        return;
    }
    const ProjectRow* row = selected_row();
    if (row == nullptr) {
        return;
    }
    if (ProjectPhp now = project_php_of(*row); now != php_before_) {
        php_before_ = now;
        php_after_ = std::move(now);
        make_project_php_edits();
    }
}

void MainWindow::open_settings_php(const std::string& series) {
    open_settings();
    if (settings_ && settings_->is_open() && !series.empty()) {
        settings_->show_php(series);
    }
}

// --- the boxes -------------------------------------------------------------------------

void MainWindow::make_project_php_edits() {
    std::vector<std::string> keys;
    for (const PhpIniField& field : project_php_ini_fields()) {
        keys.push_back(field.key);
    }
    for (const auto& [key, value] : php_before_.ini) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);  // the project's others get fields of their own
        }
    }
    keys.emplace_back();  // Another setting
    if (keys != php_keys_ || php_edits_.empty()) {
        for (const auto& edit : php_edits_) {
            if (edit->focused()) {
                SetFocus(hwnd_);
            }
        }
        php_edits_.clear();
        php_field_edit_.clear();
        php_keys_ = std::move(keys);
        for (const std::string& key : php_keys_) {
            if (key == "display_errors") {
                php_field_edit_.push_back(-1);  // a picker
                continue;
            }
            const int index = static_cast<int>(php_edits_.size());
            const int id = kPhpEditBase + index;
            auto edit = std::make_unique<FieldEdit>(hwnd_, id, fonts_.button.get(), rtl_,
                                                    [this, id](WPARAM k) { return on_php_edit_key(id, k); });
            edit->set_cue_color(palette(dark_).text_subtle);
            php_field_edit_.push_back(index);
            php_edits_.push_back(std::move(edit));
        }
    }
    // Their text and placeholders: the project's value over the series'.
    for (std::size_t f = 0; f < php_keys_.size(); ++f) {
        FieldEdit* edit = php_edit_of(f);
        if (edit == nullptr) {
            continue;
        }
        const std::string& key = php_keys_[f];
        if (key.empty()) {
            edit->set_cue(text(lang_, Str::ProjectPhpAnotherCue));
            edit->set_text(php_another_);
            continue;
        }
        std::string cue(text(lang_, Str::PhpDefaultCue));
        if (php_info_) {
            if (const auto it = php_info_->ini.find(key); it != php_info_->ini.end()) {
                cue = it->second;
            }
        }
        edit->set_cue(cue);
        const auto value = php_after_.ini.find(key);
        edit->set_text(value != php_after_.ini.end() ? value->second : std::string());
        InvalidateRect(edit->hwnd(), nullptr, TRUE);
    }
    layout();
}

FieldEdit* MainWindow::php_edit_of(std::size_t field) const {
    if (field >= php_field_edit_.size() || php_field_edit_[field] < 0) {
        return nullptr;
    }
    return php_edits_[static_cast<std::size_t>(php_field_edit_[field])].get();
}

// The boxes follow the content's scroll: one partly scrolled out is cut at
// the content's edge, one wholly out is hidden (they're real windows, drawn
// over the panel otherwise).
void MainWindow::place_project_php_edits() {
    const bool shown = php_open_ && panel_open() && !wizard_open() && !www_open() && shown_tab() == PanelTab::Php;
    if (shown) {
        HDC dc = GetDC(hwnd_);
        update_panel_geometry(dc);
        ReleaseDC(hwnd_, dc);
    }
    const RECT& content = panel_geometry_.content;
    for (std::size_t f = 0; f < php_keys_.size(); ++f) {
        FieldEdit* edit = php_edit_of(f);
        if (edit == nullptr) {
            continue;
        }
        const RECT box = shown && f < php_geometry_.boxes.size() ? php_geometry_.boxes[f] : RECT{};
        edit->place(box, px(8), px(20), shown && !is_empty(box), content.top, content.bottom);
    }
}

void MainWindow::on_php_edit_change(int id) {
    const int index = id - kPhpEditBase;
    for (std::size_t f = 0; f < php_keys_.size() && f < php_field_edit_.size(); ++f) {
        if (php_field_edit_[f] != index) {
            continue;
        }
        FieldEdit* edit = php_edit_of(f);
        if (edit == nullptr || edit->setting() || php_saving_) {
            return;
        }
        if (php_keys_[f].empty()) {
            php_another_ = edit->text();
        } else {
            set_project_ini(php_after_, php_keys_[f], edit->text());
        }
        if (php_error_key_ == php_keys_[f] || (php_keys_[f].empty() && php_error_key_ == " ")) {
            php_error_key_.clear();  // being fixed
        }
        InvalidateRect(edit->hwnd(), nullptr, TRUE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
}

void MainWindow::on_php_edit_focus(int id) {
    const int index = id - kPhpEditBase;
    const std::vector<Hit> order = panel_focus_order();
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i].part == Part::PanelPhpField && order[i].row >= 0 &&
            std::cmp_less(order[i].row, php_field_edit_.size()) &&
            php_field_edit_[static_cast<std::size_t>(order[i].row)] == index) {
            panel_focus_ = static_cast<int>(i);
            ensure_panel_visible(order[i]);  // a box scrolled half out comes into view
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::on_php_edit_key(int id, WPARAM key) {
    if (key == VK_ESCAPE) {
        // Back to the tab bar (after this message); Escape there closes the panel.
        PostMessageW(hwnd_, kPhpBackMessage, 0, 0);
        return true;
    }
    if (key == VK_RETURN) {
        return true;  // kept as typed; Save sends it
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        // Ctrl+Tab: another tab, after this message (the box goes with this one).
        PostMessageW(hwnd_, kStepTabMessage, (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 0 : 1, 0);
        return true;
    }
    on_php_edit_focus(id);
    SetFocus(hwnd_);
    on_panel_key(VK_TAB);
    return true;
}

// --- geometry -----------------------------------------------------------------------------

std::vector<std::pair<std::string, bool>> MainWindow::php_chips() const {
    std::vector<std::pair<std::string, bool>> out;
    for (const auto& [name, on] : php_after_.extensions) {
        if (on) {
            out.emplace_back(name, true);
        }
    }
    for (const auto& [name, on] : php_after_.extensions) {
        if (!on) {
            out.emplace_back(name, false);
        }
    }
    return out;
}

MainWindow::PhpPanelGeometry MainWindow::php_panel_geometry(HDC dc) const {
    PhpPanelGeometry g;
    const ProjectRow* row = selected_row();
    if (row == nullptr) {
        return g;
    }
    g.project = row->name;
    g.panel = panel_geometry_.panel;
    g.content = panel_geometry_.content;
    const int x0 = g.panel.left + px(kPanelPad);
    const int x1 = g.panel.right - px(kPanelPad);
    const int width = x1 - x0;
    const int top = g.content.top + px(kPanelGap);
    int y = top - panel_scroll_;
    const auto measure = [&](std::string_view s, HFONT font, bool code = false) {
        return Canvas::measure(dc, w(s), font, rtl_ && !code);
    };

    // The PHP it uses, and its settings in Settings.
    g.line2 = {x0, y, x1, y + px(20)};
    y = g.line2.bottom + px(8);
    if (!php_series_.empty()) {
        const int bw = measure(fill(lang_, Str::ProjectPhpOpenSettings, {php_series_}), fonts_.button.get()) + px(24);
        g.open_settings = {x0, y, std::min(x1, x0 + bw), y + px(kButtonH)};
        y += px(kButtonH) + px(kSection);
    }

    // A section's title with its hint after it, or under it when it doesn't fit.
    const auto heading = [&](RECT& title, RECT& hint, std::string_view title_text, const std::string& hint_text,
                             bool code) {
        const int tw = measure(title_text, fonts_.button_bold.get(), code) + px(2);
        title = {x0, y, std::min(x1, x0 + tw), y + px(18)};
        const int hw = measure(hint_text, fonts_.caption.get());
        if (tw + px(8) + hw <= width) {
            hint = {title.right + px(8), y, x1, y + px(18)};
            y += px(18);
        } else {
            const int hh = text_height(dc, w(hint_text), fonts_.caption.get(), width, rtl_);
            hint = {x0, y + px(18), x1, y + px(18) + hh};
            y = hint.bottom;
        }
        y += px(8);
    };
    heading(g.ext_title, g.ext_hint, text(lang_, Str::PhpExtensions),
            fill(lang_, Str::ProjectPhpExtHint, {php_series_}), false);

    // The chips, wrapping, then Change….
    int x = x0;
    const auto place = [&](int cw) {
        cw = std::min(cw, width);
        if (x > x0 && x + cw > x1) {
            x = x0;
            y += px(kPillH) + px(6);
        }
        const RECT r{x, y, x + cw, y + px(kPillH)};
        x += cw + px(6);
        return r;
    };
    for (const auto& [name, on] : php_chips()) {
        g.chips.push_back(place(measure(on ? "+ " + name : name, fonts_.button.get(), true) + px(20)));
    }
    if (g.chips.empty()) {
        g.no_changes = place(measure(text(lang_, Str::ProjectPhpNoChanges), fonts_.caption.get()) + px(4));
    }
    g.change = place(measure(text(lang_, Str::ProjectPhpChange), fonts_.button.get()) + px(24));
    y += px(kPillH) + px(kSection);

    heading(g.ini_title, g.ini_hint, "php.ini", fill(lang_, Str::ProjectPhpIniHint, {php_series_}), true);
    const int cols = width >= px(520) ? 3 : 2;
    const int col_w = (width - (cols - 1) * px(kGap)) / cols;
    const int cell_h = px(kLabelH) + px(4) + px(kBoxH);
    // A name PHP doesn't know gets a line under its row of fields (the whole
    // width: the words are longer than a field), one per such field.
    std::vector<std::string> notes(php_keys_.size());
    if (php_info_) {
        for (std::size_t i = 0; i < php_keys_.size(); ++i) {
            std::string key = php_keys_[i];
            if (key.empty()) {
                if (const auto line = parse_ini_line(php_another_)) {
                    key = line->first;
                }
            }
            if (const auto suggestion = unknown_ini_name(php_info_->known_ini, key)) {
                notes[i] = unknown_ini_text(php_series_, key, *suggestion, lang_);
            }
        }
    }
    g.warned.assign(php_keys_.size(), -1);
    const int lines = (static_cast<int>(php_keys_.size()) + cols - 1) / cols;
    for (int line = 0; line < lines; ++line) {
        for (int col = 0; col < cols; ++col) {
            const auto i = static_cast<std::size_t>(line * cols + col);
            if (i >= php_keys_.size()) {
                break;
            }
            const int left = x0 + col * (col_w + px(kGap));
            g.labels.push_back({left, y, left + col_w, y + px(kLabelH)});
            g.boxes.push_back({left, y + px(kLabelH) + px(4), left + col_w, y + cell_h});
        }
        y += cell_h;
        for (int col = 0; col < cols; ++col) {
            const auto i = static_cast<std::size_t>(line * cols + col);
            if (i >= php_keys_.size() || notes[i].empty()) {
                continue;
            }
            const int nh = text_height(dc, w(notes[i]), fonts_.caption.get(), width, rtl_);
            g.warned[i] = static_cast<int>(g.warnings.size());
            g.warnings.emplace_back(RECT{x0, y + px(4), x1, y + px(4) + nh}, notes[i]);
            y += px(4) + nh;
        }
        y += px(kGap);
    }
    y += px(kSection) - px(kGap);

    const std::string info = fill(lang_, Str::ProjectPhpSavedIn, {floserver::core::kProjectConfigFileName});
    const int ih = text_height(dc, w(info), fonts_.caption.get(), width - px(24), rtl_);
    g.info = {x0, y, x1, y + ih + px(20)};
    y = g.info.bottom + px(kGap);
    if (!php_message_.empty()) {
        const int mh = text_height(dc, w(php_message_), fonts_.caption.get(), width - px(24), rtl_) + px(14);
        g.message = {x0, y, x1, y + mh};
        y = g.message.bottom + px(kGap);
    }

    // The buttons at the content's bottom when there's room, else after the rest.
    const int bottom_top = static_cast<int>(g.content.bottom) - px(kPanelPad) - px(kButtonH);
    const int buttons_top = panel_scroll_ == 0 ? std::max(y, bottom_top) : y;
    const int rw = measure(text(lang_, Str::ProjectPhpReset), fonts_.button.get()) + px(24);
    const int sw = std::max(measure(text(lang_, Str::PhpSave), fonts_.button.get()),
                            measure(text(lang_, Str::PhpSaving), fonts_.button.get())) +
                   px(24);
    const int dw = measure(text(lang_, Str::PhpDiscard), fonts_.button.get()) + px(24);
    g.save = {x1 - sw, buttons_top, x1, buttons_top + px(kButtonH)};
    g.discard = {static_cast<int>(g.save.left) - px(8) - dw, buttons_top, static_cast<int>(g.save.left) - px(8),
                 buttons_top + px(kButtonH)};
    g.reset = {x0, buttons_top, std::min(x0 + rw, static_cast<int>(g.discard.left) - px(8)), buttons_top + px(kButtonH)};
    // The height without the buttons pushed down: it only scrolls when it doesn't fit.
    g.content_h = y + px(kButtonH) + px(kPanelPad) + panel_scroll_ - static_cast<int>(g.content.top);
    return g;
}

MainWindow::Hit MainWindow::php_panel_hit_test(int x, int y) const {
    const PhpPanelGeometry& g = php_geometry_;
    const auto hit = [&](Part part, int index) { return Hit{part, index, g.project}; };
    for (const auto& [part, r] : {std::pair{Part::PanelPhpSettings, g.open_settings}, std::pair{Part::PanelPhpChange, g.change},
                                  std::pair{Part::PanelPhpReset, g.reset}, std::pair{Part::PanelPhpDiscard, g.discard},
                                  std::pair{Part::PanelPhpSave, g.save}}) {
        if (contains(r, x, y)) {
            return hit(part, -1);
        }
    }
    for (std::size_t i = 0; i < g.boxes.size(); ++i) {
        if (contains(g.boxes[i], x, y)) {
            return hit(Part::PanelPhpField, static_cast<int>(i));
        }
    }
    return {};
}

void MainWindow::add_php_focus_order(std::vector<Hit>& order) const {
    const PhpPanelGeometry& g = php_geometry_;
    if (g.project.empty()) {
        return;
    }
    if (!is_empty(g.open_settings)) {
        order.push_back({Part::PanelPhpSettings, -1, g.project});
    }
    order.push_back({Part::PanelPhpChange, -1, g.project});
    for (std::size_t i = 0; i < g.boxes.size(); ++i) {
        order.push_back({Part::PanelPhpField, static_cast<int>(i), g.project});
    }
    order.push_back({Part::PanelPhpReset, -1, g.project});
    order.push_back({Part::PanelPhpDiscard, -1, g.project});
    order.push_back({Part::PanelPhpSave, -1, g.project});
}

// --- input -------------------------------------------------------------------------------

void MainWindow::on_php_panel_click(const Hit& hit) {
    if (!php_open_ || hit.project != selected_) {
        return;
    }
    switch (hit.part) {
        case Part::PanelPhpSettings:
            open_settings_php(php_series_);
            break;
        case Part::PanelPhpChange:
            if (!php_saving_) {
                php_change_menu();
            }
            break;
        case Part::PanelPhpField:
            if (hit.row >= 0) {
                if (FieldEdit* edit = php_edit_of(static_cast<std::size_t>(hit.row)); edit != nullptr) {
                    SetFocus(edit->hwnd());
                } else if (!php_saving_) {
                    php_display_menu(static_cast<std::size_t>(hit.row));
                }
            }
            break;
        case Part::PanelPhpReset:
            if (php_saving_) {
                break;
            }
            php_after_ = {};
            php_another_.clear();
            php_error_key_.clear();
            make_project_php_edits();
            break;
        case Part::PanelPhpDiscard:
            if (php_saving_) {
                break;
            }
            php_after_ = php_before_;
            php_another_.clear();
            php_error_key_.clear();
            php_message_.clear();
            make_project_php_edits();
            break;
        case Part::PanelPhpSave:
            (void)save_project_php();
            break;
        default:
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// A checklist: each pick turns one on or off and the list opens again,
// until it's closed without a pick.
void MainWindow::php_change_menu() {
    if (!php_info_ || php_info_->extensions_fixed) {
        return;
    }
    const std::string project = selected_;
    const RECT box = php_geometry_.change;
    for (;;) {
        if (!php_open_ || selected_ != project || !php_info_) {
            return;
        }
        const std::vector<PhpExtension> list = ordered_extensions(*php_info_);
        const win::UniqueMenu menu(CreatePopupMenu());
        if (!menu || list.empty()) {
            return;
        }
        std::vector<std::wstring> labels;
        labels.reserve(list.size());
        for (std::size_t i = 0; i < list.size(); ++i) {
            const bool on = project_extension_on(&*php_info_, php_after_, list[i].name);
            labels.push_back(w(list[i].name));
            UINT flags = MF_STRING | (on ? UINT{MF_CHECKED} : 0U);
            if (i > 0 && i % kMenuColumn == 0) {
                flags |= MF_MENUBARBREAK;
            }
            if (list[i].missing && !on) {
                flags |= MF_GRAYED;  // this install doesn't have it
            }
            AppendMenuW(menu.get(), flags, kFirstChoice + static_cast<UINT>(i), labels.back().c_str());
        }
        POINT at{box.left, box.bottom};
        ClientToScreen(hwnd_, &at);
        UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
        flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
        const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
        if (command < kFirstChoice || command - kFirstChoice >= list.size() || !php_open_ || selected_ != project ||
            !php_info_) {
            return;
        }
        toggle_project_extension(&*php_info_, php_after_, list[command - kFirstChoice].name);
        layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
}

void MainWindow::php_display_menu(std::size_t index) {
    if (index >= php_keys_.size() || index >= php_geometry_.boxes.size()) {
        return;
    }
    const std::string key = php_keys_[index];
    std::string series_value;
    if (php_info_) {
        if (const auto it = php_info_->ini.find(key); it != php_info_->ini.end()) {
            series_value = it->second;
        }
    }
    const auto own = php_after_.ini.find(key);
    const std::string current = own != php_after_.ini.end() ? own->second : std::string();
    std::vector<std::string> values{"", "On", "Off"};
    if (!current.empty() && std::find(values.begin(), values.end(), current) == values.end()) {
        values.push_back(current);
    }
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    std::vector<std::wstring> labels;
    for (std::size_t i = 0; i < values.size(); ++i) {
        labels.push_back(w(values[i].empty() ? fill(lang_, Str::ProjectPhpDefaultChoice,
                                                    {series_value.empty() ? std::string("?") : series_value})
                                             : values[i]));
        AppendMenuW(menu.get(), MF_STRING | (values[i] == current ? UINT{MF_CHECKED} : 0U),
                    kFirstChoice + static_cast<UINT>(i), labels.back().c_str());
    }
    const RECT box = php_geometry_.boxes[index];
    POINT at{box.left, box.bottom};
    ClientToScreen(hwnd_, &at);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
    flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    if (command < kFirstChoice || command - kFirstChoice >= values.size() || !php_open_) {
        return;
    }
    set_project_ini(php_after_, key, values[command - kFirstChoice]);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- painting -------------------------------------------------------------------------------

void MainWindow::paint_php_panel(Canvas& c, bool text_pass) const {
    const PhpPanelGeometry& g = php_geometry_;
    const ProjectRow* row = selected_row();
    if (row == nullptr || g.project != row->name || height_of(g.panel) <= 0) {
        return;
    }
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    const bool keys = GetFocus() == hwnd_ && show_focus_;
    const std::vector<Hit> order = panel_focus_order();
    const auto focused = [&](Part part, int index) {
        return keys && panel_focus_ >= 0 && std::cmp_less(panel_focus_, order.size()) &&
               order[static_cast<std::size_t>(panel_focus_)].part == part &&
               order[static_cast<std::size_t>(panel_focus_)].row == index;
    };
    const auto hovered = [&](Part part, int index) { return hover_.part == part && hover_.row == index; };
    const auto ring = [&](const RECT& r, float radius) {
        RECT around = r;
        InflateRect(&around, px(2), px(2));
        c.round_outline(around, radius, p.focus, pxf(2));
    };
    const bool dirty = php_dirty();
    const bool can_save = dirty && !php_saving_ && snap_.link == Link::Connected;
    const bool can_reset = !php_after_.extensions.empty() || !php_after_.ini.empty() || !trimmed(php_another_).empty();
    const bool can_change = php_info_ && !php_info_->extensions_fixed;
    const std::vector<std::pair<std::string, bool>> chips = php_chips();

    // Drawn by paint_panel inside the content, which it has clipped to.
    if (!text_pass) {
        if (!is_empty(g.open_settings)) {
            paint_button(c, g.open_settings, hovered(Part::PanelPhpSettings, -1), focused(Part::PanelPhpSettings, -1));
        }
        const float pill = pxf(static_cast<float>(kPillH) / 2.0F);
        for (std::size_t i = 0; i < g.chips.size() && i < chips.size(); ++i) {
            const RECT& r = g.chips[i];
            if (chips[i].second) {
                c.round_rect(r, pill, p.chip_added.fill, p.chip_added.border, border);
            } else {
                c.round_rect(r, pill, p.input, p.button_border, border);
                // Crossed out: off for this project.
                const float mid = static_cast<float>(r.top + r.bottom) / 2.0F;
                c.line(static_cast<float>(r.left) + pxf(10), mid, static_cast<float>(r.right) - pxf(10), mid,
                       p.text_subtle, pxf(1));
            }
        }
        c.round_rect(g.change, pill, can_change && hovered(Part::PanelPhpChange, -1) ? p.row_hover : p.button,
                     p.button_border, border);
        if (focused(Part::PanelPhpChange, -1)) {
            ring(g.change, pill + pxf(2));
        }
        for (std::size_t i = 0; i < g.boxes.size() && i < php_keys_.size(); ++i) {
            const int index = static_cast<int>(i);
            const std::string& key = php_keys_[i];
            const bool marked = !key.empty() && php_after_.ini.contains(key);
            const bool refused = !php_error_key_.empty() && (php_error_key_ == key || (key.empty() && php_error_key_ == " "));
            const bool unknown = i < g.warned.size() && g.warned[i] >= 0;
            const FieldEdit* edit = php_edit_of(i);
            const Rgb outline = refused ? p.banner_error.text : unknown ? p.asleep.text : marked ? p.accent : p.input_border;
            c.round_rect(g.boxes[i], pxf(4),
                         marked ? p.input_set : (edit == nullptr && hovered(Part::PanelPhpField, index) ? p.row_hover : p.input),
                         outline, refused || unknown ? pxf(2) : border);
            if (edit != nullptr && edit->focused()) {
                c.round_outline(g.boxes[i], pxf(4), p.accent, pxf(2));
            } else if (focused(Part::PanelPhpField, index)) {
                ring(g.boxes[i], pxf(6));
            }
            if (edit == nullptr) {
                const float vx = static_cast<float>(g.boxes[i].right) - pxf(14);
                const float vy = static_cast<float>(g.boxes[i].top + g.boxes[i].bottom) / 2.0F;
                c.line(vx - pxf(4), vy - pxf(2), vx, vy + pxf(2), p.text_subtle, pxf(1.3F));
                c.line(vx, vy + pxf(2), vx + pxf(4), vy - pxf(2), p.text_subtle, pxf(1.3F));
            }
        }
        c.round_rect(g.info, pxf(6), p.panel_box);
        if (!is_empty(g.message)) {
            c.round_rect(g.message, pxf(4), php_message_error_ ? p.banner_error.fill : p.banner_info.fill);
        }
        paint_button(c, g.reset, can_reset && hovered(Part::PanelPhpReset, -1), focused(Part::PanelPhpReset, -1));
        paint_button(c, g.discard, dirty && hovered(Part::PanelPhpDiscard, -1), focused(Part::PanelPhpDiscard, -1));
        if (can_save) {
            c.round_rect(g.save, pxf(4), p.accent);
            if (focused(Part::PanelPhpSave, -1)) {
                ring(g.save, pxf(6));
            }
        } else {
            c.round_rect(g.save, pxf(4), p.button_disabled, p.button_border, border);
            if (focused(Part::PanelPhpSave, -1)) {
                ring(g.save, pxf(6));
            }
        }
        return;
    }

    std::string line2(text(lang_, Str::PanelPhpSettings));
    if (php_info_ && !php_info_->version.empty()) {
        line2 += " · " + (rtl_ ? isolate_ltr("PHP " + php_info_->version) : "PHP " + php_info_->version);
    } else if (php_series_.empty()) {
        line2 = std::string(text(lang_, Str::ProjectPhpNoSeries));
    } else if (php_loading_) {
        line2 += " · " + std::string(text(lang_, Str::SettingsLoading));
    }
    c.text(g.line2, w(line2), fonts_.button.get(), p.text_subtle, Align::Start, false);
    if (!is_empty(g.open_settings)) {
        c.text(g.open_settings, w(fill(lang_, Str::ProjectPhpOpenSettings, {php_series_})), fonts_.button.get(), p.text,
               Align::Center, false);
    }
    c.text(g.ext_title, w(text(lang_, Str::PhpExtensions)), fonts_.button_bold.get(), p.text, Align::Start, false);
    c.paragraph(g.ext_hint, w(fill(lang_, Str::ProjectPhpExtHint, {php_series_})), fonts_.caption.get(), p.text_subtle);
    for (std::size_t i = 0; i < g.chips.size() && i < chips.size(); ++i) {
        const auto& [name, on] = chips[i];
        c.text(g.chips[i], w(on ? "+ " + name : name), fonts_.button.get(), on ? p.chip_added.text : p.text_subtle,
               Align::Center, true);
    }
    if (!is_empty(g.no_changes)) {
        c.text(g.no_changes, w(text(lang_, Str::ProjectPhpNoChanges)), fonts_.caption.get(), p.text_subtle,
               Align::Start, false);
    }
    c.text(g.change, w(text(lang_, Str::ProjectPhpChange)), fonts_.button.get(),
           can_change ? p.text : p.button_disabled_text, Align::Center, false);
    c.text(g.ini_title, L"php.ini", fonts_.button_bold.get(), p.text, Align::Start, true);
    c.paragraph(g.ini_hint, w(fill(lang_, Str::ProjectPhpIniHint, {php_series_})), fonts_.caption.get(), p.text_subtle);
    for (std::size_t i = 0; i < g.boxes.size() && i < php_keys_.size(); ++i) {
        const std::string& key = php_keys_[i];
        if (key.empty()) {
            c.text(g.labels[i], w(text(lang_, Str::ProjectPhpAnother)), fonts_.caption.get(), p.text_subtle,
                   Align::Start, false);
        } else {
            c.text(g.labels[i], w(key), fonts_.caption.get(), p.text_subtle, Align::Start, true);
        }
        if (php_edit_of(i) != nullptr) {
            continue;  // its box draws the text
        }
        RECT inner = g.boxes[i];
        inner.left += px(8);
        inner.right -= px(26);
        const auto own = php_after_.ini.find(key);
        if (own != php_after_.ini.end()) {
            c.text(inner, w(own->second), fonts_.button.get(), p.text, Align::Start, true);
        } else {
            std::string series_value = "?";
            if (php_info_) {
                if (const auto it = php_info_->ini.find(key); it != php_info_->ini.end()) {
                    series_value = it->second;
                }
            }
            c.text(inner, w(fill(lang_, Str::ProjectPhpDefaultChoice, {series_value})), fonts_.button.get(),
                   p.text_subtle, Align::Start, false);
        }
    }
    {
        RECT inner = g.info;
        InflateRect(&inner, -px(12), -px(10));
        c.paragraph(inner, w(fill(lang_, Str::ProjectPhpSavedIn, {floserver::core::kProjectConfigFileName})),
                    fonts_.caption.get(), p.text);
    }
    if (!is_empty(g.message)) {
        RECT inner = g.message;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(php_message_), fonts_.caption.get(),
                    php_message_error_ ? p.banner_error.text : p.banner_info.text);
    }
    c.text(g.reset, w(text(lang_, Str::ProjectPhpReset)), fonts_.button.get(),
           can_reset ? p.text : p.button_disabled_text, Align::Center, false);
    c.text(g.discard, w(text(lang_, Str::PhpDiscard)), fonts_.button.get(), dirty ? p.text : p.button_disabled_text,
           Align::Center, false);
    c.text(g.save, w(text(lang_, php_saving_ ? Str::PhpSaving : Str::PhpSave)), fonts_.button.get(),
           can_save ? p.accent_text : p.button_disabled_text, Align::Center, false);
    for (const auto& [r, words] : g.warnings) {
        c.paragraph(r, w(words), fonts_.caption.get(), p.asleep.text);
    }
}

}  // namespace floserver::ui

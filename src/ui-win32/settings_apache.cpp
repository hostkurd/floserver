// Settings > Apache: the shared Apache behind nginx (D6). Its version, its
// modules as a grid of checkboxes with a find box (FloServer's own ticked
// and greyed, those that don't apply behind nginx greyed and empty, one an
// on module needs ticked for it), and the user's own httpd.conf lines
// (custom.conf) and the final httpd.conf opened in Notepad. Built as
// Settings > PHP is: one apache.get when the section opens (the service
// reads the modules folder then), changes wait for Save (one apache.set),
// and nothing polls.

#include "settings_window.h"

#include "main_window_layout.h"
#include "model/theme.h"

#include <shellapi.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace floserver::ui {

using nlohmann::json;

namespace {

constexpr int kApacheEditBase = 4000;  // apache_edits_[0], the find box
constexpr UINT kFirstChoice = 100;
constexpr int kGap = 12;
constexpr int kCardPad = 16;
constexpr int kControlH = 30;
constexpr int kCellH = 28;
constexpr int kCellMinW = 170;
constexpr int kCellGap = 16;
constexpr int kNoteH = 18;
constexpr int kCheckBox = 16;
constexpr std::size_t kShownModules = 24;

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

std::string joined(const std::vector<std::string>& lines) {
    std::string all;
    for (const std::string& line : lines) {
        all += (all.empty() ? "" : "; ") + line;
    }
    return all;
}

}  // namespace

// --- loading, saving, leaving ----------------------------------------------------------

bool SettingsWindow::settle_apache_changes(AfterSave then) {
    if (!apache_ || apache_changes_.empty()) {
        return true;
    }
    if (asking_ || apache_saving_) {
        return false;
    }
    asking_ = true;
    const SaveChoice choice = ask_to_save(hwnd_, lang_, std::string(text(lang_, Str::ApacheAskSave)));
    asking_ = false;
    if (quit_after_ask_) {
        PostMessageW(owner_, closed_message_, 1, 0);
        return false;
    }
    if (hwnd_ == nullptr) {
        return false;
    }
    switch (choice) {
        case SaveChoice::Save:
            if (save_apache()) {
                after_save_ = then;
            }
            return false;
        case SaveChoice::DontSave:
            discard_apache();
            return true;
        case SaveChoice::Cancel:
            break;
    }
    return false;
}

void SettingsWindow::load_apache() {
    apache_.reset();
    apache_changes_ = {};
    apache_all_ = false;
    apache_loading_ = true;
    // The one request the section makes; the service reads the modules folder for it.
    worker_.database_call("apache.get", json::object());
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::save_apache() {
    if (!apache_ || apache_changes_.empty() || apache_saving_) {
        return false;
    }
    if (!connected_) {
        show_message(fill(lang_, Str::PhpSaveFailed, {text(lang_, Str::ConnectionLost)}), true);
        return false;
    }
    apache_saving_ = true;
    message_.clear();
    worker_.database_call("apache.set", apache_set_params(apache_changes_));
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void SettingsWindow::discard_apache() {
    if (apache_saving_) {
        return;
    }
    apache_changes_ = {};
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::on_apache_answers(const Snapshot& snap) {
    bool close_now = false;
    const auto service_words = [this](const std::string& s) { return rtl_ ? isolate_ltr(s) : s; };
    for (const Snapshot::DbAnswer& answer : snap.db) {
        if (!answer.method.starts_with("apache.")) {
            continue;
        }
        const bool save = answer.method == "apache.set";
        if (save) {
            apache_saving_ = false;
        }
        if (answer.method == "apache.get" || save) {
            if (!answer.error.empty() || !answer.reply) {
                if (save) {
                    after_save_ = AfterSave::Stay;  // refused: stay here with the reason shown
                    show_message(fill(lang_, Str::PhpSaveFailed, {service_words(answer.error)}), true);
                } else {
                    apache_loading_ = false;
                    show_message(fill(lang_, Str::ApacheReadFailed, {service_words(answer.error)}), true);
                }
                continue;
            }
            auto info = apache_info_from_reply(*answer.reply);
            if (!info) {
                continue;
            }
            const bool restarting = info->restarting;
            apache_ = std::move(info);
            apache_loading_ = false;
            if (save) {
                apache_changes_ = {};
                show_message(std::string(text(lang_, restarting ? Str::ApacheRestarting : Str::PhpSaved)), false);
                const AfterSave then = std::exchange(after_save_, AfterSave::Stay);
                if (then == AfterSave::Close) {
                    close_now = true;
                } else if (then == AfterSave::Runtimes) {
                    open_section(after_save_section_);
                }
            }
            continue;
        }
        // apache.custom_conf / apache.final_conf: Notepad, to edit or to read.
        const std::string path = answer.reply && answer.reply->is_object() && answer.reply->contains("path") &&
                                         (*answer.reply)["path"].is_string()
                                     ? (*answer.reply)["path"].get<std::string>()
                                     : std::string();
        if (path.empty()) {
            show_message(fill(lang_, Str::ApacheOpenFailed, {service_words(answer.error)}), true);
            continue;
        }
        const std::wstring quoted = L"\"" + w(path) + L"\"";
        const auto opened = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL));
        if (opened <= 32) {
            show_message(fill(lang_, Str::ApacheOpenFailed, {path}), true);
        } else {
            show_message(std::string(text(lang_, answer.method == "apache.final_conf" ? Str::ApacheFinalOpened
                                                                                      : Str::ApacheConfOpened)),
                         false);
            if (answer.method == "apache.custom_conf" && apache_) {
                apache_->custom_conf_exists = true;
            }
        }
    }
    return close_now;
}

// --- the find box ------------------------------------------------------------------

void SettingsWindow::make_apache_edits() {
    if (!apache_edits_.empty()) {
        return;
    }
    auto edit = std::make_unique<FieldEdit>(hwnd_, kApacheEditBase, fonts_.button.get(), rtl_,
                                            [this](WPARAM key) { return on_edit_key(kApacheEditBase, key); });
    edit->set_cue(text(lang_, Str::ApacheFindModule));
    edit->set_cue_color(palette(dark_).text_subtle);
    edit->set_text(apache_query_);
    apache_edits_.push_back(std::move(edit));
}

void SettingsWindow::place_apache_edits() {
    if (section_ != Section::Apache || apache_edits_.empty()) {
        return;
    }
    const RECT& find = geometry_.apache.find;
    apache_edits_.front()->place(find, px(8), px(20), apache_.has_value() && !is_empty(find));
}

// --- geometry ------------------------------------------------------------------------

std::vector<ApacheModule> SettingsWindow::shown_apache_modules(std::size_t& hidden) const {
    hidden = 0;
    if (!apache_) {
        return {};
    }
    std::vector<ApacheModule> all = ordered_apache_modules(*apache_);  // as saved: nothing jumps on a click
    if (!apache_query_.empty()) {
        std::erase_if(all, [&](const ApacheModule& m) { return !extension_matches(m.name, apache_query_); });
        return all;
    }
    if (apache_all_ || all.size() <= kShownModules + 4) {
        return all;
    }
    hidden = all.size() - kShownModules;
    all.resize(kShownModules);
    return all;
}

void SettingsWindow::layout_apache(HDC dc, Geometry& g, int x0, int x1, int y) const {
    ApacheGeometry& ag = g.apache;
    const auto measure = [&](std::string_view s, HFONT font) { return Canvas::measure(dc, w(s), font, rtl_); };
    const auto button_w = [&](Str label) { return measure(text(lang_, label), fonts_.button.get()) + px(24); };

    // The title and what the section does, with the version picker beside them.
    int picker_w = 0;
    int label_w = 0;
    if (apache_ && !apache_->versions.empty()) {
        picker_w = px(150);
        picker_w = std::max(picker_w, measure(apache_choice_label(*apache_, std::nullopt, lang_), fonts_.button.get()) + px(34));
        for (const std::string& v : apache_->versions) {
            picker_w = std::max(picker_w, Canvas::measure(dc, w("Apache " + v), fonts_.button.get(), false) + px(34));
        }
        label_w = measure(text(lang_, Str::ApacheVersion), fonts_.caption.get());
    }
    const int right_w = picker_w > 0 ? picker_w + px(8) + label_w + px(kGap) : 0;
    const int hint_w = std::max(px(100), x1 - x0 - right_w);
    const int hint_h = text_height(dc, w(text(lang_, Str::ApacheHint)), fonts_.caption.get(), hint_w, rtl_);
    g.title = {x0, y, x0 + hint_w, y + px(28)};
    g.hint = {x0, g.title.bottom, x0 + hint_w, g.title.bottom + hint_h};
    if (picker_w > 0) {
        const int top = std::max(y, static_cast<int>(g.hint.bottom) - px(kControlH));
        ag.version = {x1 - picker_w, top, x1, top + px(kControlH)};
        ag.version_label = {static_cast<int>(ag.version.left) - px(8) - label_w, top,
                            static_cast<int>(ag.version.left) - px(8), top + px(kControlH)};
    }
    y = std::max(g.hint.bottom, ag.version.bottom) + px(kGap);

    if (!message_.empty()) {
        const int mh = std::max(px(32), text_height(dc, w(message_), fonts_.button.get(), x1 - x0 - px(24), rtl_) + px(14));
        g.message = {x0, y, x1, y + mh};
        y += mh + px(kGap);
    }
    if (!apache_) {
        ag.note = {x0, y, x1, y + px(24)};
        g.content_h = ag.note.bottom + scroll_ + px(8);
        return;
    }
    if (apache_->versions.empty()) {
        const int nh = text_height(dc, w(text(lang_, Str::ApacheNoneInstalled)), fonts_.button.get(), x1 - x0, rtl_);
        ag.note = {x0, y, x1, y + nh};
        y += nh + px(kGap);
        ag.runtimes = {x0, y, x0 + button_w(Str::PhpGoToRuntimes), y + px(kControlH)};
        g.content_h = ag.runtimes.bottom + scroll_ + px(8);
        return;
    }
    // What Apache refused (the running one keeps its last good config), then what was skipped.
    const auto banner = [&](RECT& r, const std::string& words) {
        const int h = text_height(dc, w(words), fonts_.caption.get(), x1 - x0 - px(24), rtl_) + px(14);
        r = {x0, y, x1, y + h};
        y += h + px(kGap);
    };
    if (!apache_->refused.empty()) {
        banner(ag.refused, fill(lang_, Str::ApacheRefused, {apache_->refused}));
    }
    if (!apache_->problems.empty()) {
        banner(ag.problems, fill(lang_, Str::ApacheProblems, {joined(apache_->problems)}));
    }

    // Modules.
    const int cx0 = x0 + px(kCardPad);
    const int cx1 = x1 - px(kCardPad);
    const int avail = cx1 - cx0;
    int top = y + px(14);
    ag.modules_card = {x0, y, x1, y};
    const int find_w = std::min(px(200), avail / 2);
    ag.find = {cx1 - find_w, top, cx1, top + px(kControlH)};
    const int title_w = measure(text(lang_, Str::ApacheModules), fonts_.body_bold.get());
    const int title_end = static_cast<int>(ag.find.left) - px(kGap);
    ag.modules_title = {cx0, top, std::min(title_end, cx0 + title_w), top + px(kControlH)};
    ag.count = {std::min(title_end, static_cast<int>(ag.modules_title.right) + px(8)), top, title_end, top + px(kControlH)};
    int yy = top + px(kControlH) + px(kGap);

    std::size_t hidden = 0;
    const std::vector<ApacheModule> shown = shown_apache_modules(hidden);
    const int cols = std::clamp((avail + px(kCellGap)) / (px(kCellMinW) + px(kCellGap)), 1, 4);
    const int col_w = (avail - (cols - 1) * px(kCellGap)) / cols;
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const int left = cx0 + static_cast<int>(i % static_cast<std::size_t>(cols)) * (col_w + px(kCellGap));
        const int cell_top = yy + static_cast<int>(i / static_cast<std::size_t>(cols)) * px(kCellH);
        const RECT cell{left, cell_top, left + col_w, cell_top + px(kCellH)};
        ag.cells.push_back(cell);
        ag.names.push_back(shown[i].name);
        // The few words after the name, when there's room.
        RECT extra{};
        const std::string note = apache_module_note(*apache_, apache_changes_, shown[i], lang_);
        if (!note.empty()) {
            const int name_end = left + px(2) + px(kCheckBox) + px(8) +
                                 Canvas::measure(dc, w(shown[i].name), fonts_.button.get(), false) + px(6);
            if (name_end < cell.right) {
                extra = {name_end, cell_top, static_cast<int>(cell.right), static_cast<int>(cell.bottom)};
            }
        }
        ag.extras.push_back(extra);
        ag.notes.push_back(note);
    }
    const int lines = (static_cast<int>(shown.size()) + cols - 1) / cols;
    yy += lines * px(kCellH);
    if (lines > 0) {
        yy += px(8);
    }
    // The note under the grid, with "show all" after it.
    std::string link;
    if (apache_query_.empty()) {
        if (hidden > 0) {
            link = std::string(text(lang_, Str::PhpShowAll));
        } else if (apache_all_ && apache_->modules.size() > kShownModules + 4) {
            link = std::string(text(lang_, Str::PhpShowFewer));
        }
    }
    const int link_w = link.empty() ? 0 : measure(link, fonts_.caption.get()) + px(4);
    std::string note_text = !apache_query_.empty() && shown.empty() ? fill(lang_, Str::ApacheNoMatch, {apache_query_})
                                                                      : std::string(text(lang_, Str::ApacheModulesNote));
    if (hidden > 0) {
        note_text += " " + fill(lang_, Str::PhpMoreCount, {std::to_string(hidden)});
    }
    // The note wraps when it's longer than the card; "show all" goes on the line after it.
    const int note_h = std::max(px(kNoteH), text_height(dc, w(note_text), fonts_.caption.get(), avail, rtl_));
    ag.modules_note = {cx0, yy, cx1, yy + note_h};
    yy += note_h;
    if (!link.empty()) {
        ag.more = {cx0, yy + px(2), cx0 + link_w, yy + px(2) + px(kNoteH)};
        yy += px(2) + px(kNoteH);
    }
    ag.modules_card.bottom = yy + px(14);
    y = static_cast<int>(ag.modules_card.bottom) + px(kGap);

    // httpd.conf: the two buttons at the card's end, what custom.conf left out, the rules.
    top = y + px(14);
    ag.conf_card = {x0, y, x1, y};
    const int edit_w = button_w(Str::ApacheEditConf);
    ag.edit_conf = {std::max(cx0, cx1 - edit_w), top, cx1, top + px(kControlH)};
    const int final_w = button_w(Str::ApacheShowFinal);
    ag.final_conf = {std::max(cx0, static_cast<int>(ag.edit_conf.left) - px(8) - final_w), top,
                     static_cast<int>(ag.edit_conf.left) - px(8), top + px(kControlH)};
    const int conf_end = static_cast<int>(ag.final_conf.left) - px(kGap);
    const int conf_w = Canvas::measure(dc, L"httpd.conf", fonts_.body_bold.get(), false);
    ag.conf_title = {cx0, top, std::min(conf_end, cx0 + conf_w), top + px(kControlH)};
    ag.conf_hint = {std::min(conf_end, static_cast<int>(ag.conf_title.right) + px(8)), top, conf_end, top + px(kControlH)};
    yy = top + px(kControlH) + px(kGap);
    if (!apache_->custom_problems.empty()) {
        const std::string words = fill(lang_, Str::ApacheCustomProblems, {joined(apache_->custom_problems)});
        const int h = text_height(dc, w(words), fonts_.caption.get(), avail, rtl_);
        ag.custom_problems = {cx0, yy, cx1, yy + h};
        yy += h + px(8);
    }
    const int rules_h = std::max(px(kNoteH), text_height(dc, w(text(lang_, Str::ApacheConfNote)), fonts_.caption.get(),
                                                         avail, rtl_));
    ag.conf_note = {cx0, yy, cx1, yy + rules_h};
    yy += rules_h;
    ag.conf_card.bottom = yy + px(14);
    y = static_cast<int>(ag.conf_card.bottom) + px(kGap);

    // The footer: at the window's bottom when there's room, else after the cards.
    const int save_w = std::max(measure(text(lang_, Str::PhpSave), fonts_.button.get()),
                                measure(text(lang_, Str::PhpSaving), fonts_.button.get())) +
                       px(24);
    const int discard_w = button_w(Str::PhpDiscard);
    const std::string restart = apache_restart_text(*apache_, lang_);
    const int text_w = std::max(px(80), x1 - x0 - save_w - discard_w - px(8) - px(kGap));
    const int text_h = text_height(dc, w(restart), fonts_.caption.get(), text_w, rtl_);
    const int footer_h = std::max(px(kControlH), text_h);
    const int footer_top = std::max(y, height_ - px(16) - footer_h);
    ag.save = {x1 - save_w, footer_top, x1, footer_top + px(kControlH)};
    ag.discard = {static_cast<int>(ag.save.left) - px(8) - discard_w, footer_top, static_cast<int>(ag.save.left) - px(8),
                  footer_top + px(kControlH)};
    const int text_top = footer_top + std::max(0, (px(kControlH) - text_h) / 2);
    ag.footer_text = {x0, text_top, x0 + text_w, text_top + text_h};
    g.content_h = footer_top + footer_h + scroll_ + px(16);
}

const RECT* SettingsWindow::apache_rect_of(const Hit& hit) const {
    const ApacheGeometry& g = geometry_.apache;
    switch (hit.part) {
        case Part::ApacheVersion:
            return &g.version;
        case Part::ApacheRuntimes:
            return &g.runtimes;
        case Part::ApacheFind:
            return &g.find;
        case Part::ApacheModule:
            return hit.row >= 0 && std::cmp_less(hit.row, g.cells.size()) ? &g.cells[static_cast<std::size_t>(hit.row)]
                                                                         : nullptr;
        case Part::ApacheMore:
            return &g.more;
        case Part::ApacheFinalConf:
            return &g.final_conf;
        case Part::ApacheEditConf:
            return &g.edit_conf;
        case Part::ApacheDiscard:
            return &g.discard;
        case Part::ApacheSave:
            return &g.save;
        default:
            break;
    }
    return nullptr;
}

void SettingsWindow::apache_focus_order(std::vector<Hit>& order) const {
    const ApacheGeometry& g = geometry_.apache;
    const auto add = [&](Part part, const RECT& r, int row = -1) {
        if (!is_empty(r)) {
            order.push_back({part, -1, row});
        }
    };
    add(Part::ApacheVersion, g.version);
    add(Part::ApacheRuntimes, g.runtimes);
    if (!apache_) {
        return;
    }
    add(Part::ApacheFind, g.find);
    for (std::size_t i = 0; i < g.cells.size(); ++i) {
        add(Part::ApacheModule, g.cells[i], static_cast<int>(i));
    }
    add(Part::ApacheMore, g.more);
    add(Part::ApacheFinalConf, g.final_conf);
    add(Part::ApacheEditConf, g.edit_conf);
    add(Part::ApacheDiscard, g.discard);
    add(Part::ApacheSave, g.save);
}

// --- input -----------------------------------------------------------------------------

void SettingsWindow::on_apache_click(const Hit& hit) {
    switch (hit.part) {
        case Part::ApacheVersion:
            apache_version_menu();
            break;
        case Part::ApacheRuntimes:
            open_section(Section::Runtimes);
            break;
        case Part::ApacheFind:
            if (!apache_edits_.empty()) {
                SetFocus(apache_edits_.front()->hwnd());
            }
            break;
        case Part::ApacheModule:
            if (apache_ && !apache_saving_ && hit.row >= 0 && std::cmp_less(hit.row, geometry_.apache.names.size())) {
                toggle_apache_module(*apache_, apache_changes_, geometry_.apache.names[static_cast<std::size_t>(hit.row)]);
                relayout();  // the notes ("for cache_disk") follow
            }
            break;
        case Part::ApacheMore:
            apache_all_ = !apache_all_;
            relayout();
            break;
        case Part::ApacheEditConf:
        case Part::ApacheFinalConf:
            if (connected_) {
                message_.clear();
                // Made with a header when it isn't there; its path comes back to open.
                worker_.database_call(hit.part == Part::ApacheEditConf ? "apache.custom_conf" : "apache.final_conf",
                                      json::object());
                relayout();
            }
            break;
        case Part::ApacheDiscard:
            discard_apache();
            break;
        case Part::ApacheSave:
            (void)save_apache();
            break;
        default:
            break;
    }
}

void SettingsWindow::apache_version_menu() {
    if (!apache_ || apache_->versions.empty() || apache_saving_) {
        return;
    }
    // Newest installed, then each version.
    std::vector<std::optional<std::string>> choices{std::nullopt};
    for (const std::string& v : apache_->versions) {
        choices.emplace_back(v);
    }
    const auto shown = apache_shown_choice(*apache_, apache_changes_);
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    std::vector<std::wstring> labels;
    labels.reserve(choices.size());
    for (std::size_t i = 0; i < choices.size(); ++i) {
        labels.push_back(w(apache_choice_label(*apache_, choices[i], lang_)));
        AppendMenuW(menu.get(), MF_STRING | (choices[i] == shown ? UINT{MF_CHECKED} : 0U),
                    kFirstChoice + static_cast<UINT>(i), labels.back().c_str());
    }
    const RECT box = geometry_.apache.version;
    POINT at{box.left, box.bottom};
    ClientToScreen(hwnd_, &at);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
    flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    if (command < kFirstChoice || command - kFirstChoice >= choices.size() || hwnd_ == nullptr || !apache_) {
        return;
    }
    const std::optional<std::string>& picked = choices[command - kFirstChoice];
    if (picked == apache_->chosen) {
        apache_changes_.version.reset();  // back to what's saved
    } else {
        apache_changes_.version = picked;
    }
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- painting ----------------------------------------------------------------------

void SettingsWindow::paint_apache_shapes(Canvas& c) const {
    const Palette& p = palette(dark_);
    const ApacheGeometry& ag = geometry_.apache;
    const float border = std::max(1.0F, pxf(1.0F));
    c.clip({main_left() - px(4), 0, width_, height_});
    if (!is_empty(ag.version)) {
        const Hit hit{Part::ApacheVersion, -1, -1};
        const bool changed = apache_changes_.version.has_value();
        c.round_rect(ag.version, pxf(4), changed ? p.input_set : (hover_ == hit ? p.row_hover : p.input),
                     changed ? p.accent : p.input_border, border);
        const float vx = static_cast<float>(ag.version.right) - pxf(14);
        const float vy = static_cast<float>(ag.version.top + ag.version.bottom) / 2.0F;
        c.line(vx - pxf(4), vy - pxf(2), vx, vy + pxf(2), p.text_subtle, pxf(1.3F));
        c.line(vx, vy + pxf(2), vx + pxf(4), vy - pxf(2), p.text_subtle, pxf(1.3F));
        if (is_focused(hit)) {
            paint_ring(c, ag.version);
        }
    }
    if (!is_empty(geometry_.message)) {
        c.round_rect(geometry_.message, pxf(4), message_error_ ? p.banner_error.fill : p.banner_info.fill);
    }
    if (!is_empty(ag.runtimes)) {
        const Hit hit{Part::ApacheRuntimes, -1, -1};
        paint_button(c, ag.runtimes, hover_ == hit, is_focused(hit), connected_);
    }
    if (!is_empty(ag.refused)) {
        c.round_rect(ag.refused, pxf(4), p.banner_error.fill);
    }
    if (!is_empty(ag.problems)) {
        c.round_rect(ag.problems, pxf(4), p.banner_error.fill);
    }
    if (apache_ && !apache_->versions.empty()) {
        c.round_rect(ag.modules_card, pxf(8), p.card, p.card_border, border);
        const bool find_focused = !apache_edits_.empty() && apache_edits_.front()->focused();
        c.round_rect(ag.find, pxf(4), p.input, p.input_border, border);
        if (find_focused) {
            c.round_outline(ag.find, pxf(4), p.accent, pxf(2));
        }
        for (std::size_t i = 0; i < ag.cells.size() && i < ag.names.size(); ++i) {
            const RECT& cell = ag.cells[i];
            const Hit hit{Part::ApacheModule, -1, static_cast<int>(i)};
            const auto it = std::find_if(apache_->modules.begin(), apache_->modules.end(),
                                         [&](const ApacheModule& m) { return m.name == ag.names[i]; });
            if (it == apache_->modules.end()) {
                continue;
            }
            const bool locked = apache_module_locked(*apache_, apache_changes_, *it);
            const bool on = apache_module_on(*apache_, apache_changes_, it->name);
            if (hover_ == hit && !locked) {
                c.round_rect(cell, pxf(4), p.row_hover);
            }
            const int mid = (cell.top + cell.bottom) / 2;
            const RECT box{cell.left + px(2), mid - px(kCheckBox) / 2, cell.left + px(2) + px(kCheckBox),
                           mid + px(kCheckBox) / 2};
            if (locked) {
                c.round_rect(box, pxf(3), p.button_disabled, p.button_border, border);
            } else if (on) {
                c.round_rect(box, pxf(3), p.accent);
            } else {
                c.round_rect(box, pxf(3), p.input, p.switch_off_border, border);
            }
            if (on) {
                const Rgb mark = locked ? p.button_disabled_text : p.accent_text;
                const auto bx = static_cast<float>(box.left);
                const auto by = static_cast<float>(box.top);
                c.line(bx + pxf(3.5F), by + pxf(8), bx + pxf(6.5F), by + pxf(11), mark, pxf(1.6F));
                c.line(bx + pxf(6.5F), by + pxf(11), bx + pxf(12.5F), by + pxf(5), mark, pxf(1.6F));
            }
            if (is_focused(hit)) {
                paint_ring(c, cell);
            }
        }
        if (is_focused({Part::ApacheMore, -1, -1})) {
            paint_ring(c, ag.more);
        }

        c.round_rect(ag.conf_card, pxf(8), p.card, p.card_border, border);
        const Hit edit{Part::ApacheEditConf, -1, -1};
        paint_button(c, ag.edit_conf, hover_ == edit, is_focused(edit), connected_);
        const Hit final_conf{Part::ApacheFinalConf, -1, -1};
        paint_button(c, ag.final_conf, hover_ == final_conf, is_focused(final_conf), connected_);

        const bool changes = !apache_changes_.empty();
        const Hit discard{Part::ApacheDiscard, -1, -1};
        paint_button(c, ag.discard, hover_ == discard && changes, is_focused(discard), changes && !apache_saving_);
        const Hit save{Part::ApacheSave, -1, -1};
        if (changes && !apache_saving_ && connected_) {
            c.round_rect(ag.save, pxf(4), p.accent);
            if (is_focused(save)) {
                paint_ring(c, ag.save);
            }
        } else {
            paint_button(c, ag.save, false, is_focused(save), false);
        }
    }
    // The scroll bar, when the page is taller than the window.
    scroll_bar_.paint(c, p);
    c.unclip();
}

void SettingsWindow::paint_apache_text(Canvas& c) const {
    const Palette& p = palette(dark_);
    const ApacheGeometry& ag = geometry_.apache;
    c.clip_text({main_left() - px(4), 0, width_, height_});
    c.text(geometry_.title, w(text(lang_, Str::SettingsApache)), fonts_.title.get(), p.text, Align::Start, false);
    c.paragraph(geometry_.hint, w(text(lang_, Str::ApacheHint)), fonts_.caption.get(), p.text_subtle);
    if (!is_empty(ag.version) && apache_) {
        c.text(ag.version_label, w(text(lang_, Str::ApacheVersion)), fonts_.caption.get(), p.text_subtle, Align::End,
               false);
        RECT box = ag.version;
        box.left += px(8);
        box.right -= px(26);
        c.text(box, w(apache_choice_label(*apache_, apache_shown_choice(*apache_, apache_changes_), lang_)),
               fonts_.button.get(), p.text, Align::Start, false);
    }
    if (!is_empty(geometry_.message)) {
        RECT inner = geometry_.message;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(message_), fonts_.button.get(), message_error_ ? p.banner_error.text : p.banner_info.text);
    }
    if (!is_empty(ag.note)) {
        if (is_empty(ag.runtimes)) {
            c.text(ag.note, w(text(lang_, Str::SettingsLoading)), fonts_.button.get(), p.text_subtle, Align::Start, false);
        } else {
            c.paragraph(ag.note, w(text(lang_, Str::ApacheNoneInstalled)), fonts_.button.get(), p.text);
        }
    }
    if (!is_empty(ag.runtimes)) {
        c.text(ag.runtimes, w(text(lang_, Str::PhpGoToRuntimes)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (!apache_ || apache_->versions.empty()) {
        c.unclip_text();
        return;
    }
    const auto service_words = [this](const std::string& s) { return rtl_ ? isolate_ltr(s) : s; };
    if (!is_empty(ag.refused)) {
        RECT inner = ag.refused;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(fill(lang_, Str::ApacheRefused, {service_words(apache_->refused)})), fonts_.caption.get(),
                    p.banner_error.text);
    }
    if (!is_empty(ag.problems)) {
        RECT inner = ag.problems;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(fill(lang_, Str::ApacheProblems, {service_words(joined(apache_->problems))})),
                    fonts_.caption.get(), p.banner_error.text);
    }

    c.text(ag.modules_title, w(text(lang_, Str::ApacheModules)), fonts_.body_bold.get(), p.text, Align::Start, false);
    c.text(ag.count,
           w(fill(lang_, Str::ApacheModulesOn,
                  {std::to_string(apache_modules_on(*apache_, apache_changes_)), std::to_string(apache_->modules.size())})),
           fonts_.caption.get(), p.text_subtle, Align::Start, false);
    for (std::size_t i = 0; i < ag.cells.size() && i < ag.names.size(); ++i) {
        const RECT& cell = ag.cells[i];
        const auto it = std::find_if(apache_->modules.begin(), apache_->modules.end(),
                                     [&](const ApacheModule& m) { return m.name == ag.names[i]; });
        const bool locked = it != apache_->modules.end() && apache_module_locked(*apache_, apache_changes_, *it);
        const RECT label{cell.left + px(2) + px(kCheckBox) + px(8), cell.top,
                         is_empty(ag.extras[i]) ? cell.right : ag.extras[i].left, cell.bottom};
        c.text(label, w(ag.names[i]), fonts_.button.get(), locked ? p.button_disabled_text : p.text, Align::Start, true);
        if (!is_empty(ag.extras[i])) {
            c.text(ag.extras[i], w(ag.notes[i]), fonts_.caption.get(), p.text_subtle, Align::Start, false);
        }
    }
    {
        std::size_t hidden = 0;
        const std::vector<ApacheModule> shown = shown_apache_modules(hidden);
        std::string note_text = !apache_query_.empty() && shown.empty()
                                    ? fill(lang_, Str::ApacheNoMatch, {apache_query_})
                                    : std::string(text(lang_, Str::ApacheModulesNote));
        if (hidden > 0) {
            note_text += " " + fill(lang_, Str::PhpMoreCount, {std::to_string(hidden)});
        }
        c.paragraph(ag.modules_note, w(note_text), fonts_.caption.get(), p.text_subtle);
        if (!is_empty(ag.more)) {
            c.text(ag.more, w(text(lang_, hidden > 0 ? Str::PhpShowAll : Str::PhpShowFewer)), fonts_.caption.get(), p.link,
                   Align::Start, false);
        }
    }

    c.text(ag.conf_title, L"httpd.conf", fonts_.body_bold.get(), p.text, Align::Start, true);
    c.text(ag.conf_hint, w(text(lang_, Str::ApacheConfHint)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    c.text(ag.edit_conf, w(text(lang_, Str::ApacheEditConf)), fonts_.button.get(),
           connected_ ? p.text : p.button_disabled_text, Align::Center, false);
    c.text(ag.final_conf, w(text(lang_, Str::ApacheShowFinal)), fonts_.button.get(),
           connected_ ? p.text : p.button_disabled_text, Align::Center, false);
    if (!is_empty(ag.custom_problems)) {
        c.paragraph(ag.custom_problems,
                    w(fill(lang_, Str::ApacheCustomProblems, {service_words(joined(apache_->custom_problems))})),
                    fonts_.caption.get(), p.asleep.text);
    }
    c.paragraph(ag.conf_note, w(text(lang_, Str::ApacheConfNote)), fonts_.caption.get(), p.text_subtle);

    const bool changes = !apache_changes_.empty();
    c.paragraph(ag.footer_text, w(apache_restart_text(*apache_, lang_)), fonts_.caption.get(), p.text_subtle);
    c.text(ag.discard, w(text(lang_, Str::PhpDiscard)), fonts_.button.get(),
           changes && !apache_saving_ ? p.text : p.button_disabled_text, Align::Center, false);
    const bool can_save = changes && !apache_saving_ && connected_;
    c.text(ag.save, w(text(lang_, apache_saving_ ? Str::PhpSaving : Str::PhpSave)), fonts_.button.get(),
           can_save ? p.accent_text : p.button_disabled_text, Align::Center, false);
    c.unclip_text();
}

}  // namespace floserver::ui

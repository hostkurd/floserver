// Settings > PHP (D20): the defaults for every project and shared sites on
// one PHP series. Extensions as a grid of checkboxes with a find box, the
// php.ini settings people change most as fields, and the full php.ini
// (the series' custom.ini) opened in Notepad. Changes wait for Save, which
// sends one php.set with only what changed.

#include "settings_window.h"

#include "main_window_layout.h"
#include "model/theme.h"

#include <shellapi.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace floserver::ui {

using layout::width_of;
using nlohmann::json;

namespace {

constexpr int kEditBase = 1000;  // edits_[i] has id kEditBase + i
constexpr UINT kFirstChoice = 100;
constexpr int kGap = 12;
constexpr int kCardPad = 16;
constexpr int kHeadH = 30;
constexpr int kControlH = 30;
constexpr int kExtCellH = 28;
constexpr int kExtMinW = 150;
constexpr int kExtGap = 16;
constexpr int kFieldMinW = 170;
constexpr int kLabelH = 16;
constexpr int kNoteH = 18;
constexpr int kCheckBox = 16;

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

const PhpIniField& field_at(int index) { return settings_php_ini_fields()[static_cast<std::size_t>(index)]; }

}  // namespace

// --- sections, closing, loading ------------------------------------------------------

std::vector<std::pair<std::string, std::string>> SettingsWindow::php_versions() const {
    return has_installed_ ? installed_php_series(installed_) : std::vector<std::pair<std::string, std::string>>{};
}

bool SettingsWindow::settle_php_changes(AfterSave then, const std::string& series) {
    if (!php_ || php_changes_.empty()) {
        return true;
    }
    if (asking_ || php_saving_) {
        return false;  // asked already, or a save is on its way: its answer comes first
    }
    asking_ = true;
    const SaveChoice choice = ask_to_save(hwnd_, lang_, fill(lang_, Str::PhpAskSave, {php_series_}));
    asking_ = false;
    if (quit_after_ask_) {
        // FloServer quit meanwhile: the owner goes on with it; nothing else here.
        PostMessageW(owner_, closed_message_, 1, 0);
        return false;
    }
    if (hwnd_ == nullptr) {
        return false;
    }
    switch (choice) {
        case SaveChoice::Save:
            // Done only once the save is answered without a refusal; until
            // then the window stays where it is, so a refusal shows here.
            if (save_php()) {
                after_save_ = then;
                after_save_series_ = series;
            }
            return false;
        case SaveChoice::DontSave:
            discard_php();
            return true;
        case SaveChoice::Cancel:
            break;
    }
    return false;
}

void SettingsWindow::try_close() {
    if (hwnd_ == nullptr || asking_ || (section_ == Section::Php && !settle_php_changes(AfterSave::Close))) {
        return;
    }
    DestroyWindow(hwnd_);
}

void SettingsWindow::show_php(const std::string& series) {
    if (hwnd_ == nullptr) {
        return;
    }
    bring_to_front();
    if (asking_) {
        return;
    }
    if (section_ != Section::Php) {
        open_section(Section::Php, series);
    } else if (series != php_series_ && settle_php_changes(AfterSave::Version, series)) {
        load_php(series);
    }
}

void SettingsWindow::open_section(Section section, const std::string& series) {
    if (section == section_) {
        return;
    }
    if (section_ == Section::Php) {
        if (!settle_php_changes(AfterSave::Runtimes)) {
            return;
        }
        if (GetFocus() != hwnd_) {
            SetFocus(hwnd_);  // before its box goes
        }
        edits_.clear();
        field_edit_.clear();
    }
    section_ = section;
    scroll_ = 0;
    focus_ = section == Section::Php ? 1 : 0;  // its own entry in the sidebar
    hover_ = {};
    message_.clear();
    if (section == Section::Php) {
        make_php_edits();
        std::string pick = series;
        if (pick.empty()) {
            const auto versions = php_versions();
            const bool still = std::any_of(versions.begin(), versions.end(),
                                           [&](const auto& v) { return v.first == php_series_; });
            pick = still ? php_series_ : (versions.empty() ? std::string() : versions.front().first);
        }
        // Each time the section opens: one php.get, nothing else.
        if (!pick.empty()) {
            load_php(pick);
        }
    }
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::load_php(const std::string& series) {
    php_series_ = series;
    php_error_key_.clear();
    php_error_.clear();
    php_.reset();
    php_changes_ = {};
    php_all_ = false;
    php_loading_ = true;
    worker_.php_get(series, ServiceWorker::kPhpSettings);
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::on_php_answers(const Snapshot& snap) {
    bool close_now = false;
    for (const Snapshot::PhpAnswer& answer : snap.php) {
        if (answer.to != ServiceWorker::kPhpSettings) {
            continue;
        }
        if (answer.save && php_saving_) {
            php_saving_ = false;
            set_php_read_only(false);
        }
        // A late answer for a version the user has left: dropped, errors too.
        if (!php_answer_matches(answer.to, answer.series, ServiceWorker::kPhpSettings, php_series_)) {
            continue;
        }
        if (!answer.error.empty() || !answer.reply) {
            if (!answer.save) {
                php_loading_ = false;
            }
            if (answer.save) {
                after_save_ = AfterSave::Stay;  // refused: stay here with the reason shown
                // A refused value is shown under its field too.
                std::vector<std::string> keys;
                for (const PhpIniField& field : settings_php_ini_fields()) {
                    keys.push_back(field.key);
                }
                php_error_key_ = ini_error_key(answer.error, keys);
                php_error_ = php_error_key_.empty() ? std::string() : answer.error;
            }
            show_message(answer.save ? fill(lang_, Str::PhpSaveFailed, {answer.error})
                                     : fill(lang_, Str::PhpReadFailed, {php_series_, answer.error}),
                         true);
            continue;
        }
        auto series = php_series_from_reply(*answer.reply);
        if (!series || series->series != php_series_) {
            continue;
        }
        php_ = std::move(series);
        php_loading_ = false;
        if (answer.save) {
            php_changes_ = {};
            php_error_key_.clear();
            php_error_.clear();
            show_message(std::string(text(lang_, Str::PhpSaved)), false);
        }
        fill_php_edits();
        if (answer.save) {
            // What the user was on the way to when they chose Save.
            const AfterSave then = std::exchange(after_save_, AfterSave::Stay);
            const std::string target = std::exchange(after_save_series_, {});
            if (then == AfterSave::Close) {
                close_now = true;
            } else if (then == AfterSave::Runtimes) {
                open_section(Section::Runtimes);
            } else if (then == AfterSave::Version && !target.empty()) {
                load_php(target);
            }
        }
    }
    if (!snap.custom_ini.empty()) {
        // Notepad, so the user edits it; FloServer reads it within ~5 s of a save.
        const std::wstring quoted = L"\"" + w(snap.custom_ini) + L"\"";
        const auto opened = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL));
        if (opened <= 32) {
            show_message(fill(lang_, Str::PhpOpenIniFailed, {snap.custom_ini}), true);
        } else {
            show_message(std::string(text(lang_, snap.custom_ini_final ? Str::PhpFinalOpened : Str::PhpIniOpened)),
                         false);
        }
    } else if (!snap.custom_ini_error.empty()) {
        show_message(fill(lang_, Str::PhpOpenIniFailed, {snap.custom_ini_error}), true);
    }
    // The section was opened before anything installed was known.
    if (section_ == Section::Php && php_series_.empty() && !php_loading_) {
        const auto versions = php_versions();
        if (!versions.empty()) {
            load_php(versions.front().first);
        }
    }
    return close_now;
}

bool SettingsWindow::save_php() {
    if (!php_ || php_changes_.empty() || php_saving_) {
        return false;
    }
    if (!connected_) {
        show_message(fill(lang_, Str::PhpSaveFailed, {text(lang_, Str::ConnectionLost)}), true);
        return false;
    }
    php_saving_ = true;
    set_php_read_only(true);  // what's typed now would be lost when the answer comes
    message_.clear();
    worker_.php_set(php_set_params(php_series_, php_changes_));
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void SettingsWindow::set_php_read_only(bool read_only) {
    for (std::size_t i = 1; i < edits_.size(); ++i) {  // the find box stays usable
        edits_[i]->set_read_only(read_only);
    }
}

void SettingsWindow::discard_php() {
    if (php_saving_) {
        return;
    }
    php_changes_ = {};
    php_error_key_.clear();
    php_error_.clear();
    fill_php_edits();
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- the edit boxes ----------------------------------------------------------------

void SettingsWindow::make_php_edits() {
    if (!edits_.empty()) {
        return;
    }
    const Palette& p = palette(dark_);
    const auto make = [&](std::string_view cue) {
        const int id = kEditBase + static_cast<int>(edits_.size());
        auto edit = std::make_unique<FieldEdit>(hwnd_, id, fonts_.button.get(), rtl_,
                                                [this, id](WPARAM key) { return on_edit_key(id, key); });
        edit->set_cue(cue);
        edit->set_cue_color(p.text_subtle);
        edits_.push_back(std::move(edit));
    };
    make(text(lang_, Str::PhpFindExtension));
    edits_.front()->set_text(php_query_);
    field_edit_.clear();
    for (const PhpIniField& field : settings_php_ini_fields()) {
        if (field.choices.empty()) {
            field_edit_.push_back(static_cast<int>(edits_.size()));
            make(text(lang_, Str::PhpDefaultCue));
        } else {
            field_edit_.push_back(-1);
        }
    }
    fill_php_edits();
}

void SettingsWindow::fill_php_edits() {
    if (!php_) {
        return;
    }
    const auto& fields = settings_php_ini_fields();
    for (std::size_t i = 0; i < fields.size() && i < field_edit_.size(); ++i) {
        if (field_edit_[i] >= 0) {
            edits_[static_cast<std::size_t>(field_edit_[i])]->set_text(ini_text(*php_, php_changes_, fields[i].key));
        }
    }
}

void SettingsWindow::place_php_edits() {
    if (section_ != Section::Php || edits_.empty()) {
        return;
    }
    const PhpGeometry& g = geometry_.php;
    const bool shown = php_.has_value();
    const int h = px(20);
    edits_.front()->place(g.find, px(8), h, shown && !is_empty(g.find));
    for (std::size_t i = 0; i < field_edit_.size(); ++i) {
        if (field_edit_[i] >= 0) {
            const RECT box = i < g.boxes.size() ? g.boxes[i] : RECT{};
            edits_[static_cast<std::size_t>(field_edit_[i])]->place(box, px(8), h, shown && !is_empty(box));
        }
    }
}

SettingsWindow::Hit SettingsWindow::hit_of_edit(int id) const {
    const int index = id - kEditBase;
    if (index < 0 || std::cmp_greater_equal(index, edits_.size())) {
        return {};
    }
    if (index == 0) {
        return {Part::PhpFind, -1, -1};
    }
    for (std::size_t f = 0; f < field_edit_.size(); ++f) {
        if (field_edit_[f] == index) {
            return {Part::PhpField, -1, static_cast<int>(f)};
        }
    }
    return {};
}

FieldEdit* SettingsWindow::edit_of(const Hit& hit) const {
    if (hit.part == Part::PhpFind && !edits_.empty()) {
        return edits_.front().get();
    }
    if (hit.part == Part::PhpField && hit.row >= 0 && std::cmp_less(hit.row, field_edit_.size())) {
        const int index = field_edit_[static_cast<std::size_t>(hit.row)];
        return index >= 0 ? edits_[static_cast<std::size_t>(index)].get() : nullptr;
    }
    return nullptr;
}

void SettingsWindow::on_edit_change(int id) {
    const Hit hit = hit_of_edit(id);
    FieldEdit* edit = edit_of(hit);
    if (edit == nullptr || edit->setting()) {
        return;
    }
    if (hit.part == Part::PhpFind) {
        php_query_ = edit->text();
        relayout();
    } else if (php_ && !php_saving_) {
        set_ini_text(*php_, php_changes_, field_at(hit.row).key, edit->text());
        if (field_at(hit.row).key == php_error_key_) {
            php_error_key_.clear();  // being fixed
            php_error_.clear();
        }
        InvalidateRect(edit->hwnd(), nullptr, TRUE);  // its tint follows
        relayout();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::on_edit_focus(int id) {
    const Hit hit = hit_of_edit(id);
    const std::vector<Hit> order = focus_order();
    if (const auto it = std::find(order.begin(), order.end(), hit); it != order.end()) {
        focus_ = static_cast<int>(it - order.begin());
        ensure_visible(hit);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::on_edit_key(int id, WPARAM key) {
    if (key == VK_ESCAPE) {
        if (id == kEditBase && !php_query_.empty()) {
            edits_.front()->set_text({});
            php_query_.clear();
            relayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            // After this message: the box may go with the window.
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        return true;
    }
    if (key == VK_RETURN) {
        return true;  // the value is kept as it's typed; Save sends it
    }
    // Tab: on through the page as the window's own keys go.
    const std::vector<Hit> order = focus_order();
    const auto it = std::find(order.begin(), order.end(), hit_of_edit(id));
    if (it == order.end() || order.empty()) {
        SetFocus(hwnd_);
        return true;
    }
    const int count = static_cast<int>(order.size());
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    focus_ = (static_cast<int>(it - order.begin()) + (shift ? count - 1 : 1)) % count;
    show_focus_ = true;
    const Hit& next = order[static_cast<std::size_t>(focus_)];
    ensure_visible(next);
    if (FieldEdit* edit = edit_of(next); edit != nullptr) {
        SetFocus(edit->hwnd());
        SendMessageW(edit->hwnd(), EM_SETSEL, 0, -1);
    } else {
        SetFocus(hwnd_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

// --- geometry ------------------------------------------------------------------------

std::vector<PhpExtension> SettingsWindow::shown_extensions(std::size_t& hidden) const {
    hidden = 0;
    if (!php_) {
        return {};
    }
    std::vector<PhpExtension> all = ordered_extensions(*php_);  // in the saved order: nothing jumps on a click
    if (!php_query_.empty()) {
        std::erase_if(all, [&](const PhpExtension& e) { return !extension_matches(e.name, php_query_); });
        return all;
    }
    // A few more than the first 20 aren't worth a "show all".
    if (php_all_ || all.size() <= kShownExtensions + 4) {
        return all;
    }
    hidden = all.size() - kShownExtensions;
    all.resize(kShownExtensions);
    return all;
}

void SettingsWindow::layout_php(HDC dc, Geometry& g, int x0, int x1, int y) const {
    PhpGeometry& pg = g.php;
    const auto measure = [&](std::string_view s, HFONT font) { return Canvas::measure(dc, w(s), font, rtl_); };

    // The title and what the section does, with the version picker beside them.
    const auto versions = php_versions();
    int picker_w = 0;
    int label_w = 0;
    if (!versions.empty()) {
        picker_w = px(150);
        for (const auto& v : versions) {
            picker_w = std::max(picker_w, Canvas::measure(dc, w("PHP " + v.second), fonts_.button.get(), false) + px(34));
        }
        label_w = measure(text(lang_, Str::PhpVersion), fonts_.caption.get());
    }
    const int right_w = picker_w > 0 ? picker_w + px(8) + label_w + px(kGap) : 0;
    const int hint_w = std::max(px(100), x1 - x0 - right_w);
    const int hint_h = text_height(dc, w(text(lang_, Str::PhpHint)), fonts_.caption.get(), hint_w, rtl_);
    g.title = {x0, y, x0 + hint_w, y + px(28)};
    g.hint = {x0, g.title.bottom, x0 + hint_w, g.title.bottom + hint_h};
    if (picker_w > 0) {
        const int top = std::max(y, static_cast<int>(g.hint.bottom) - px(kControlH));
        pg.version = {x1 - picker_w, top, x1, top + px(kControlH)};
        pg.version_label = {static_cast<int>(pg.version.left) - px(8) - label_w, top,
                            static_cast<int>(pg.version.left) - px(8), top + px(kControlH)};
    }
    y = std::max(g.hint.bottom, pg.version.bottom) + px(kGap);

    if (!message_.empty()) {
        const int mh = std::max(px(32), text_height(dc, w(message_), fonts_.button.get(), x1 - x0 - px(24), rtl_) + px(14));
        g.message = {x0, y, x1, y + mh};
        y += mh + px(kGap);
    }

    if (!has_installed_ || (!php_ && php_loading_)) {
        pg.note = {x0, y, x1, y + px(24)};
        g.content_h = pg.note.bottom + scroll_ + px(8);
        return;
    }
    if (versions.empty()) {
        const int nh = text_height(dc, w(text(lang_, Str::PhpNoneInstalled)), fonts_.button.get(), x1 - x0, rtl_);
        pg.note = {x0, y, x1, y + nh};
        y += nh + px(kGap);
        const int bw = measure(text(lang_, Str::PhpGoToRuntimes), fonts_.button.get()) + px(24);
        pg.runtimes = {x0, y, x0 + bw, y + px(kControlH)};
        g.content_h = pg.runtimes.bottom + scroll_ + px(8);
        return;
    }
    if (!php_) {
        g.content_h = y + scroll_ + px(8);
        return;
    }
    if (!php_->problems.empty()) {
        std::string all;
        for (const std::string& problem : php_->problems) {
            all += (all.empty() ? "" : "; ") + problem;
        }
        const std::string line = fill(lang_, Str::PhpProblems, {all});
        const int ph = text_height(dc, w(line), fonts_.caption.get(), x1 - x0 - px(24), rtl_) + px(14);
        pg.problems = {x0, y, x1, y + ph};
        y += ph + px(kGap);
    }

    // Extensions.
    const int cx0 = x0 + px(kCardPad);
    const int cx1 = x1 - px(kCardPad);
    const int avail = cx1 - cx0;
    int top = y + px(14);
    pg.ext_card.left = x0;
    pg.ext_card.top = y;
    pg.ext_card.right = x1;
    const int find_w = std::min(px(200), avail / 2);
    pg.find = {cx1 - find_w, top, cx1, top + px(kControlH)};
    const std::string title = std::string(text(lang_, Str::PhpExtensions));
    const int title_w = measure(title, fonts_.body_bold.get());
    const int title_end = static_cast<int>(pg.find.left) - px(kGap);
    pg.ext_title = {cx0, top, std::min(title_end, cx0 + title_w), top + px(kControlH)};
    pg.ext_count = {std::min(title_end, static_cast<int>(pg.ext_title.right) + px(8)), top, title_end, top + px(kControlH)};
    int yy = top + px(kControlH) + px(kGap);

    std::size_t hidden = 0;
    const std::vector<PhpExtension> shown = shown_extensions(hidden);
    const int cols = std::clamp((avail + px(kExtGap)) / (px(kExtMinW) + px(kExtGap)), 1, 4);
    const int col_w = (avail - (cols - 1) * px(kExtGap)) / cols;
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const int col = static_cast<int>(i) % cols;
        const int line = static_cast<int>(i) / cols;
        const int left = cx0 + col * (col_w + px(kExtGap));
        const int cell_top = yy + line * px(kExtCellH);
        const RECT cell{left, cell_top, left + col_w, cell_top + px(kExtCellH)};
        pg.ext_cells.push_back(cell);
        pg.ext_names.push_back(shown[i].name);
        RECT extra{};
        Str note = Str::Count_;
        if (shown[i].missing) {
            note = Str::PhpMissing;
        } else if (is_slow_extension(shown[i].name)) {
            note = Str::PhpSlows;
        }
        if (note != Str::Count_) {
            const int name_end = left + px(kCheckBox) + px(8) +
                                 Canvas::measure(dc, w(shown[i].name), fonts_.button.get(), false) + px(6);
            if (name_end < cell.right) {
                extra = {name_end, cell_top, static_cast<int>(cell.right), static_cast<int>(cell.bottom)};
            }
        }
        pg.ext_extras.push_back(extra);
    }
    const int lines = (static_cast<int>(shown.size()) + cols - 1) / cols;
    yy += lines * px(kExtCellH);
    if (lines > 0) {
        yy += px(8);
    }
    // The note under the grid, with "show all" after it.
    std::string link;
    if (!php_->extensions_fixed && php_query_.empty()) {
        if (hidden > 0) {
            link = std::string(text(lang_, Str::PhpShowAll));
        } else if (php_all_ && php_->extensions.size() > kShownExtensions + 4) {
            link = std::string(text(lang_, Str::PhpShowFewer));
        }
    }
    const int link_w = link.empty() ? 0 : measure(link, fonts_.caption.get()) + px(4);
    std::string note_text;
    if (php_->extensions_fixed) {
        note_text = std::string(text(lang_, Str::PhpFixedNote));
    } else if (!php_query_.empty() && shown.empty()) {
        note_text = fill(lang_, Str::PhpNoMatch, {php_query_});
    } else {
        note_text = std::string(text(lang_, Str::PhpOpcacheNote));
        if (hidden > 0) {
            note_text += " " + fill(lang_, Str::PhpMoreCount, {std::to_string(hidden)});
        }
    }
    const int note_w = std::min(measure(note_text, fonts_.caption.get()) + px(2), avail - link_w - px(4));
    pg.ext_note = {cx0, yy, cx0 + std::max(0, note_w), yy + px(kNoteH)};
    if (!link.empty()) {
        pg.more = {static_cast<int>(pg.ext_note.right) + px(4), yy, static_cast<int>(pg.ext_note.right) + px(4) + link_w,
                   yy + px(kNoteH)};
    }
    yy += px(kNoteH);
    pg.ext_card.bottom = yy + px(14);
    y = static_cast<int>(pg.ext_card.bottom) + px(kGap);

    // php.ini.
    top = y + px(14);
    pg.ini_card = {x0, y, x1, y};
    // The two buttons at the card's end: your own lines, then what PHP ends up with.
    const int edit_w = measure(text(lang_, Str::PhpEditFull), fonts_.button.get()) + px(24);
    pg.edit_ini = {std::max(cx0, cx1 - edit_w), top, cx1, top + px(kControlH)};
    const int final_w = measure(text(lang_, Str::PhpShowFinal), fonts_.button.get()) + px(24);
    pg.final_ini = {std::max(cx0, static_cast<int>(pg.edit_ini.left) - px(8) - final_w), top,
                    static_cast<int>(pg.edit_ini.left) - px(8), top + px(kControlH)};
    const int ini_end = static_cast<int>(pg.final_ini.left) - px(kGap);
    const int ini_w = Canvas::measure(dc, L"php.ini", fonts_.body_bold.get(), false);
    pg.ini_title = {cx0, top, std::min(ini_end, cx0 + ini_w), top + px(kControlH)};
    pg.ini_hint = {std::min(ini_end, static_cast<int>(pg.ini_title.right) + px(8)), top, ini_end, top + px(kControlH)};
    yy = top + px(kControlH) + px(kGap);
    const auto& fields = settings_php_ini_fields();
    const int fcols = std::clamp((avail + px(kGap)) / (px(kFieldMinW) + px(kGap)), 1, 4);
    const int fcol_w = (avail - (fcols - 1) * px(kGap)) / fcols;
    const int cell_h = px(kLabelH) + px(4) + px(kControlH);
    const int reset_w = measure(text(lang_, Str::PhpReset), fonts_.caption.get()) + px(2);
    // The line holding a refused field is taller: its reason goes under it.
    int error_line = -1;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (!php_error_key_.empty() && fields[i].key == php_error_key_) {
            pg.error_field = static_cast<int>(i);
            error_line = static_cast<int>(i) / fcols;
        }
    }
    const int error_h = px(kNoteH);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const int col = static_cast<int>(i) % fcols;
        const int line = static_cast<int>(i) / fcols;
        const int left = cx0 + col * (fcol_w + px(kGap));
        const int cell_top = yy + line * (cell_h + px(kGap)) + (error_line >= 0 && line > error_line ? error_h : 0);
        RECT label{left, cell_top, left + fcol_w, cell_top + px(kLabelH)};
        RECT reset{};
        if (ini_marked(*php_, php_changes_, fields[i].key)) {
            reset = {left + fcol_w - reset_w, cell_top, left + fcol_w, cell_top + px(kLabelH) + px(2)};
            label.right = reset.left - px(6);
        }
        pg.labels.push_back(label);
        pg.resets.push_back(reset);
        pg.boxes.push_back({left, cell_top + px(kLabelH) + px(4), left + fcol_w, cell_top + cell_h});
        if (pg.error_field == static_cast<int>(i)) {
            // From the field's start to the card's end: the reason is longer than a field.
            pg.field_error = {left, cell_top + cell_h + px(2), cx1, cell_top + cell_h + px(2) + error_h};
        }
    }
    const int flines = (static_cast<int>(fields.size()) + fcols - 1) / fcols;
    yy += flines * cell_h + (flines - 1) * px(kGap) + (error_line >= 0 ? error_h : 0);
    // Always said: where your own lines go and that they win over the fields.
    yy += px(10);
    pg.custom_note = {cx0, yy, cx1, yy + px(kNoteH)};
    yy += px(kNoteH);
    pg.ini_card.bottom = yy + px(14);
    y = static_cast<int>(pg.ini_card.bottom) + px(kGap);

    // The footer: at the window's bottom when there's room (the mockup), else after the cards.
    const int save_w = std::max(measure(text(lang_, Str::PhpSave), fonts_.button.get()),
                                measure(text(lang_, Str::PhpSaving), fonts_.button.get())) +
                       px(24);
    const int discard_w = measure(text(lang_, Str::PhpDiscard), fonts_.button.get()) + px(24);
    const std::string restart = php_restart_text(*php_, lang_);
    const int text_w = std::max(px(80), x1 - x0 - save_w - discard_w - px(8) - px(kGap));
    const int text_h = text_height(dc, w(restart), fonts_.caption.get(), text_w, rtl_);
    const int footer_h = std::max(px(kControlH), text_h);
    const int footer_top = std::max(y, height_ - px(16) - footer_h);
    pg.save = {x1 - save_w, footer_top, x1, footer_top + px(kControlH)};
    pg.discard = {static_cast<int>(pg.save.left) - px(8) - discard_w, footer_top, static_cast<int>(pg.save.left) - px(8),
                  footer_top + px(kControlH)};
    const int text_top = footer_top + std::max(0, (px(kControlH) - text_h) / 2);
    pg.footer_text = {x0, text_top, x0 + text_w, text_top + text_h};
    g.content_h = footer_top + footer_h + scroll_ + px(16);
}

const RECT* SettingsWindow::php_rect_of(const Hit& hit) const {
    const PhpGeometry& g = geometry_.php;
    const auto in = [&](const std::vector<RECT>& list) -> const RECT* {
        return hit.row >= 0 && std::cmp_less(hit.row, list.size()) ? &list[static_cast<std::size_t>(hit.row)] : nullptr;
    };
    switch (hit.part) {
        case Part::PhpVersion:
            return &g.version;
        case Part::PhpRuntimes:
            return &g.runtimes;
        case Part::PhpFind:
            return &g.find;
        case Part::PhpExt:
            return in(g.ext_cells);
        case Part::PhpMore:
            return &g.more;
        case Part::PhpEditIni:
            return &g.edit_ini;
        case Part::PhpFinalIni:
            return &g.final_ini;
        case Part::PhpField:
            return in(g.boxes);
        case Part::PhpReset:
            return in(g.resets);
        case Part::PhpDiscard:
            return &g.discard;
        case Part::PhpSave:
            return &g.save;
        default:
            break;
    }
    return nullptr;
}

void SettingsWindow::php_focus_order(std::vector<Hit>& order) const {
    const PhpGeometry& g = geometry_.php;
    const auto add = [&](Part part, const RECT& r, int row = -1) {
        if (!is_empty(r)) {
            order.push_back({part, -1, row});
        }
    };
    add(Part::PhpVersion, g.version);
    add(Part::PhpRuntimes, g.runtimes);
    if (!php_) {
        return;
    }
    add(Part::PhpFind, g.find);
    if (!php_->extensions_fixed) {
        for (std::size_t i = 0; i < g.ext_cells.size(); ++i) {
            add(Part::PhpExt, g.ext_cells[i], static_cast<int>(i));
        }
    }
    add(Part::PhpMore, g.more);
    add(Part::PhpFinalIni, g.final_ini);
    add(Part::PhpEditIni, g.edit_ini);
    for (std::size_t i = 0; i < g.boxes.size(); ++i) {
        add(Part::PhpField, g.boxes[i], static_cast<int>(i));
        add(Part::PhpReset, g.resets[i], static_cast<int>(i));
    }
    add(Part::PhpDiscard, g.discard);
    add(Part::PhpSave, g.save);
}

// --- input -----------------------------------------------------------------------------

void SettingsWindow::on_php_click(const Hit& hit) {
    switch (hit.part) {
        case Part::PhpVersion:
            php_version_menu();
            break;
        case Part::PhpRuntimes:
            open_section(Section::Runtimes);
            break;
        case Part::PhpFind:
        case Part::PhpField:
            if (FieldEdit* edit = edit_of(hit); edit != nullptr) {
                SetFocus(edit->hwnd());
            } else if (hit.part == Part::PhpField && !php_saving_) {
                php_field_menu(hit.row);
            }
            break;
        case Part::PhpExt:
            if (php_ && !php_saving_ && !php_->extensions_fixed && hit.row >= 0 &&
                std::cmp_less(hit.row, geometry_.php.ext_names.size())) {
                const std::string& name = geometry_.php.ext_names[static_cast<std::size_t>(hit.row)];
                const auto it = std::find_if(php_->extensions.begin(), php_->extensions.end(),
                                             [&](const PhpExtension& e) { return e.name == name; });
                // One this install doesn't have can only be turned off.
                if (it != php_->extensions.end() && (!it->missing || extension_on(*php_, php_changes_, name))) {
                    toggle_extension(*php_, php_changes_, name);
                }
            }
            break;
        case Part::PhpMore:
            php_all_ = !php_all_;
            relayout();
            break;
        case Part::PhpEditIni:
        case Part::PhpFinalIni:
            if (connected_ && !php_series_.empty()) {
                message_.clear();
                worker_.php_custom_ini(php_series_, hit.part == Part::PhpFinalIni);
                relayout();
            }
            break;
        case Part::PhpReset:
            if (php_ && !php_saving_ && hit.row >= 0) {
                const std::string& key = field_at(hit.row).key;
                reset_ini(*php_, php_changes_, key);
                if (FieldEdit* edit = edit_of({Part::PhpField, -1, hit.row}); edit != nullptr) {
                    edit->set_text(ini_text(*php_, php_changes_, key));
                    InvalidateRect(edit->hwnd(), nullptr, TRUE);
                }
                relayout();
            }
            break;
        case Part::PhpDiscard:
            discard_php();
            break;
        case Part::PhpSave:
            (void)save_php();
            break;
        default:
            break;
    }
}

void SettingsWindow::php_version_menu() {
    const auto versions = php_versions();
    if (versions.empty() || php_saving_) {
        return;
    }
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    std::vector<std::wstring> labels;
    labels.reserve(versions.size());
    for (std::size_t i = 0; i < versions.size(); ++i) {
        labels.push_back(w("PHP " + versions[i].second));
        AppendMenuW(menu.get(), MF_STRING | (versions[i].first == php_series_ ? UINT{MF_CHECKED} : 0U),
                    kFirstChoice + static_cast<UINT>(i), labels.back().c_str());
    }
    const RECT box = geometry_.php.version;
    POINT at{box.left, box.bottom};
    ClientToScreen(hwnd_, &at);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
    flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    if (command < kFirstChoice || command - kFirstChoice >= versions.size() || hwnd_ == nullptr) {
        return;
    }
    const std::string& series = versions[command - kFirstChoice].first;
    if (series != php_series_ && settle_php_changes(AfterSave::Version, series)) {
        load_php(series);
    }
}

void SettingsWindow::php_field_menu(int index) {
    if (!php_ || index < 0 || std::cmp_greater_equal(index, settings_php_ini_fields().size())) {
        return;
    }
    const PhpIniField& field = field_at(index);
    std::vector<std::string> choices = field.choices;
    const std::string current = ini_text(*php_, php_changes_, field.key);
    if (!current.empty() && std::find(choices.begin(), choices.end(), current) == choices.end()) {
        choices.push_back(current);  // what the series has, set some other way
    }
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    std::vector<std::wstring> labels;
    labels.reserve(choices.size());
    for (std::size_t i = 0; i < choices.size(); ++i) {
        labels.push_back(w(choices[i]));
        AppendMenuW(menu.get(), MF_STRING | (choices[i] == current ? UINT{MF_CHECKED} : 0U),
                    kFirstChoice + static_cast<UINT>(i), labels.back().c_str());
    }
    const RECT box = geometry_.php.boxes[static_cast<std::size_t>(index)];
    POINT at{box.left, box.bottom};
    ClientToScreen(hwnd_, &at);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
    flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    if (command < kFirstChoice || command - kFirstChoice >= choices.size() || hwnd_ == nullptr || !php_) {
        return;
    }
    set_ini_text(*php_, php_changes_, field.key, choices[command - kFirstChoice]);
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- painting ----------------------------------------------------------------------

void SettingsWindow::paint_php_shapes(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const PhpGeometry& pg = g.php;
    const float border = std::max(1.0F, pxf(1.0F));
    const RECT column{main_left() - px(4), 0, width_, height_};
    c.clip(column);
    const bool changes = php_ && !php_changes_.empty();
    const auto field_box = [&](const RECT& r, bool marked, bool hover, bool edit_focused, const Hit& hit) {
        const bool refused = hit.part == Part::PhpField && hit.row == pg.error_field;
        c.round_rect(r, pxf(4), marked ? p.input_set : (hover ? p.row_hover : p.input),
                     refused ? p.banner_error.text : (marked ? p.accent : p.input_border), refused ? pxf(2) : border);
        if (edit_focused) {
            c.round_outline(r, pxf(4), p.accent, pxf(2));
        } else if (is_focused(hit)) {
            paint_ring(c, r);
        }
    };
    const auto chevron = [&](const RECT& r) {
        const float vx = static_cast<float>(r.right) - pxf(14);
        const float vy = static_cast<float>(r.top + r.bottom) / 2.0F;
        c.line(vx - pxf(4), vy - pxf(2), vx, vy + pxf(2), p.text_subtle, pxf(1.3F));
        c.line(vx, vy + pxf(2), vx + pxf(4), vy - pxf(2), p.text_subtle, pxf(1.3F));
    };

    if (!is_empty(pg.version)) {
        const Hit hit{Part::PhpVersion, -1, -1};
        field_box(pg.version, false, hover_ == hit, false, hit);
        chevron(pg.version);
    }
    if (!is_empty(g.message)) {
        c.round_rect(g.message, pxf(4), message_error_ ? p.banner_error.fill : p.banner_info.fill);
    }
    if (!is_empty(pg.runtimes)) {
        const Hit hit{Part::PhpRuntimes, -1, -1};
        paint_button(c, pg.runtimes, hover_ == hit, is_focused(hit), connected_);
    }
    if (!is_empty(pg.problems)) {
        c.round_rect(pg.problems, pxf(4), p.banner_error.fill);
    }
    if (php_) {
        c.round_rect(pg.ext_card, pxf(8), p.card, p.card_border, border);
        const Hit find{Part::PhpFind, -1, -1};
        field_box(pg.find, false, false, !edits_.empty() && edits_.front()->focused(), find);
        for (std::size_t i = 0; i < pg.ext_cells.size() && i < pg.ext_names.size(); ++i) {
            const RECT& cell = pg.ext_cells[i];
            const Hit hit{Part::PhpExt, -1, static_cast<int>(i)};
            if (hover_ == hit && !php_->extensions_fixed) {
                c.round_rect(cell, pxf(4), p.row_hover);
            }
            const int mid = (cell.top + cell.bottom) / 2;
            const RECT box{cell.left + px(2), mid - px(kCheckBox) / 2, cell.left + px(2) + px(kCheckBox),
                           mid + px(kCheckBox) / 2};
            const bool on = extension_on(*php_, php_changes_, pg.ext_names[i]);
            if (php_->extensions_fixed) {
                c.round_rect(box, pxf(3), p.button_disabled, p.button_border, border);
            } else if (on) {
                c.round_rect(box, pxf(3), p.accent);
            } else {
                c.round_rect(box, pxf(3), p.input, p.switch_off_border, border);
            }
            if (on) {
                const Rgb mark = php_->extensions_fixed ? p.button_disabled_text : p.accent_text;
                const auto bx = static_cast<float>(box.left);
                const auto by = static_cast<float>(box.top);
                c.line(bx + pxf(3.5F), by + pxf(8), bx + pxf(6.5F), by + pxf(11), mark, pxf(1.6F));
                c.line(bx + pxf(6.5F), by + pxf(11), bx + pxf(12.5F), by + pxf(5), mark, pxf(1.6F));
            }
            if (is_focused(hit)) {
                paint_ring(c, cell);
            }
        }
        if (is_focused({Part::PhpMore, -1, -1})) {
            paint_ring(c, pg.more);
        }

        c.round_rect(pg.ini_card, pxf(8), p.card, p.card_border, border);
        const Hit edit_ini{Part::PhpEditIni, -1, -1};
        paint_button(c, pg.edit_ini, hover_ == edit_ini, is_focused(edit_ini), connected_);
        const Hit final_ini{Part::PhpFinalIni, -1, -1};
        paint_button(c, pg.final_ini, hover_ == final_ini, is_focused(final_ini), connected_);
        const auto& fields = settings_php_ini_fields();
        for (std::size_t i = 0; i < pg.boxes.size() && i < fields.size(); ++i) {
            const Hit hit{Part::PhpField, -1, static_cast<int>(i)};
            const FieldEdit* edit = edit_of(hit);
            field_box(pg.boxes[i], ini_marked(*php_, php_changes_, fields[i].key), edit == nullptr && hover_ == hit,
                      edit != nullptr && edit->focused(), hit);
            if (edit == nullptr) {
                chevron(pg.boxes[i]);
            }
            const Hit reset{Part::PhpReset, -1, static_cast<int>(i)};
            if (!is_empty(pg.resets[i]) && is_focused(reset)) {
                paint_ring(c, pg.resets[i]);
            }
        }

        const Hit discard{Part::PhpDiscard, -1, -1};
        paint_button(c, pg.discard, hover_ == discard && changes, is_focused(discard), changes && !php_saving_);
        const Hit save{Part::PhpSave, -1, -1};
        const bool can_save = changes && !php_saving_ && connected_;
        if (can_save) {
            c.round_rect(pg.save, pxf(4), p.accent);
            if (is_focused(save)) {
                paint_ring(c, pg.save);
            }
        } else {
            paint_button(c, pg.save, false, is_focused(save), false);
        }
    }
    // A thin scroll indicator when the page is taller than the window.
    if (g.content_h > height_ && height_ > 0) {
        const int thumb = std::max(px(24), height_ * height_ / g.content_h);
        const int top = (height_ - thumb) * scroll_ / (g.content_h - height_);
        c.round_rect({width_ - px(6), top + px(2), width_ - px(2), top + thumb - px(2)}, pxf(2), p.switch_off_border);
    }
    c.unclip();
}

void SettingsWindow::paint_php_text(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const PhpGeometry& pg = g.php;
    const RECT column{main_left() - px(4), 0, width_, height_};
    c.clip_text(column);
    c.text(g.title, w(text(lang_, Str::SettingsPhp)), fonts_.title.get(), p.text, Align::Start, false);
    c.paragraph(g.hint, w(text(lang_, Str::PhpHint)), fonts_.caption.get(), p.text_subtle);
    if (!is_empty(pg.version)) {
        c.text(pg.version_label, w(text(lang_, Str::PhpVersion)), fonts_.caption.get(), p.text_subtle, Align::End, false);
        std::string label = "PHP " + php_series_;
        for (const auto& v : php_versions()) {
            if (v.first == php_series_) {
                label = "PHP " + v.second;
            }
        }
        RECT box = pg.version;
        box.left += px(8);
        box.right -= px(26);
        c.text(box, w(label), fonts_.button.get(), p.text, Align::Start, true);
    }
    if (!is_empty(g.message)) {
        RECT inner = g.message;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(message_), fonts_.button.get(), message_error_ ? p.banner_error.text : p.banner_info.text);
    }
    if (!is_empty(pg.note)) {
        if (is_empty(pg.runtimes)) {
            c.text(pg.note, w(text(lang_, Str::SettingsLoading)), fonts_.button.get(), p.text_subtle, Align::Start, false);
        } else {
            c.paragraph(pg.note, w(text(lang_, Str::PhpNoneInstalled)), fonts_.button.get(), p.text);
        }
    }
    if (!is_empty(pg.runtimes)) {
        c.text(pg.runtimes, w(text(lang_, Str::PhpGoToRuntimes)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (!php_) {
        c.unclip_text();
        return;
    }
    if (!is_empty(pg.problems)) {
        std::string all;
        for (const std::string& problem : php_->problems) {
            all += (all.empty() ? "" : "; ") + problem;
        }
        RECT inner = pg.problems;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(fill(lang_, Str::PhpProblems, {all})), fonts_.caption.get(), p.banner_error.text);
    }

    c.text(pg.ext_title, w(text(lang_, Str::PhpExtensions)), fonts_.body_bold.get(), p.text, Align::Start, false);
    c.text(pg.ext_count,
           w(fill(lang_, Str::PhpExtensionsOn,
                  {std::to_string(extensions_on(*php_, php_changes_)), std::to_string(php_->extensions.size())})),
           fonts_.caption.get(), p.text_subtle, Align::Start, false);
    for (std::size_t i = 0; i < pg.ext_cells.size() && i < pg.ext_names.size(); ++i) {
        const RECT& cell = pg.ext_cells[i];
        const std::string& name = pg.ext_names[i];
        const auto it = std::find_if(php_->extensions.begin(), php_->extensions.end(),
                                     [&](const PhpExtension& e) { return e.name == name; });
        const bool missing = it != php_->extensions.end() && it->missing;
        const RECT label{cell.left + px(2) + px(kCheckBox) + px(8), cell.top,
                         is_empty(pg.ext_extras[i]) ? cell.right : pg.ext_extras[i].left, cell.bottom};
        c.text(label, w(name), fonts_.button.get(),
               php_->extensions_fixed || missing ? p.button_disabled_text : p.text, Align::Start, true);
        if (!is_empty(pg.ext_extras[i])) {
            c.text(pg.ext_extras[i], w(text(lang_, missing ? Str::PhpMissing : Str::PhpSlows)), fonts_.caption.get(),
                   p.text_subtle, Align::Start, false);
        }
    }
    {
        std::size_t hidden = 0;
        const std::vector<PhpExtension> shown = shown_extensions(hidden);
        std::string note_text;
        if (php_->extensions_fixed) {
            note_text = std::string(text(lang_, Str::PhpFixedNote));
        } else if (!php_query_.empty() && shown.empty()) {
            note_text = fill(lang_, Str::PhpNoMatch, {php_query_});
        } else {
            note_text = std::string(text(lang_, Str::PhpOpcacheNote));
            if (hidden > 0) {
                note_text += " " + fill(lang_, Str::PhpMoreCount, {std::to_string(hidden)});
            }
        }
        c.text(pg.ext_note, w(note_text), fonts_.caption.get(), p.text_subtle, Align::Start, false);
        if (!is_empty(pg.more)) {
            c.text(pg.more, w(text(lang_, hidden > 0 ? Str::PhpShowAll : Str::PhpShowFewer)), fonts_.caption.get(),
                   p.link, Align::Start, false);
        }
    }

    c.text(pg.ini_title, L"php.ini", fonts_.body_bold.get(), p.text, Align::Start, true);
    c.text(pg.ini_hint, w(text(lang_, Str::PhpIniHint)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    c.text(pg.edit_ini, w(text(lang_, Str::PhpEditFull)), fonts_.button.get(), connected_ ? p.text : p.button_disabled_text,
           Align::Center, false);
    c.text(pg.final_ini, w(text(lang_, Str::PhpShowFinal)), fonts_.button.get(),
           connected_ ? p.text : p.button_disabled_text, Align::Center, false);
    const auto& fields = settings_php_ini_fields();
    for (std::size_t i = 0; i < pg.boxes.size() && i < fields.size(); ++i) {
        c.text(pg.labels[i], w(fields[i].key), fonts_.caption.get(), p.text_subtle, Align::Start, true);
        if (!is_empty(pg.resets[i])) {
            c.text(pg.resets[i], w(text(lang_, Str::PhpReset)), fonts_.caption.get(), p.link, Align::End, false);
        }
        if (fields[i].choices.empty()) {
            continue;  // its edit box draws the text
        }
        RECT box = pg.boxes[i];
        box.left += px(8);
        box.right -= px(26);
        const std::string value = ini_text(*php_, php_changes_, fields[i].key);
        if (value.empty()) {
            c.text(box, w(text(lang_, Str::PhpDefaultCue)), fonts_.button.get(), p.text_subtle, Align::Start, false);
        } else {
            c.text(box, w(value), fonts_.button.get(), p.text, Align::Start, true);
        }
    }
    if (!is_empty(pg.field_error)) {
        c.text(pg.field_error, w(rtl_ ? isolate_ltr(php_error_) : php_error_), fonts_.caption.get(), p.banner_error.text,
               Align::Start, false);
    }
    if (!is_empty(pg.custom_note)) {
        c.text(pg.custom_note, w(text(lang_, Str::PhpCustomIniUsed)), fonts_.caption.get(), p.text_subtle,
               Align::Start, false);
    }

    const bool changes = !php_changes_.empty();
    c.paragraph(pg.footer_text, w(php_restart_text(*php_, lang_)), fonts_.caption.get(), p.text_subtle);
    c.text(pg.discard, w(text(lang_, Str::PhpDiscard)), fonts_.button.get(),
           changes && !php_saving_ ? p.text : p.button_disabled_text, Align::Center, false);
    const bool can_save = changes && !php_saving_ && connected_;
    c.text(pg.save, w(text(lang_, php_saving_ ? Str::PhpSaving : Str::PhpSave)), fonts_.button.get(),
           can_save ? p.accent_text : p.button_disabled_text, Align::Center, false);
    c.unclip_text();
}

}  // namespace floserver::ui

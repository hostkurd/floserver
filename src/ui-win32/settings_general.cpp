// Settings > General: the window's language and theme, starting FloServer
// with Windows, what closing the window does, and About (the version, the
// website, the licences).
//
// Opening the section reads the Run entry once; nothing is asked of the
// service. The language, theme and closing choices are the window's own:
// they're posted to FloServer's window, which saves them in window.json and
// applies them at once (a new language makes both windows again, see
// MainWindow::change_language). Starting with Windows is written here, in
// HKCU's Run key, because it's this user's and only the window needs it.

#include "settings_window.h"

#include "main_window_layout.h"
#include "model/theme.h"
#include "window_prefs.h"

#include "floserver/core/build_info.h"
#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"

#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

namespace floserver::ui {

using layout::height_of;

namespace {

constexpr int kGap = 12;
constexpr int kCardPad = 16;
constexpr int kControlH = 30;
constexpr int kRadio = 16;
constexpr int kSwitchW = 40;
constexpr int kSwitchH = 20;
constexpr std::string_view kWebsite = "https://floserver.org";
constexpr std::string_view kWebsiteName = "floserver.org";
constexpr std::string_view kLicencesFile = "licences.txt";

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

// Windows' paths are compared without case.
bool same_path(const std::string& a, const std::string& b) {
    const std::wstring wa = w(a);
    const std::wstring wb = w(b);
    return CompareStringOrdinal(wa.c_str(), static_cast<int>(wa.size()), wb.c_str(), static_cast<int>(wb.size()),
                                TRUE) == CSTR_EQUAL;
}

// A language choice's label: "Same as Windows", or the language's own name.
std::string lang_choice_label(LangChoice choice, Lang lang) {
    switch (choice) {
        case LangChoice::En:
            return std::string(language_name(Lang::En));
        case LangChoice::Ar:
            return std::string(language_name(Lang::Ar));
        case LangChoice::Ku:
            return std::string(language_name(Lang::Ku));
        case LangChoice::System:
            break;
    }
    return std::string(text(lang, Str::GeneralSameAsWindows));
}

Str theme_choice_label(ThemeChoice choice) {
    switch (choice) {
        case ThemeChoice::Light:
            return Str::GeneralLight;
        case ThemeChoice::Dark:
            return Str::GeneralDark;
        case ThemeChoice::System:
            break;
    }
    return Str::GeneralSameAsWindows;
}

}  // namespace

// --- what the owner says, and opening ---------------------------------------

void SettingsWindow::show_general() {
    open_section(Section::General);
    focus_ = static_cast<int>(Section::General);
}

void SettingsWindow::set_prefs(const WindowPrefs& prefs, const std::string& error, const std::string& kept_as) {
    prefs_ = prefs;
    if (!error.empty()) {
        show_message(fill(lang_, Str::GeneralSaveFailed, {error}), true);
        return;
    }
    if (!kept_as.empty()) {
        // window.json couldn't be read: it was kept, and a new one written.
        show_message(fill(lang_, Str::GeneralPrefsKept, {kept_as}), false);
        return;
    }
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::refresh_theme() {
    if (hwnd_ == nullptr) {
        return;
    }
    if (const bool dark = apps_use_dark_theme(); dark != dark_) {
        dark_ = dark;
        apply_theme();
    }
}

void SettingsWindow::destroy_now() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
    }
}

void SettingsWindow::load_general() {
    own_exe_ = own_exe_path();
    std::string error;
    run_entry_ = read_run_entry(error);
    if (!error.empty()) {
        show_message(fill(lang_, Str::GeneralStartFailed, {error}), true);
    }
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::shown_start_tray() const {
    // What Windows will do, when there's an entry for this copy; else the choice kept for next time.
    if (run_entry_ && !run_entry_->exe.empty() && !run_elsewhere()) {
        return run_entry_->tray;
    }
    return prefs_.start_in_tray;
}

bool SettingsWindow::run_elsewhere() const {
    return run_entry_ && !run_entry_->exe.empty() && !own_exe_.empty() && !same_path(run_entry_->exe, own_exe_);
}

// --- changes ---------------------------------------------------------------------

void SettingsWindow::post_pref(PrefChange change, std::size_t value) {
    PostMessageW(owner_, prefs_message_, static_cast<WPARAM>(change), static_cast<LPARAM>(value));
}

void SettingsWindow::set_start(bool on, bool tray) {
    std::string error;
    if (!write_run_entry(on, tray, error)) {
        show_message(fill(lang_, Str::GeneralStartFailed, {error}), true);
        return;
    }
    // What was written, without reading the registry again.
    if (on) {
        run_entry_ = RunEntry{own_exe_, tray, instance_suffix(core::ipc::default_instance())};
    } else {
        run_entry_.reset();
    }
    message_.clear();
    relayout();
}

void SettingsWindow::open_licences() {
    const auto data = core::data_dir();
    if (!data) {
        show_message(fill(lang_, Str::GeneralLicencesFailed, {"LOCALAPPDATA"}), true);
        return;
    }
    // runtimes.list's answer from when Settings opened: no request of its own.
    std::string body = licences_text(lang_, core::kVersion, has_reply_ ? &reply_ : nullptr);
    std::string bytes = "\xEF\xBB\xBF";  // UTF-8's mark, so every Notepad reads the Arabic and Kurdish right
    bytes += body;
    const std::filesystem::path file = *data / std::string(kLicencesFile);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::string error;
    if (!core::write_file_atomically(file, bytes, error)) {
        show_message(fill(lang_, Str::GeneralLicencesFailed, {error}), true);
        return;
    }
    const std::wstring quoted = L"\"" + file.wstring() + L"\"";
    const auto opened = reinterpret_cast<INT_PTR>(
        ShellExecuteW(hwnd_, L"open", L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL));
    if (opened <= 32) {
        show_message(fill(lang_, Str::GeneralLicencesFailed, {core::path_to_utf8(file)}), true);
        return;
    }
    show_message(std::string(text(lang_, Str::GeneralLicencesOpened)), false);
}

void SettingsWindow::on_general_click(const Hit& hit) {
    const auto row = static_cast<std::size_t>(std::max(0, hit.row));
    switch (hit.part) {
        case Part::GeneralLang:
            if (row < kLangChoices.size() && kLangChoices.at(row) != prefs_.lang) {
                prefs_.lang = kLangChoices.at(row);
                post_pref(PrefChange::Language, row);  // both windows are made again in it
            }
            break;
        case Part::GeneralTheme:
            if (row < kThemeChoices.size() && kThemeChoices.at(row) != prefs_.theme) {
                prefs_.theme = kThemeChoices.at(row);
                post_pref(PrefChange::Theme, row);
            }
            break;
        case Part::GeneralStart: {
            const bool on = !run_entry_.has_value();
            set_start(on, prefs_.start_in_tray);
            break;
        }
        case Part::GeneralStartMode: {
            const bool tray = row == 1;
            if (tray != prefs_.start_in_tray) {
                prefs_.start_in_tray = tray;
                post_pref(PrefChange::StartInTray, tray ? 1 : 0);
            }
            if (run_entry_ && (tray != run_entry_->tray || run_elsewhere() || run_entry_->exe.empty())) {
                set_start(true, tray);  // the entry follows (and starts this copy)
            }
            break;
        }
        case Part::GeneralClose: {
            const CloseAction close = row == 1 ? CloseAction::Quit : CloseAction::Tray;
            if (close != prefs_.close) {
                prefs_.close = close;
                post_pref(PrefChange::Close, row == 1 ? 1 : 0);
            }
            break;
        }
        case Part::GeneralUpdates: {
            // No updater yet: the website says what's new.
            const auto opened = reinterpret_cast<INT_PTR>(
                ShellExecuteW(hwnd_, L"open", w(kWebsite).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (opened <= 32) {
                show_message(fill(lang_, Str::OpenFailed, {kWebsite}), true);
            }
            break;
        }
        case Part::GeneralLicences:
            open_licences();
            break;
        default:
            break;
    }
    relayout();
}

// --- geometry ------------------------------------------------------------------------

void SettingsWindow::layout_general(HDC dc, Geometry& g, int x0, int x1, int y) const {
    GeneralGeometry& gg = g.general;
    const auto measure = [&](std::string_view s, HFONT font) { return Canvas::measure(dc, w(s), font, rtl_); };
    const auto height = [&](std::string_view s, HFONT font, int width) { return text_height(dc, w(s), font, width, rtl_); };

    const int hint_h = height(text(lang_, Str::GeneralHint), fonts_.caption.get(), x1 - x0);
    g.title = {x0, y, x1, y + px(28)};
    g.hint = {x0, g.title.bottom, x1, g.title.bottom + hint_h};
    y = static_cast<int>(g.hint.bottom) + px(kGap);
    if (!message_.empty()) {
        const int mh = std::max(px(32), height(message_, fonts_.button.get(), x1 - x0 - px(24)) + px(14));
        g.message = {x0, y, x1, y + mh};
        y += mh + px(kGap);
    }
    const int cx0 = x0 + px(kCardPad);
    const int cx1 = x1 - px(kCardPad);
    const int radio_x = cx0 + px(kRadio) + px(10);

    // One choice's row at yy: a label line and, when there's one, a note under it.
    const auto choice = [&](ChoiceGeometry& c, int left, int text_x, int& yy, std::string_view note) {
        const int text_w = std::max(px(80), cx1 - text_x);
        const int note_h = note.empty() ? 0 : height(note, fonts_.caption.get(), text_w);
        const int row_h = std::max(px(kControlH), px(20) + note_h + px(8));
        c.row = {left - px(6), yy, cx1, yy + row_h};
        c.label = {text_x, yy + px(4), cx1, yy + px(24)};
        c.note = note.empty() ? RECT{} : RECT{text_x, static_cast<int>(c.label.bottom), cx1,
                                              static_cast<int>(c.label.bottom) + note_h};
        yy += row_h + px(2);
    };
    const auto card_top = [&](RECT& card, RECT& title, int& yy) {
        card = {x0, y, x1, y};
        yy = y + px(14);
        title = {cx0, yy, cx1, yy + px(24)};
        yy = static_cast<int>(title.bottom) + px(6);
    };
    const auto card_end = [&](RECT& card, int yy) {
        card.bottom = yy + px(12);
        y = static_cast<int>(card.bottom) + px(kGap);
    };
    int yy = 0;

    // Language: four choices, the languages in their own script.
    card_top(gg.lang_card, gg.lang_title, yy);
    for (std::size_t i = 0; i < kLangChoices.size(); ++i) {
        choice(gg.langs.at(i), cx0, radio_x, yy, {});
    }
    const int note_h = height(text(lang_, Str::GeneralLanguageNote), fonts_.caption.get(), cx1 - cx0);
    gg.lang_note = {cx0, yy + px(4), cx1, yy + px(4) + note_h};
    card_end(gg.lang_card, static_cast<int>(gg.lang_note.bottom));

    // Theme: three choices.
    card_top(gg.theme_card, gg.theme_title, yy);
    for (std::size_t i = 0; i < kThemeChoices.size(); ++i) {
        choice(gg.themes.at(i), cx0, radio_x, yy, {});
    }
    card_end(gg.theme_card, yy);

    // Starting with Windows: the switch, then what it opens, set in under it.
    card_top(gg.start_card, gg.start_title, yy);
    const int switch_text_x = cx0 + px(kSwitchW) + px(12);
    choice(gg.start, cx0, switch_text_x, yy, text(lang_, Str::GeneralStartNote));
    gg.start_switch = {cx0, gg.start.row.top + px(4), cx0 + px(kSwitchW), gg.start.row.top + px(4) + px(kSwitchH)};
    for (std::size_t i = 0; i < gg.modes.size(); ++i) {
        choice(gg.modes.at(i), switch_text_x, switch_text_x + px(kRadio) + px(10), yy, {});
    }
    if (run_elsewhere()) {
        const std::string note = fill(lang_, Str::GeneralStartElsewhere, {run_entry_->exe});
        const int h = height(note, fonts_.caption.get(), cx1 - switch_text_x);
        gg.elsewhere = {switch_text_x, yy + px(2), cx1, yy + px(2) + h};
        yy = static_cast<int>(gg.elsewhere.bottom);
    }
    card_end(gg.start_card, yy);

    // Closing the window: two choices, each with what it means.
    card_top(gg.close_card, gg.close_title, yy);
    choice(gg.closes[0], cx0, radio_x, yy, text(lang_, Str::GeneralCloseTrayNote));
    choice(gg.closes[1], cx0, radio_x, yy, text(lang_, Str::GeneralCloseQuitNote));
    card_end(gg.close_card, yy);

    // About: the version, two buttons and where the first one goes.
    card_top(gg.about_card, gg.about_title, yy);
    gg.version = {cx0, yy, cx1, yy + px(20)};
    yy = static_cast<int>(gg.version.bottom) + px(10);
    int bx = cx0;
    const auto button = [&](RECT& r, Str label) {
        const int bw = measure(text(lang_, label), fonts_.button.get()) + px(24);
        if (bx > cx0 && bx + bw > cx1) {  // no room on this line
            bx = cx0;
            yy += px(kControlH) + px(8);
        }
        r = {bx, yy, bx + bw, yy + px(kControlH)};
        bx += bw + px(8);
    };
    button(gg.updates, Str::GeneralCheckUpdates);
    button(gg.licences, Str::GeneralLicences);
    yy += px(kControlH) + px(8);
    const std::string updates_note = fill(lang_, Str::GeneralUpdatesNote, {kWebsiteName});
    gg.updates_note = {cx0, yy, cx1, yy + height(updates_note, fonts_.caption.get(), cx1 - cx0)};
    card_end(gg.about_card, static_cast<int>(gg.updates_note.bottom));

    g.content_h = y + scroll_ + px(8);
}

const RECT* SettingsWindow::general_rect_of(const Hit& hit) const {
    const GeneralGeometry& g = geometry_.general;
    const auto at = [&](const auto& list) -> const RECT* {
        return hit.row >= 0 && std::cmp_less(hit.row, list.size()) ? &list[static_cast<std::size_t>(hit.row)].row
                                                                     : nullptr;
    };
    switch (hit.part) {
        case Part::GeneralLang:
            return at(g.langs);
        case Part::GeneralTheme:
            return at(g.themes);
        case Part::GeneralStart:
            return &g.start.row;
        case Part::GeneralStartMode:
            return at(g.modes);
        case Part::GeneralClose:
            return at(g.closes);
        case Part::GeneralUpdates:
            return &g.updates;
        case Part::GeneralLicences:
            return &g.licences;
        default:
            break;
    }
    return nullptr;
}

void SettingsWindow::general_focus_order(std::vector<Hit>& order) const {
    for (std::size_t i = 0; i < kLangChoices.size(); ++i) {
        order.push_back({Part::GeneralLang, -1, static_cast<int>(i)});
    }
    for (std::size_t i = 0; i < kThemeChoices.size(); ++i) {
        order.push_back({Part::GeneralTheme, -1, static_cast<int>(i)});
    }
    order.push_back({Part::GeneralStart, -1, -1});
    order.push_back({Part::GeneralStartMode, -1, 0});
    order.push_back({Part::GeneralStartMode, -1, 1});
    order.push_back({Part::GeneralClose, -1, 0});
    order.push_back({Part::GeneralClose, -1, 1});
    order.push_back({Part::GeneralUpdates, -1, -1});
    order.push_back({Part::GeneralLicences, -1, -1});
}

// --- painting ----------------------------------------------------------------------

void SettingsWindow::paint_radio(Canvas& c, const ChoiceGeometry& choice, bool on, const Hit& hit) const {
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    if (hover_ == hit) {
        c.round_rect(choice.row, pxf(4), p.row_hover);
    }
    // Beside the label's line, at the label's start.
    const float cx = static_cast<float>(choice.label.left - px(10)) - pxf(kRadio / 2.0F);
    const float cy = static_cast<float>(choice.label.top + choice.label.bottom) / 2.0F;
    if (on) {
        c.ellipse(cx, cy, pxf(kRadio / 2.0F), p.accent);
        c.ellipse(cx, cy, pxf(3.5F), p.accent_text);
    } else {
        c.ellipse(cx, cy, pxf(kRadio / 2.0F), p.input);
        c.circle(cx, cy, pxf(kRadio / 2.0F) - border / 2.0F, p.switch_off_border, border);
    }
    if (is_focused(hit)) {
        paint_ring(c, choice.row);
    }
}

void SettingsWindow::paint_general_shapes(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const GeneralGeometry& gg = g.general;
    const float border = std::max(1.0F, pxf(1.0F));
    const RECT column{main_left() - px(4), 0, width_, height_};
    c.clip(column);
    if (!is_empty(g.message)) {
        c.round_rect(g.message, pxf(4), message_error_ ? p.banner_error.fill : p.banner_info.fill);
    }
    for (const RECT* card : {&gg.lang_card, &gg.theme_card, &gg.start_card, &gg.close_card, &gg.about_card}) {
        c.round_rect(*card, pxf(8), p.card, p.card_border, border);
    }
    for (std::size_t i = 0; i < kLangChoices.size(); ++i) {
        paint_radio(c, gg.langs.at(i), kLangChoices.at(i) == prefs_.lang, {Part::GeneralLang, -1, static_cast<int>(i)});
    }
    for (std::size_t i = 0; i < kThemeChoices.size(); ++i) {
        paint_radio(c, gg.themes.at(i), kThemeChoices.at(i) == prefs_.theme,
                    {Part::GeneralTheme, -1, static_cast<int>(i)});
    }

    // The switch, as in Ports & HTTPS.
    const Hit start{Part::GeneralStart, -1, -1};
    if (hover_ == start) {
        c.round_rect(gg.start.row, pxf(4), p.row_hover);
    }
    const RECT& r = gg.start_switch;
    const float radius = static_cast<float>(height_of(r)) / 2.0F;
    const float knob = pxf(hover_ == start ? 6.0F : 5.0F);
    const float ky = static_cast<float>(r.top) + radius;
    if (run_entry_) {
        c.round_rect(r, radius, p.accent);
        c.ellipse(static_cast<float>(r.right) - pxf(9), ky, knob, p.switch_on_knob);
    } else {
        c.round_rect(r, radius, p.switch_off_fill, p.switch_off_border, border);
        c.ellipse(static_cast<float>(r.left) + pxf(9), ky, knob, p.switch_off_knob);
    }
    if (is_focused(start)) {
        paint_ring(c, gg.start.row);
    }
    const bool tray = shown_start_tray();
    paint_radio(c, gg.modes[0], !tray, {Part::GeneralStartMode, -1, 0});
    paint_radio(c, gg.modes[1], tray, {Part::GeneralStartMode, -1, 1});

    paint_radio(c, gg.closes[0], prefs_.close == CloseAction::Tray, {Part::GeneralClose, -1, 0});
    paint_radio(c, gg.closes[1], prefs_.close == CloseAction::Quit, {Part::GeneralClose, -1, 1});

    for (const auto& [part, rect] : {std::pair{Part::GeneralUpdates, gg.updates}, std::pair{Part::GeneralLicences, gg.licences}}) {
        const Hit hit{part, -1, -1};
        paint_button(c, rect, hover_ == hit, is_focused(hit), true);
    }
    // The scroll bar, when the page is taller than the window.
    scroll_bar_.paint(c, p);
    c.unclip();
}

void SettingsWindow::paint_general_text(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const GeneralGeometry& gg = g.general;
    const RECT column{main_left() - px(4), 0, width_, height_};
    c.clip_text(column);
    c.text(g.title, w(text(lang_, Str::SettingsGeneral)), fonts_.title.get(), p.text, Align::Start, false);
    c.paragraph(g.hint, w(text(lang_, Str::GeneralHint)), fonts_.caption.get(), p.text_subtle);
    if (!is_empty(g.message)) {
        RECT inner = g.message;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(message_), fonts_.button.get(), message_error_ ? p.banner_error.text : p.banner_info.text);
    }
    const auto title = [&](const RECT& r, Str key) {
        c.text(r, w(text(lang_, key)), fonts_.body_bold.get(), p.text, Align::Start, false);
    };
    const auto label = [&](const ChoiceGeometry& choice, std::string_view s, std::string_view note) {
        c.text(choice.label, w(s), fonts_.body.get(), p.text, Align::Start, false);
        if (!note.empty() && !is_empty(choice.note)) {
            c.paragraph(choice.note, w(note), fonts_.caption.get(), p.text_subtle);
        }
    };

    title(gg.lang_title, Str::GeneralLanguage);
    for (std::size_t i = 0; i < kLangChoices.size(); ++i) {
        label(gg.langs.at(i), lang_choice_label(kLangChoices.at(i), lang_), {});
    }
    c.paragraph(gg.lang_note, w(text(lang_, Str::GeneralLanguageNote)), fonts_.caption.get(), p.text_subtle);

    title(gg.theme_title, Str::GeneralTheme);
    for (std::size_t i = 0; i < kThemeChoices.size(); ++i) {
        label(gg.themes.at(i), text(lang_, theme_choice_label(kThemeChoices.at(i))), {});
    }

    title(gg.start_title, Str::GeneralStartup);
    label(gg.start, text(lang_, Str::GeneralStartWithWindows), text(lang_, Str::GeneralStartNote));
    label(gg.modes[0], text(lang_, Str::GeneralStartOpen), {});
    label(gg.modes[1], text(lang_, Str::GeneralStartTray), {});
    if (!is_empty(gg.elsewhere)) {
        c.paragraph(gg.elsewhere, w(fill(lang_, Str::GeneralStartElsewhere, {run_entry_ ? run_entry_->exe : std::string()})),
                    fonts_.caption.get(), p.asleep.text);
    }

    title(gg.close_title, Str::GeneralClosing);
    label(gg.closes[0], text(lang_, Str::GeneralCloseTray), text(lang_, Str::GeneralCloseTrayNote));
    label(gg.closes[1], text(lang_, Str::GeneralCloseQuit), text(lang_, Str::GeneralCloseQuitNote));

    title(gg.about_title, Str::GeneralAbout);
    c.text(gg.version, w(fill(lang_, Str::GeneralVersion, {core::kVersion})), fonts_.body.get(), p.text, Align::Start,
           false);
    c.text(gg.updates, w(text(lang_, Str::GeneralCheckUpdates)), fonts_.button.get(), p.text, Align::Center, false);
    c.text(gg.licences, w(text(lang_, Str::GeneralLicences)), fonts_.button.get(), p.text, Align::Center, false);
    c.paragraph(gg.updates_note, w(fill(lang_, Str::GeneralUpdatesNote, {kWebsiteName})), fonts_.caption.get(),
                p.text_subtle);
    c.unclip_text();
}

}  // namespace floserver::ui

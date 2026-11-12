// Settings > General: the window's language and theme, starting FloServer
// with Windows, what closing the window does, Updates (Q5: when FloServer
// last looked, Check now, and a newer version that's ready), AI coding tools
// (D32: the lines that set up `flo mcp`, each with Copy), and About (the
// version, the licences), then Remove FloServer (D21).
//
// Opening the section reads the Run entry once; nothing is asked of the
// service. The language, theme and closing choices are the window's own:
// they're posted to FloServer's window, which saves them in window.json and
// applies them at once (a new language makes both windows again, see
// MainWindow::change_language). Starting with Windows is written here, in
// HKCU's Run key, because it's this user's and only the window needs it.
// Remove FloServer… asks FloServer's window to open the Uninstall window,
// which runs the clean-up (main_window_uninstall.cpp, D25).

#include "settings_window.h"

#include "code_editors.h"
#include "main_window_layout.h"
#include "model/theme.h"
#include "uninstall.h"
#include "window_prefs.h"

#include "floserver/core/build_info.h"
#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/user_path.h"

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

// "2026-11-05T09:12:00Z" in this computer's time zone.
std::optional<LocalTime> local_time_of(const std::string& utc) {
    const auto t = parse_checked(utc);
    if (!t) {
        return std::nullopt;
    }
    constexpr std::int64_t kEpochDifference = 116444736000000000LL;  // 1601 -> 1970, in 100 ns
    const std::int64_t ticks = t->time_since_epoch().count() * 10000000LL + kEpochDifference;
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(ticks) & 0xFFFFFFFFU);
    ft.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(ticks) >> 32U);
    SYSTEMTIME st_utc{};
    SYSTEMTIME local{};
    if (FileTimeToSystemTime(&ft, &st_utc) == FALSE || SystemTimeToTzSpecificLocalTime(nullptr, &st_utc, &local) == FALSE) {
        return std::nullopt;
    }
    return LocalTime{local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute};
}

LocalTime local_now() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return {st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute};
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

void SettingsWindow::show_general_editor() {
    if (hwnd_ == nullptr) {
        return;
    }
    bring_to_front();
    if (asking_) {
        return;
    }
    if (section_ != Section::General) {
        open_section(Section::General);
    } else {
        load_general();  // looked for again: the user may have just installed one
    }
    if (section_ != Section::General) {
        return;  // a Save question elsewhere was answered with Cancel
    }
    // Its card at the top (as far as the page scrolls).
    relayout();
    scroll_to(scroll_ + static_cast<int>(geometry_.general.editor_card.top) - px(8));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::load_general() {
    editors_found_ = detect_editors();  // files and the registry; nothing is run
    check_editor_program();
    own_exe_ = own_exe_path();
    std::string error;
    run_entry_ = read_run_entry(error);
    if (!error.empty()) {
        show_message(fill(lang_, Str::GeneralStartFailed, {error}), true);
    }
    load_mcp_lines();
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::load_mcp_lines() {
    // flo.exe beside FloServer.exe; "flo" when that's the one a new terminal
    // finds (the installer puts this folder on the user's Path), else its full path.
    const std::filesystem::path flo =
        own_exe_.empty() ? std::filesystem::path() : core::path_from_utf8(own_exe_).parent_path() / L"flo.exe";
    const auto found = core::first_on_path("flo");
    const bool on_path = found && !flo.empty() && same_path(core::path_to_utf8(*found), core::path_to_utf8(flo));
    mcp_lines_ = mcp_lines(flo, on_path || flo.empty());
}

const std::string& SettingsWindow::mcp_line(std::size_t row) const {
    return row == 0 ? mcp_lines_.command : row == 1 ? mcp_lines_.claude : row == 2 ? mcp_lines_.json : mcp_lines_.vscode;
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
    clear_toast();
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

std::string SettingsWindow::updates_summary_text() const {
    // The service's own version when it has said it; else this window's.
    const std::string current = updates_.current.empty() ? std::string(core::kVersion) : updates_.current;
    return updates_summary(updates_, current, local_time_of(updates_.last_checked), local_now(), lang_);
}

bool SettingsWindow::can_check_updates() const {
    return connected_ && updates_.available && updates_.state != UpdateState::Unknown &&
           updates_.state != UpdateState::Checking && updates_.state != UpdateState::Downloading;
}

void SettingsWindow::remove_floserver() {
    if (asking_) {
        return;
    }
    // FloServer's window opens the Uninstall window (D25, floserver-setup.dll):
    // it owns the service's link and quits at the end.
    PostMessageW(owner_, prefs_message_, static_cast<WPARAM>(PrefChange::RemoveFloServer), 0);
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
        case Part::GeneralEditor: {
            const std::vector<EditorRow> rows = editor_rows();
            if (row >= rows.size()) {
                break;
            }
            if (rows[row].program) {
                choose_editor_program();  // also to change the one chosen before
            } else if (rows[row].choice != prefs_.code_editor) {
                prefs_.code_editor = rows[row].choice;
                check_editor_program();
                post_pref(PrefChange::CodeEditor, row);
            }
            break;
        }
        case Part::GeneralUpdates:
            // The answer comes with the main window's next poll ("checking now…" meanwhile).
            if (can_check_updates()) {
                worker_.check_updates();
                updates_.state = UpdateState::Checking;
            }
            break;
        case Part::GeneralUpdateNotes: {
            // Only the signed list's https address ever reaches the shell.
            if (!is_openable_notes(updates_.notes_url)) {
                break;
            }
            const auto opened = reinterpret_cast<INT_PTR>(
                ShellExecuteW(hwnd_, L"open", w(updates_.notes_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (opened <= 32) {
                show_message(fill(lang_, Str::OpenFailed, {updates_.notes_url}), true);
            }
            break;
        }
        case Part::GeneralUpdateNow:
            // FloServer's window starts the update and quits (Settings goes with it).
            if (updates_.state == UpdateState::Ready) {
                post_pref(PrefChange::UpdateNow, 0);
            }
            return;
        case Part::GeneralAiCopy:
            // Nothing secret: a command and where flo is.
            if (row < geometry_.general.ai_copy.size()) {
                if (put_text_on_clipboard(hwnd_, mcp_line(row))) {
                    show_message(std::string(text(lang_, Str::MailCopied)), false);
                } else {
                    show_message(fill(lang_, Str::OpenFailed, {"clipboard"}), true);
                }
            }
            break;
        case Part::GeneralLicences:
            open_licences();
            break;
        case Part::GeneralRemove:
            remove_floserver();
            return;  // Settings may be on its way out: nothing more of it here
        default:
            break;
    }
    relayout();
}

std::vector<SettingsWindow::EditorRow> SettingsWindow::editor_rows() const {
    std::vector<EditorRow> rows;
    const EditorChoice& chosen = prefs_.code_editor;
    // Automatic: the order it follows, and what it opens now.
    const auto now = pick_editor({}, editors_found_);
    std::string note(text(lang_, Str::GeneralEditorAutoNote));
    note += ' ';
    note += now ? fill(lang_, Str::GeneralEditorUses, {now->name}) : std::string(text(lang_, Str::GeneralEditorNoneFound));
    rows.push_back({EditorChoice{}, std::string(text(lang_, Str::GeneralEditorAuto)), std::move(note), false});
    for (const EditorId id : kEditors) {
        const auto found = std::find_if(editors_found_.begin(), editors_found_.end(),
                                        [&](const FoundEditor& f) { return f.id == id; });
        const EditorChoice choice{EditorChoice::Kind::Known, id, {}};
        if (found != editors_found_.end()) {
            // Where it is: a path, left to right in every language.
            const std::string path = core::path_to_utf8(found->program);
            rows.push_back({choice, std::string(editor_name(id)), rtl_ ? isolate_ltr(path) : path, false});
        } else if (chosen == choice) {
            rows.push_back({choice, std::string(editor_name(id)), std::string(text(lang_, Str::GeneralEditorNotFound)),
                            false});
        }
    }
    // Choose program…: the one chosen, under it.
    EditorChoice program = chosen.kind == EditorChoice::Kind::Program ? chosen : EditorChoice{EditorChoice::Kind::Program, EditorId::VsCode, {}};
    std::string where;
    if (!program.path.empty()) {
        where = rtl_ ? isolate_ltr(program.path) : program.path;
        if (editor_program_missing_) {
            where = std::string(text(lang_, Str::GeneralEditorNotFound)) + ": " + where;
        }
    }
    rows.push_back({std::move(program), std::string(text(lang_, Str::GeneralEditorChoose)), std::move(where), true});
    return rows;
}

void SettingsWindow::check_editor_program() {
    // Once here, not at each paint: a program on a share that's offline can take a while to answer.
    const EditorChoice& chosen = prefs_.code_editor;
    editor_program_missing_ = chosen.kind == EditorChoice::Kind::Program && !resolve_editor(chosen, {});
}

void SettingsWindow::choose_editor_program() {
    asking_ = true;  // the picker runs its own message loop
    const auto picked = pick_program(hwnd_, std::string(text(lang_, Str::GeneralEditorPick)),
                                     std::string(text(lang_, Str::GeneralEditorPrograms)));
    asking_ = false;
    modal_closed();
    if (!picked || picked->empty()) {
        return;  // cancelled (or no picker): the choice stays
    }
    if (!is_program_file(*picked)) {
        // A .bat or .cmd typed in the box: it would run through cmd.exe.
        show_message(std::string(text(lang_, Str::GeneralEditorNotProgram)), true);
        return;
    }
    const EditorChoice choice{EditorChoice::Kind::Program, EditorId::VsCode, *picked};
    if (choice != prefs_.code_editor) {
        prefs_.code_editor = choice;
        post_pref(PrefChange::CodeEditor, 0);
    }
    check_editor_program();
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
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

    // Code editor (approved 3 Nov): Automatic, the editors found, Choose program….
    card_top(gg.editor_card, gg.editor_title, yy);
    const std::vector<EditorRow> editors = editor_rows();
    gg.editors.assign(editors.size(), ChoiceGeometry{});
    for (std::size_t i = 0; i < editors.size(); ++i) {
        choice(gg.editors[i], cx0, radio_x, yy, editors[i].note);
    }
    card_end(gg.editor_card, yy);

    // Updates (Q5, approved 5 Nov): when it last looked and Check now, at
    // its end; then a newer version on its way or ready, with What's new
    // and Update now.
    card_top(gg.updates_card, gg.updates_title, yy);
    {
        const int check_w = measure(text(lang_, Str::UpdatesCheckNow), fonts_.button.get()) + px(24);
        gg.updates = {cx1 - check_w, yy, cx1, yy + px(kControlH)};
        const int words_end = std::max(cx0 + px(80), static_cast<int>(gg.updates.left) - px(12));
        const int summary_h = std::max(px(20), height(updates_summary_text(), fonts_.body.get(), words_end - cx0));
        gg.updates_summary = {cx0, yy + px(4), words_end, yy + px(4) + summary_h};
        const int updates_note_h = height(text(lang_, Str::UpdatesNote), fonts_.caption.get(), words_end - cx0);
        gg.updates_note = {cx0, gg.updates_summary.bottom, words_end, gg.updates_summary.bottom + updates_note_h};
        yy = std::max(static_cast<int>(gg.updates_note.bottom), static_cast<int>(gg.updates.bottom));
        if (const std::string problem = updates_problem(updates_, lang_); !problem.empty()) {
            gg.updates_problem = {cx0, yy + px(6), cx1, yy + px(6) + height(problem, fonts_.caption.get(), cx1 - cx0)};
            yy = static_cast<int>(gg.updates_problem.bottom);
        }
        if (const UpdatesRow row = updates_row(updates_, lang_); !row.title.empty()) {
            gg.update_divider = {cx0, yy + px(12), cx1, yy + px(12) + std::max(1, px(1))};
            yy = static_cast<int>(gg.update_divider.bottom) + px(12);
            int end = cx1;
            if (row.ready) {
                const int now_w = measure(text(lang_, Str::UpdateNow), fonts_.button_bold.get()) + px(24);
                gg.update_now = {end - now_w, yy, end, yy + px(kControlH)};
                end = static_cast<int>(gg.update_now.left) - px(8);
            }
            if (is_openable_notes(updates_.notes_url)) {
                const int notes_w = measure(text(lang_, Str::UpdateWhatsNew), fonts_.button.get()) + px(12);
                gg.update_notes = {end - notes_w, yy, end, yy + px(kControlH)};
                end = static_cast<int>(gg.update_notes.left) - px(8);
            }
            const int words_w = std::max(px(80), end - px(4) - cx0);
            gg.update_title = {cx0, yy, cx0 + words_w, yy + px(20)};
            const int line_h = height(row.line, fonts_.caption.get(), words_w);
            gg.update_line = {cx0, gg.update_title.bottom, cx0 + words_w, gg.update_title.bottom + line_h};
            // Below the words, or below the buttons beside them when they reach lower.
            const bool buttons = !is_empty(gg.update_now) || !is_empty(gg.update_notes);
            yy = std::max(static_cast<int>(gg.update_line.bottom), buttons ? yy + px(kControlH) : 0);
        }
    }
    card_end(gg.updates_card, yy);

    // AI coding tools (D32, mockup SettingsGeneralAI): what it is, then three
    // lines, each with Copy; the JSON in a box. Then, when flo isn't on the
    // Path, why the lines name it in full, and that nothing runs until a tool starts it.
    card_top(gg.ai_card, gg.ai_title, yy);
    {
        const int ai_note_h = height(text(lang_, Str::GeneralAiNote), fonts_.caption.get(), cx1 - cx0);
        gg.ai_note = {cx0, yy - px(2), cx1, yy - px(2) + ai_note_h};
        yy = static_cast<int>(gg.ai_note.bottom) + px(10);
        const int label_w = std::min(px(170), (cx1 - cx0) / 4);
        const int copy_w = measure(text(lang_, Str::DbCopy), fonts_.button.get()) + px(24);
        for (std::size_t i = 0; i < gg.ai_copy.size(); ++i) {
            gg.ai_labels.at(i) = {cx0, yy, cx0 + label_w, yy + px(kControlH)};
            gg.ai_copy.at(i) = {std::max(cx0 + label_w, cx1 - copy_w), yy, cx1, yy + px(kControlH)};
            const int value_end = std::max(cx0 + label_w + px(40), static_cast<int>(gg.ai_copy.at(i).left) - px(kGap));
            gg.ai_values.at(i) = {cx0 + label_w, yy, value_end, yy + px(kControlH)};
            yy += px(kControlH) + px(8);
        }
        if (mcp_lines_.full_path) {
            const int path_h = height(text(lang_, Str::GeneralAiFullPath), fonts_.caption.get(), cx1 - cx0);
            gg.ai_path_note = {cx0, yy, cx1, yy + path_h};
            yy = static_cast<int>(gg.ai_path_note.bottom) + px(4);
        } else {
            gg.ai_path_note = {};
        }
        const int footer_h = height(text(lang_, Str::GeneralAiFooter), fonts_.caption.get(), cx1 - cx0);
        gg.ai_footer = {cx0, yy, cx1, yy + footer_h};
        yy = static_cast<int>(gg.ai_footer.bottom);
    }
    card_end(gg.ai_card, yy);

    // About: the version and the licences.
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
    button(gg.licences, Str::GeneralLicences);
    card_end(gg.about_card, static_cast<int>(gg.licences.bottom));

    // Remove FloServer (D21): what it does, and its button.
    card_top(gg.remove_card, gg.remove_title, yy);
    const int remove_h = height(text(lang_, Str::GeneralRemoveNote), fonts_.caption.get(), cx1 - cx0);
    gg.remove_note = {cx0, yy, cx1, yy + remove_h};
    yy = static_cast<int>(gg.remove_note.bottom) + px(10);
    bx = cx0;
    button(gg.remove, Str::GeneralRemoveButton);
    card_end(gg.remove_card, static_cast<int>(gg.remove.bottom));

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
        case Part::GeneralEditor:
            return at(g.editors);
        case Part::GeneralUpdates:
            return &g.updates;
        case Part::GeneralUpdateNotes:
            return &g.update_notes;
        case Part::GeneralUpdateNow:
            return &g.update_now;
        case Part::GeneralAiCopy:
            return hit.row >= 0 && std::cmp_less(hit.row, g.ai_copy.size()) ? &g.ai_copy.at(static_cast<std::size_t>(hit.row))
                                                                             : nullptr;
        case Part::GeneralLicences:
            return &g.licences;
        case Part::GeneralRemove:
            return &g.remove;
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
    for (std::size_t i = 0; i < geometry_.general.editors.size(); ++i) {
        order.push_back({Part::GeneralEditor, -1, static_cast<int>(i)});
    }
    order.push_back({Part::GeneralUpdates, -1, -1});
    if (!is_empty(geometry_.general.update_notes)) {
        order.push_back({Part::GeneralUpdateNotes, -1, -1});
    }
    if (!is_empty(geometry_.general.update_now)) {
        order.push_back({Part::GeneralUpdateNow, -1, -1});
    }
    for (std::size_t i = 0; i < geometry_.general.ai_copy.size(); ++i) {
        order.push_back({Part::GeneralAiCopy, -1, static_cast<int>(i)});
    }
    order.push_back({Part::GeneralLicences, -1, -1});
    order.push_back({Part::GeneralRemove, -1, -1});
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
    for (const RECT* card :
         {&gg.lang_card, &gg.theme_card, &gg.start_card, &gg.close_card, &gg.editor_card, &gg.updates_card,
          &gg.ai_card, &gg.about_card, &gg.remove_card}) {
        c.round_rect(*card, pxf(8), p.card, p.card_border, border);
    }
    // AI coding tools: the JSON's boxes, and the Copy buttons.
    for (const std::size_t box : {std::size_t{2}, std::size_t{3}}) {
        if (!is_empty(gg.ai_values.at(box))) {
            c.round_rect(gg.ai_values.at(box), pxf(4), p.input, p.input_border, border);
        }
    }
    for (std::size_t i = 0; i < gg.ai_copy.size(); ++i) {
        const Hit copy{Part::GeneralAiCopy, -1, static_cast<int>(i)};
        paint_button(c, gg.ai_copy.at(i), hover_ == copy, is_focused(copy), true);
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

    const std::vector<EditorRow> editors = editor_rows();
    for (std::size_t i = 0; i < editors.size() && i < gg.editors.size(); ++i) {
        // Choose program… is on once a program is chosen.
        const bool on = editors[i].program ? prefs_.code_editor.kind == EditorChoice::Kind::Program
                                           : editors[i].choice == prefs_.code_editor;
        paint_radio(c, gg.editors[i], on, {Part::GeneralEditor, -1, static_cast<int>(i)});
    }

    for (const auto& [part, rect] : {std::pair{Part::GeneralLicences, gg.licences}, std::pair{Part::GeneralRemove, gg.remove}}) {
        const Hit hit{part, -1, -1};
        paint_button(c, rect, hover_ == hit, is_focused(hit), true);
    }
    const Hit check{Part::GeneralUpdates, -1, -1};
    paint_button(c, gg.updates, hover_ == check && can_check_updates(), is_focused(check), can_check_updates());
    if (!is_empty(gg.update_divider)) {
        c.fill(gg.update_divider, p.divider);
    }
    if (const Hit notes{Part::GeneralUpdateNotes, -1, -1}; !is_empty(gg.update_notes)) {
        if (hover_ == notes) {
            c.round_rect(gg.update_notes, pxf(4), p.row_hover);
        }
        if (is_focused(notes)) {
            paint_ring(c, gg.update_notes);
        }
    }
    if (const Hit now{Part::GeneralUpdateNow, -1, -1}; !is_empty(gg.update_now)) {
        // The primary button, as in the main window's banner.
        c.round_rect(gg.update_now, pxf(4), p.accent);
        if (is_focused(now)) {
            paint_ring(c, gg.update_now);
        }
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

    title(gg.editor_title, Str::GeneralEditor);
    const std::vector<EditorRow> editors = editor_rows();
    for (std::size_t i = 0; i < editors.size() && i < gg.editors.size(); ++i) {
        // Editors' names and paths read left to right; the rest is in the window's language.
        const bool code = !editors[i].program && editors[i].choice.kind == EditorChoice::Kind::Known;
        c.text(gg.editors[i].label, w(editors[i].label), fonts_.body.get(), p.text, Align::Start, code);
        if (!editors[i].note.empty() && !is_empty(gg.editors[i].note)) {
            c.paragraph(gg.editors[i].note, w(editors[i].note), fonts_.caption.get(), p.text_subtle);
        }
    }

    title(gg.updates_title, Str::UpdatesTitle);
    c.paragraph(gg.updates_summary, w(updates_summary_text()), fonts_.body.get(), p.text);
    c.paragraph(gg.updates_note, w(text(lang_, Str::UpdatesNote)), fonts_.caption.get(), p.text_subtle);
    c.text(gg.updates, w(text(lang_, Str::UpdatesCheckNow)), fonts_.button.get(),
           can_check_updates() ? p.text : p.button_disabled_text, Align::Center, false);
    if (!is_empty(gg.updates_problem)) {
        c.paragraph(gg.updates_problem, w(updates_problem(updates_, lang_)), fonts_.caption.get(), p.problem.text);
    }
    if (const UpdatesRow row = updates_row(updates_, lang_); !row.title.empty() && !is_empty(gg.update_title)) {
        c.text(gg.update_title, w(row.title), fonts_.body_bold.get(), p.text, Align::Start, false);
        c.paragraph(gg.update_line, w(row.line), fonts_.caption.get(), p.text_subtle);
        if (!is_empty(gg.update_notes)) {
            c.text(gg.update_notes, w(text(lang_, Str::UpdateWhatsNew)), fonts_.button.get(), p.link, Align::Center,
                   false);
        }
        if (!is_empty(gg.update_now)) {
            c.text(gg.update_now, w(text(lang_, Str::UpdateNow)), fonts_.button_bold.get(), p.accent_text,
                   Align::Center, false);
        }
    }

    title(gg.ai_title, Str::GeneralAiTitle);
    c.paragraph(gg.ai_note, w(text(lang_, Str::GeneralAiNote)), fonts_.caption.get(), p.text_subtle);
    for (std::size_t i = 0; i < gg.ai_copy.size(); ++i) {
        c.text(gg.ai_labels.at(i), w(text(lang_, mcp_label(mcp_lines_, i))), fonts_.caption.get(), p.text_subtle, Align::Start, false);
        // Commands read left to right in every language (embedded, as paths
        // are, in Arabic and Kurdish); the boxes' text sits inside their padding.
        RECT value = gg.ai_values.at(i);
        if (i >= 2) {
            value.left += px(8);
            value.right -= px(8);
        }
        c.text(value, w(rtl_ ? isolate_ltr(mcp_line(i)) : mcp_line(i)), fonts_.mono.get(), p.text, Align::Start, true);
        c.text(gg.ai_copy.at(i), w(text(lang_, Str::DbCopy)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (!is_empty(gg.ai_path_note)) {
        c.paragraph(gg.ai_path_note, w(text(lang_, Str::GeneralAiFullPath)), fonts_.caption.get(), p.asleep.text);
    }
    c.paragraph(gg.ai_footer, w(text(lang_, Str::GeneralAiFooter)), fonts_.caption.get(), p.text_subtle);

    title(gg.about_title, Str::GeneralAbout);
    c.text(gg.version, w(fill(lang_, Str::GeneralVersion, {core::kVersion})), fonts_.body.get(), p.text, Align::Start,
           false);
    c.text(gg.licences, w(text(lang_, Str::GeneralLicences)), fonts_.button.get(), p.text, Align::Center, false);

    title(gg.remove_title, Str::GeneralRemoveTitle);
    c.paragraph(gg.remove_note, w(text(lang_, Str::GeneralRemoveNote)), fonts_.caption.get(), p.text_subtle);
    // In the problem colour: it removes FloServer (the question comes first).
    c.text(gg.remove, w(text(lang_, Str::GeneralRemoveButton)), fonts_.button.get(), p.problem.text, Align::Center,
           false);
    c.unclip_text();
}

}  // namespace floserver::ui

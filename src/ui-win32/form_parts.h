#pragma once

// Pieces Settings and the project panel share for their forms (D20): a
// one-line edit box inside a field the window draws, and the "Save your
// changes?" question.

#include "model/project_menu.h"
#include "model/theme.h"
#include "model/uninstall.h"
#include "model/ui_strings.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// A borderless EDIT that sits inside a field box the window draws (its
// outline, fill and focus mark), the way the main window's search box does.
// The placeholder is drawn in the theme's colour: the system's cue banner
// grey can't be changed and is hard to read on dark. Tab, Enter and Escape
// go to `on_key` (true: handled); the parent gets EN_CHANGE, EN_SETFOCUS
// and WM_CTLCOLOREDIT as usual, with `id` as the control's id.
class FieldEdit {
public:
    using KeyFn = std::function<bool(WPARAM key)>;
    FieldEdit(HWND parent, int id, HFONT font, bool rtl, KeyFn on_key);
    ~FieldEdit();
    FieldEdit(const FieldEdit&) = delete;
    FieldEdit& operator=(const FieldEdit&) = delete;
    FieldEdit(FieldEdit&&) = delete;
    FieldEdit& operator=(FieldEdit&&) = delete;

    [[nodiscard]] HWND hwnd() const { return hwnd_; }
    [[nodiscard]] int id() const { return id_; }
    [[nodiscard]] bool focused() const { return hwnd_ != nullptr && GetFocus() == hwnd_; }
    [[nodiscard]] std::string text() const;
    // Without EN_CHANGE reaching the parent's handling: `setting()` is true meanwhile.
    void set_text(std::string_view text);
    [[nodiscard]] bool setting() const { return setting_; }
    void set_cue(std::string_view cue);
    void set_cue_color(Rgb color);
    void set_font(HFONT font);
    // While a save is on its way: what's typed then would be lost. The
    // parent gets WM_CTLCOLORSTATIC instead of WM_CTLCOLOREDIT meanwhile.
    void set_read_only(bool read_only);
    // Shows ● for each character (a password being typed).
    void set_password(bool on);
    // Inside `box` (the parent's client coordinates, which are logical in a
    // mirrored window), `pad` from its sides; hidden when `visible` is false.
    void place(const RECT& box, int pad, int height, bool visible);
    // The same inside a scrolling part from `clip_top` to `clip_bottom`
    // (client coordinates): cut at its edges, hidden when wholly outside.
    void place(const RECT& box, int pad, int height, bool visible, int clip_top, int clip_bottom);

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR self);
    void paint_cue() const;

    HWND hwnd_ = nullptr;
    int id_;
    bool rtl_;
    KeyFn on_key_;
    std::wstring cue_;
    Rgb cue_color_ = 0x5F5F5F;
    bool setting_ = false;
    bool clipped_ = false;  // a window region cuts it at a scrolling part's edge
    RECT placed_{};         // where place() last put it (parent's client coordinates)
};

// Puts a secret (a password, .env lines with one) on the clipboard, kept
// out of Windows' clipboard history and cloud clipboard; `value` is wiped
// from memory. False when the clipboard couldn't be had.
bool put_secret_on_clipboard(HWND owner, std::string& value);

// Windows' folder picker titled `title`, opening in `start` when it's an
// existing folder. The folder picked; empty when cancelled; nothing when the
// picker couldn't be shown. It runs its own message loop.
[[nodiscard]] std::optional<std::string> pick_folder(HWND owner, const std::string& title, const std::string& start);

// Windows' file picker for a program (.exe), titled `title`, `filter` naming
// the .exe files ("Programs"). The program picked; empty when cancelled;
// nothing when the picker couldn't be shown. It runs its own message loop.
[[nodiscard]] std::optional<std::string> pick_program(HWND owner, const std::string& title, const std::string& filter);

// Opens an existing folder in Explorer ("explore" opens folders only, never
// runs a file). False when it isn't one or couldn't be opened.
bool explore_folder(HWND owner, const std::string& path);

// A dialog template built in memory (DLGTEMPLATE, then its items), WORD by
// WORD: the questions below, the backup dialogs (backup_dialogs.cpp) and the
// updating dialog (apply_update.cpp). Standard controls, so the keys and
// screen readers work as in any Windows dialog; texts are set when it opens.
class DialogTemplate {
public:
    // Control kinds by their atoms.
    static constexpr WORD kButton = 0x0080;
    static constexpr WORD kEdit = 0x0081;
    static constexpr WORD kStatic = 0x0082;

    DialogTemplate(DWORD style, DWORD ex_style, short cx, short cy) {
        add_dword(style);
        add_dword(ex_style);
        count_at_ = words_.size();
        add_word(0);  // the items, counted as they're added
        add_word(0);  // x, y
        add_word(0);
        add_word(static_cast<WORD>(cx));
        add_word(static_cast<WORD>(cy));
        add_word(0);  // no menu
        add_word(0);  // the dialog class
        add_word(0);  // no title: set when it opens
        add_word(9);  // DS_SETFONT: 9 point Segoe UI
        add_text(L"Segoe UI");
    }
    // `kind`: kButton, kEdit or kStatic.
    void add(DWORD style, short x, short y, short cx, short cy, WORD id, WORD kind) {
        begin_item(style, x, y, cx, cy, id);
        add_word(0xFFFF);
        add_word(kind);
        end_item();
    }
    // A control of a class named in full (msctls_progress32).
    void add(DWORD style, short x, short y, short cx, short cy, WORD id, const wchar_t* class_name) {
        begin_item(style, x, y, cx, cy, id);
        add_text(class_name);
        end_item();
    }
    [[nodiscard]] const DLGTEMPLATE* get() const { return reinterpret_cast<const DLGTEMPLATE*>(words_.data()); }

private:
    void begin_item(DWORD style, short x, short y, short cx, short cy, WORD id) {
        while (words_.size() % 2 != 0) {
            add_word(0);  // each item starts on a DWORD
        }
        add_dword(style | WS_CHILD | WS_VISIBLE);
        add_dword(0);
        add_word(static_cast<WORD>(x));
        add_word(static_cast<WORD>(y));
        add_word(static_cast<WORD>(cx));
        add_word(static_cast<WORD>(cy));
        add_word(id);
    }
    void end_item() {
        add_word(0);  // no text: set when it opens
        add_word(0);  // no creation data
        ++words_[count_at_];
    }
    void add_word(WORD v) { words_.push_back(v); }
    void add_dword(DWORD v) {
        add_word(LOWORD(v));
        add_word(HIWORD(v));
    }
    void add_text(const wchar_t* s) {
        for (; *s != L'\0'; ++s) {
            add_word(static_cast<WORD>(*s));
        }
        add_word(0);
    }
    std::vector<WORD> words_;
    std::size_t count_at_ = 0;
};

enum class SaveChoice : std::uint8_t { Save, DontSave, Cancel };

// "Save your changes to PHP 8.5?" with Save, Don't save and Cancel.
[[nodiscard]] SaveChoice ask_to_save(HWND owner, Lang lang, const std::string& question);

// The question before a project is removed from FloServer (approved 2 and
// 4 Nov): a small dialog in the window's language (laid out right to left in
// Arabic and Kurdish) with Remove and Cancel (the default; Enter and Esc
// keep everything) and up to two boxes, both unticked: delete the database
// (when it has one) and move the folder to the Recycle Bin (when it's
// offered), the folder shown under it. True for Remove; `drop` and `trash`
// are the boxes.
[[nodiscard]] bool ask_remove_project(HWND owner, Lang lang, const RemoveQuestion& question, bool& drop, bool& trash);

// Remove FloServer… (Settings > General, D21, approved with its mockup): the
// same kind of dialog with "Also delete" and three boxes, each with its note
// under it (also its help for screen readers): the runtimes and caches,
// the databases, and the projects folder (only when there's one,
// `projects_dir`). `ticks` are the boxes as they start (the runtimes
// ticked) and, for Remove FloServer (drawn red), as chosen. Cancel is the
// default: Enter and Esc keep everything. `count` gives the sizes; it runs
// on a thread of its own and the notes say "…" until it's done. True for
// Remove FloServer.
[[nodiscard]] bool ask_remove_floserver(HWND owner, Lang lang, const std::string& projects_dir,
                                        std::function<UninstallSizes()> count, UninstallTicks& ticks);
// The Rename dialog (D22, approved 5 Nov), laid out as the remove question:
// the New name box (filled with `current`, all of it selected; a–z as
// typed), its help under it (the rule and the new address, or what's wrong,
// in the error colour), "Also rename the folder" (unticked) with the folder
// and its new name under it, the grey note on the database, then Rename
// (the default, greyed out while the name can't be used) and Cancel (Esc).
// `question` gives the words for what's typed, asked again on every key;
// screen readers get the help as the box's. True for Rename, with the name
// typed and the box.
[[nodiscard]] bool ask_rename_project(HWND owner, Lang lang, const std::string& current,
                                      const std::function<RenameQuestion(const std::string&)>& question,
                                      std::string& new_name, bool& rename_folder);

}  // namespace floserver::ui

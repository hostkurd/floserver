#pragma once

// A dialog template built in memory (DLGTEMPLATE, then its items), WORD by
// WORD, for the window's own dialogs (remove a project, backups):
// standard controls, so the keys and screen readers work as in any Windows
// dialog. Texts are set when it opens (WM_INITDIALOG), in the window's language.

#include <windows.h>

#include <cstddef>
#include <vector>

namespace floserver::ui {

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

}  // namespace floserver::ui

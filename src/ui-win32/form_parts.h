#pragma once

// Pieces Settings and the project panel share for their forms (D20): a
// one-line edit box inside a field the window draws, and the "Save your
// changes?" question.

#include "model/theme.h"
#include "model/ui_strings.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

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
    // Inside `box` (the parent's client coordinates, which are logical in a
    // mirrored window), `pad` from its sides; hidden when `visible` is false.
    void place(const RECT& box, int pad, int height, bool visible);

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
};

enum class SaveChoice : std::uint8_t { Save, DontSave, Cancel };

// "Save your changes to PHP 8.5?" with Save, Don't save and Cancel.
[[nodiscard]] SaveChoice ask_to_save(HWND owner, Lang lang, const std::string& question);

}  // namespace floserver::ui

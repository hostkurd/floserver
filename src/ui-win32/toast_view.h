#pragma once

// A toast (model/toast.h) over a window's content: a rounded card at the
// bottom centre, its words wrapped on up to three lines, a close mark at
// its end. It's a small popup of its own, owned by the window (never
// activated, never taking the keys), so nothing under it, painted or a
// child control, shows through it. FloServer's window and Settings both use it.

#include "canvas.h"
#include "model/toast.h"

#include <windows.h>

namespace floserver::ui {

struct ToastGeometry {
    RECT card{};   // empty: no toast
    RECT text{};
    RECT close{};  // the close mark's hit target
    RECT action{};  // its button, when it has one (between the words and the close mark)
    RECT icon{};    // new mail's envelope, at the start
    RECT detail{};  // new mail's second line, under `text`
};

// The card for `toast` in a window `window_w` wide, its bottom edge at
// `bottom` (logical coordinates, centred, so the same either way round).
[[nodiscard]] ToastGeometry toast_geometry(HDC dc, const Toast& toast, HFONT font, int window_w, int bottom, UINT dpi,
                                           bool rtl);

void paint_toast(Canvas& c, const ToastGeometry& g, const Toast& toast, HFONT font, bool dark, UINT dpi,
                 bool close_hover, bool text_pass, bool action_hover = false);

// What the popup tells its owner (the wparam of the owner's message).
inline constexpr WPARAM kToastMouseEntered = 0;
inline constexpr WPARAM kToastMouseLeft = 1;
inline constexpr WPARAM kToastCloseClicked = 2;
inline constexpr WPARAM kToastActionClicked = 3;

class ToastPopup {
public:
    ToastPopup() = default;
    ~ToastPopup();
    ToastPopup(const ToastPopup&) = delete;
    ToastPopup& operator=(const ToastPopup&) = delete;
    ToastPopup(ToastPopup&&) = delete;
    ToastPopup& operator=(ToastPopup&&) = delete;

    // The window it belongs to (made again in another language: the new one),
    // and the message it posts there.
    void attach(HWND owner, UINT message);
    // Shown at `g` (the owner's logical client coordinates), or hidden when
    // there's no toast or the owner isn't shown. Called with each paint.
    void show(const Toast& toast, const ToastGeometry& g, HFONT font, bool dark, UINT dpi, bool rtl);
    void hide();
    // The owner moved: the card goes with it.
    void follow();

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT msg, WPARAM wparam, LPARAM lparam);
    void place();
    void paint();
    [[nodiscard]] bool over_close(LPARAM lparam) const;
    [[nodiscard]] bool over_action(LPARAM lparam) const;
    [[nodiscard]] bool over(const RECT& r, LPARAM lparam) const;

    HWND owner_ = nullptr;
    UINT message_ = 0;
    HWND hwnd_ = nullptr;
    Toast toast_;
    ToastGeometry local_;  // the card at (0, 0)
    RECT card_{};          // where it is in the owner's client area (logical)
    HFONT font_ = nullptr;  // its own copy of the window's font
    LOGFONTW font_spec_{};
    UINT region_dpi_ = 0;
    bool dark_ = false;
    UINT dpi_ = 96;
    bool rtl_ = false;
    bool close_hover_ = false;
    bool action_hover_ = false;
    bool tracking_ = false;
    SIZE size_{};
};

}  // namespace floserver::ui

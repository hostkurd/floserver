#pragma once

// A toast (model/toast.h) drawn in a window, over its content: a rounded
// card at the bottom centre, its words wrapped on up to three lines, and a
// close mark at its end. FloServer's window and Settings both use it.

#include "canvas.h"
#include "model/toast.h"

#include <windows.h>

namespace floserver::ui {

struct ToastGeometry {
    RECT card{};   // empty: no toast
    RECT text{};
    RECT close{};  // the close mark's hit target
};

// The card for `toast` in a window `window_w` wide, its bottom edge at
// `bottom` (logical coordinates, centred, so the same either way round).
[[nodiscard]] ToastGeometry toast_geometry(HDC dc, const Toast& toast, HFONT font, int window_w, int bottom, UINT dpi,
                                           bool rtl);

void paint_toast(Canvas& c, const ToastGeometry& g, const Toast& toast, HFONT font, bool dark, UINT dpi,
                 bool close_hover, bool text_pass);

}  // namespace floserver::ui

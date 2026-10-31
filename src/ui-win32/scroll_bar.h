#pragma once

// The thin scroll bar drawn at the end of a scrolling part (the project
// list, the project panel's tabs, Settings), working as a Windows one does:
// the thumb follows the mouse while it's dragged (anywhere on the screen,
// with the mouse captured), a click on the track pages toward the mouse and
// keeps paging while the button is held, and the bar widens and darkens
// under the mouse, as in Windows 11. The wheel and the keys stay the
// window's own.
//
// Logical coordinates as everywhere in the window (canvas.h): its end is
// the reading end, so in Arabic and Kurdish the bar is on the left, as
// Windows puts it. The window tells the bar where it is (place()) before
// each mouse message and paint, and how to scroll (the callback, which
// moves anything that scrolls with the content, edit boxes included).

#include "canvas.h"
#include "model/scrollbar.h"
#include "model/theme.h"

#include <windows.h>

#include <functional>

namespace floserver::ui {

class ScrollBar {
public:
    // `timer_id`: the window's timer for paging while the button is held.
    ScrollBar(UINT_PTR timer_id, std::function<void(int)> scroll_to);

    // The scrolling part (its height is the view), the content's height and
    // the offset now, at `dpi`.
    void place(const RECT& area, int content, int offset, UINT dpi);
    [[nodiscard]] bool visible() const { return metrics_.scrolls(); }
    // Over the bar: the strip at the part's end, a little wider than drawn.
    [[nodiscard]] bool over(int x, int y) const;
    // Dragging the thumb or paging: the window leaves the mouse to the bar.
    [[nodiscard]] bool active() const { return mode_ != Mode::None; }
    [[nodiscard]] bool hot() const { return hot_ || active(); }

    // WM_LBUTTONDOWN at (x, y): true when the bar took it (the mouse is captured then).
    bool press(HWND hwnd, int x, int y);
    // WM_MOUSEMOVE: drags the thumb; else notes whether the mouse is over the
    // bar. True when the window should redraw (the bar's look changed).
    bool move(HWND hwnd, int x, int y);
    // WM_LBUTTONUP: true when the bar had the mouse (the capture is let go).
    bool release(HWND hwnd);
    // WM_CAPTURECHANGED (another window took the mouse), WM_CANCELMODE: stops.
    void cancel(HWND hwnd);
    // WM_MOUSELEAVE: no longer under the mouse. True when it was.
    bool leave();
    // WM_TIMER: pages again while the button is held on the track. True for its own timer.
    bool tick(HWND hwnd, UINT_PTR id);

    void paint(Canvas& c, const Palette& p) const;

private:
    enum class Mode : std::uint8_t { None, Drag, Page };
    [[nodiscard]] int px(int dip) const;
    void page(HWND hwnd);

    UINT_PTR timer_id_;
    std::function<void(int)> scroll_to_;
    RECT area_{};
    ScrollMetrics metrics_;
    int offset_ = 0;
    UINT dpi_ = 96;
    Mode mode_ = Mode::None;
    ScrollDrag drag_;
    int page_y_ = 0;  // where the button is held on the track
    bool hot_ = false;
};

}  // namespace floserver::ui

#include "scroll_bar.h"

#include <algorithm>
#include <utility>

namespace floserver::ui {

namespace {

constexpr int kGrabW = 12;       // the strip that takes the mouse
constexpr int kThinW = 4;        // the thumb as it rests
constexpr int kWideW = 8;        // under the mouse
constexpr int kEndGap = 2;       // from the part's end and its top and bottom
constexpr int kMinThumb = 24;
constexpr UINT kFirstRepeatMs = 350;  // as Windows' own bars: a pause, then quick pages
constexpr UINT kRepeatMs = 60;

}  // namespace

ScrollBar::ScrollBar(UINT_PTR timer_id, std::function<void(int)> scroll_to)
    : timer_id_(timer_id), scroll_to_(std::move(scroll_to)) {}

int ScrollBar::px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }

void ScrollBar::place(const RECT& area, int content, int offset, UINT dpi) {
    area_ = area;
    dpi_ = dpi;
    offset_ = offset;
    metrics_.track_top = area.top;
    metrics_.track_bottom = area.bottom;
    metrics_.view = area.bottom - area.top;
    metrics_.content = content;
    metrics_.min_thumb = px(kMinThumb);
}

bool ScrollBar::over(int x, int y) const {
    return visible() && x >= area_.right - px(kGrabW) && x < area_.right && y >= area_.top && y < area_.bottom;
}

bool ScrollBar::press(HWND hwnd, int x, int y) {
    if (!over(x, y)) {
        return false;
    }
    if (scroll_zone(metrics_, offset_, y) == ScrollZone::Thumb) {
        mode_ = Mode::Drag;
        drag_ = ScrollDrag::start(metrics_, offset_, y);
    } else {
        mode_ = Mode::Page;
        page_y_ = y;
        page(hwnd);
        SetTimer(hwnd, timer_id_, kFirstRepeatMs, nullptr);
    }
    SetCapture(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

void ScrollBar::page(HWND hwnd) {
    // A page is the view less a little, so a line stays in sight across it.
    const int step = std::max(px(24), metrics_.view - px(24));
    const int next = scroll_page_toward(metrics_, offset_, page_y_, step);
    if (next == offset_) {
        KillTimer(hwnd, timer_id_);  // the thumb has reached the mouse
        return;
    }
    offset_ = next;
    scroll_to_(next);
}

bool ScrollBar::move(HWND hwnd, int x, int y) {
    if (mode_ == Mode::Drag) {
        const int next = drag_.offset_at(metrics_, y);
        if (next != offset_) {
            offset_ = next;
            scroll_to_(next);
        }
        return true;
    }
    if (mode_ == Mode::Page) {
        page_y_ = y;  // pages on toward where the mouse is now
        return true;
    }
    (void)hwnd;
    const bool now = over(x, y);
    if (now == hot_) {
        return false;
    }
    hot_ = now;
    return true;
}

bool ScrollBar::release(HWND hwnd) {
    if (mode_ == Mode::None) {
        return false;
    }
    mode_ = Mode::None;  // first: letting go of the capture sends WM_CAPTURECHANGED
    KillTimer(hwnd, timer_id_);
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

void ScrollBar::cancel(HWND hwnd) {
    if (mode_ == Mode::None) {
        return;
    }
    mode_ = Mode::None;
    KillTimer(hwnd, timer_id_);
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool ScrollBar::leave() { return std::exchange(hot_, false); }

bool ScrollBar::tick(HWND hwnd, UINT_PTR id) {
    if (id != timer_id_) {
        return false;
    }
    if (mode_ != Mode::Page) {
        KillTimer(hwnd, timer_id_);
        return true;
    }
    SetTimer(hwnd, timer_id_, kRepeatMs, nullptr);
    page(hwnd);
    return true;
}

void ScrollBar::paint(Canvas& c, const Palette& p) const {
    if (!visible()) {
        return;
    }
    const bool wide = hot();
    const int w = px(wide ? kWideW : kThinW);
    const int end = area_.right - px(kEndGap);
    const float radius = static_cast<float>(w) / 2.0F;
    if (wide) {
        // The track shows under the mouse, as in Windows 11.
        c.round_rect({end - w, area_.top + px(kEndGap), end, area_.bottom - px(kEndGap)}, radius, p.bar_track);
    }
    const int top = metrics_.thumb_top(offset_);
    const int bottom = top + metrics_.thumb_length();
    c.round_rect({end - w, top + px(kEndGap), end, bottom - px(kEndGap)}, radius,
                 wide ? p.text_subtle : p.switch_off_border);
}

}  // namespace floserver::ui

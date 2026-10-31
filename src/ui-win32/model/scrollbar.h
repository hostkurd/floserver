#pragma once

// The thin scroll bars' sums (the project list, the project panel, Settings),
// shared by the three: where the thumb is for an offset, the offset for a
// thumb dragged somewhere, and what a click on the track does. Plain C++, in
// window pixels along the bar, so it's tested on every platform; the windows
// draw it and take the mouse (scroll_bar.h).

#include <cstdint>

namespace floserver::ui {

struct ScrollMetrics {
    int track_top = 0;     // where the thumb can be, top to bottom
    int track_bottom = 0;
    int view = 0;          // how much of the content is seen at once
    int content = 0;       // all of it
    int min_thumb = 0;     // the shortest the thumb gets (DPI-scaled)

    // There's more content than view, and a track to show it on.
    [[nodiscard]] bool scrolls() const;
    // The largest offset: content - view, or 0.
    [[nodiscard]] int most() const;
    [[nodiscard]] int track_length() const;
    // As long as the view is of the content, never shorter than min_thumb
    // nor longer than the track.
    [[nodiscard]] int thumb_length() const;
    // Where the thumb's top is for `offset` (clamped).
    [[nodiscard]] int thumb_top(int offset) const;
    // The offset that puts the thumb's top at `top` (clamped to 0..most()).
    [[nodiscard]] int offset_for_thumb(int top) const;
};

// Where `y` is on the track: above the thumb, on it, or below it.
enum class ScrollZone : std::uint8_t { None, Before, Thumb, After };
[[nodiscard]] ScrollZone scroll_zone(const ScrollMetrics& m, int offset, int y);

// A click (or the button held) on the track at `y`: one page (`page`
// pixels) toward it, as a Windows scroll bar does. The same offset when `y`
// is on the thumb.
[[nodiscard]] int scroll_page_toward(const ScrollMetrics& m, int offset, int y, int page);

// The thumb being dragged: grabbed `grab` pixels below its top, so it stays
// under the mouse however far the mouse goes (outside the bar too).
struct ScrollDrag {
    int grab = 0;
    [[nodiscard]] static ScrollDrag start(const ScrollMetrics& m, int offset, int y);
    [[nodiscard]] int offset_at(const ScrollMetrics& m, int y) const;
};

}  // namespace floserver::ui

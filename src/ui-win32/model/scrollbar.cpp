#include "model/scrollbar.h"

#include <algorithm>
#include <cstdint>

namespace floserver::ui {

namespace {

// a * b / c, rounded to the nearest, without overflowing int on the way.
int scale(int a, int b, int c) {
    if (c <= 0) {
        return 0;
    }
    const std::int64_t n = static_cast<std::int64_t>(a) * b;
    return static_cast<int>((n + c / 2) / c);
}

}  // namespace

bool ScrollMetrics::scrolls() const { return view > 0 && content > view && track_length() > 0; }

int ScrollMetrics::most() const { return std::max(0, content - view); }

int ScrollMetrics::track_length() const { return std::max(0, track_bottom - track_top); }

int ScrollMetrics::thumb_length() const {
    const int track = track_length();
    if (content <= 0) {
        return track;
    }
    return std::min(track, std::max(min_thumb, scale(track, view, content)));
}

int ScrollMetrics::thumb_top(int offset) const {
    const int room = track_length() - thumb_length();
    const int top = most() > 0 ? scale(room, std::clamp(offset, 0, most()), most()) : 0;
    return track_top + top;
}

int ScrollMetrics::offset_for_thumb(int top) const {
    const int room = track_length() - thumb_length();
    if (room <= 0) {
        return 0;
    }
    return std::clamp(scale(std::clamp(top - track_top, 0, room), most(), room), 0, most());
}

ScrollZone scroll_zone(const ScrollMetrics& m, int offset, int y) {
    if (!m.scrolls() || y < m.track_top || y >= m.track_bottom) {
        return ScrollZone::None;
    }
    const int top = m.thumb_top(offset);
    if (y < top) {
        return ScrollZone::Before;
    }
    return y < top + m.thumb_length() ? ScrollZone::Thumb : ScrollZone::After;
}

int scroll_page_toward(const ScrollMetrics& m, int offset, int y, int page) {
    const int step = std::max(1, page);
    switch (scroll_zone(m, offset, y)) {
        case ScrollZone::Before:
            return std::clamp(offset - step, 0, m.most());
        case ScrollZone::After:
            return std::clamp(offset + step, 0, m.most());
        case ScrollZone::Thumb:
        case ScrollZone::None:
            break;
    }
    return std::clamp(offset, 0, m.most());
}

ScrollDrag ScrollDrag::start(const ScrollMetrics& m, int offset, int y) { return {y - m.thumb_top(offset)}; }

int ScrollDrag::offset_at(const ScrollMetrics& m, int y) const { return m.offset_for_thumb(y - grab); }

}  // namespace floserver::ui

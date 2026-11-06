#include "model/glyphs.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <numbers>
#include <utility>

namespace floserver::ui {

namespace {

using Point = std::pair<float, float>;

void path(std::vector<GlyphShape>& out, std::initializer_list<Point> points) {
    const std::vector<Point> list(points);
    for (std::size_t i = 1; i < list.size(); ++i) {
        out.push_back({GlyphShape::Kind::Line, list[i - 1].first, list[i - 1].second, list[i].first, list[i].second});
    }
}

void line(std::vector<GlyphShape>& out, float x1, float y1, float x2, float y2) {
    out.push_back({GlyphShape::Kind::Line, x1, y1, x2, y2});
}

}  // namespace

std::vector<GlyphShape> glyph_shapes(Glyph glyph) {
    std::vector<GlyphShape> out;
    out.reserve(24);  // every glyph fits; also keeps GCC 13 -O3 from a false -Wstringop-overflow
    switch (glyph) {
        case Glyph::Globe:
            // Its outline, a meridian and the equator.
            out.push_back({GlyphShape::Kind::Ellipse, 8.0F, 8.0F, 6.5F, 6.5F});
            out.push_back({GlyphShape::Kind::Ellipse, 8.0F, 8.0F, 2.9F, 6.5F});
            line(out, 1.5F, 8.0F, 14.5F, 8.0F);
            break;
        case Glyph::Folder:
            path(out, {{1.5F, 13.0F}, {1.5F, 3.0F}, {6.0F, 3.0F}, {7.5F, 4.75F}, {14.5F, 4.75F}, {14.5F, 13.0F},
                       {1.5F, 13.0F}});
            line(out, 1.5F, 6.75F, 14.5F, 6.75F);
            break;
        case Glyph::Terminal:  // >_
            path(out, {{2.5F, 4.0F}, {6.5F, 8.0F}, {2.5F, 12.0F}});
            line(out, 8.5F, 12.5F, 13.5F, 12.5F);
            break;
        case Glyph::Code:  // </>
            path(out, {{5.0F, 4.0F}, {1.5F, 8.0F}, {5.0F, 12.0F}});
            path(out, {{11.0F, 4.0F}, {14.5F, 8.0F}, {11.0F, 12.0F}});
            line(out, 9.5F, 2.5F, 6.5F, 13.5F);
            break;
        case Glyph::Studio:  // two stacked layers
            path(out, {{8.0F, 2.0F}, {14.5F, 5.5F}, {8.0F, 9.0F}, {1.5F, 5.5F}, {8.0F, 2.0F}});
            path(out, {{1.5F, 9.0F}, {8.0F, 12.5F}, {14.5F, 9.0F}});
            break;
        case Glyph::Copy:  // a sheet over another
            path(out, {{3.0F, 10.5F}, {3.0F, 2.5F}, {10.5F, 2.5F}});
            path(out, {{5.5F, 5.0F}, {13.5F, 5.0F}, {13.5F, 13.5F}, {5.5F, 13.5F}, {5.5F, 5.0F}});
            break;
        case Glyph::Power: {
            // An open ring with the bar through its gap.
            constexpr float kPi = std::numbers::pi_v<float>;
            constexpr int kSteps = 14;
            const float from = -kPi / 2.0F + 0.75F;
            const float to = 3.0F * kPi / 2.0F - 0.75F;
            for (int i = 0; i < kSteps; ++i) {
                const float a = from + (to - from) * static_cast<float>(i) / static_cast<float>(kSteps);
                const float b = from + (to - from) * static_cast<float>(i + 1) / static_cast<float>(kSteps);
                line(out, 8.0F + 5.75F * std::cos(a), 8.75F + 5.75F * std::sin(a), 8.0F + 5.75F * std::cos(b),
                     8.75F + 5.75F * std::sin(b));
            }
            line(out, 8.0F, 1.5F, 8.0F, 8.0F);
            break;
        }
        case Glyph::Trash:
            line(out, 2.5F, 4.0F, 13.5F, 4.0F);
            path(out, {{6.0F, 4.0F}, {6.5F, 2.0F}, {9.5F, 2.0F}, {10.0F, 4.0F}});
            path(out, {{4.0F, 4.0F}, {5.0F, 14.0F}, {11.0F, 14.0F}, {12.0F, 4.0F}});
            line(out, 7.0F, 6.5F, 7.0F, 11.5F);
            line(out, 9.0F, 6.5F, 9.0F, 11.5F);
            break;
        case Glyph::Download:  // an arrow down onto a tray (the update banner)
            line(out, 8.0F, 1.5F, 8.0F, 10.0F);
            path(out, {{4.5F, 6.75F}, {8.0F, 10.25F}, {11.5F, 6.75F}});
            path(out, {{2.0F, 10.5F}, {2.0F, 14.0F}, {14.0F, 14.0F}, {14.0F, 10.5F}});
            break;
        case Glyph::Tick:  // ready (the update banner)
            path(out, {{2.5F, 8.5F}, {6.25F, 12.25F}, {13.5F, 4.0F}});
            break;
    }
    return out;
}

}  // namespace floserver::ui

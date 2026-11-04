// The small stroke icons (src/ui-win32/model/glyphs.h): every one has
// shapes, all inside its 16 x 16 box.

#include "model/glyphs.h"

#include <doctest/doctest.h>

using namespace floserver::ui;

TEST_CASE("glyphs: each has shapes inside its box") {
    for (const Glyph g : {Glyph::Globe, Glyph::Folder, Glyph::Terminal, Glyph::Code, Glyph::Studio, Glyph::Copy,
                          Glyph::Power, Glyph::Trash}) {
        CAPTURE(static_cast<int>(g));
        const auto shapes = glyph_shapes(g);
        REQUIRE_FALSE(shapes.empty());
        for (const GlyphShape& s : shapes) {
            if (s.kind == GlyphShape::Kind::Line) {
                for (const float v : {s.x1, s.y1, s.x2, s.y2}) {
                    CHECK(v >= 0.0F);
                    CHECK(v <= 16.0F);
                }
            } else {
                CHECK(s.x1 - s.x2 >= 0.0F);
                CHECK(s.x1 + s.x2 <= 16.0F);
                CHECK(s.y1 - s.y2 >= 0.0F);
                CHECK(s.y1 + s.y2 <= 16.0F);
            }
        }
    }
}

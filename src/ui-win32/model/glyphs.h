#pragma once

// The app's small stroke icons, as shapes in a 16 x 16 box: the panel's
// open buttons and the row menu's items draw the same ones (approved 3 and
// 4 Nov). Plain C++; the window draws them with GDI+ (canvas.h).

#include <cstdint>
#include <vector>

namespace floserver::ui {

enum class Glyph : std::uint8_t { Globe, Folder, Terminal, Code, Studio, Copy, Power, Trash };

struct GlyphShape {
    enum class Kind : std::uint8_t { Line, Ellipse } kind = Kind::Line;
    // A line from (x1, y1) to (x2, y2); an ellipse centred on (x1, y1), x2 and y2 its radii.
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
};

// Its shapes, inside 0..16 both ways. Drawn the same way round in every
// language, as Windows draws its own icons.
[[nodiscard]] std::vector<GlyphShape> glyph_shapes(Glyph glyph);

}  // namespace floserver::ui

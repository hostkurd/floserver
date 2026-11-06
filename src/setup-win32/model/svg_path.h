#pragma once

// SVG path data ("M10,20c1,2,3,4,5,6Z…") read into closed shapes made of
// cubic curves, so the setup screens (D25) can draw the app icon's feather
// at any size from the same data as icons/FloServer.svg. Plain C++ so it's
// tested on every platform; the Win32 side turns each figure into a GDI+ path.
//
// Only what that drawing uses: M, L, H, V, C, S and Z, absolute and
// relative. Anything else (arcs, quadratic curves) makes the whole path
// unreadable, so a broken drawing is never half drawn.

#include <optional>
#include <string_view>
#include <vector>

namespace floserver::setup {

struct PathPoint {
    double x = 0;
    double y = 0;
    bool operator==(const PathPoint&) const = default;
};

// One figure: its start, then three points (two controls and the end) per
// curve. Straight lines become curves with their controls on the line.
struct PathFigure {
    std::vector<PathPoint> points;
    bool closed = false;
};

[[nodiscard]] std::optional<std::vector<PathFigure>> parse_svg_path(std::string_view data);

}  // namespace floserver::setup

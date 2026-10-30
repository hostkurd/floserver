#pragma once

// Drawing for the owner-drawn window. Layout is in logical coordinates: x runs
// from the reading start, so in Arabic and Kurdish x = 0 is the right edge,
// the same as the mouse coordinates of a WS_EX_LAYOUTRTL window. The canvas
// draws into an unmirrored bitmap and mirrors each rectangle itself, which
// keeps GDI+ (anti-aliased shapes) and GDI (ClearType text) in step.
//
// A paint has two passes: shapes first (GDI+), then text (GDI), because the
// two shouldn't interleave on one DC.

#include "model/theme.h"

#include <windows.h>

#include <memory>
#include <string_view>

namespace floserver::ui {

// GdiplusStartup for the life of the program.
class GdiplusSession {
public:
    GdiplusSession();
    ~GdiplusSession();
    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;
    GdiplusSession(GdiplusSession&&) = delete;
    GdiplusSession& operator=(GdiplusSession&&) = delete;
    [[nodiscard]] bool ok() const { return ok_; }

private:
    ULONG_PTR token_ = 0;
    bool ok_ = false;
};

enum class Align : std::uint8_t { Start, Center, End };

class Canvas {
public:
    // `dc` is an unmirrored memory DC `width` pixels wide.
    Canvas(HDC dc, int width, bool rtl);
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    Canvas(Canvas&&) = delete;
    Canvas& operator=(Canvas&&) = delete;

    [[nodiscard]] RECT physical(const RECT& logical) const;
    [[nodiscard]] float physical_x(float logical_x) const;

    // Pass 1: shapes.
    void fill(const RECT& r, Rgb color);  // plain, no anti-aliasing
    void round_rect(const RECT& r, float radius, Rgb fill);
    // A 1-pixel-or-more border in `border`, then `fill` inside it.
    void round_rect(const RECT& r, float radius, Rgb fill, Rgb border, float border_width);
    void round_outline(const RECT& r, float radius, Rgb color, float width);
    void ellipse(float cx, float cy, float radius, Rgb fill);
    void circle(float cx, float cy, float radius, Rgb color, float width);
    void line(float x1, float y1, float x2, float y2, Rgb color, float width);
    void clip(const RECT& r);
    void unclip();

    // Pass 2: text. Ends pass 1.
    void begin_text();
    // `code`: a name, address or version, read left to right even in
    // right-to-left languages. Ellipsis when it doesn't fit.
    void text(const RECT& r, std::wstring_view s, HFONT font, Rgb color, Align align, bool code);
    // Several lines, broken at words, cut with an ellipsis when it runs out of room.
    void paragraph(const RECT& r, std::wstring_view s, HFONT font, Rgb color);
    void clip_text(const RECT& r);
    void unclip_text();

    // Width of one line of text.
    [[nodiscard]] static int measure(HDC dc, std::wstring_view s, HFONT font, bool rtl_reading);

private:
    struct Shapes;
    HDC dc_;
    int width_;
    bool rtl_;
    std::unique_ptr<Shapes> shapes_;
    int saved_ = 0;
};

[[nodiscard]] constexpr COLORREF colorref(Rgb c) { return RGB(red(c), green(c), blue(c)); }

// The FloServer mark (the mockup's title-bar logo) as an icon `size` pixels square.
[[nodiscard]] HICON make_app_icon(int size);

}  // namespace floserver::ui

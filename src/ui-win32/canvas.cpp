#include "canvas.h"

#include "win_util.h"

#include <algorithm>
#include <optional>

// The GDI+ headers use min and max unqualified; NOMINMAX takes the macros away.
namespace Gdiplus {
using std::max;  // NOLINT(misc-unused-using-decls): used by the headers below on some SDKs
using std::min;  // NOLINT(misc-unused-using-decls)
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

namespace floserver::ui {

namespace {

Gdiplus::Color gp(Rgb c, BYTE alpha = 255) { return {alpha, red(c), green(c), blue(c)}; }

Gdiplus::RectF rectf(const RECT& r) {
    return {static_cast<Gdiplus::REAL>(r.left), static_cast<Gdiplus::REAL>(r.top),
            static_cast<Gdiplus::REAL>(r.right - r.left), static_cast<Gdiplus::REAL>(r.bottom - r.top)};
}

void add_round(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    radius = std::min(radius, std::min(r.Width, r.Height) / 2.0F);
    if (radius <= 0.5F) {
        path.AddRectangle(r);
        return;
    }
    const float d = radius * 2.0F;
    path.AddArc(r.X, r.Y, d, d, 180.0F, 90.0F);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0F, 90.0F);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0F, 90.0F);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0F, 90.0F);
    path.CloseFigure();
}

void prepare(Gdiplus::Graphics& g) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

}  // namespace

GdiplusSession::GdiplusSession() {
    const Gdiplus::GdiplusStartupInput input;
    ok_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
}

GdiplusSession::~GdiplusSession() {
    if (ok_) {
        Gdiplus::GdiplusShutdown(token_);
    }
}

struct Canvas::Shapes {
    explicit Shapes(HDC dc) : g(dc) { prepare(g); }
    Gdiplus::Graphics g;
};

Canvas::Canvas(HDC dc, int width, bool rtl)
    : dc_(dc), width_(width), rtl_(rtl), shapes_(std::make_unique<Shapes>(dc)) {}

Canvas::~Canvas() {
    unclip_text();
}

RECT Canvas::physical(const RECT& r) const {
    if (!rtl_) {
        return r;
    }
    return {width_ - r.right, r.top, width_ - r.left, r.bottom};
}

float Canvas::physical_x(float x) const { return rtl_ ? static_cast<float>(width_) - x : x; }

void Canvas::fill(const RECT& r, Rgb color) {
    if (shapes_) {
        const Gdiplus::SolidBrush brush(gp(color));
        shapes_->g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        shapes_->g.FillRectangle(&brush, rectf(physical(r)));
        shapes_->g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        return;
    }
    SetDCBrushColor(dc_, colorref(color));
    const RECT p = physical(r);
    FillRect(dc_, &p, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Canvas::round_rect(const RECT& r, float radius, Rgb fill_color) {
    if (!shapes_) {
        return;
    }
    Gdiplus::GraphicsPath path;
    add_round(path, rectf(physical(r)), radius);
    const Gdiplus::SolidBrush brush(gp(fill_color));
    shapes_->g.FillPath(&brush, &path);
}

void Canvas::round_rect(const RECT& r, float radius, Rgb fill_color, Rgb border, float border_width) {
    if (!shapes_) {
        return;
    }
    const Gdiplus::RectF outer = rectf(physical(r));
    Gdiplus::GraphicsPath out_path;
    add_round(out_path, outer, radius);
    const Gdiplus::SolidBrush border_brush(gp(border));
    shapes_->g.FillPath(&border_brush, &out_path);

    const Gdiplus::RectF inner(outer.X + border_width, outer.Y + border_width, outer.Width - 2 * border_width,
                               outer.Height - 2 * border_width);
    Gdiplus::GraphicsPath in_path;
    add_round(in_path, inner, std::max(0.0F, radius - border_width));
    const Gdiplus::SolidBrush fill_brush(gp(fill_color));
    shapes_->g.FillPath(&fill_brush, &in_path);
}

void Canvas::round_outline(const RECT& r, float radius, Rgb color, float width) {
    if (!shapes_) {
        return;
    }
    Gdiplus::RectF f = rectf(physical(r));
    f.Inflate(-width / 2.0F, -width / 2.0F);
    Gdiplus::GraphicsPath path;
    add_round(path, f, radius);
    const Gdiplus::Pen pen(gp(color), width);
    shapes_->g.DrawPath(&pen, &path);
}

void Canvas::ellipse(float cx, float cy, float radius, Rgb fill_color) {
    if (!shapes_) {
        return;
    }
    const Gdiplus::SolidBrush brush(gp(fill_color));
    shapes_->g.FillEllipse(&brush, physical_x(cx) - radius, cy - radius, radius * 2, radius * 2);
}

void Canvas::circle(float cx, float cy, float radius, Rgb color, float width) {
    if (!shapes_) {
        return;
    }
    const Gdiplus::Pen pen(gp(color), width);
    shapes_->g.DrawEllipse(&pen, physical_x(cx) - radius, cy - radius, radius * 2, radius * 2);
}

void Canvas::line(float x1, float y1, float x2, float y2, Rgb color, float width) {
    if (!shapes_) {
        return;
    }
    Gdiplus::Pen pen(gp(color), width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    shapes_->g.DrawLine(&pen, physical_x(x1), y1, physical_x(x2), y2);
}

void Canvas::clip(const RECT& r) {
    if (shapes_) {
        shapes_->g.SetClip(rectf(physical(r)));
    }
}

void Canvas::unclip() {
    if (shapes_) {
        shapes_->g.ResetClip();
    }
}

void Canvas::begin_text() {
    shapes_.reset();  // flushes GDI+ before GDI draws
    SetBkMode(dc_, TRANSPARENT);
}

void Canvas::text(const RECT& r, std::wstring_view s, HFONT font, Rgb color, Align align, bool code) {
    if (s.empty()) {
        return;
    }
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
    switch (align) {
        case Align::Start:
            flags |= rtl_ ? DT_RIGHT : DT_LEFT;
            break;
        case Align::End:
            flags |= rtl_ ? DT_LEFT : DT_RIGHT;
            break;
        case Align::Center:
            flags |= DT_CENTER;
            break;
    }
    if (rtl_ && !code) {
        flags |= DT_RTLREADING;
    }
    RECT p = physical(r);
    const win::Selected selected(dc_, font);
    SetTextColor(dc_, colorref(color));
    DrawTextW(dc_, s.data(), static_cast<int>(s.size()), &p, flags);
}

void Canvas::paragraph(const RECT& r, std::wstring_view s, HFONT font, Rgb color) {
    if (s.empty()) {
        return;
    }
    UINT flags = DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS | DT_EDITCONTROL | (rtl_ ? DT_RIGHT | DT_RTLREADING : DT_LEFT);
    RECT p = physical(r);
    const win::Selected selected(dc_, font);
    SetTextColor(dc_, colorref(color));
    DrawTextW(dc_, s.data(), static_cast<int>(s.size()), &p, flags);
}

void Canvas::clip_text(const RECT& r) {
    unclip_text();
    saved_ = SaveDC(dc_);
    const RECT p = physical(r);
    IntersectClipRect(dc_, p.left, p.top, p.right, p.bottom);
}

void Canvas::unclip_text() {
    if (saved_ != 0) {
        RestoreDC(dc_, saved_);
        saved_ = 0;
    }
}

int Canvas::measure(HDC dc, std::wstring_view s, HFONT font, bool rtl_reading) {
    if (s.empty()) {
        return 0;
    }
    const win::Selected selected(dc, font);
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, s.data(), static_cast<int>(s.size()), &r,
              DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT | (rtl_reading ? DT_RTLREADING : 0U));
    return r.right - r.left;
}

HICON make_app_icon(int size) {
    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bitmap);
        prepare(g);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        const float s = static_cast<float>(size) / 16.0F;
        Gdiplus::GraphicsPath path;
        add_round(path, Gdiplus::RectF(1 * s, 1 * s, 14 * s, 14 * s), 3 * s);
        const Gdiplus::SolidBrush brush(gp(0x0F6CBD));
        g.FillPath(&brush, &path);
        Gdiplus::Pen pen(gp(0xFFFFFF), std::max(1.0F, 1.6F * s));
        pen.SetLineJoin(Gdiplus::LineJoinMiter);
        const Gdiplus::PointF stroke[] = {{5 * s, 11 * s}, {5 * s, 5 * s}, {11 * s, 5 * s}};
        g.DrawLines(&pen, stroke, 3);
        g.DrawLine(&pen, 5 * s, 8 * s, 9 * s, 8 * s);
    }
    HICON icon = nullptr;
    if (bitmap.GetHICON(&icon) != Gdiplus::Ok) {
        return nullptr;
    }
    return icon;
}

}  // namespace floserver::ui

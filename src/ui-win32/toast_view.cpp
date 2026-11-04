#include "toast_view.h"

#include "model/theme.h"
#include "win_util.h"

#include <algorithm>
#include <string>

namespace floserver::ui {

namespace {

int px(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }
float pxf(float dip, UINT dpi) { return dip * static_cast<float>(dpi) / 96.0F; }

}  // namespace

ToastGeometry toast_geometry(HDC dc, const Toast& toast, HFONT font, int window_w, int bottom, UINT dpi, bool rtl) {
    ToastGeometry g;
    if (toast.text.empty() || window_w <= px(64, dpi)) {
        return g;
    }
    const std::wstring text = win::widen(toast.text);
    const int pad_x = px(14, dpi);
    const int pad_y = px(10, dpi);
    const int close = px(24, dpi);
    const int gap = px(8, dpi);
    const int most = std::min(window_w - px(32, dpi), toast_max_width(window_w, px(240, dpi)));
    const int text_most = std::max(px(40, dpi), most - 2 * pad_x - close - gap);
    const int one_line = Canvas::measure(dc, text, font, rtl);
    const int text_w = std::min(one_line + px(2, dpi), text_most);
    // Its height, wrapped as the canvas wraps it, at most kToastMaxLines lines.
    const win::Selected selected(dc, font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int line_h = static_cast<int>(tm.tmHeight);
    RECT measured{0, 0, text_w, 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | (rtl ? UINT{DT_RTLREADING} : 0U));
    const int text_h = std::clamp(static_cast<int>(measured.bottom - measured.top), line_h, line_h * kToastMaxLines);
    const int card_w = text_w + 2 * pad_x + close + gap;
    const int card_h = std::max(text_h + 2 * pad_y, close + px(8, dpi));
    const int left = (window_w - card_w) / 2;
    g.card = {left, bottom - card_h, left + card_w, bottom};
    g.text = {left + pad_x, g.card.top + (card_h - text_h) / 2, left + pad_x + text_w,
              g.card.top + (card_h - text_h) / 2 + text_h};
    const int cy = (g.card.top + g.card.bottom) / 2;
    g.close = {g.card.right - px(6, dpi) - close, cy - close / 2, g.card.right - px(6, dpi), cy - close / 2 + close};
    return g;
}

void paint_toast(Canvas& c, const ToastGeometry& g, const Toast& toast, HFONT font, bool dark, UINT dpi,
                 bool close_hover, bool text_pass) {
    if (g.card.right <= g.card.left || toast.text.empty()) {
        return;
    }
    const Palette& p = palette(dark);
    const bool error = toast.kind == ToastKind::Error;
    const Rgb fill = error ? p.banner_error.fill : p.card;
    const Rgb ink = error ? p.banner_error.text : p.text;
    if (text_pass) {
        c.paragraph(g.text, win::widen(toast.text), font, ink);
        return;
    }
    // A soft edge under it, then the card.
    RECT shade = g.card;
    OffsetRect(&shade, 0, px(2, dpi));
    c.round_rect(shade, pxf(8, dpi), p.card_border);
    c.round_rect(g.card, pxf(8, dpi), fill, error ? p.banner_error.border : p.card_border,
                 std::max(1.0F, pxf(1.0F, dpi)));
    if (close_hover && error) {
        c.round_outline(g.close, pxf(4, dpi), ink, std::max(1.0F, pxf(1.0F, dpi)));
    } else if (close_hover) {
        c.round_rect(g.close, pxf(4, dpi), p.row_hover);
    }
    const float cx = static_cast<float>(g.close.left + g.close.right) / 2.0F;
    const float cy = static_cast<float>(g.close.top + g.close.bottom) / 2.0F;
    const float k = pxf(4.5F, dpi);
    c.line(cx - k, cy - k, cx + k, cy + k, ink, pxf(1.3F, dpi));
    c.line(cx - k, cy + k, cx + k, cy - k, ink, pxf(1.3F, dpi));
}

}  // namespace floserver::ui

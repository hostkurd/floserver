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
    // Opaque, edge to edge (the popup's own region rounds its corners), with a border.
    c.fill(g.card, fill);
    c.round_rect(g.card, pxf(8, dpi), fill, error ? p.banner_error.text : p.card_border,
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

namespace {

constexpr wchar_t kToastClass[] = L"FloServerToast";

}  // namespace

ToastPopup::~ToastPopup() {
    if (hwnd_ != nullptr) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

void ToastPopup::attach(HWND owner, UINT message) {
    if (owner != owner_ && hwnd_ != nullptr) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);  // it belonged to the window that's gone
        hwnd_ = nullptr;
    }
    owner_ = owner;
    message_ = message;
}

void ToastPopup::show(const Toast& toast, const ToastGeometry& g, HFONT font, bool dark, UINT dpi, bool rtl) {
    if (owner_ == nullptr || toast.text.empty() || g.card.right <= g.card.left || IsWindowVisible(owner_) == FALSE ||
        IsIconic(owner_) != FALSE) {
        hide();
        return;
    }
    if (hwnd_ == nullptr) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        if (GetClassInfoExW(instance, kToastClass, &wc) == FALSE) {
            wc = {};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kToastClass;
            RegisterClassExW(&wc);
        }
        // Owned by the window (above it, hidden with it), never activated.
        hwnd_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kToastClass, L"", WS_POPUP, 0, 0, 0, 0, owner_,
                                nullptr, instance, nullptr);
        if (hwnd_ == nullptr) {
            return;
        }
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        close_hover_ = false;
        tracking_ = false;
    }
    toast_ = toast;
    card_ = g.card;
    const int w = g.card.right - g.card.left;
    const int h = g.card.bottom - g.card.top;
    local_ = g;
    for (RECT* r : {&local_.card, &local_.text, &local_.close}) {
        OffsetRect(r, -g.card.left, -g.card.top);
    }
    font_ = font;
    dark_ = dark;
    dpi_ = dpi;
    rtl_ = rtl;
    if (w != size_.cx || h != size_.cy) {
        size_ = {w, h};
        const int round = px(16, dpi);
        SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, w + 1, h + 1, round, round), TRUE);
    }
    place();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ToastPopup::hide() {
    if (hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    toast_ = {};
}

void ToastPopup::follow() {
    if (hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE) {
        place();
    }
}

void ToastPopup::place() {
    // The owner's client area on the screen (two points: right for a mirrored window too).
    RECT client{};
    GetClientRect(owner_, &client);
    MapWindowPoints(owner_, nullptr, reinterpret_cast<POINT*>(&client), 2);
    const int width = client.right - client.left;
    const int left = rtl_ ? width - card_.right : card_.left;  // logical x runs from the right in Arabic and Kurdish
    const bool shown = IsWindowVisible(hwnd_) != FALSE;
    SetWindowPos(hwnd_, nullptr, client.left + left, client.top + card_.top, size_.cx, size_.cy,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    if (!shown) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
}

bool ToastPopup::over_close(LPARAM lparam) const {
    int x = static_cast<short>(LOWORD(lparam));
    const int y = static_cast<short>(HIWORD(lparam));
    if (rtl_) {
        x = size_.cx - x;
    }
    return x >= local_.close.left && x < local_.close.right && y >= local_.close.top && y < local_.close.bottom;
}

void ToastPopup::paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    const int w = size_.cx;
    const int h = size_.cy;
    if (w > 0 && h > 0 && !toast_.text.empty()) {
        const win::UniqueDc mem(CreateCompatibleDC(hdc));
        const win::UniqueBitmap bitmap(CreateCompatibleBitmap(hdc, w, h));
        if (mem && bitmap) {
            const win::Selected selected(mem.get(), bitmap.get());
            {
                Canvas canvas(mem.get(), w, rtl_);
                paint_toast(canvas, local_, toast_, font_, dark_, dpi_, close_hover_, false);
                canvas.begin_text();
                paint_toast(canvas, local_, toast_, font_, dark_, dpi_, close_hover_, true);
            }
            BitBlt(hdc, 0, 0, w, h, mem.get(), 0, 0, SRCCOPY);
        }
    }
    EndPaint(hwnd_, &ps);
}

LRESULT CALLBACK ToastPopup::proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<ToastPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && self->hwnd_ == hwnd) {
        return self->handle(msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT ToastPopup::handle(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // a click never takes the keys from the window
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof tme;
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd_;
                tracking_ = TrackMouseEvent(&tme) != FALSE;
                PostMessageW(owner_, message_, kToastMouseEntered, 0);
            }
            if (const bool over = over_close(lparam); over != close_hover_) {
                close_hover_ = over;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (close_hover_) {
                close_hover_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            PostMessageW(owner_, message_, kToastMouseLeft, 0);
            return 0;
        case WM_LBUTTONUP:
            if (over_close(lparam)) {
                PostMessageW(owner_, message_, kToastCloseClicked, 0);
            }
            return 0;
        case WM_NCDESTROY: {
            const HWND gone = hwnd_;
            hwnd_ = nullptr;
            size_ = {};
            tracking_ = false;
            return DefWindowProcW(gone, msg, wparam, lparam);
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

}  // namespace floserver::ui

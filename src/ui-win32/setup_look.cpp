#include "setup_look.h"

#include "feather_paths.h"
#include "model/svg_path.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

// The GDI+ headers use min and max unqualified; NOMINMAX takes the macros away.
using std::max;  // NOLINT(misc-unused-using-decls): used by the headers below on some SDKs
using std::min;  // NOLINT(misc-unused-using-decls)

#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

namespace floserver::ui::setup_look {

namespace {

constexpr wchar_t kClassName[] = L"FloServerSetupLook";
constexpr COLORREF kPanel = RGB(0x03, 0x64, 0xB5);
constexpr COLORREF kPanelText = RGB(0xDC, 0xEE, 0xFF);
constexpr COLORREF kPanelSmall = RGB(0xCF, 0xE6, 0xFF);
constexpr COLORREF kFooterLine = RGB(0xE6, 0xE6, 0xE6);
constexpr COLORREF kStepLine = RGB(0xD1, 0xD1, 0xD1);
constexpr COLORREF kStepMuted = RGB(0x61, 0x61, 0x61);
constexpr COLORREF kButtonBorder = RGB(0xC9, 0xC9, 0xC9);
constexpr COLORREF kCaptionHover = RGB(0xE9, 0xE9, 0xE9);

Gdiplus::Color gp(COLORREF c, BYTE alpha = 255) { return {alpha, GetRValue(c), GetGValue(c), GetBValue(c)}; }

// A rounded rectangle as a GDI+ path.
void rounded(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    radius = std::min(radius, std::min(r.Width, r.Height) / 2.0F);
    const float d = radius * 2;
    if (d <= 0) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void fill_round(Gdiplus::Graphics& g, Gdiplus::RectF r, float radius, COLORREF fill) {
    Gdiplus::GraphicsPath path;
    rounded(path, r, radius);
    const Gdiplus::SolidBrush brush(gp(fill));
    g.FillPath(&brush, &path);
}

void frame_round(Gdiplus::Graphics& g, Gdiplus::RectF r, float radius, COLORREF colour, float width) {
    Gdiplus::GraphicsPath path;
    rounded(path, r, radius);
    const Gdiplus::Pen pen(gp(colour), width);
    g.DrawPath(&pen, &path);
}

// The feather's two parts, read once from feather_paths.h, and the box they fill.
struct Feather {
    std::vector<PathFigure> light;
    std::vector<PathFigure> white;
    double left = 0;
    double top = 0;
    double size = 1;
    bool ok = false;
};

const Feather& feather() {
    static const Feather f = [] {
        Feather out;
        const auto light = parse_svg_path(kFeatherLight);
        const auto white = parse_svg_path(kFeatherWhite);
        if (!light || !white) {
            return out;
        }
        out.light = *light;
        out.white = *white;
        double x0 = 1e9;
        double y0 = 1e9;
        double x1 = -1e9;
        double y1 = -1e9;
        for (const auto* figures : {&out.light, &out.white}) {
            for (const PathFigure& fig : *figures) {
                for (const PathPoint& p : fig.points) {
                    x0 = std::min(x0, p.x);
                    y0 = std::min(y0, p.y);
                    x1 = std::max(x1, p.x);
                    y1 = std::max(y1, p.y);
                }
            }
        }
        out.size = std::max(x1 - x0, y1 - y0);
        out.left = x0 - ((out.size - (x1 - x0)) / 2);
        out.top = y0 - ((out.size - (y1 - y0)) / 2);
        out.ok = out.size > 0;
        return out;
    }();
    return f;
}

void add_figures(Gdiplus::GraphicsPath& path, const std::vector<PathFigure>& figures) {
    for (const PathFigure& fig : figures) {
        std::vector<Gdiplus::PointF> pts;
        pts.reserve(fig.points.size());
        for (const PathPoint& p : fig.points) {
            pts.emplace_back(static_cast<Gdiplus::REAL>(p.x), static_cast<Gdiplus::REAL>(p.y));
        }
        if (pts.size() >= 4) {
            path.StartFigure();
            path.AddBeziers(pts.data(), static_cast<INT>(pts.size()));
            if (fig.closed) {
                path.CloseFigure();
            }
        }
    }
}

// The feather in a `size` square at x, y, turned by `angle` degrees about its middle.
void draw_feather(Gdiplus::Graphics& g, float x, float y, float size, BYTE alpha, float angle) {
    const Feather& f = feather();
    if (!f.ok) {
        return;
    }
    Gdiplus::GraphicsPath light(Gdiplus::FillModeWinding);
    Gdiplus::GraphicsPath white(Gdiplus::FillModeWinding);
    add_figures(light, f.light);
    add_figures(white, f.white);
    const auto scale = static_cast<Gdiplus::REAL>(size / f.size);
    Gdiplus::Matrix m;
    m.Translate(x + (size / 2), y + (size / 2));
    m.Rotate(angle);
    m.Translate(-size / 2, -size / 2);
    m.Scale(scale, scale);
    m.Translate(static_cast<Gdiplus::REAL>(-f.left), static_cast<Gdiplus::REAL>(-f.top));
    light.Transform(&m);
    white.Transform(&m);
    const Gdiplus::SolidBrush lb(Gdiplus::Color(alpha, 0xB3, 0xE4, 0xFF));
    const Gdiplus::SolidBrush wb(Gdiplus::Color(alpha, 0xFF, 0xFF, 0xFF));
    g.FillPath(&lb, &light);
    g.FillPath(&wb, &white);
}

void draw_tick(Gdiplus::Graphics& g, float x, float y, float size, COLORREF colour, float width) {
    // The mockups' tick: (3.5, 8.5) (6.5, 11.5) (12.5, 4.5) in a 16 square.
    const float k = size / 16;
    const Gdiplus::PointF pts[] = {{x + (3.5F * k), y + (8.5F * k)},
                                   {x + (6.5F * k), y + (11.5F * k)},
                                   {x + (12.5F * k), y + (4.5F * k)}};
    Gdiplus::Pen pen(gp(colour), width);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLines(&pen, pts, 3);
}

bool font_exists(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    bool found = false;
    const HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc, &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lparam) -> int {
            *win::int_to_ptr<bool*>(lparam) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the callback's context
    ReleaseDC(nullptr, dc);
    return found;
}

win::UniqueFont make_font(int px_height, int weight, const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfHeight = -px_height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    return win::UniqueFont(CreateFontIndirectW(&lf));
}

void draw_text(HDC dc, HFONT font, COLORREF colour, std::wstring_view text, RECT r, UINT format) {
    const win::Selected sel(dc, font);
    SetTextColor(dc, colour);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &r, format | DT_NOPREFIX);
}

}  // namespace

WizardWindow::WizardWindow(HINSTANCE instance, std::wstring title, std::wstring panel_note, std::wstring version)
    : instance_(instance), title_(std::move(title)), panel_note_(std::move(panel_note)), version_(std::move(version)) {}

WizardWindow::~WizardWindow() {
    if (hwnd_ != nullptr) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

int WizardWindow::px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }

RECT WizardWindow::px(RECT dip) const { return {px(dip.left), px(dip.top), px(dip.right), px(dip.bottom)}; }

bool WizardWindow::create(int show, const POINT* at) {
    static const ATOM atom = [this] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &WizardWindow::window_proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        wc.hIconSm = static_cast<HICON>(
            LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                       GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        wc.lpszClassName = kClassName;
        return RegisterClassExW(&wc);
    }();
    if (atom == 0) {
        return false;
    }
    // Its own title row, so no Windows caption (WM_NCCALCSIZE takes it
    // away); WS_CAPTION and WS_SYSMENU keep the taskbar button, Alt+Space
    // and Windows' shadow and round corners.
    hwnd_ = CreateWindowExW(0, kClassName, title_.c_str(),
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT,
                            CW_USEDEFAULT, kWidth, kHeight, nullptr, nullptr, instance_, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) {
        dpi_ = 96;
    }
    const int w = px(kWidth);
    const int h = px(kHeight);
    int x = 0;
    int y = 0;
    if (at != nullptr) {
        x = at->x;
        y = at->y;
    } else {
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
        x = mi.rcWork.left + (((mi.rcWork.right - mi.rcWork.left) - w) / 2);
        y = mi.rcWork.top + (((mi.rcWork.bottom - mi.rcWork.top) - h) / 2);
    }
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    // The frame drawn into the window: its shadow stays (and round corners on Windows 11).
    const MARGINS margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    const DWORD round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    make_fonts();
    caption_buttons();
    rebuild();
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    return true;
}

bool WizardWindow::dialog_message(MSG& msg) const {
    return hwnd_ != nullptr && IsDialogMessageW(hwnd_, &msg) != FALSE;
}

void WizardWindow::bring_forward() const {
    if (IsIconic(hwnd_) != FALSE) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
}

void WizardWindow::make_fonts() {
    static const bool variable = font_exists(L"Segoe UI Variable Text");
    const wchar_t* text_face = variable ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* display_face = variable ? L"Segoe UI Variable Display" : L"Segoe UI";
    constexpr int kSemibold = 600;
    fonts_.heading = make_font(px(26), kSemibold, display_face);
    fonts_.sub = make_font(px(14), FW_NORMAL, text_face);
    fonts_.body = make_font(px(14), FW_NORMAL, text_face);
    fonts_.body_bold = make_font(px(14), kSemibold, text_face);
    fonts_.note = make_font(px(12), FW_NORMAL, text_face);
    fonts_.note_bold = make_font(px(12), kSemibold, text_face);
    fonts_.mono = make_font(px(13), FW_NORMAL, L"Consolas");
    fonts_.button = make_font(px(14), FW_NORMAL, text_face);
    fonts_.button_bold = make_font(px(14), kSemibold, text_face);
    fonts_.panel_title = make_font(px(36), kSemibold, display_face);
    fonts_.panel_text = make_font(px(14), FW_NORMAL, text_face);
    fonts_.panel_small = make_font(px(12), FW_NORMAL, text_face);
    fonts_.step = make_font(px(13), FW_NORMAL, text_face);
    fonts_.step_bold = make_font(px(13), kSemibold, text_face);
    fonts_.title = make_font(px(12), FW_NORMAL, text_face);
}

HFONT WizardWindow::font(TextKind kind) const {
    switch (kind) {
        case TextKind::Heading:
            return fonts_.heading.get();
        case TextKind::Sub:
            return fonts_.sub.get();
        case TextKind::Body:
            return fonts_.body.get();
        case TextKind::BodyBold:
            return fonts_.body_bold.get();
        case TextKind::Note:
            return fonts_.note.get();
        case TextKind::NoteBold:
            return fonts_.note_bold.get();
        case TextKind::Mono:
            return fonts_.mono.get();
    }
    return fonts_.body.get();
}

HBRUSH WizardWindow::brush_for(COLORREF colour) {
    auto& b = brushes_[colour];
    if (!b) {
        b.reset(CreateSolidBrush(colour));
    }
    return b.get();
}

void WizardWindow::caption_buttons() {
    for (const int id : {kMinimizeId, kCloseId}) {
        HWND b = CreateWindowExW(0, WC_BUTTONW, id == kCloseId ? L"Close" : L"Minimize",
                                 WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
                                 win::int_to_ptr<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SetWindowSubclass(b, &WizardWindow::button_proc, 1, reinterpret_cast<DWORD_PTR>(this));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the subclass's context
    }
    const int w = px(46);
    SetWindowPos(GetDlgItem(hwnd_, kCloseId), nullptr, px(kWidth) - w, 0, w, px(kTitleH), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd_, kMinimizeId), nullptr, px(kWidth) - (2 * w), 0, w, px(kTitleH), SWP_NOZORDER);
}

void WizardWindow::clear_controls() {
    for (HWND c : controls_) {
        DestroyWindow(c);
    }
    controls_.clear();
    backgrounds_.clear();
    colours_.clear();
    kinds_.clear();
    cards_.clear();
    marks_.clear();
    has_progress_ = false;
    fraction_ = 0;
    hot_ = nullptr;
    default_id_ = 0;
    cancel_id_ = 0;
}

void WizardWindow::rebuild() {
    if (hwnd_ == nullptr) {
        return;
    }
    const HWND had_focus = GetFocus();
    const int focus_id = had_focus != nullptr && GetParent(had_focus) == hwnd_ ? GetDlgCtrlID(had_focus) : 0;
    SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
    clear_controls();
    build_page();
    SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (focus_id != 0 && control(focus_id) != nullptr && IsWindowEnabled(control(focus_id)) != FALSE) {
        SetFocus(control(focus_id));
    } else if (default_id_ != 0 && control(default_id_) != nullptr && IsWindowEnabled(control(default_id_)) != FALSE) {
        SetFocus(control(default_id_));
    } else if (cancel_id_ != 0 && control(cancel_id_) != nullptr) {
        SetFocus(control(cancel_id_));
    }
}

void WizardWindow::start_page(std::vector<std::wstring> steps, int current) {
    clear_controls();
    steps_ = std::move(steps);
    current_step_ = current;
}

HWND WizardWindow::add_text(RECT r, std::wstring_view text, TextKind kind, COLORREF background, COLORREF color) {
    const RECT p = px(r);
    HWND c = CreateWindowExW(0, WC_STATICW, std::wstring(text).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                             p.left, p.top, p.right - p.left, p.bottom - p.top, hwnd_, nullptr, instance_, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font(kind)), FALSE);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): WM_SETFONT's handle
    controls_.push_back(c);
    backgrounds_[c] = background;
    colours_[c] = color;
    return c;
}

HWND WizardWindow::add_button(int id, RECT r, std::wstring_view text, ButtonKind kind, bool enabled) {
    const RECT p = px(r);
    HWND c = CreateWindowExW(0, WC_BUTTONW, std::wstring(text).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             p.left, p.top, p.right - p.left, p.bottom - p.top, hwnd_,
                             win::int_to_ptr<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    SendMessageW(c, WM_SETFONT,
                 reinterpret_cast<WPARAM>(kind == ButtonKind::Secondary || kind == ButtonKind::Link  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): WM_SETFONT's handle
                                              ? fonts_.button.get()
                                              : fonts_.button_bold.get()),
                 FALSE);
    SetWindowSubclass(c, &WizardWindow::button_proc, 1, reinterpret_cast<DWORD_PTR>(this));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the subclass's context
    EnableWindow(c, enabled ? TRUE : FALSE);
    controls_.push_back(c);
    kinds_[c] = kind;
    return c;
}

HWND WizardWindow::add_check(int id, RECT r, std::wstring_view text, bool checked, bool enabled) {
    const RECT p = px(r);
    HWND c = CreateWindowExW(0, WC_BUTTONW, std::wstring(text).c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP, p.left, p.top,
                             p.right - p.left, p.bottom - p.top, hwnd_,
                             win::int_to_ptr<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body.get()), FALSE);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): WM_SETFONT's handle
    SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(c, enabled ? TRUE : FALSE);
    controls_.push_back(c);
    backgrounds_[c] = kWhite;
    return c;
}

HWND WizardWindow::adopt(HWND c, COLORREF background) {
    if (c != nullptr) {
        controls_.push_back(c);
        backgrounds_[c] = background;
    }
    return c;
}

void WizardWindow::add_card(RECT r, COLORREF fill, COLORREF border) { cards_.push_back({r, fill, border}); }

int WizardWindow::add_mark(int x, int y, MarkKind kind) {
    marks_.push_back({x, y, kind});
    return static_cast<int>(marks_.size()) - 1;
}

void WizardWindow::set_mark(int index, MarkKind kind) {
    if (index < 0 || static_cast<std::size_t>(index) >= marks_.size()) {
        return;
    }
    Mark& m = marks_[static_cast<std::size_t>(index)];
    if (m.kind == kind) {
        return;
    }
    m.kind = kind;
    const RECT r = px(RECT{m.x - 2, m.y - 2, m.x + 60, m.y + 60});
    InvalidateRect(hwnd_, &r, FALSE);
}

void WizardWindow::add_progress(RECT r) {
    progress_ = r;
    has_progress_ = true;
    fraction_ = 0;
}

void WizardWindow::set_progress(double fraction) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (!has_progress_ || std::abs(fraction - fraction_) < 0.001) {
        return;
    }
    fraction_ = fraction;
    const RECT r = px(progress_);
    InvalidateRect(hwnd_, &r, FALSE);
}

void WizardWindow::set_text(HWND c, std::wstring_view text) const {
    if (c == nullptr) {
        return;
    }
    SetWindowTextW(c, std::wstring(text).c_str());
    // A static's old words would show through: its part of the window paints first.
    RECT r{};
    GetWindowRect(c, &r);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&r), 2);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): a RECT is two POINTs
    InvalidateRect(hwnd_, &r, TRUE);
}

HWND WizardWindow::control(int id) const { return GetDlgItem(hwnd_, id); }

bool WizardWindow::checked(int id) const { return IsDlgButtonChecked(hwnd_, id) == BST_CHECKED; }

void WizardWindow::enable(int id, bool on) const {
    if (HWND c = control(id); c != nullptr) {
        EnableWindow(c, on ? TRUE : FALSE);
        InvalidateRect(c, nullptr, FALSE);
    }
}

void WizardWindow::focus(int id) const {
    if (HWND c = control(id); c != nullptr) {
        SetFocus(c);
    }
}

bool WizardWindow::request_close() {
    if (on_close_request()) {
        DestroyWindow(hwnd_);
        return true;
    }
    return false;
}

LRESULT CALLBACK WizardWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* cs = win::int_to_ptr<const CREATESTRUCTW*>(lparam);
        auto* self = static_cast<WizardWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the window's object
    }
    auto* self = win::int_to_ptr<WizardWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return self->handle(message, wparam, lparam);
}

LRESULT CALLBACK WizardWindow::button_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR /*id*/,
                                           DWORD_PTR data) {
    auto* self = win::int_to_ptr<WizardWindow*>(data);
    switch (message) {
        case WM_MOUSEMOVE:
            if (self->hot_ != hwnd) {
                HWND old = self->hot_;
                self->hot_ = hwnd;
                if (old != nullptr) {
                    InvalidateRect(old, nullptr, FALSE);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof tme;
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            break;
        case WM_MOUSELEAVE:
            if (self->hot_ == hwnd) {
                self->hot_ = nullptr;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_SETCURSOR:
            if (const auto it = self->kinds_.find(hwnd); it != self->kinds_.end() && it->second == ButtonKind::Link) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, &WizardWindow::button_proc, 1);
            if (self->hot_ == hwnd) {
                self->hot_ = nullptr;
            }
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

LRESULT WizardWindow::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCCALCSIZE:
            if (wparam != FALSE) {
                return 0;  // no Windows caption or border: the window paints all of itself
            }
            break;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &pt);
            if (pt.y >= 0 && pt.y < px(kTitleH)) {
                return HTCAPTION;  // the title row moves the window
            }
            return HTCLIENT;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            const HDC dc = BeginPaint(hwnd_, &ps);
            RECT client{};
            GetClientRect(hwnd_, &client);
            const win::UniqueDc mem(CreateCompatibleDC(dc));
            const win::UniqueBitmap bmp(CreateCompatibleBitmap(dc, client.right, client.bottom));
            {
                const win::Selected sel(mem.get(), bmp.get());
                paint(mem.get());
                BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                       ps.rcPaint.bottom - ps.rcPaint.top, mem.get(), ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            }
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const HDC dc = win::int_to_ptr<HDC>(wparam);
            HWND c = win::int_to_ptr<HWND>(lparam);
            const auto bg = backgrounds_.find(c);
            const COLORREF background = bg == backgrounds_.end() ? kWhite : bg->second;
            const auto fg = colours_.find(c);
            SetTextColor(dc, fg == colours_.end() ? kText : fg->second);
            SetBkColor(dc, background);
            return reinterpret_cast<LRESULT>(brush_for(background));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the brush handle
        }
        case WM_DRAWITEM:
            draw_button(*win::int_to_ptr<const DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        case DM_GETDEFID:
            if (default_id_ != 0 && control(default_id_) != nullptr && IsWindowEnabled(control(default_id_)) != FALSE) {
                return MAKELRESULT(default_id_, DC_HASDEFID);
            }
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            if (id == kCloseId) {
                request_close();
            } else if (id == kMinimizeId) {
                ShowWindow(hwnd_, SW_MINIMIZE);
            } else if (id == IDCANCEL) {
                // Esc: the page's cancel button, else as ✕.
                if (cancel_id_ != 0 && control(cancel_id_) != nullptr && IsWindowEnabled(control(cancel_id_)) != FALSE) {
                    on_command(cancel_id_);
                } else if (cancel_id_ == 0) {
                    request_close();
                }
            } else if (id == IDOK) {
                if (default_id_ != 0 && control(default_id_) != nullptr && IsWindowEnabled(control(default_id_)) != FALSE) {
                    on_command(default_id_);
                }
            } else if (HIWORD(wparam) == BN_CLICKED) {
                on_command(id);
            }
            return 0;
        }
        case WM_CLOSE:
            request_close();
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam);
            const auto* r = win::int_to_ptr<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, px(kWidth), px(kHeight), SWP_NOZORDER | SWP_NOACTIVATE);
            make_fonts();
            const int w = px(46);
            SetWindowPos(GetDlgItem(hwnd_, kCloseId), nullptr, px(kWidth) - w, 0, w, px(kTitleH), SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd_, kMinimizeId), nullptr, px(kWidth) - (2 * w), 0, w, px(kTitleH),
                         SWP_NOZORDER);
            rebuild();
            return 0;
        }
        case WM_DESTROY:
            clear_controls();
            return 0;
        case WM_NCDESTROY: {
            hwnd_ = nullptr;
            on_destroyed();
            return 0;
        }
        default:
            if (message >= WM_APP && message <= 0xBFFF) {
                on_app_message(message, wparam, lparam);
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

void WizardWindow::paint(HDC dc) const {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const auto s = static_cast<float>(dpi_) / 96.0F;
    auto f = [s](int dip) { return static_cast<float>(dip) * s; };

    // The page, white; the panel, blue.
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const RECT panel{0, 0, px(kPanelW), client.bottom};
    const win::UniqueBrush panel_brush(CreateSolidBrush(kPanel));
    FillRect(dc, &panel, panel_brush.get());

    // Where each step of the step bar goes: its circle, then its name.
    struct StepPos {
        int x = 0;
        int label_x = 0;
        int label_w = 0;
    };
    constexpr int kStepY = kTitleH + 4;
    std::vector<StepPos> steps;
    {
        int x = kContentX;
        for (std::size_t i = 0; i < steps_.size(); ++i) {
            const bool now = static_cast<int>(i) == current_step_;
            const win::Selected sel(dc, now ? fonts_.step_bold.get() : fonts_.step.get());
            SIZE ext{};
            GetTextExtentPoint32W(dc, steps_[i].c_str(), static_cast<int>(steps_[i].size()), &ext);
            StepPos p;
            p.x = x;
            p.label_x = x + 26 + 8;
            p.label_w = MulDiv(ext.cx, 96, static_cast<int>(dpi_)) + 2;
            steps.push_back(p);
            x = p.label_x + p.label_w + 12 + 48 + 12;
        }
    }

    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        // The faint large feather at the panel's foot, cut at its edge.
        g.SetClip(Gdiplus::RectF(0, 0, f(kPanelW), f(kHeight)));
        draw_feather(g, f(kPanelW - 270), f(kHeight - 300), f(420), 26, -12);
        g.ResetClip();
        // The feather above the name.
        draw_feather(g, f(32), f(150), f(112), 255, 0);
        // The title row's small icon: the feather on a faint tile.
        fill_round(g, Gdiplus::RectF(f(14), f(10), f(16), f(16)), f(3), RGB(0x2A, 0x7C, 0xC4));
        draw_feather(g, f(15), f(11), f(14), 255, 0);

        // Cards (behind their texts).
        for (const Card& c : cards_) {
            const Gdiplus::RectF r(f(c.r.left), f(c.r.top), f(c.r.right - c.r.left), f(c.r.bottom - c.r.top));
            fill_round(g, r, f(8), c.fill);
            if (c.border != c.fill) {
                frame_round(g, r, f(8), c.border, std::max(1.0F, s));
            }
        }
        // The progress bar.
        if (has_progress_) {
            const Gdiplus::RectF r(f(progress_.left), f(progress_.top), f(progress_.right - progress_.left),
                                   f(progress_.bottom - progress_.top));
            fill_round(g, r, r.Height / 2, RGB(0xE6, 0xE6, 0xE6));
            if (fraction_ > 0) {
                Gdiplus::RectF done = r;
                done.Width = std::max(done.Height, static_cast<float>(r.Width * fraction_));
                fill_round(g, done, r.Height / 2, kAccent);
            }
        }
        // Marks.
        for (const Mark& m : marks_) {
            const float x = f(m.x);
            const float y = f(m.y);
            switch (m.kind) {
                case MarkKind::Done:
                    fill_round(g, Gdiplus::RectF(x, y, f(20), f(20)), f(10), kAccentSoft);
                    draw_tick(g, x + f(3), y + f(3), f(14), kAccent, 2 * s);
                    break;
                case MarkKind::Now: {
                    const Gdiplus::Pen ring(gp(kAccentSoft), 2 * s);
                    g.DrawEllipse(&ring, x + f(3), y + f(3), f(14), f(14));
                    const Gdiplus::Pen arc(gp(kAccent), 2 * s);
                    g.DrawArc(&arc, x + f(3), y + f(3), f(14), f(14), -90, 120);
                    break;
                }
                case MarkKind::Next:
                    fill_round(g, Gdiplus::RectF(x + f(6), y + f(6), f(8), f(8)), f(4), RGB(0xC9, 0xC9, 0xC9));
                    break;
                case MarkKind::Info: {
                    const Gdiplus::Pen pen(gp(kAccent), 1.4F * s);
                    g.DrawEllipse(&pen, x + f(1), y + f(1), f(14), f(14));
                    Gdiplus::Pen stem(gp(kAccent), 1.6F * s);
                    stem.SetStartCap(Gdiplus::LineCapRound);
                    stem.SetEndCap(Gdiplus::LineCapRound);
                    g.DrawLine(&stem, x + f(8), y + f(7), x + f(8), y + (11.2F * s));
                    g.DrawLine(&stem, x + f(8), y + (4.6F * s), x + f(8), y + (4.8F * s));
                    break;
                }
                case MarkKind::BigCheck:
                    fill_round(g, Gdiplus::RectF(x, y, f(56), f(56)), f(28), kAccentSoft);
                    draw_tick(g, x + f(14), y + f(14), f(28), kAccent, 3 * s);
                    break;
                case MarkKind::Warning: {
                    fill_round(g, Gdiplus::RectF(x, y, f(56), f(56)), f(28), kDangerSoft);
                    Gdiplus::Pen pen(gp(kDanger), 3.5F * s);
                    pen.SetStartCap(Gdiplus::LineCapRound);
                    pen.SetEndCap(Gdiplus::LineCapRound);
                    g.DrawLine(&pen, x + f(28), y + f(16), x + f(28), y + f(32));
                    g.DrawLine(&pen, x + f(28), y + f(40), x + f(28), y + (40.2F * s));
                    break;
                }
            }
        }
        // The step bar's circles and lines (its words go on below, with GDI).
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const StepPos& p = steps[i];
            const int idx = static_cast<int>(i);
            const Gdiplus::RectF dot(f(p.x), f(kStepY), f(26), f(26));
            if (idx < current_step_) {
                fill_round(g, dot, f(13), kAccentSoft);
                draw_tick(g, f(p.x + 6), f(kStepY + 6), f(14), kAccent, 2 * s);
            } else if (idx == current_step_) {
                fill_round(g, dot, f(13), kAccent);
            } else {
                const Gdiplus::Pen pen(gp(RGB(0xA8, 0xA8, 0xA8)), std::max(1.0F, s));
                g.DrawEllipse(&pen, f(p.x) + (0.5F * s), f(kStepY) + (0.5F * s), f(25), f(25));
            }
            if (i + 1 < steps.size()) {
                const int lx = p.label_x + p.label_w + 12;
                fill_round(g, Gdiplus::RectF(f(lx), f(kStepY + 12), f(48), f(2)), f(1),
                           idx < current_step_ ? kAccent : kStepLine);
            }
        }
        // The footer's line.
        const Gdiplus::SolidBrush line(gp(kFooterLine));
        g.FillRectangle(&line, f(kPanelW), f(kFooterY), f(kWidth - kPanelW), std::max(1.0F, s));
    }

    // The step bar's numbers and names.
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const StepPos& p = steps[i];
        const int idx = static_cast<int>(i);
        const bool now = idx == current_step_;
        const bool done = idx < current_step_;
        const HFONT sf = now ? fonts_.step_bold.get() : fonts_.step.get();
        if (!done) {
            draw_text(dc, sf, now ? kWhite : kStepMuted, std::to_wstring(i + 1), px(RECT{p.x, kStepY, p.x + 26, kStepY + 26}),
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        draw_text(dc, sf, now ? kText : (done ? kText2 : kStepMuted), steps_[i],
                  px(RECT{p.label_x, kStepY, p.label_x + p.label_w, kStepY + 26}), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // The panel's words.
    draw_text(dc, fonts_.title.get(), RGB(0xE6, 0xF3, 0xFF), title_, px(RECT{38, 0, kPanelW, kTitleH}),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(dc, fonts_.panel_title.get(), kWhite, L"FloServer", px(RECT{32, 274, kPanelW - 16, 318}),
              DT_LEFT | DT_SINGLELINE);
    draw_text(dc, fonts_.panel_text.get(), kPanelText, panel_note_, px(RECT{32, 330, kPanelW - 32, 430}),
              DT_LEFT | DT_WORDBREAK);
    draw_text(dc, fonts_.panel_small.get(), kPanelSmall, version_, px(RECT{32, kHeight - 62, kPanelW - 16, kHeight - 22}),
              DT_LEFT | DT_WORDBREAK);
}

void WizardWindow::draw_button(const DRAWITEMSTRUCT& item) const {
    const HDC dc = item.hDC;
    const RECT r = item.rcItem;
    const int id = static_cast<int>(item.CtlID);
    const bool hot = item.hwndItem == hot_;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0;
    const auto s = static_cast<float>(dpi_) / 96.0F;

    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(item.hwndItem)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(GetWindowTextW(item.hwndItem, text.data(), static_cast<int>(text.size()))));

    const win::UniqueDc mem(CreateCompatibleDC(dc));
    const win::UniqueBitmap bmp(CreateCompatibleBitmap(dc, r.right - r.left, r.bottom - r.top));
    const win::Selected sel(mem.get(), bmp.get());
    const RECT local{0, 0, r.right - r.left, r.bottom - r.top};

    // Caption buttons: on the white page side of the title row.
    if (id == kMinimizeId || id == kCloseId) {
        const COLORREF bg = hot ? (id == kCloseId ? kDanger : kCaptionHover) : kWhite;
        const win::UniqueBrush b(CreateSolidBrush(bg));
        FillRect(mem.get(), &local, b.get());
        Gdiplus::Graphics g(mem.get());
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const COLORREF fg = hot && id == kCloseId ? kWhite : kMuted;
        const Gdiplus::Pen pen(gp(fg), std::max(1.0F, s));
        const float cx = static_cast<float>(local.right) / 2;
        const float cy = static_cast<float>(local.bottom) / 2;
        const float k = 5 * s;
        if (id == kCloseId) {
            g.DrawLine(&pen, cx - k, cy - k, cx + k, cy + k);
            g.DrawLine(&pen, cx - k, cy + k, cx + k, cy - k);
        } else {
            g.DrawLine(&pen, cx - k, cy, cx + k, cy);
        }
        BitBlt(dc, r.left, r.top, local.right, local.bottom, mem.get(), 0, 0, SRCCOPY);
        return;
    }

    FillRect(mem.get(), &local, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const auto it = kinds_.find(item.hwndItem);
    const ButtonKind kind = it == kinds_.end() ? ButtonKind::Secondary : it->second;
    COLORREF fill = kWhite;
    COLORREF border = kButtonBorder;
    COLORREF fg = kText;
    switch (kind) {
        case ButtonKind::Primary:
            fill = pressed ? RGB(0x0B, 0x55, 0x96) : (hot ? RGB(0x0E, 0x61, 0xAA) : kAccent);
            border = fill;
            fg = kWhite;
            break;
        case ButtonKind::Danger:
            fill = pressed ? RGB(0x9E, 0x22, 0x16) : (hot ? RGB(0xB0, 0x27, 0x19) : kDanger);
            border = fill;
            fg = kWhite;
            break;
        case ButtonKind::Secondary:
            fill = pressed ? RGB(0xEC, 0xEC, 0xEC) : (hot ? RGB(0xF5, 0xF5, 0xF5) : kWhite);
            break;
        case ButtonKind::Link:
            fill = kWhite;
            border = kWhite;
            fg = pressed ? RGB(0x0B, 0x4F, 0x8A) : kAccent;
            break;
    }
    {
        Gdiplus::Graphics g(mem.get());
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        if (kind != ButtonKind::Link) {
            const Gdiplus::RectF box(0.5F * s, 0.5F * s, static_cast<float>(local.right) - s,
                                     static_cast<float>(local.bottom) - s);
            fill_round(g, box, 6 * s, fill);
            if (border != fill) {
                frame_round(g, box, 6 * s, border, std::max(1.0F, s));
            }
            if (focused) {
                const Gdiplus::RectF ring(2 * s, 2 * s, static_cast<float>(local.right) - (4 * s),
                                          static_cast<float>(local.bottom) - (4 * s));
                frame_round(g, ring, 5 * s, kind == ButtonKind::Secondary ? kText : kWhite, std::max(1.0F, s));
            }
        }
    }
    if (disabled) {
        fg = kind == ButtonKind::Secondary || kind == ButtonKind::Link ? RGB(0xA0, 0xA0, 0xA0) : RGB(0xE8, 0xF1, 0xFA);
        if (kind == ButtonKind::Primary || kind == ButtonKind::Danger) {
            // A pale button that still reads as the main one.
            Gdiplus::Graphics g(mem.get());
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const Gdiplus::RectF box(0.5F * s, 0.5F * s, static_cast<float>(local.right) - s,
                                     static_cast<float>(local.bottom) - s);
            fill_round(g, box, 6 * s, kind == ButtonKind::Primary ? RGB(0x9F, 0xC4, 0xE6) : RGB(0xE3, 0xA5, 0x9F));
        }
    }
    const HFONT bfont = win::int_to_ptr<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    if (kind == ButtonKind::Link) {
        // Left-aligned, underlined while the mouse is on it or it has the focus.
        LOGFONTW lf{};
        GetObjectW(bfont, sizeof lf, &lf);
        lf.lfUnderline = hot || focused ? TRUE : FALSE;
        const win::UniqueFont lfont(CreateFontIndirectW(&lf));
        draw_text(mem.get(), lfont.get(), fg, text, local, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        draw_text(mem.get(), bfont, fg, text, local, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    BitBlt(dc, r.left, r.top, local.right, local.bottom, mem.get(), 0, 0, SRCCOPY);
}

}  // namespace floserver::ui::setup_look

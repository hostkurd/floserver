#include "window_style.h"

#include "model/theme.h"
#include "canvas.h"

#include <dwmapi.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>

namespace floserver::ui {

namespace {

// DWM attributes newer than some SDKs.
constexpr DWORD kDwmDarkMode = 20;       // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmDarkModeOld = 19;    // the same before Windows 10 20H1
constexpr DWORD kDwmCaptionColor = 35;   // DWMWA_CAPTION_COLOR, Windows 11

int CALLBACK font_found(const LOGFONTW* /*lf*/, const TEXTMETRICW* /*tm*/, DWORD /*type*/, LPARAM found) {
    *win::int_to_ptr<bool*>(found) = true;
    return 0;
}

bool font_exists(const std::wstring& face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    const std::size_t n = std::min(face.size(), std::size(lf.lfFaceName) - 1);
    face.copy(std::begin(lf.lfFaceName), n);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, font_found, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

void set_face(LOGFONTW& lf, const std::wstring& face) {
    const std::size_t n = std::min(face.size(), std::size(lf.lfFaceName) - 1);
    face.copy(std::begin(lf.lfFaceName), n);
    *(std::begin(lf.lfFaceName) + n) = L'\0';
}

}  // namespace

bool apps_use_dark_theme() {
    wchar_t forced[8] = {};
    if (GetEnvironmentVariableW(L"FLOSERVER_THEME", forced, 8) > 0) {
        return std::wstring_view(forced) == L"dark";
    }
    DWORD value = 1;
    DWORD size = sizeof value;
    const LSTATUS status =
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value == 0;
}

void apply_title_bar(HWND hwnd, bool dark) {
    const BOOL on = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmDarkMode, &on, sizeof on))) {
        DwmSetWindowAttribute(hwnd, kDwmDarkModeOld, &on, sizeof on);
    }
    const COLORREF caption = colorref(palette(dark).window);
    DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &caption, sizeof caption);  // fails quietly before Windows 11
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void make_ui_fonts(UiFonts& fonts, UINT dpi, Lang lang) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    LOGFONTW base{};
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi) != FALSE) {
        base = ncm.lfMessageFont;
    } else {
        set_face(base, L"Segoe UI");
    }
    std::wstring face = std::begin(base.lfFaceName);
    // Segoe UI Variable (Windows 11) has no Arabic letters; the Arabic and
    // Kurdish window keeps the system's Segoe UI.
    if (lang == Lang::En && font_exists(L"Segoe UI Variable Text")) {
        face = L"Segoe UI Variable Text";
    }
    const std::wstring semibold = face + L" Semibold";
    const bool has_semibold = font_exists(semibold);

    const auto make = [&](int size, bool bold) {
        LOGFONTW lf = base;
        lf.lfHeight = -MulDiv(size, static_cast<int>(dpi), 96);
        lf.lfWidth = 0;
        lf.lfItalic = 0;
        lf.lfUnderline = 0;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfWeight = bold && !has_semibold ? FW_SEMIBOLD : FW_NORMAL;
        set_face(lf, bold && has_semibold ? semibold : face);
        return win::UniqueFont(CreateFontIndirectW(&lf));
    };
    fonts.title = make(20, true);
    fonts.panel_title = make(18, true);
    fonts.body = make(14, false);
    fonts.body_bold = make(14, true);
    fonts.caption = make(12, false);
    fonts.small_bold = make(12, true);
    fonts.button = make(13, false);
    fonts.button_bold = make(13, true);
    // The Log box: Cascadia Mono (Windows 11) or Consolas, 11.5 px.
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(23, static_cast<int>(dpi), 192);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    set_face(lf, font_exists(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas");
    fonts.mono = win::UniqueFont(CreateFontIndirectW(&lf));
}

}  // namespace floserver::ui

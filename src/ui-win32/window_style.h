#pragma once

// What the main window and Settings share so they look the same: Windows'
// light or dark app theme, the title bar's colours and the fonts.

#include "model/ui_strings.h"
#include "win_util.h"

#include <windows.h>

namespace floserver::ui {

// Windows' app theme, unless FLOSERVER_THEME says "light" or "dark" (for
// screenshots and tests).
[[nodiscard]] bool apps_use_dark_theme();

// The title bar in the theme's window colour (Windows 11; older ones keep theirs).
void apply_title_bar(HWND hwnd, bool dark);

struct UiFonts {
    win::UniqueFont title;
    win::UniqueFont panel_title;
    win::UniqueFont mono;
    win::UniqueFont body;
    win::UniqueFont body_bold;
    win::UniqueFont caption;  // not "small": the Windows headers #define small
    win::UniqueFont small_bold;
    win::UniqueFont button;
    win::UniqueFont button_bold;
};

// Segoe UI Variable Text where it can draw the language, at `dpi`.
void make_ui_fonts(UiFonts& fonts, UINT dpi, Lang lang);

}  // namespace floserver::ui

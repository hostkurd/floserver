#pragma once

// The window's colours, light and dark, from the approved mockups (Main,
// MainDark). Every colour the window draws comes from here.

#include "model/view_model.h"

#include <cstdint>

namespace floserver::ui {

// 0xRRGGBB
using Rgb = std::uint32_t;

struct StateColors {
    Rgb fill;
    Rgb text;
    Rgb border;  // the same as fill when the label has no outline
};

struct Palette {
    Rgb window;         // behind everything, and the title bar on Windows 11
    Rgb text;
    Rgb text_subtle;
    Rgb card;           // the project list
    Rgb card_border;
    Rgb divider;        // between rows
    Rgb row_hover;
    Rgb focus;          // keyboard focus ring
    Rgb chip;
    Rgb chip_text;
    Rgb link;
    Rgb accent;         // New project, switch On
    Rgb accent_text;
    Rgb input;
    Rgb input_border;
    Rgb button;
    Rgb button_border;
    Rgb switch_off_fill;
    Rgb switch_off_border;
    Rgb switch_off_knob;
    Rgb switch_on_knob;
    Rgb footer;
    Rgb footer_border;
    Rgb footer_dot;
    Rgb selected;       // the row whose panel is open
    Rgb panel_box;      // the database box in the panel
    Rgb log;            // the Log box: dark in both themes, as in the mockups
    Rgb log_text;
    Rgb log_subtle;     // the time column
    // Settings (D20).
    Rgb nav_selected;   // the open section in the sidebar
    Rgb button_disabled;
    Rgb button_disabled_text;
    Rgb bar_track;      // under a download's progress
    StateColors on;
    StateColors asleep;
    StateColors waking;
    StateColors off;
    StateColors needs_download;
    StateColors problem;
    StateColors banner_info;
    StateColors banner_error;
    StateColors chip_default;  // "Default" in Settings > Runtimes
};

[[nodiscard]] const Palette& palette(bool dark);
[[nodiscard]] const StateColors& state_colors(const Palette& p, RowState state);

[[nodiscard]] constexpr std::uint8_t red(Rgb c) { return static_cast<std::uint8_t>((c >> 16U) & 0xFFU); }
[[nodiscard]] constexpr std::uint8_t green(Rgb c) { return static_cast<std::uint8_t>((c >> 8U) & 0xFFU); }
[[nodiscard]] constexpr std::uint8_t blue(Rgb c) { return static_cast<std::uint8_t>(c & 0xFFU); }

// WCAG contrast ratio of two colours, 1 to 21.
[[nodiscard]] double contrast(Rgb a, Rgb b);

}  // namespace floserver::ui

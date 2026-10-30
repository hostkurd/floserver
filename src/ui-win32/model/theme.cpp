#include "model/theme.h"

#include <algorithm>
#include <cmath>

namespace floserver::ui {

namespace {

constexpr Palette kLight{
    .window = 0xF3F3F3,
    .text = 0x1B1B1B,
    .text_subtle = 0x5F5F5F,
    .card = 0xFFFFFF,
    .card_border = 0xE1E1E1,
    .divider = 0xECECEC,
    .row_hover = 0xF7F7F7,
    .focus = 0x1B1B1B,
    .chip = 0xEEF0F3,
    .chip_text = 0x3B3B3B,
    .link = 0x0F6CBD,
    .accent = 0x0F6CBD,
    .accent_text = 0xFFFFFF,
    .input = 0xFFFFFF,
    .input_border = 0xD1D1D1,
    .button = 0xFFFFFF,
    .button_border = 0xD1D1D1,
    .switch_off_fill = 0xFFFFFF,
    .switch_off_border = 0x8A8A8A,
    .switch_off_knob = 0x5F5F5F,
    .switch_on_knob = 0xFFFFFF,
    .footer = 0xFAFAFA,
    .footer_border = 0xE1E1E1,
    .footer_dot = 0x0E7A0D,
    .selected = 0xEBF3FB,
    .panel_box = 0xF7F7F7,
    .log = 0x1E1E1E,
    .log_text = 0xD4D4D4,
    .log_subtle = 0x9D9D9D,
    .nav_selected = 0xE6E6E6,
    .button_disabled = 0xF7F7F7,
    .button_disabled_text = 0x8A8A8A,
    .bar_track = 0xE6E6E6,
    .on = {0xDFF6DD, 0x0E700E, 0xDFF6DD},
    .asleep = {0xFFF4CE, 0x7A4A00, 0xFFF4CE},
    .waking = {0xE8F1FB, 0x0B5394, 0xE8F1FB},
    .off = {0xF0F0F0, 0x5F5F5F, 0xF0F0F0},
    .needs_download = {0xFFFFFF, 0xA4262C, 0xE8A6AA},
    .problem = {0xFDE7E9, 0xA4262C, 0xFDE7E9},
    .banner_info = {0xE8F1FB, 0x0B5394, 0xE8F1FB},
    .banner_error = {0xFDE7E9, 0xA4262C, 0xFDE7E9},
    .chip_default = {0xDCEBFA, 0x0B4F8A, 0xDCEBFA},
};

constexpr Palette kDark{
    .window = 0x202020,
    .text = 0xF3F3F3,
    .text_subtle = 0xB4B4B4,
    .card = 0x2B2B2B,
    .card_border = 0x3A3A3A,
    .divider = 0x353535,
    .row_hover = 0x333333,
    .focus = 0xF3F3F3,
    .chip = 0x3A3A3A,
    .chip_text = 0xDADADA,
    .link = 0x60CDFF,
    .accent = 0x60CDFF,
    .accent_text = 0x000000,
    .input = 0x2D2D2D,
    .input_border = 0x454545,
    .button = 0x2D2D2D,
    .button_border = 0x454545,
    .switch_off_fill = 0x2B2B2B,
    .switch_off_border = 0x9A9A9A,
    .switch_off_knob = 0xCFCFCF,
    .switch_on_knob = 0x000000,
    .footer = 0x1C1C1C,
    .footer_border = 0x3A3A3A,
    .footer_dot = 0x6CCB5F,
    .selected = 0x2F3B48,
    .panel_box = 0x333333,
    .log = 0x161616,
    .log_text = 0xD4D4D4,
    .log_subtle = 0x9D9D9D,
    .nav_selected = 0x2D2D2D,
    .button_disabled = 0x272727,
    .button_disabled_text = 0x7A7A7A,
    .bar_track = 0x3A3A3A,
    .on = {0x1E3A1E, 0x6CCB5F, 0x1E3A1E},
    .asleep = {0x433519, 0xFCE100, 0x433519},
    .waking = {0x1C3550, 0x8AC7FF, 0x1C3550},
    .off = {0x3A3A3A, 0xC8C8C8, 0x3A3A3A},
    .needs_download = {0x2B2B2B, 0xFF99A4, 0x6B2B30},
    .problem = {0x442726, 0xFF99A4, 0x442726},
    .banner_info = {0x1C3550, 0x8AC7FF, 0x1C3550},
    .banner_error = {0x442726, 0xFF99A4, 0x442726},
    .chip_default = {0x1C3550, 0x8AC7FF, 0x1C3550},
};

double channel(std::uint8_t v) {
    const double c = static_cast<double>(v) / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(Rgb c) {
    return 0.2126 * channel(red(c)) + 0.7152 * channel(green(c)) + 0.0722 * channel(blue(c));
}

}  // namespace

const Palette& palette(bool dark) { return dark ? kDark : kLight; }

const StateColors& state_colors(const Palette& p, RowState state) {
    switch (state) {
        case RowState::On:
            return p.on;
        case RowState::Asleep:
            return p.asleep;
        case RowState::Waking:
            return p.waking;
        case RowState::Off:
            return p.off;
        case RowState::NeedsDownload:
            return p.needs_download;
        case RowState::Problem:
            break;
    }
    return p.problem;
}

double contrast(Rgb a, Rgb b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

}  // namespace floserver::ui

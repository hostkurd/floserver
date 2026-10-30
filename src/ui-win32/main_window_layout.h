#pragma once

// Sizes shared by the window's source files, in device-independent pixels,
// from the mockups, and small RECT helpers.

#include <windows.h>

namespace floserver::ui::layout {

// Sizes in device-independent pixels, from the mockups. The window opens at
// the mockups' size when the screen has room for it, else smaller.
inline constexpr int kWindowW = 1120;
inline constexpr int kWindowH = 720;
inline constexpr int kSmallWindowW = 960;
inline constexpr int kSmallWindowH = 640;
inline constexpr int kMinW = 560;
inline constexpr int kMinH = 380;
inline constexpr int kTopBarH = 56;
inline constexpr int kPad = 20;
inline constexpr int kGap = 12;
inline constexpr int kControlH = 32;
inline constexpr int kSearchW = 240;
inline constexpr int kSearchMinLeft = 170;
inline constexpr int kBannerH = 32;
inline constexpr int kFooterH = 28;
inline constexpr int kRowH = 60;
inline constexpr int kRowPad = 16;
inline constexpr int kSwitchW = 40;
inline constexpr int kSwitchH = 20;
inline constexpr int kChipH = 20;
inline constexpr int kStateH = 22;
inline constexpr int kMinTextW = 140;

// The project panel (step 5b).
inline constexpr int kPanelW = 360;
// Narrower than this, the panel takes the list's place instead of sitting beside it.
inline constexpr int kPanelBesideMinW = 860;
inline constexpr int kPanelPad = 16;
inline constexpr int kPanelGap = 14;

inline bool contains(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
inline int height_of(const RECT& r) { return static_cast<int>(r.bottom - r.top); }
inline int width_of(const RECT& r) { return static_cast<int>(r.right - r.left); }

}  // namespace floserver::ui::layout

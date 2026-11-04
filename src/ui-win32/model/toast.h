#pragma once

// Messages as toasts (approved 4 Nov): a card at the bottom centre of the
// window, over the content, so nothing moves. Information goes by itself
// after a few seconds, but not while the mouse is over it; an error stays
// until it's closed. A new message takes the old one's place. Plain C++ so
// the rules are tested on every platform; the windows draw it and run the
// one timer, only while an information toast is shown.

#include <chrono>
#include <cstdint>
#include <string>

namespace floserver::ui {

enum class ToastKind : std::uint8_t { Info, Error };

// How long information stays once the mouse isn't over it.
inline constexpr std::chrono::milliseconds kToastInfoTime{4000};
// At most this share of the window's width, and this many lines.
inline constexpr int kToastWidthPercent = 60;
inline constexpr int kToastMaxLines = 3;

struct Toast {
    std::string text;  // empty: none shown
    ToastKind kind = ToastKind::Info;
    bool hovered = false;  // the mouse is over it
    bool operator==(const Toast&) const = default;
};

// What happened to it.
enum class ToastEvent : std::uint8_t {
    MouseEnter,
    MouseLeave,
    TimeUp,  // the timer went off
    Close,   // its close button, or Esc
};

// The toast after `event`.
[[nodiscard]] Toast toast_after(const Toast& toast, ToastEvent event);
// A new message: it replaces whatever was shown (the mouse may already be over it).
[[nodiscard]] Toast toast_with(const Toast& shown, std::string text, ToastKind kind);
// Whether the timer runs now: information, shown, and the mouse not over it.
[[nodiscard]] bool toast_timer_runs(const Toast& toast);
// The card's widest, for a window `window_w` wide; never under `least`.
[[nodiscard]] int toast_max_width(int window_w, int least);

}  // namespace floserver::ui

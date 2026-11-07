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
    // A button on it ("Show in folder", D22); empty: none. A toast with one
    // stays until it's closed, like an error, so there's time to press it.
    std::string action;
    // New mail (D27, approved 7 Nov): `text` is its bold first line, `detail`
    // the second ("to customer@example.com"); `mail` gives it an envelope at
    // its start and a primary (filled) button.
    std::string detail;
    bool mail = false;
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
// New mail: the same, with the mail look (stays until it's closed: it has a button).
[[nodiscard]] Toast toast_mail(const Toast& shown, std::string title, std::string detail, std::string action);
// A new message: it replaces whatever was shown (the mouse may already be over it).
[[nodiscard]] Toast toast_with(const Toast& shown, std::string text, ToastKind kind, std::string action = {});
// Whether the timer runs now: information, shown, and the mouse not over it.
[[nodiscard]] bool toast_timer_runs(const Toast& toast);
// The card's widest, for a window `window_w` wide; never under `least`.
[[nodiscard]] int toast_max_width(int window_w, int least);

}  // namespace floserver::ui

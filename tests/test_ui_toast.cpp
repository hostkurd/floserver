// Messages as toasts (src/ui-win32/model/toast.h, approved 4 Nov).

#include "model/toast.h"

#include <doctest/doctest.h>

using namespace floserver::ui;

TEST_CASE("toast: one with a button stays until it's closed") {
    Toast t = toast_with({}, "Backed up my-shop", ToastKind::Info, "Show in folder");
    CHECK(t.action == "Show in folder");
    CHECK_FALSE(toast_timer_runs(t));
    CHECK(toast_after(t, ToastEvent::TimeUp) == t);
    CHECK(toast_after(t, ToastEvent::Close).text.empty());
    // The next message brings its own button, or none.
    CHECK(toast_with(t, "The address is on the clipboard", ToastKind::Info).action.empty());
}

TEST_CASE("toast: information goes by itself, but not while it's read") {
    Toast t = toast_with({}, "The address is on the clipboard", ToastKind::Info);
    CHECK(t.text == "The address is on the clipboard");
    CHECK(toast_timer_runs(t));
    t = toast_after(t, ToastEvent::MouseEnter);
    CHECK(t.hovered);
    CHECK_FALSE(toast_timer_runs(t));  // no timer while the mouse is over it
    CHECK(toast_after(t, ToastEvent::TimeUp).text == t.text);
    t = toast_after(t, ToastEvent::MouseLeave);
    CHECK(toast_timer_runs(t));  // started again on the way out
    t = toast_after(t, ToastEvent::TimeUp);
    CHECK(t.text.empty());
    CHECK_FALSE(toast_timer_runs(t));  // and none once it's gone
}

TEST_CASE("toast: an error stays until it's closed") {
    Toast t = toast_with({}, "Couldn't change shop: no answer", ToastKind::Error);
    CHECK_FALSE(toast_timer_runs(t));
    t = toast_after(t, ToastEvent::TimeUp);
    CHECK(t.kind == ToastKind::Error);
    CHECK_FALSE(t.text.empty());
    t = toast_after(toast_after(t, ToastEvent::MouseEnter), ToastEvent::MouseLeave);
    CHECK_FALSE(t.text.empty());
    CHECK_FALSE(toast_timer_runs(t));
    CHECK(toast_after(t, ToastEvent::Close) == Toast{});
}

TEST_CASE("toast: a new message replaces the old one") {
    Toast t = toast_with({}, "first", ToastKind::Error);
    t = toast_with(t, "second", ToastKind::Info);
    CHECK(t.text == "second");
    CHECK(t.kind == ToastKind::Info);
    CHECK(toast_timer_runs(t));
    // The mouse still over the card: the new one waits for it to leave.
    t = toast_with(toast_after(t, ToastEvent::MouseEnter), "third", ToastKind::Info);
    CHECK(t.hovered);
    CHECK_FALSE(toast_timer_runs(t));
    // Nothing to say keeps what's shown; nothing shown can't be hovered.
    CHECK(toast_with(t, "", ToastKind::Error) == t);
    CHECK_FALSE(toast_after(Toast{}, ToastEvent::MouseEnter).hovered);
}

TEST_CASE("toast: its width") {
    CHECK(toast_max_width(1000, 200) == 600);
    CHECK(toast_max_width(200, 180) == 180);
    CHECK(kToastInfoTime.count() == 4000);
}

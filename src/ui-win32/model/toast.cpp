#include "model/toast.h"

#include <algorithm>
#include <utility>

namespace floserver::ui {

Toast toast_after(const Toast& toast, ToastEvent event) {
    Toast out = toast;
    switch (event) {
        case ToastEvent::MouseEnter:
            out.hovered = !out.text.empty();
            break;
        case ToastEvent::MouseLeave:
            out.hovered = false;
            break;
        case ToastEvent::TimeUp:
            // An error never goes by itself; nor anything while it's being read.
            if (out.kind == ToastKind::Info && !out.hovered) {
                out = Toast{};
            }
            break;
        case ToastEvent::Close:
            out = Toast{};
            break;
    }
    return out;
}

Toast toast_with(const Toast& shown, std::string text, ToastKind kind) {
    if (text.empty()) {
        return shown;  // nothing to say: what's shown stays
    }
    return Toast{std::move(text), kind, shown.hovered && !shown.text.empty()};
}

bool toast_timer_runs(const Toast& toast) { return !toast.text.empty() && toast.kind == ToastKind::Info && !toast.hovered; }

int toast_max_width(int window_w, int least) { return std::max(least, window_w * kToastWidthPercent / 100); }

}  // namespace floserver::ui

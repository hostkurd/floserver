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
            if (out.kind == ToastKind::Info && !out.hovered && out.action.empty()) {
                out = Toast{};
            }
            break;
        case ToastEvent::Close:
            out = Toast{};
            break;
    }
    return out;
}

Toast toast_with(const Toast& shown, std::string text, ToastKind kind, std::string action) {
    if (text.empty()) {
        return shown;  // nothing to say: what's shown stays
    }
    return Toast{std::move(text), kind, shown.hovered && !shown.text.empty(), std::move(action), {}, false};
}

Toast toast_mail(const Toast& shown, std::string title, std::string detail, std::string action) {
    if (title.empty()) {
        return shown;
    }
    Toast out{std::move(title), ToastKind::Info, shown.hovered && !shown.text.empty(), std::move(action), {}, true};
    out.detail = std::move(detail);
    return out;
}

bool toast_timer_runs(const Toast& toast) {
    return !toast.text.empty() && toast.kind == ToastKind::Info && !toast.hovered && toast.action.empty();
}

int toast_max_width(int window_w, int least) { return std::max(least, window_w * kToastWidthPercent / 100); }

}  // namespace floserver::ui

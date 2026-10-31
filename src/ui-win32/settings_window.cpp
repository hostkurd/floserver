#include "settings_window.h"

#include "main_window_layout.h"
#include "model/theme.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <string_view>
#include <utility>

namespace floserver::ui {

using layout::contains;
using layout::height_of;
using layout::width_of;
using nlohmann::json;

namespace {

constexpr wchar_t kClassName[] = L"FloServerSettings";
constexpr UINT_PTR kMessageTimer = 1;
constexpr UINT kMessageShownMs = 8000;
constexpr UINT_PTR kTipId = 1;
constexpr UINT kFirstChoice = 100;

// Sizes in device-independent pixels, from the mockup (SettingsRuntimes).
constexpr int kOpenW = 1040;
constexpr int kOpenH = 720;
constexpr int kSmallOpenW = 860;
constexpr int kSmallOpenH = 600;
constexpr int kMinW = 560;
constexpr int kMinH = 400;
// No wider or taller than this, and no Maximize: a page of settings
// stretched across a big screen is hard to read.
constexpr int kMaxW = 1200;
constexpr int kMaxH = 1000;
constexpr int kSidebarW = 220;
constexpr int kNarrowSidebarW = 168;  // below kWideFromW
constexpr int kWideFromW = 760;
constexpr int kNavH = 36;
constexpr int kHeadH = 44;   // a group's title line
constexpr int kRowH = 48;
constexpr int kButtonH = 30;
constexpr int kPickerH = 28;
constexpr int kChipH = 20;
constexpr int kVersionW = 110;
constexpr int kBarW = 140;
constexpr int kGridMinW = 240;  // a Node.js, Python or Web servers card
constexpr int kGap = 12;
constexpr int kRowPad = 16;

std::wstring w(std::string_view s) { return win::widen(s); }

// The height of `s` broken into lines `width` wide, as Canvas::paragraph draws it.
int text_height(HDC dc, std::wstring_view s, HFONT font, int width, bool rtl) {
    if (s.empty() || width <= 0) {
        return 0;
    }
    const win::Selected selected(dc, font);
    RECT r{0, 0, width, 0};
    DrawTextW(dc, s.data(), static_cast<int>(s.size()), &r,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | (rtl ? UINT{DT_RTLREADING} : 0U));
    return static_cast<int>(r.bottom - r.top);
}

bool is_empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

}  // namespace

SettingsWindow::SettingsWindow(HINSTANCE instance, Lang lang, ServiceWorker& worker, HWND owner, UINT closed_message)
    : instance_(instance), lang_(lang), rtl_(is_rtl(lang)), worker_(worker), owner_(owner),
      closed_message_(closed_message) {}

SettingsWindow::~SettingsWindow() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
    }
}

bool SettingsWindow::create(const std::optional<WINDOWPLACEMENT>& placement) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof existing;
    if (GetClassInfoExW(instance_, kClassName, &existing) == FALSE) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = window_proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (RegisterClassExW(&wc) == 0) {
            return false;
        }
    }
    const auto system_dpi = static_cast<int>(GetDpiForSystem());
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const bool roomy = MulDiv(kOpenW + 40, system_dpi, 96) <= width_of(work) &&
                       MulDiv(kOpenH + 40, system_dpi, 96) <= height_of(work);
    const int open_w = roomy ? kOpenW : kSmallOpenW;
    const int open_h = roomy ? kOpenH : kSmallOpenH;
    const std::wstring title = w(text(lang_, Str::SettingsTitle));
    // No owner: its own taskbar button, and it stays when the main window hides.
    if (CreateWindowExW(rtl_ ? WS_EX_LAYOUTRTL : 0, kClassName, title.c_str(),
                        (WS_OVERLAPPEDWINDOW & ~static_cast<DWORD>(WS_MAXIMIZEBOX)) | WS_CLIPCHILDREN,
                        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(open_w, system_dpi, 96), MulDiv(open_h, system_dpi, 96),
                        nullptr, nullptr, instance_, this) == nullptr) {
        return false;
    }
    if (placement) {
        // Where it was last time it closed (while FloServer has been running).
        WINDOWPLACEMENT wp = *placement;
        wp.length = sizeof wp;
        wp.flags = 0;
        wp.showCmd = SW_SHOWNORMAL;
        SetWindowPlacement(hwnd_, &wp);
    } else {
        // Centred over FloServer's window (Windows would put it at the
        // screen's corner), kept inside that screen's work area.
        RECT anchor{};
        const HMONITOR monitor = MonitorFromWindow(owner_ != nullptr ? owner_ : hwnd_, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(monitor, &mi);
        const RECT area = mi.rcWork;
        if (owner_ == nullptr || IsWindowVisible(owner_) == FALSE || IsIconic(owner_) != FALSE ||
            GetWindowRect(owner_, &anchor) == FALSE) {
            anchor = area;
        }
        const int ww = std::min(px(open_w), width_of(area));
        const int wh = std::min(px(open_h), height_of(area));
        int x = static_cast<int>(anchor.left) + (width_of(anchor) - ww) / 2;
        int y = static_cast<int>(anchor.top) + (height_of(anchor) - wh) / 2;
        x = std::clamp(x, static_cast<int>(area.left), static_cast<int>(area.right) - ww);
        y = std::clamp(y, static_cast<int>(area.top), static_cast<int>(area.bottom) - wh);
        SetWindowPos(hwnd_, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
    }
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    // The one request opening makes: the list in hand or on disk, no network.
    worker_.watch_runtimes(true);
    worker_.list_runtimes(false);
    return true;
}

void SettingsWindow::bring_to_front() {
    if (hwnd_ == nullptr) {
        return;
    }
    ShowWindow(hwnd_, IsIconic(hwnd_) != FALSE ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

LRESULT CALLBACK SettingsWindow::window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = win::int_to_ptr<const CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = win::int_to_ptr<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (msg == WM_NCDESTROY) {
        // The owner lets go of this object once the message arrives.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        PostMessageW(self->owner_, self->closed_message_, 0, 0);
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return self->handle(msg, wparam, lparam);
}

LRESULT SettingsWindow::handle(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            on_create();
            return 0;
        case WM_DESTROY:
            on_destroy();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            on_paint();
            return 0;
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                width_ = LOWORD(lparam);
                height_ = HIWORD(lparam);
                relayout();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = win::int_to_ptr<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {px(kMinW), px(kMinH)};
            info->ptMaxTrackSize = {px(kMaxW), px(kMaxH)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = LOWORD(wparam);
            make_ui_fonts(fonts_, dpi_, lang_);
            const RECT& r = *win::int_to_ptr<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            relayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lparam != 0 && std::wcscmp(win::int_to_ptr<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
                if (const bool dark = apps_use_dark_theme(); dark != dark_) {
                    dark_ = dark;
                    apply_theme();
                }
            } else if (wparam == SPI_SETNONCLIENTMETRICS) {
                make_ui_fonts(fonts_, dpi_, lang_);
                relayout();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE:
            on_mouse_move(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            if (hover_.part != Part::None) {
                hover_ = {};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            pressed_ = hit_test(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            SetCapture(hwnd_);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const Hit hit = hit_test(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (hit == pressed_ && hit.part != Part::None) {
                // The keys carry on from what was clicked.
                const std::vector<Hit> order = focus_order();
                if (const auto it = std::find(order.begin(), order.end(), hit); it != order.end()) {
                    focus_ = static_cast<int>(it - order.begin());
                }
                show_focus_ = false;
                on_click(hit);
            }
            pressed_ = {};
            return 0;
        }
        case WM_MOUSEWHEEL:
            scroll_to(scroll_ - GET_WHEEL_DELTA_WPARAM(wparam) * px(kRowH) / WHEEL_DELTA);
            return 0;
        case WM_KEYDOWN:
            if ((wparam == VK_SPACE || wparam == VK_RETURN) && (static_cast<std::uintptr_t>(lparam) & (1U << 30U)) != 0) {
                return 0;  // a held key would send a request per repeat
            }
            on_key(wparam);
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wparam == kMessageTimer) {
                KillTimer(hwnd_, kMessageTimer);
                message_.clear();
                relayout();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

// --- lifetime -------------------------------------------------------------

void SettingsWindow::on_create() {
    dpi_ = GetDpiForWindow(hwnd_);
    dark_ = apps_use_dark_theme();
    RECT client{};
    GetClientRect(hwnd_, &client);
    width_ = client.right;
    height_ = client.bottom;
    make_ui_fonts(fonts_, dpi_, lang_);

    win::UniqueIcon small_icon(make_app_icon(GetSystemMetricsForDpi(SM_CXSMICON, dpi_)));
    win::UniqueIcon big_icon(make_app_icon(GetSystemMetricsForDpi(SM_CXICON, dpi_)));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon.get()));
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon.get()));
    icon_small_ = std::move(small_icon);
    icon_big_ = std::move(big_icon);

    // One tool, moved to the disabled Remove under the mouse: "Used by …".
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance_,
                               nullptr);
    TOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_SUBCLASS | (rtl_ ? TTF_RTLREADING : 0U);
    ti.hwnd = hwnd_;
    ti.uId = kTipId;
    ti.lpszText = tip_text_.data();
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, px(360));

    apply_theme();
    rebuild();
}

void SettingsWindow::on_destroy() {
    KillTimer(hwnd_, kMessageTimer);
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    if (GetWindowPlacement(hwnd_, &wp) != FALSE) {
        placement_ = wp;
    }
    // Nothing is asked for Settings once it's gone.
    worker_.watch_runtimes(false);
}

void SettingsWindow::apply_theme() {
    apply_title_bar(hwnd_, dark_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int SettingsWindow::px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }
float SettingsWindow::pxf(float dip) const { return dip * static_cast<float>(dpi_) / 96.0F; }

// --- what the service said --------------------------------------------------------

void SettingsWindow::on_snapshot(const Snapshot& snap) {
    if (hwnd_ == nullptr) {
        return;
    }
    const bool was_connected = connected_;
    connected_ = snap.link == Link::Connected;
    installed_ = snap.installed;
    has_installed_ = snap.has_installed;
    databases_ = snap.databases;
    jobs_ = snap.runtime_jobs;
    for (const std::string& key : snap.settled) {
        starting_.erase(key);
        removing_.erase(key);
        if (key.starts_with("default\n")) {
            pending_default_.erase(key.substr(8));
        }
    }
    const auto service_words = [this](const std::string& s) { return rtl_ ? isolate_ltr(s) : s; };
    if (snap.runtimes) {
        const json& fresh = *snap.runtimes;
        if (snap.runtimes_refresh) {
            checking_ = false;
            const bool listed = fresh.is_object() && fresh.contains("list") && fresh["list"].is_object();
            if (!listed && fresh.is_object() && fresh.contains("list_error") && fresh["list_error"].is_string()) {
                show_message(fill_pattern(text(lang_, Str::SettingsCheckFailed), false,
                                          {service_words(fresh["list_error"].get<std::string>())}),
                             true);
            }
        }
        reply_ = merge_runtimes_reply(reply_, fresh);
        has_reply_ = true;
        loading_ = false;
    } else if (!snap.runtimes_error.empty()) {
        if (snap.runtimes_refresh) {
            checking_ = false;
            show_message(fill_pattern(text(lang_, Str::SettingsCheckFailed), false, {service_words(snap.runtimes_error)}),
                         true);
        } else {
            loading_ = false;
            show_message(fill_pattern(text(lang_, Str::SettingsReadFailed), false, {service_words(snap.runtimes_error)}),
                         true);
        }
    }
    if (!snap.settings_error.empty()) {
        show_message(snap.settings_error, true);
    }
    // The service came back before Settings had its answer: asked once more.
    if (!was_connected && connected_ && !has_reply_ && !loading_) {
        loading_ = true;
        worker_.list_runtimes(false);
    }
    rebuild();
}

void SettingsWindow::rebuild() {
    page_ = has_reply_ ? runtimes_page(reply_, has_installed_ ? &installed_ : nullptr, databases_, lang_) : RuntimesPage{};
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::show_message(std::string message, bool error) {
    message_ = std::move(message);
    message_error_ = error;
    SetTimer(hwnd_, kMessageTimer, kMessageShownMs, nullptr);  // only while there's a message to clear
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- geometry (logical coordinates: x from the reading start) ---------------------

int SettingsWindow::main_left() const {
    return px(width_ >= px(kWideFromW) ? kSidebarW : kNarrowSidebarW) + px(8);
}

SettingsWindow::Geometry SettingsWindow::layout(HDC dc) const {
    Geometry g;
    const int side = main_left() - px(8);
    g.nav = {px(8), px(8), side - px(8), px(8) + px(kNavH)};

    const int x0 = main_left();
    const int x1 = std::max(x0 + px(200), width_ - px(24));
    int y = px(4) - scroll_;

    // The title and what the page does, with Check for updates beside them.
    const int check_w = std::max(Canvas::measure(dc, w(text(lang_, Str::SettingsCheckUpdates)), fonts_.button.get(), rtl_),
                                 Canvas::measure(dc, w(text(lang_, Str::SettingsChecking)), fonts_.button.get(), rtl_)) +
                        px(24);
    const int hint_w = std::max(px(100), x1 - x0 - check_w - px(kGap));
    const std::string hint = has_reply_ ? page_.hint : std::string(text(lang_, Str::SettingsRuntimesHint));
    const int hint_h = text_height(dc, w(hint), fonts_.caption.get(), hint_w, rtl_);
    g.title = {x0, y, x0 + hint_w, y + px(28)};
    g.hint = {x0, g.title.bottom, x0 + hint_w, g.title.bottom + hint_h};
    const int check_top = std::max(y, static_cast<int>(g.hint.bottom) - px(kButtonH));
    g.check = {x1 - check_w, check_top, x1, check_top + px(kButtonH)};
    y = std::max(g.hint.bottom, g.check.bottom) + px(kGap);

    if (!message_.empty()) {
        const int mh = std::max(px(32), text_height(dc, w(message_), fonts_.button.get(), x1 - x0 - px(24), rtl_) + px(14));
        g.message = {x0, y, x1, y + mh};
        y += mh + px(kGap);
    }
    if (!has_reply_) {
        g.note = {x0, y, x1, y + px(24)};
        y += px(24) + px(kGap);
    }

    // The rows of one group inside its card.
    const auto make_rows = [&](GroupGeometry& gg, const RuntimeGroup& group, bool wide) {
        const RECT card = gg.card;
        gg.title = {card.left + px(kRowPad), card.top, card.right - px(kRowPad), card.top + px(kHeadH)};
        if (group.picker) {
            int pw = px(120);
            for (const Choice& choice : group.picker->choices) {
                pw = std::max(pw, Canvas::measure(dc, w(choice.label), fonts_.button.get(), rtl_) + px(34));
            }
            const int mid = card.top + px(kHeadH) / 2;
            const int title_w = Canvas::measure(dc, w(group.title), fonts_.body_bold.get(), rtl_) + px(kGap);
            pw = std::min(pw, std::max(px(80), width_of(card) - 2 * px(kRowPad) - title_w));
            gg.picker = {card.right - px(kRowPad) - pw, mid - px(kPickerH) / 2, card.right - px(kRowPad),
                         mid + px(kPickerH) / 2};
            const int lw = Canvas::measure(dc, w(text(lang_, Str::SettingsDefaultFor)), fonts_.caption.get(), rtl_);
            const int label_left = static_cast<int>(gg.picker.left) - px(8) - lw;
            if (label_left >= card.left + px(kRowPad) + title_w) {  // left out when there's no room
                gg.picker_label = {label_left, card.top, static_cast<int>(gg.picker.left) - px(8), card.top + px(kHeadH)};
            }
        }
        int ry = card.top + px(kHeadH);
        for (const RuntimeRow& row : group.rows) {
            RowGeometry rg;
            rg.row = {card.left, ry, card.right, ry + px(kRowH)};
            const int mid = ry + px(kRowH) / 2;
            int x = card.left + px(kRowPad);
            const int right = card.right - px(kRowPad);

            // What it shows at its end: a running install, what was just
            // asked for, or the row's own button.
            const RuntimeJob* job = job_for(row, jobs_);
            const std::string starting_key = ServiceWorker::install_key(settings_install_key(row.name, row.version));
            const std::string removing_key = ServiceWorker::remove_key(row.name, row.folder);
            Str label = Str::SettingsRemove;
            if (job != nullptr) {
                rg.button_kind = Button::Cancel;
                label = Str::WizardCancel;
                rg.line = job_text(*job, lang_);
                rg.fraction = job_fraction(*job).value_or(-1.0);
            } else if (!row.installed && starting_.contains(starting_key)) {
                rg.line = std::string(text(lang_, Str::SettingsStarting));
            } else if (row.installed && removing_.contains(removing_key)) {
                rg.line = std::string(text(lang_, Str::SettingsRemoving));
            } else {
                if (row.action == RowAction::Remove) {
                    rg.button_kind = row.used_by.empty() ? Button::Remove : Button::RemoveDisabled;
                } else if (row.action == RowAction::Download && connected_) {
                    rg.button_kind = Button::Download;
                    label = Str::SettingsDownload;
                }
                if (!row.note.empty()) {
                    rg.line = row.note;
                    rg.line_code = true;
                } else if (wide && row.installed) {
                    rg.line = row.usage;
                } else if (wide && row.size > 0) {
                    rg.line = size_text(row.size);
                    rg.line_code = true;
                }
            }
            if (rg.button_kind != Button::None) {
                const int bw = Canvas::measure(dc, w(text(lang_, label)), fonts_.button.get(), rtl_) + px(24);
                rg.button = {right - bw, mid - px(kButtonH) / 2, right, mid + px(kButtonH) / 2};
            }
            const int end = (rg.button_kind != Button::None ? static_cast<int>(rg.button.left) : right) - px(kGap);

            const int vw = wide ? px(kVersionW) : Canvas::measure(dc, w(row.label), fonts_.body.get(), false) + px(2);
            rg.version = {x, mid - px(10), std::min(end, x + vw), mid + px(10)};
            x = static_cast<int>(rg.version.right) + px(kGap);

            const bool chip = row.always_used || (row.installed && (row.is_default || wide));
            if (chip) {
                rg.chip_text = row.always_used ? Str::SettingsAlwaysUsed
                               : row.is_default ? Str::SettingsDefaultChip
                                                : Str::SettingsInstalledChip;
                const int cw = Canvas::measure(dc, w(text(lang_, rg.chip_text)),
                                               rg.chip_text == Str::SettingsDefaultChip ? fonts_.small_bold.get()
                                                                                         : fonts_.caption.get(),
                                               rtl_) +
                               px(16);
                if (x + cw <= end) {
                    rg.chip = {x, mid - px(kChipH) / 2, x + cw, mid + px(kChipH) / 2};
                    x += cw + px(kGap);
                }
            }
            if (!rg.line.empty() && x < end) {
                int text_right = end;
                if (rg.fraction >= 0) {
                    const int bar_w = std::min(px(kBarW), (end - x) / 2);
                    const int tw = Canvas::measure(dc, w(rg.line), fonts_.caption.get(), rtl_) + px(2);
                    text_right = std::min(end - bar_w - px(kGap), x + tw);
                    rg.bar = {text_right + px(kGap), mid - px(2), text_right + px(kGap) + bar_w, mid + px(2)};
                }
                rg.text = {x, mid - px(10), std::max(x, text_right), mid + px(10)};
            }
            gg.rows.push_back(std::move(rg));
            ry += px(kRowH);
        }
    };
    const auto card_h = [&](const RuntimeGroup& group) {
        return px(kHeadH) + static_cast<int>(group.rows.size()) * px(kRowH);
    };

    // PHP and MariaDB take the full width; the rest share a row of cards.
    std::vector<std::size_t> grid;
    for (std::size_t i = 0; i < page_.groups.size(); ++i) {
        const RuntimeGroup& group = page_.groups[i];
        GroupGeometry gg;
        if (group.wide) {
            gg.card = {x0, y, x1, y + card_h(group)};
            make_rows(gg, group, true);
            y = static_cast<int>(gg.card.bottom) + px(kGap);
        } else {
            grid.push_back(i);
        }
        g.groups.push_back(std::move(gg));
    }
    if (!grid.empty()) {
        const int avail = x1 - x0;
        const auto fits = [&](int n) { return avail >= n * px(kGridMinW) + (n - 1) * px(kGap); };
        const int cols = std::min(static_cast<int>(grid.size()), fits(3) ? 3 : fits(2) ? 2 : 1);
        const int col_w = (avail - (cols - 1) * px(kGap)) / cols;
        for (std::size_t k = 0; k < grid.size(); k += static_cast<std::size_t>(cols)) {
            int line_h = 0;
            for (std::size_t j = k; j < std::min(grid.size(), k + static_cast<std::size_t>(cols)); ++j) {
                line_h = std::max(line_h, card_h(page_.groups[grid[j]]));
            }
            for (std::size_t j = k; j < std::min(grid.size(), k + static_cast<std::size_t>(cols)); ++j) {
                const int left = x0 + static_cast<int>(j - k) * (col_w + px(kGap));
                GroupGeometry& gg = g.groups[grid[j]];
                gg.card = {left, y, left + col_w, y + line_h};
                make_rows(gg, page_.groups[grid[j]], false);
            }
            y += line_h + px(kGap);
        }
    }
    g.content_h = y + scroll_ + px(8);
    return g;
}

void SettingsWindow::relayout() {
    if (hwnd_ == nullptr || !fonts_.button) {
        return;
    }
    HDC dc = GetDC(hwnd_);
    geometry_ = layout(dc);
    const int most = std::max(0, geometry_.content_h - height_);
    if (scroll_ > most) {
        scroll_ = most;
        geometry_ = layout(dc);
    }
    ReleaseDC(hwnd_, dc);
    const int count = static_cast<int>(focus_order().size());
    focus_ = std::min(focus_, count - 1);
    update_tip(hover_);
}

void SettingsWindow::scroll_to(int target) {
    const int most = std::max(0, geometry_.content_h - height_);
    const int value = std::clamp(target, 0, most);
    if (value != scroll_) {
        scroll_ = value;
        relayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void SettingsWindow::ensure_visible(const Hit& hit) {
    const RECT* r = rect_of(hit);
    if (r == nullptr || hit.part == Part::Nav) {
        return;
    }
    // The whole row, so a button isn't shown cut at the edge.
    RECT want = *r;
    if (hit.part == Part::Button) {
        want = geometry_.groups[static_cast<std::size_t>(hit.group)].rows[static_cast<std::size_t>(hit.row)].row;
    }
    if (want.top < px(8)) {
        scroll_to(scroll_ + static_cast<int>(want.top) - px(8));
    } else if (want.bottom > height_ - px(8)) {
        scroll_to(scroll_ + static_cast<int>(want.bottom) - height_ + px(8));
    }
}

const RECT* SettingsWindow::rect_of(const Hit& hit) const {
    switch (hit.part) {
        case Part::Nav:
            return &geometry_.nav;
        case Part::Check:
            return &geometry_.check;
        case Part::Picker:
            if (hit.group >= 0 && std::cmp_less(hit.group, geometry_.groups.size())) {
                return &geometry_.groups[static_cast<std::size_t>(hit.group)].picker;
            }
            return nullptr;
        case Part::Button:
            if (hit.group >= 0 && std::cmp_less(hit.group, geometry_.groups.size())) {
                const GroupGeometry& gg = geometry_.groups[static_cast<std::size_t>(hit.group)];
                if (hit.row >= 0 && std::cmp_less(hit.row, gg.rows.size())) {
                    return &gg.rows[static_cast<std::size_t>(hit.row)].button;
                }
            }
            return nullptr;
        case Part::None:
            break;
    }
    return nullptr;
}

std::vector<SettingsWindow::Hit> SettingsWindow::focus_order() const {
    std::vector<Hit> order{{Part::Nav, -1, -1}, {Part::Check, -1, -1}};
    for (std::size_t i = 0; i < geometry_.groups.size(); ++i) {
        const GroupGeometry& gg = geometry_.groups[i];
        if (!is_empty(gg.picker)) {
            order.push_back({Part::Picker, static_cast<int>(i), -1});
        }
        for (std::size_t r = 0; r < gg.rows.size(); ++r) {
            // A disabled Remove too: Enter says who uses it.
            if (gg.rows[r].button_kind != Button::None) {
                order.push_back({Part::Button, static_cast<int>(i), static_cast<int>(r)});
            }
        }
    }
    return order;
}

SettingsWindow::Hit SettingsWindow::hit_test(int x, int y) const {
    for (const Hit& hit : focus_order()) {
        if (const RECT* r = rect_of(hit); r != nullptr && contains(*r, x, y)) {
            return hit;
        }
    }
    return {};
}

const RuntimeRow* SettingsWindow::row_of(const Hit& hit) const {
    if (hit.group < 0 || std::cmp_greater_equal(hit.group, page_.groups.size())) {
        return nullptr;
    }
    const RuntimeGroup& group = page_.groups[static_cast<std::size_t>(hit.group)];
    if (hit.row < 0 || std::cmp_greater_equal(hit.row, group.rows.size())) {
        return nullptr;
    }
    return &group.rows[static_cast<std::size_t>(hit.row)];
}

// --- input ----------------------------------------------------------------

void SettingsWindow::on_mouse_move(int x, int y) {
    if (!tracking_mouse_) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof tme;
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        tracking_mouse_ = TrackMouseEvent(&tme) != FALSE;
    }
    const Hit hit = hit_test(x, y);
    if (!(hit == hover_)) {
        hover_ = hit;
        update_tip(hit);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void SettingsWindow::update_tip(const Hit& hit) {
    if (tooltip_ == nullptr) {
        return;
    }
    RECT rect{};
    std::wstring tip;
    if (hit.part == Part::Button) {
        const RuntimeRow* row = row_of(hit);
        const RECT* r = rect_of(hit);
        if (row != nullptr && r != nullptr &&
            geometry_.groups[static_cast<std::size_t>(hit.group)].rows[static_cast<std::size_t>(hit.row)].button_kind ==
                Button::RemoveDisabled) {
            rect = *r;
            tip = w(fill_pattern(text(lang_, Str::SettingsUsedBy), false, {row->used_by}));
        }
    }
    if (tip != tip_text_) {
        tip_text_ = tip;
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.hwnd = hwnd_;
        ti.uId = kTipId;
        ti.lpszText = tip_text_.data();
        SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    TOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.hwnd = hwnd_;
    ti.uId = kTipId;
    ti.rect = rect;
    SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
}

void SettingsWindow::on_click(const Hit& hit) {
    switch (hit.part) {
        case Part::Check:
            check_for_updates();
            break;
        case Part::Picker:
            picker_menu(hit.group);
            break;
        case Part::Button:
            run_button(hit);
            break;
        case Part::Nav:  // the only section so far
        case Part::None:
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::on_key(WPARAM key) {
    const std::vector<Hit> order = focus_order();
    const int count = static_cast<int>(order.size());
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int next = focus_;
    switch (key) {
        case VK_TAB:
            next = focus_ < 0 ? (shift ? count - 1 : 0) : (focus_ + (shift ? count - 1 : 1)) % count;
            break;
        case VK_DOWN:
            next = std::min(count - 1, focus_ + 1);
            break;
        case VK_UP:
            next = std::max(0, focus_ - 1);
            break;
        case VK_RETURN:
        case VK_SPACE:
            if (focus_ >= 0 && focus_ < count) {
                on_click(order[static_cast<std::size_t>(focus_)]);
            }
            return;
        case VK_ESCAPE:
            DestroyWindow(hwnd_);
            return;
        case VK_HOME:
            scroll_to(0);
            return;
        case VK_END:
            scroll_to(geometry_.content_h);
            return;
        case VK_NEXT:
            scroll_to(scroll_ + std::max(px(kRowH), height_ - px(kRowH)));
            return;
        case VK_PRIOR:
            scroll_to(scroll_ - std::max(px(kRowH), height_ - px(kRowH)));
            return;
        case VK_F5:
            // Read again what's on this computer (no network).
            if (!loading_) {
                loading_ = true;
                worker_.list_runtimes(false);
            }
            return;
        default:
            return;
    }
    if (count == 0) {
        return;
    }
    focus_ = next;
    show_focus_ = true;
    ensure_visible(order[static_cast<std::size_t>(focus_)]);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::check_for_updates() {
    if (checking_ || !connected_) {
        return;
    }
    checking_ = true;
    message_.clear();
    worker_.list_runtimes(true);
    relayout();
}

std::size_t SettingsWindow::picker_value(const RuntimeGroup& group) const {
    const VersionField& field = *group.picker;
    if (const auto it = pending_default_.find(group.runtime); it != pending_default_.end()) {
        for (std::size_t i = 0; i < field.choices.size(); ++i) {
            if (field.choices[i].value == it->second) {
                return i;
            }
        }
    }
    return field.current;
}

void SettingsWindow::picker_menu(int group) {
    if (group < 0 || std::cmp_greater_equal(group, page_.groups.size()) || !connected_) {
        return;
    }
    const RuntimeGroup g = page_.groups[static_cast<std::size_t>(group)];  // a copy: snapshots keep arriving
    if (!g.picker) {
        return;
    }
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    const std::size_t shown = picker_value(g);
    std::vector<std::wstring> labels;
    labels.reserve(g.picker->choices.size());
    for (std::size_t i = 0; i < g.picker->choices.size(); ++i) {
        labels.push_back(w(g.picker->choices[i].label));
        AppendMenuW(menu.get(), MF_STRING | (i == shown ? UINT{MF_CHECKED} : 0U), kFirstChoice + static_cast<UINT>(i),
                    labels.back().c_str());
    }
    const RECT box = geometry_.groups[static_cast<std::size_t>(group)].picker;
    POINT at{box.left, box.bottom};
    ClientToScreen(hwnd_, &at);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
    flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    if (command < kFirstChoice || command - kFirstChoice >= g.picker->choices.size() || hwnd_ == nullptr) {
        return;
    }
    const std::size_t picked = command - kFirstChoice;
    if (picked == shown) {
        return;
    }
    // Saved at once: there's no Save button. The list is asked again after.
    const std::optional<std::string>& value = g.picker->choices[picked].value;
    pending_default_[g.runtime] = value;
    message_.clear();
    worker_.set_default(g.runtime, value);
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::run_button(const Hit& hit) {
    const RuntimeRow* row = row_of(hit);
    if (row == nullptr || !connected_) {
        return;
    }
    const RowGeometry& rg = geometry_.groups[static_cast<std::size_t>(hit.group)].rows[static_cast<std::size_t>(hit.row)];
    switch (rg.button_kind) {
        case Button::Download: {
            const std::string key = settings_install_key(row->name, row->version);
            starting_.insert(ServiceWorker::install_key(key));
            message_.clear();
            worker_.install(key, {{row->name, row->version}});
            break;
        }
        case Button::Cancel:
            if (const RuntimeJob* job = job_for(*row, jobs_); job != nullptr) {
                worker_.cancel_job(job->id);
            }
            break;
        case Button::Remove:
            removing_.insert(ServiceWorker::remove_key(row->name, row->folder));
            message_.clear();
            worker_.remove_runtime(row->name, row->folder);
            break;
        case Button::RemoveDisabled:
            // The tooltip's words, for the keyboard.
            show_message(fill_pattern(text(lang_, Str::SettingsUsedBy), false, {row->used_by}), false);
            return;
        case Button::None:
            return;
    }
    rebuild();
}

// --- painting -------------------------------------------------------------

void SettingsWindow::on_paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    width_ = client.right;
    height_ = client.bottom;
    if (width_ > 0 && height_ > 0) {
        const win::UniqueDc mem(CreateCompatibleDC(hdc));
        const win::UniqueBitmap bitmap(CreateCompatibleBitmap(hdc, width_, height_));
        if (mem && bitmap) {
            SetLayout(mem.get(), 0);
            const win::Selected selected(mem.get(), bitmap.get());
            {
                Canvas canvas(mem.get(), width_, rtl_);
                canvas.fill({0, 0, width_, height_}, palette(dark_).window);
                paint_shapes(canvas);
                canvas.begin_text();
                paint_text(canvas);
            }
            // The bitmap is already laid out right to left; copy it as it is.
            SetLayout(hdc, 0);
            BitBlt(hdc, 0, 0, width_, height_, mem.get(), 0, 0, SRCCOPY);
        }
    }
    EndPaint(hwnd_, &ps);
}

void SettingsWindow::paint_button(Canvas& c, const RECT& r, bool hover, bool focused, bool enabled) const {
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    if (!enabled) {
        c.round_rect(r, pxf(4), p.button_disabled, p.button_border, border);
    } else {
        c.round_rect(r, pxf(4), hover ? p.row_hover : p.button, p.button_border, border);
    }
    if (focused) {
        paint_ring(c, r);
    }
}

void SettingsWindow::paint_ring(Canvas& c, const RECT& r) const {
    RECT ring = r;
    InflateRect(&ring, px(2), px(2));
    c.round_outline(ring, pxf(6), palette(dark_).focus, pxf(2));
}

void SettingsWindow::paint_shapes(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const float border = std::max(1.0F, pxf(1.0F));
    const bool keys = show_focus_ && GetFocus() == hwnd_;
    const std::vector<Hit> order = focus_order();
    const auto focused = [&](const Hit& hit) {
        return keys && focus_ >= 0 && std::cmp_less(focus_, order.size()) && order[static_cast<std::size_t>(focus_)] == hit;
    };

    // The sidebar: the open section, with the accent mark at its start.
    c.round_rect(g.nav, pxf(4), p.nav_selected);
    c.round_rect({g.nav.left, g.nav.top + px(10), g.nav.left + px(3), g.nav.bottom - px(10)}, pxf(1.5F), p.accent);
    {
        // Runtimes' icon: two stacked boxes, 16 px.
        const int ix = g.nav.left + px(12);
        const int iy = (g.nav.top + g.nav.bottom) / 2 - px(8);
        c.round_outline({ix + px(2), iy + px(2), ix + px(14), iy + px(7)}, pxf(1), p.text, pxf(1.3F));
        c.round_outline({ix + px(2), iy + px(9), ix + px(14), iy + px(14)}, pxf(1), p.text, pxf(1.3F));
    }
    if (focused({Part::Nav, -1, -1})) {
        paint_ring(c, g.nav);
    }

    const RECT column{main_left() - px(4), 0, width_, height_};
    c.clip(column);
    paint_button(c, g.check, hover_.part == Part::Check, focused({Part::Check, -1, -1}), !checking_ && connected_);
    if (!is_empty(g.message)) {
        c.round_rect(g.message, pxf(4), message_error_ ? p.banner_error.fill : p.banner_info.fill);
    }
    for (std::size_t i = 0; i < g.groups.size() && i < page_.groups.size(); ++i) {
        const GroupGeometry& gg = g.groups[i];
        if (gg.card.bottom < 0 || gg.card.top > height_) {
            continue;
        }
        const int index = static_cast<int>(i);
        c.round_rect(gg.card, pxf(8), p.card, p.card_border, border);
        if (!gg.rows.empty()) {
            c.fill({gg.card.left + 1, gg.rows.front().row.top - static_cast<int>(border), gg.card.right - 1,
                    gg.rows.front().row.top},
                   p.divider);
        }
        if (!is_empty(gg.picker)) {
            const Hit hit{Part::Picker, index, -1};
            c.round_rect(gg.picker, pxf(4), hover_ == hit ? p.row_hover : p.input, p.input_border, border);
            const float vx = static_cast<float>(gg.picker.right) - pxf(14);
            const float vy = static_cast<float>(gg.picker.top + gg.picker.bottom) / 2.0F;
            c.line(vx - pxf(4), vy - pxf(2), vx, vy + pxf(2), p.text_subtle, pxf(1.3F));
            c.line(vx, vy + pxf(2), vx + pxf(4), vy - pxf(2), p.text_subtle, pxf(1.3F));
            if (focused(hit)) {
                paint_ring(c, gg.picker);
            }
        }
        for (std::size_t r = 0; r < gg.rows.size(); ++r) {
            const RowGeometry& rg = gg.rows[r];
            if (r + 1 < gg.rows.size()) {
                c.fill({rg.row.left + 1, rg.row.bottom - static_cast<int>(border), rg.row.right - 1, rg.row.bottom},
                       p.divider);
            }
            if (!is_empty(rg.chip)) {
                c.round_rect(rg.chip, pxf(10), rg.chip_text == Str::SettingsDefaultChip ? p.chip_default.fill : p.chip);
            }
            if (!is_empty(rg.bar)) {
                c.round_rect(rg.bar, pxf(2), p.bar_track);
                const int done = static_cast<int>(static_cast<double>(width_of(rg.bar)) * rg.fraction);
                if (done > 0) {
                    c.round_rect({rg.bar.left, rg.bar.top, rg.bar.left + done, rg.bar.bottom}, pxf(2), p.accent);
                }
            }
            if (rg.button_kind != Button::None) {
                const Hit hit{Part::Button, index, static_cast<int>(r)};
                paint_button(c, rg.button, hover_ == hit, focused(hit), rg.button_kind != Button::RemoveDisabled);
            }
        }
    }
    // A thin scroll indicator when the page is taller than the window.
    if (g.content_h > height_ && height_ > 0) {
        const int thumb = std::max(px(24), height_ * height_ / g.content_h);
        const int top = (height_ - thumb) * scroll_ / (g.content_h - height_);
        c.round_rect({width_ - px(6), top + px(2), width_ - px(2), top + thumb - px(2)}, pxf(2), p.switch_off_border);
    }
    c.unclip();
}

void SettingsWindow::paint_text(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    c.text({g.nav.left + px(38), g.nav.top, g.nav.right - px(8), g.nav.bottom}, w(text(lang_, Str::SettingsRuntimes)),
           fonts_.body_bold.get(), p.text, Align::Start, false);

    const RECT column{main_left() - px(4), 0, width_, height_};
    c.clip_text(column);
    c.text(g.title, w(text(lang_, Str::SettingsRuntimes)), fonts_.title.get(), p.text, Align::Start, false);
    const std::string hint = has_reply_ ? page_.hint : std::string(text(lang_, Str::SettingsRuntimesHint));
    c.paragraph(g.hint, w(hint), fonts_.caption.get(), p.text_subtle);
    c.text(g.check, w(text(lang_, checking_ ? Str::SettingsChecking : Str::SettingsCheckUpdates)), fonts_.button.get(),
           !checking_ && connected_ ? p.text : p.button_disabled_text, Align::Center, false);
    if (!is_empty(g.message)) {
        RECT inner = g.message;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(message_), fonts_.button.get(), message_error_ ? p.banner_error.text : p.banner_info.text);
    }
    if (!is_empty(g.note)) {
        c.text(g.note, w(text(lang_, Str::SettingsLoading)), fonts_.button.get(), p.text_subtle, Align::Start, false);
    }
    for (std::size_t i = 0; i < g.groups.size() && i < page_.groups.size(); ++i) {
        const GroupGeometry& gg = g.groups[i];
        const RuntimeGroup& group = page_.groups[i];
        if (gg.card.bottom < 0 || gg.card.top > height_) {
            continue;
        }
        RECT title = gg.title;
        if (!is_empty(gg.picker)) {
            title.right = (is_empty(gg.picker_label) ? gg.picker.left : gg.picker_label.left) - px(kGap);
        }
        c.text(title, w(group.title), fonts_.body_bold.get(), p.text, Align::Start, false);
        if (!is_empty(gg.picker_label)) {
            c.text(gg.picker_label, w(text(lang_, Str::SettingsDefaultFor)), fonts_.caption.get(), p.text_subtle,
                   Align::End, false);
        }
        if (!is_empty(gg.picker) && group.picker) {
            RECT box = gg.picker;
            box.left += px(8);
            box.right -= px(26);
            const std::size_t shown = picker_value(group);
            if (shown < group.picker->choices.size()) {
                c.text(box, w(group.picker->choices[shown].label), fonts_.button.get(), p.text, Align::Start, false);
            }
        }
        for (std::size_t r = 0; r < gg.rows.size() && r < group.rows.size(); ++r) {
            const RowGeometry& rg = gg.rows[r];
            const RuntimeRow& row = group.rows[r];
            c.text(rg.version, w(row.label), fonts_.body.get(), p.text, Align::Start, true);
            if (!is_empty(rg.chip)) {
                const bool def = rg.chip_text == Str::SettingsDefaultChip;
                c.text(rg.chip, w(text(lang_, rg.chip_text)), def ? fonts_.small_bold.get() : fonts_.caption.get(),
                       def ? p.chip_default.text : p.chip_text, Align::Center, false);
            }
            if (!is_empty(rg.text)) {
                c.text(rg.text, w(rg.line), fonts_.caption.get(), p.text_subtle, Align::Start, rg.line_code);
            }
            if (rg.button_kind != Button::None) {
                const Str label = rg.button_kind == Button::Download ? Str::SettingsDownload
                                  : rg.button_kind == Button::Cancel ? Str::WizardCancel
                                                                     : Str::SettingsRemove;
                c.text(rg.button, w(text(lang_, label)), fonts_.button.get(),
                       rg.button_kind == Button::RemoveDisabled ? p.button_disabled_text : p.text, Align::Center, false);
            }
        }
    }
    c.unclip_text();
}

}  // namespace floserver::ui

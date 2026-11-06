#include "main_window.h"

#include "main_window_layout.h"
#include "model/theme.h"
#include "model/view_model.h"
#include "window_prefs.h"
#include "window_style.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string_view>
#include <utility>

namespace floserver::ui {

using namespace layout;

namespace {

constexpr UINT kSnapshotMessage = WM_APP + 2;
constexpr UINT kTrayMessage = WM_APP + 3;
constexpr UINT kSettingsClosedMessage = WM_APP + 4;
// WM_APP + 5 and + 6 are the panel's (main_window.h).
// Only while an information toast is shown, and not while the mouse is over it.
constexpr UINT_PTR kToastTimer = 2;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kSearchSubclass = 1;
constexpr UINT_PTR kTipNewProject = 1;
constexpr UINT_PTR kTipSettings = 2;
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuQuit = 2;

std::wstring w(std::string_view s) { return win::widen(s); }

// Ctrl without Alt: AltGr is Ctrl+Alt, and AltGr+N types a letter (Polish ń).
bool ctrl_only() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0; }

}  // namespace

MainWindow::MainWindow(HINSTANCE instance, Lang lang, const WindowPrefs& prefs, std::wstring class_name)
    : instance_(instance), lang_(lang), rtl_(is_rtl(lang)), prefs_(prefs), class_name_(std::move(class_name)) {}

bool MainWindow::create(int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = class_name_.c_str();
    if (RegisterClassExW(&wc) == 0) {
        return false;
    }
    return make_window(show, nullptr);
}

bool MainWindow::make_window(int show, const WINDOWPLACEMENT* placement) {
    const auto system_dpi = static_cast<int>(GetDpiForSystem());
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const bool roomy = MulDiv(kWindowW + 40, system_dpi, 96) <= width_of(work) &&
                       MulDiv(kWindowH + 40, system_dpi, 96) <= height_of(work);
    const int open_w = roomy ? kWindowW : kSmallWindowW;
    const int open_h = roomy ? kWindowH : kSmallWindowH;
    if (CreateWindowExW(rtl_ ? WS_EX_LAYOUTRTL : 0, class_name_.c_str(), L"FloServer",
                        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                        MulDiv(open_w, system_dpi, 96), MulDiv(open_h, system_dpi, 96), nullptr, nullptr, instance_,
                        this) == nullptr) {
        return false;
    }
    if (placement != nullptr) {
        // Where the window it replaces was (a new language), shown as it was.
        WINDOWPLACEMENT wp = *placement;
        wp.length = sizeof wp;
        wp.flags = 0;
        wp.showCmd = static_cast<UINT>(show);
        SetWindowPlacement(hwnd_, &wp);
        UpdateWindow(hwnd_);
        return true;
    }
    // Centred on the screen it opened on (Windows would put it near the
    // top-left corner), at that screen's scale.
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT area = mi.rcWork;
    const int ww = std::min(px(open_w), width_of(area));
    const int wh = std::min(px(open_h), height_of(area));
    SetWindowPos(hwnd_, nullptr, static_cast<int>(area.left) + (width_of(area) - ww) / 2,
                 static_cast<int>(area.top) + (height_of(area) - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK MainWindow::window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = win::int_to_ptr<const CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = win::int_to_ptr<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return self->handle(msg, wparam, lparam);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_GETOBJECT) {
        // UI Automation asking for the window's elements (main_window_access.cpp).
        LRESULT result = 0;
        if (access_.on_get_object(wparam, lparam, result)) {
            return result;
        }
        return DefWindowProcW(hwnd_, msg, wparam, lparam);
    }
    // Anything may change what's shown: the elements are made again when next asked for.
    access_dirty_ = true;
    if (msg == taskbar_created_ && taskbar_created_ != 0) {
        tray_added_ = false;  // Explorer restarted
        add_tray_icon();
        return 0;
    }
    switch (msg) {
        case WM_CREATE:
            on_create();
            return 0;
        case WM_DESTROY:
            on_destroy();
            return 0;
        case WM_CLOSE:
            // Closing hides the window; the service keeps running (D2).
            if (quitting_) {
                DestroyWindow(hwnd_);
            } else {
                // Closing the first-run page's window at any step leaves it for good.
                finish_first_run();
                // Settings belongs to this window; it would otherwise stay
                // on screen with no taskbar button.
                if (prefs_.close == CloseAction::Quit) {
                    // Settings > General: closing quits, as the tray's Quit does. A
                    // Save chosen in Settings on the way is done first; quit follows
                    // (its closed message); Cancel stays.
                    if (settings_ && !settings_->close_and_quit()) {
                        return 0;
                    }
                    quit();
                    return 0;
                }
                if (settings_ && !settings_->close()) {
                    return 0;  // PHP changes not saved, and the user chose to stay
                }
                ShowWindow(hwnd_, SW_HIDE);
            }
            return 0;
        case WM_ENDSESSION:
            // Windows is signing out or shutting down: the first-run page is done, as when it's closed.
            if (wparam != FALSE) {
                finish_first_run();
                // The installer (an upgrade or an uninstall) needs FloServer's
                // files: Windows' Restart Manager asks the window to close.
                // Quit as the tray's Quit does, service and all.
                if ((static_cast<ULONG_PTR>(lparam) & ENDSESSION_CLOSEAPP) != 0) {
                    quit();
                }
            }
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
                layout();
                scroll_to(scroll_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = win::int_to_ptr<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {px(kMinW), px(kMinH)};
            return 0;
        }
        case WM_DPICHANGED:
            on_dpi_changed(LOWORD(wparam), *win::int_to_ptr<const RECT*>(lparam));
            return 0;
        case WM_SETTINGCHANGE:
            if (lparam != 0 && std::wcscmp(win::int_to_ptr<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
                const bool dark = apps_use_dark_theme();
                if (dark != dark_) {
                    dark_ = dark;
                    apply_theme();
                }
            } else if (wparam == SPI_SETNONCLIENTMETRICS) {
                make_fonts();
                layout();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_CTLCOLORSTATIC:  // a read-only PHP box (a save on its way)
        case WM_CTLCOLOREDIT:
            if (const int id = GetDlgCtrlID(win::int_to_ptr<HWND>(lparam)); id >= kPhpEditBase && php_open_) {
                // The panel's PHP boxes: tinted where the project has its own value.
                const std::size_t index = static_cast<std::size_t>(id - kPhpEditBase);
                bool marked = false;
                for (std::size_t f = 0; f < php_field_edit_.size() && f < php_keys_.size(); ++f) {
                    if (php_field_edit_[f] == static_cast<int>(index)) {
                        marked = !php_keys_[f].empty() && php_after_.ini.contains(php_keys_[f]);
                    }
                }
                const Palette& p = palette(dark_);
                auto* dc = win::int_to_ptr<HDC>(wparam);
                SetTextColor(dc, colorref(p.text));
                SetBkColor(dc, colorref(marked ? p.input_set : p.input));
                return reinterpret_cast<LRESULT>(marked ? input_set_brush_.get() : input_brush_.get());
            }
            if (win::int_to_ptr<HWND>(lparam) == search_ || win::int_to_ptr<HWND>(lparam) == name_edit_) {
                const Palette& p = palette(dark_);
                auto* dc = win::int_to_ptr<HDC>(wparam);
                SetTextColor(dc, colorref(p.text));
                SetBkColor(dc, colorref(p.input));
                return reinterpret_cast<LRESULT>(input_brush_.get());
            }
            break;
        case WM_COMMAND:
            if (lparam != 0 && LOWORD(wparam) >= kPhpEditBase && HIWORD(wparam) == EN_CHANGE) {
                on_php_edit_change(LOWORD(wparam));
                return 0;
            }
            if (lparam != 0 && LOWORD(wparam) >= kPhpEditBase && HIWORD(wparam) == EN_SETFOCUS) {
                on_php_edit_focus(LOWORD(wparam));
                return 0;
            }
            if (name_edit_ != nullptr && win::int_to_ptr<HWND>(lparam) == name_edit_ && HIWORD(wparam) == EN_CHANGE) {
                sync_name_help();
                InvalidateRect(hwnd_, nullptr, FALSE);  // the address and folder lines follow the name
                return 0;
            }
            if (win::int_to_ptr<HWND>(lparam) == search_ && HIWORD(wparam) == EN_CHANGE) {
                const int n = GetWindowTextLengthW(search_);
                std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
                GetWindowTextW(search_, text.data(), n + 1);
                text.resize(static_cast<std::size_t>(n));
                query_ = win::narrow(text);
                scroll_ = 0;
                focus_ = -1;
                update_visible();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (!scroll_bar_mouse(msg, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
                on_mouse_move(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            }
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            list_bar_.cancel(hwnd_);
            panel_bar_.cancel(hwnd_);
            first_run_bar_.cancel(hwnd_);
            break;
        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            if (list_bar_.leave() || panel_bar_.leave() || first_run_bar_.leave()) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            if (hover_.part != Part::None) {
                hover_ = {};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && win::int_to_ptr<HWND>(wparam) == hwnd_ &&
                (!remove_checking_.empty() || !backup_planning_.empty())) {
                // The remove question or the Back up dialog is on its way.
                SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
                return TRUE;
            }
            if (LOWORD(lparam) == HTCLIENT && win::int_to_ptr<HWND>(wparam) == hwnd_) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd_, &pt);
                const Hit hit = hit_test(pt.x, pt.y);
                const bool hand = hit.part == Part::Switch || hit.part == Part::Address || hit.part == Part::Download ||
                                  hit.part == Part::FrChange ||
                                  hit.part == Part::PanelAddress || hit.part == Part::PanelKeep ||
                                  hit.part == Part::SharedSwitch || hit.part == Part::SharedEnd ||
                                  (hit.part == Part::Banner && status_line().clickable);
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            mouse_used();
            break;  // a right-click's menu comes with WM_CONTEXTMENU
        case WM_UPDATEUISTATE:
            // Windows' own word on the focus cues, for those it sends (the
            // keys and clicks handled here set show_focus_ themselves).
            if ((HIWORD(wparam) & UISF_HIDEFOCUS) != 0 && LOWORD(wparam) != UIS_INITIALIZE) {
                const bool show = LOWORD(wparam) == UIS_CLEAR;
                if (show != show_focus_) {
                    show_focus_ = show;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            break;
        case WM_LBUTTONDOWN:
            mouse_used();
            if (!highlighted_.empty()) {
                highlighted_.clear();  // a click elsewhere (or on it: its panel opens) ends the right-click's selection
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            if (scroll_bar_mouse(msg, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
                return 0;  // a scroll bar's: the keyboard focus stays where it is
            }
            SetFocus(hwnd_);
            pressed_ = hit_test(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            SetCapture(hwnd_);
            return 0;
        case WM_LBUTTONUP: {
            if (scroll_bar_mouse(msg, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
                return 0;
            }
            ReleaseCapture();
            const Hit hit = hit_test(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (hit == pressed_) {
                on_click(hit);
            }
            pressed_ = {};
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (first_run_open()) {
                first_run_scroll_to(first_run_.scroll - GET_WHEEL_DELTA_WPARAM(wparam) * px(48) / WHEEL_DELTA);
                return 0;
            }
            if (wizard_open()) {
                return 0;  // the list under the page stays put
            }
            // The panel's content scrolls under the mouse (also from a PHP
            // box, which hands the wheel on to this window).
            POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &at);
            if (panel_open() && (contains(panel_geometry_.panel, at.x, at.y) || !panel_beside_list())) {
                panel_scroll_to(panel_scroll_ - GET_WHEEL_DELTA_WPARAM(wparam) * px(48) / WHEEL_DELTA);
                return 0;
            }
            scroll_to(scroll_ - GET_WHEEL_DELTA_WPARAM(wparam) * px(kRowH) / WHEEL_DELTA);
            return 0;
        }
        case WM_KEYDOWN:
            // The menu key: Windows makes it a WM_CONTEXTMENU (as Shift+F10).
            if (wparam == VK_APPS) {
                break;
            }
            // A held Space would queue a command per repeat.
            if (wparam == VK_SPACE && (static_cast<std::uintptr_t>(lparam) & (1U << 30U)) != 0) {
                return 0;
            }
            on_key(wparam);
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_CONTEXTMENU:
            // A row's menu (a right-click, the menu key, Shift+F10); a box's own goes to the box.
            if (win::int_to_ptr<HWND>(wparam) == hwnd_) {
                on_context_menu(lparam);
                return 0;
            }
            break;
        case WM_TIMER:
            if (list_bar_.tick(hwnd_, wparam) || panel_bar_.tick(hwnd_, wparam) || first_run_bar_.tick(hwnd_, wparam)) {
                return 0;
            }
            if (wparam == kToastTimer) {
                KillTimer(hwnd_, kToastTimer);
                toast_event(ToastEvent::TimeUp);
            }
            return 0;
        case kSnapshotMessage:
            on_snapshot();
            return 0;
        case kTrayMessage:
            on_tray(lparam, wparam);
            return 0;
        case kShowWindowMessage:
            show_window();
            return 0;
        case kBackupMessage:
            run_backup_steps();
            return 0;
        case WM_COPYDATA: {
            // A second FloServer.exe opened with a .zip: New project from a backup here.
            const auto* data = win::int_to_ptr<const COPYDATASTRUCT*>(lparam);
            if (data != nullptr && data->dwData == kOpenBackupData && data->lpData != nullptr && data->cbData > 0 &&
                data->cbData < 32768) {
                std::string file(static_cast<const char*>(data->lpData), data->cbData);
                if (file.find('\0') == std::string::npos) {
                    show_window();
                    open_backup(std::move(file));
                }
                return TRUE;
            }
            break;
        }
        case kPhpBackMessage:
            // Escape in a PHP box: the keys go back to the tab bar.
            SetFocus(hwnd_);
            if (panel_open()) {
                panel_focus_ = 0;
                show_focus_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case kStepTabMessage:
            SetFocus(hwnd_);
            if (panel_open()) {
                step_panel_tab(wparam == 1 ? 1 : -1);
            }
            return 0;
        case kPrefsMessage:
            on_pref_change(wparam, lparam);
            return 0;
        case kFirstRunTrustMessage:
            first_run_trust_now();
            return 0;
        case kUninstallMessage:
            on_uninstall();
            return 0;
        case kShowUninstallMessage:
            open_uninstall(false);
            return 0;
        case kUninstallRemoveMessage:
            start_uninstall(uninstall_ticks_from(static_cast<unsigned>(wparam)));
            return 0;
        case kUninstallClosedMessage:
            on_uninstall_window_closed();
            return 0;
        case kUninstallSizesMessage: {
            std::optional<UninstallSizes> sizes;
            {
                const std::lock_guard lock(uninstall_sizes_->mutex);
                sizes = uninstall_sizes_->sizes;
            }
            if (sizes) {
                push_uninstall_choices(*sizes);
            }
            return 0;
        }
        case kRecycledMessage:
            on_recycled();
            return 0;
        case kAccessMessage:
            run_access_requests();
            return 0;
        case kToastMessage:
            // Its button: the backup it names, in its folder; then it goes.
            if (wparam == kToastActionClicked) {
                if (!toast_.action.empty() && !toast_file_.empty()) {
                    show_file_in_folder(toast_file_);
                }
                toast_event(ToastEvent::Close);
                return 0;
            }
            // The popup: information stays while it's read; its close mark.
            toast_event(wparam == kToastCloseClicked  ? ToastEvent::Close
                        : wparam == kToastMouseEntered ? ToastEvent::MouseEnter
                                                       : ToastEvent::MouseLeave);
            return 0;
        case WM_MOVE:
            toast_popup_.follow();
            break;
        case WM_SHOWWINDOW:
            if (wparam == FALSE) {
                toast_popup_.hide();  // to the tray: an owned popup would stay on the screen
            }
            break;
        case kSettingsClosedMessage:
            if (wparam == 1) {
                quit();  // asked while Settings' save question was open; it's answered now
                return 0;
            }
            // An editor installed or removed while General was open is seen
            // the next time the window needs one.
            forget_editor();
            // The Settings window is gone: nothing of it stays but where it was.
            if (settings_ && !settings_->is_open()) {
                settings_placement_ = settings_->placement();
                settings_.reset();
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

// --- lifetime -------------------------------------------------------------

void MainWindow::on_create() {
    dpi_ = GetDpiForWindow(hwnd_);
    dark_ = apps_use_dark_theme();
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    RECT client{};
    GetClientRect(hwnd_, &client);
    width_ = client.right;
    height_ = client.bottom;

    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                              nullptr, instance_, nullptr);
    // The placeholder is drawn by search_proc in the theme's colour: the
    // system's cue banner grey can't be changed and is hard to read on dark.
    search_cue_ = w(text(lang_, Str::SearchProjects));
    SetWindowSubclass(search_, search_proc, kSearchSubclass, reinterpret_cast<DWORD_PTR>(this));
    // Its placeholder is drawn, not a label: screen readers are told its name.
    set_accessible_name(search_, text(lang_, Str::SearchProjects));
    access_.attach(hwnd_, this);

    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance_,
                               nullptr);
    tip_new_project_ = w(text(lang_, Str::NewProject));
    tip_settings_ = w(text(lang_, Str::Settings));
    for (const auto& [id, tip] : {std::pair{kTipNewProject, &tip_new_project_}, std::pair{kTipSettings, &tip_settings_}}) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_SUBCLASS | (rtl_ ? TTF_RTLREADING : 0U);
        ti.hwnd = hwnd_;
        ti.uId = id;
        ti.lpszText = tip->data();
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    recycle_bin_.retarget(hwnd_, kRecycledMessage);  // made again in another language: the new window
    uninstaller_.retarget(hwnd_, kUninstallMessage);
    toast_popup_.attach(hwnd_, kToastMessage);
    // A move or check that finished while the old window was going is taken now.
    PostMessageW(hwnd_, kRecycledMessage, 0, 0);
    // The panel's icon buttons: placed and worded by place_panel_tips().
    panel_tips_ = {};
    panel_tip_rects_ = {};
    for (std::size_t i = 0; i < kIconActions.size(); ++i) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_SUBCLASS | (rtl_ ? TTF_RTLREADING : 0U);
        ti.hwnd = hwnd_;
        ti.uId = kTipPanelFirst + i;
        ti.lpszText = panel_tips_[i].data();
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }

    make_fonts();
    make_icons();
    apply_theme();
    layout();
    add_tray_icon();
    if (worker_) {
        worker_->retarget(hwnd_, lang_);  // made again in another language: the same link to the service
    } else {
        worker_ = std::make_unique<ServiceWorker>(hwnd_, kSnapshotMessage, lang_);
    }
    decide_first_run();  // FLOSERVER_FIRST_RUN=1: at once; else once the service has answered
}

void MainWindow::on_destroy() {
    // Screen readers holding an element from this window are told it's gone.
    access_.detach();
    access_said_.reset();
    access_queue_.clear();
    set_accessible_name(search_, {});
    set_accessible_name(name_edit_, {});
    set_accessible_help(name_edit_, {});
    KillTimer(hwnd_, kToastTimer);
    list_bar_.cancel(hwnd_);
    panel_bar_.cancel(hwnd_);
    first_run_bar_.cancel(hwnd_);
    if (tray_added_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof nid;
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        tray_added_ = false;
    }
    if (rebuilding_) {
        return;  // made again at once: Settings, the worker and the message loop stay
    }
    settings_.reset();  // before the worker it uses
    worker_.reset();
    PostQuitMessage(0);
}

// phpmyadmin.login's answers, from the panel and from Settings → Database:
// the link is opened in the browser here. A refusal is shown where it was
// asked (the panel's here; Settings' stays in the list for it).
void MainWindow::open_phpmyadmin_links(std::vector<Snapshot::DbAnswer>& answers) {
    for (auto it = answers.begin(); it != answers.end();) {
        if (it->method != "phpmyadmin.login") {
            ++it;
            continue;
        }
        const bool panel = it->purpose == "panel";
        if (panel) {
            pma_busy_ = false;
        }
        std::string url;
        if (it->error.empty() && it->reply && it->reply->is_object() && it->reply->contains("url") &&
            it->reply->at("url").is_string()) {
            url = it->reply->at("url").get<std::string>();
        }
        // Only phpMyAdmin's own address is ever opened.
        const bool ours = is_phpmyadmin_url(url);
        if (ours) {
            ShellExecuteW(hwnd_, L"open", w(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (panel) {
            if (!ours) {
                show_notice(fill(lang_, Str::PmaOpenFailed, {it->error}), true);
            }
            it = answers.erase(it);
        } else {
            ++it;  // Settings clears its busy state and shows a refusal
        }
    }
}

void MainWindow::on_snapshot() {
    if (!worker_) {
        return;
    }
    Snapshot s = worker_->take();
    // A switch or picker shows the user's choice until its last command has finished.
    settle(s.settled);
    if (!s.change_error.empty()) {
        show_notice(s.change_error, true);
    }
    // Copy .env lines from Settings > Shared sites comes here too: it's told after.
    std::optional<bool> copied;
    if (!s.clipboard.empty()) {
        copied = put_on_clipboard(s.clipboard);
    }
    if (!s.added.empty()) {
        pending_added_ = s.added;
    }
    open_phpmyadmin_links(s.db);
    const std::vector<Removed> removed = on_remove_answers(s.db);
    const std::vector<Renamed> renamed = on_rename_answers(s.db);
    on_backup_answers(s.db);
    on_first_run_answers(s);
    const bool quit_done = s.quit_done;
    const std::string quit_error = s.quit_error;
    // A project made from a template that hands over to FloCMS Studio (D9, D14).
    const bool just_made = wizard_running() && s.create.state == "done" && !s.create.project.empty();
    // Keyboard focus stays on the same project when the list changes.
    std::string focused;
    if (focus_ >= 0 && focus_ < static_cast<int>(visible_.size())) {
        focused = snap_.rows[visible_[static_cast<std::size_t>(focus_)]].name;
    }
    const UpdateBanner update_before = update_shown();
    snap_ = std::move(s);
    on_update_snapshot(update_before);
    // Renamed: what was the old name's in the window follows the new one.
    for (const Renamed& r : renamed) {
        renamed_[r.from] = r;
        if (focused == r.from) {
            focused = r.to;
        }
        if (highlighted_ == r.from) {
            highlighted_ = r.to;
        }
        if (selected_ == r.from) {
            // The panel stays open on it, its PHP part too.
            selected_ = r.to;
            if (php_info_project_ == r.from) {
                php_info_project_ = r.to;
            }
            if (worker_) {
                worker_->select(r.to);
            }
        }
        for (auto* pending : {&pending_, &pending_keep_}) {
            if (const auto it = pending->find(r.from); it != pending->end()) {
                (*pending)[r.to] = it->second;
                pending->erase(it);
            }
        }
        show_notice(rename_outcome(r.from, r.to, r.folder, lang_), false);
    }
    apply_renames();
    if (settings_) {
        settings_->on_snapshot(snap_);
        if (copied) {
            settings_->env_copied(*copied);
        }
    }
    snap_.runtimes.reset();  // Settings' alone; not kept once it has it
    // Nor its other one-shot answers: a Settings opened next must not get
    // them again (Notepad would open once more).
    snap_.custom_ini.clear();
    snap_.custom_ini_error.clear();
    update_visible();
    // A .zip opened with FloServer before the service answered, or a backup
    // step that waited for another question to be answered.
    const bool backup_waits = (!open_backup_.empty() && snap_.link == Link::Connected) || backup_plan_;
    if (backup_waits && !asking_ && !backup_job_) {
        PostMessageW(hwnd_, kBackupMessage, 0, 0);
    }
    // The panel's project may have been removed (flo remove).
    if (!selected_.empty() && shows_rows() && selected_row() == nullptr) {
        (void)leave_php_part(false);
        close_panel();
    }
    // Removed from the window: its panel closes now, and the list is asked
    // for again (the row goes with the next answer).
    for (const Removed& r : removed) {
        removed_.insert(r.project);
        if (selected_ == r.project) {
            (void)leave_php_part(false);
            close_panel();
        }
        if (r.trash) {
            // Removed from the list first; only now the folder (said once it's moved, or not).
            recycle_bin_.start(r.project, r.dropped, r.path, folder_guard(r.project),
                               std::string(text(lang_, Str::RemoveFolderRefused)),
                               std::string(text(lang_, Str::ProgramMissing)));
        } else {
            const RemoveOutcome said =
                remove_outcome(r.project, r.dropped, r.refused ? FolderOutcome::Stayed : FolderOutcome::NotAsked,
                               std::string(text(lang_, Str::RemoveFolderRefused)), lang_);
            show_notice(said.text, said.error);
        }
    }
    if (!removed.empty() || !renamed.empty()) {
        update_visible();
        if (worker_) {
            worker_->refresh();
        }
    }
    on_project_php_answers();
    sync_project_php();
    // The PHP tab was shown while the service wasn't there: asked now it is.
    if (php_info_wanted_ && php_open_ && snap_.link == Link::Connected) {
        ask_php_info();
    }
    update_fields();
    // A tab came or went with the project (a version picked here or in floserver.json).
    sync_panel_tab();
    // Shared sites: the row goes when the service stops answering shared.get.
    if (!snap_.shared.known) {
        shared_focused_ = false;
    }
    if (!focused.empty()) {
        if (const int again = visible_index(focused); again >= 0) {
            focus_ = again;
        }
    }
    // A folder the user just added: its panel opens once the list has it.
    if (!pending_added_.empty() && !wizard_open() && shows_rows() && snap_.link == Link::Connected) {
        const bool listed = std::any_of(snap_.rows.begin(), snap_.rows.end(),
                                        [&](const ProjectRow& r) { return r.name == pending_added_; });
        if (listed) {
            if (const int row = visible_index(pending_added_); row >= 0) {
                focus_ = row;
                ensure_visible(row);
            }
            select(pending_added_);
            pending_added_.clear();
        }
    }
    // The first-run page: shown once the service has said what's there;
    // its list asked for once the service is there.
    decide_first_run();
    if (first_run_open() && first_run_.step == FirstRunStep::Runtimes && first_run_.list == FirstRunList::Idle) {
        first_run_ask_list();
    }
    if (just_made && wizard_.studio) {
        // The new project's folder, from the list that came with the answer.
        const auto made = std::find_if(snap_.rows.begin(), snap_.rows.end(),
                                       [&](const ProjectRow& r) { return r.name == snap_.create.project; });
        if (made != snap_.rows.end()) {
            open_studio(made->path, true);
        }
    }
    layout();
    update_tray_tip();
    // The name box's help has the address in it: the domain or port may have changed.
    if (wizard_open()) {
        sync_name_help();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (quit_done && uninstalling_) {
        uninstall_stopped(quit_error);  // Remove FloServer… goes on: the clean-up
        return;
    }
    if (quit_done) {
        if (!quit_error.empty()) {
            const std::wstring message = w(fill(lang_, Str::ServiceFailed, {quit_error}));
            MessageBoxW(nullptr, message.c_str(), L"FloServer",
                        MB_OK | MB_ICONWARNING | (rtl_ ? MB_RTLREADING | MB_RIGHT : 0U));
        }
        quitting_ = true;
        DestroyWindow(hwnd_);
    }
}

void MainWindow::quit() {
    // Remove FloServer… quits by itself once its last step is done.
    if (quitting_ || !worker_ || uninstalling_) {
        return;
    }
    // Quitting with the first-run page open leaves it for good, as closing does.
    finish_first_run();
    // A save question is open (Settings' or the panel's): its code is still
    // running under the dialog, so nothing it uses may go now. Quit follows
    // once it's answered.
    if (settings_ && settings_->asking()) {
        settings_->quit_when_asked();
        return;
    }
    if (asking_) {
        quit_after_ask_ = true;
        return;
    }
    // A folder on its way to the Recycle Bin: Windows' progress and question
    // belong to this window, so FloServer quits once it's done (on_recycled()).
    if (recycle_bin_.busy()) {
        quit_after_folder_ = true;
        show_notice(std::string(text(lang_, Str::QuitAfterFolder)), false);
        return;
    }
    quitting_ = true;
    settings_.reset();
    ShowWindow(hwnd_, SW_HIDE);
    worker_->quit_service();
}

void MainWindow::open_settings() {
    if (settings_ && settings_->is_open()) {
        settings_->bring_to_front();  // one at a time
        return;
    }
    if (!worker_ || quitting_ || uninstalling_) {
        return;
    }
    // An earlier open's answers mustn't show before this one's arrive.
    snap_.runtimes_error.clear();
    snap_.runtimes_refresh = false;
    snap_.settings_error.clear();
    snap_.db.clear();
    settings_ = std::make_unique<SettingsWindow>(instance_, lang_, prefs_, *worker_, hwnd_, kSettingsClosedMessage,
                                                 kPrefsMessage);
    if (!settings_->create(settings_placement_)) {
        settings_.reset();
        return;
    }
    settings_->on_snapshot(snap_);  // the link, what's installed, the installs running
}

void MainWindow::on_pref_change(WPARAM change, LPARAM value) {
    if (static_cast<PrefChange>(change) == PrefChange::RemoveFloServer) {
        (void)value;
        open_uninstall(false);  // not a preference: nothing saved
        return;
    }
    if (static_cast<PrefChange>(change) == PrefChange::UpdateNow) {
        start_update();  // Settings > General's Update now: not a preference either
        return;
    }
    const auto index = static_cast<std::size_t>(value);
    const WindowPrefs before = prefs_;
    switch (static_cast<PrefChange>(change)) {
        case PrefChange::Language:
            if (index < kLangChoices.size()) {
                prefs_.lang = kLangChoices.at(index);
            }
            break;
        case PrefChange::Theme:
            if (index < kThemeChoices.size()) {
                prefs_.theme = kThemeChoices.at(index);
            }
            break;
        case PrefChange::Close:
            prefs_.close = index == 1 ? CloseAction::Quit : CloseAction::Tray;
            break;
        case PrefChange::StartInTray:
            prefs_.start_in_tray = index == 1;
            break;
        case PrefChange::CodeEditor:
            if (settings_) {
                prefs_.code_editor = settings_->prefs().code_editor;
            }
            forget_editor();  // looked for again with the new choice
            break;
        case PrefChange::RemoveFloServer:
        case PrefChange::UpdateNow:
            break;  // handled above
    }
    if (prefs_ == before) {
        return;
    }
    // Not saved: still used now, only not kept. Settings shows `error` with
    // fill(), which isolates it left to right in Arabic and Kurdish.
    std::string error;
    std::string kept_as;  // a damaged window.json, kept beside the new one
    (void)save_window_prefs(prefs_, error, kept_as);
    if (prefs_.theme != before.theme) {
        set_theme_choice(prefs_.theme);
        if (const bool dark = apps_use_dark_theme(); dark != dark_) {
            dark_ = dark;
            apply_theme();
        }
        if (settings_) {
            settings_->refresh_theme();
        }
    }
    if (prefs_.lang != before.lang) {
        const Lang lang = window_lang(prefs_);
        if (lang != lang_) {
            change_language(lang);  // Settings is made again with prefs_; an error shows there
            if (settings_ && (!error.empty() || !kept_as.empty())) {
                settings_->set_prefs(prefs_, error, kept_as);
            }
            return;
        }
    }
    if (settings_) {
        settings_->set_prefs(prefs_, error, kept_as);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);  // the first-run page's language choice
}

void MainWindow::change_language(Lang lang) {
    // Not while a save question is open (its code runs under the dialog):
    // the language is kept and shown the next time FloServer starts.
    if (quitting_ || hwnd_ == nullptr || asking_ || (settings_ && settings_->asking())) {
        return;
    }
    // Settings first: it belongs to this window. Nothing in it waits for a
    // Save while General is shown (each section settles its changes when
    // it's left), so it goes without asking and comes back on General.
    const bool settings_open = settings_ && settings_->is_open();
    if (settings_) {
        settings_->destroy_now();
        settings_placement_ = settings_->placement();
        settings_.reset();
    }
    // What the new window keeps: where it is, whether it's shown, what's typed.
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    const bool placed = GetWindowPlacement(hwnd_, &wp) != FALSE;
    const bool visible = IsWindowVisible(hwnd_) != FALSE;
    int show = SW_HIDE;
    if (visible) {
        show = IsIconic(hwnd_) != FALSE ? SW_SHOWMINNOACTIVE : IsZoomed(hwnd_) != FALSE ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    }
    const std::string wizard_text = wizard_name();
    const int list_scroll = scroll_;
    const int panel_scroll = panel_scroll_;
    // The child windows go with it: their owners let go of them first.
    php_edits_.clear();
    php_field_edit_.clear();
    rebuilding_ = true;
    DestroyWindow(hwnd_);
    rebuilding_ = false;
    hwnd_ = nullptr;
    search_ = nullptr;
    tooltip_ = nullptr;
    name_edit_ = nullptr;
    hover_ = {};
    pressed_ = {};
    tracking_mouse_ = false;

    // The direction is set when a window is made (WS_EX_LAYOUTRTL), so
    // the new one is made in the new language.
    lang_ = lang;
    rtl_ = is_rtl(lang);
    if (!make_window(show, placed ? &wp : nullptr)) {
        PostQuitMessage(1);  // no window: nothing left to show or to quit from
        return;
    }
    if (!query_.empty()) {
        const std::string query = query_;
        SetWindowTextW(search_, w(query).c_str());  // EN_CHANGE sets query_ again
    }
    remake_wizard_edit(wizard_text);
    if (php_open_) {
        make_project_php_edits();  // from what's typed (php_after_, php_another_)
    }
    // The words made from the snapshot, in the new language.
    update_fields();
    toast_.hovered = false;  // a new window: the mouse isn't over it yet
    sync_toast_timer();
    layout();
    scroll_to(list_scroll);
    panel_scroll_to(panel_scroll);
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (settings_open) {
        open_settings();
        if (settings_) {
            settings_->show_general();
        }
    }
}

void MainWindow::open_settings_shared() {
    open_settings();
    if (settings_ && settings_->is_open()) {
        settings_->show_shared();  // brought to the front if it was open already
    }
}

void MainWindow::show_window() {
    if (quitting_) {
        return;
    }
    ShowWindow(hwnd_, IsIconic(hwnd_) != FALSE ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

// --- theme, DPI, fonts ----------------------------------------------------

int MainWindow::px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }
float MainWindow::pxf(float dip) const { return dip * static_cast<float>(dpi_) / 96.0F; }

void MainWindow::apply_theme() {
    apply_title_bar(hwnd_, dark_);
    input_brush_.reset(CreateSolidBrush(colorref(palette(dark_).input)));
    input_set_brush_.reset(CreateSolidBrush(colorref(palette(dark_).input_set)));
    for (const auto& edit : php_edits_) {
        edit->set_cue_color(palette(dark_).text_subtle);
    }
    if (search_ != nullptr) {
        InvalidateRect(search_, nullptr, TRUE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::make_fonts() {
    make_ui_fonts(fonts_, dpi_, lang_);
    if (name_edit_ != nullptr) {
        SendMessageW(name_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body.get()), TRUE);
    }
    for (const auto& edit : php_edits_) {
        edit->set_font(fonts_.button.get());
    }
    if (search_ != nullptr) {
        SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.button.get()), TRUE);
        SendMessageW(search_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
    HDC dc = GetDC(hwnd_);
    new_button_width_ =
        Canvas::measure(dc, w(text(lang_, Str::NewProject)), fonts_.button_bold.get(), rtl_) + px(24);
    ReleaseDC(hwnd_, dc);
}

void MainWindow::make_icons() {
    win::UniqueIcon small_icon(make_app_icon(GetSystemMetricsForDpi(SM_CXSMICON, dpi_)));
    win::UniqueIcon big_icon(make_app_icon(GetSystemMetricsForDpi(SM_CXICON, dpi_)));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon.get()));
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon.get()));
    icon_small_ = std::move(small_icon);
    icon_big_ = std::move(big_icon);
    if (tray_added_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof nid;
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        nid.uFlags = NIF_ICON;
        nid.hIcon = icon_small_.get();
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void MainWindow::on_dpi_changed(UINT dpi, const RECT& suggested) {
    dpi_ = dpi;
    make_fonts();
    make_icons();
    SetWindowPos(hwnd_, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                 suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- geometry (logical coordinates: x from the reading start) -------------

RECT MainWindow::top_bar() const { return {0, 0, width_, px(kTopBarH)}; }

RECT MainWindow::settings_button() const {
    const int top = (px(kTopBarH) - px(kControlH)) / 2;
    return {width_ - px(kPad) - px(kControlH), top, width_ - px(kPad), top + px(kControlH)};
}

RECT MainWindow::new_project_button() const {
    const RECT s = settings_button();
    return {s.left - px(kGap) - new_button_width_, s.top, s.left - px(kGap), s.bottom};
}

RECT MainWindow::search_box() const {
    const RECT n = new_project_button();
    const int right = n.left - px(kGap);
    return {std::min(right, std::max(right - px(kSearchW), px(kSearchMinLeft))), n.top, right, n.bottom};
}

RECT MainWindow::banner_rect() const {
    // Only the first-run page, which has no status line, shows the service's
    // state at its top; elsewhere it's in the status line and nothing moves.
    if (!first_run_open() || status_line().text.empty()) {
        return {};
    }
    return {px(kPad), px(8), width_ - px(kPad), px(8) + px(kBannerH)};
}

RECT MainWindow::card_rect() const {
    const int top = content_top();
    const int bottom = std::max(top, height_ - px(kFooterH) - px(kGap));
    if (panel_open()) {
        if (!panel_beside_list()) {
            return {px(kPad), top, px(kPad), bottom};  // the panel takes the list's place
        }
        return {px(kPad), top, panel_rect().left - px(kGap), bottom};
    }
    return {px(kPad), top, width_ - px(kPad), bottom};
}

RECT MainWindow::list_rect() const {
    RECT r = card_rect();
    const int border = std::max(1, px(1));
    InflateRect(&r, -border, -border);
    // The Shared sites row is pinned under the list; the list scrolls above it.
    if (shows_shared() && width_of(r) > 0 && height_of(r) >= px(kRowH)) {
        r.bottom -= px(kRowH);
    }
    return r;
}

RECT MainWindow::footer_rect() const { return {0, height_ - px(kFooterH), width_, height_}; }

int MainWindow::list_height() const { return static_cast<int>(visible_.size()) * px(kRowH); }

std::vector<MainWindow::RowGeometry> MainWindow::row_geometry(HDC dc) const {
    std::vector<RowGeometry> out;
    if (!shows_rows()) {
        return out;
    }
    const RECT list = list_rect();
    const int row_h = px(kRowH);
    out.reserve(visible_.size());
    for (std::size_t i = 0; i < visible_.size(); ++i) {
        const ProjectRow& row = snap_.rows[visible_[i]];
        RowGeometry g;
        g.project = row.name;
        const int top = list.top + static_cast<int>(i) * row_h - scroll_;
        g.row = {list.left, top, list.right, top + row_h};
        const int mid = top + row_h / 2;
        g.toggle = {list.left + px(kRowPad), mid - px(kSwitchH) / 2, list.left + px(kRowPad) + px(kSwitchW),
                    mid - px(kSwitchH) / 2 + px(kSwitchH)};
        const int text_start = g.toggle.right + px(kRowPad);

        // From the end: the state label, then the chips while the text keeps room.
        g.end = row_end(row, g.end_text);
        const int state_w = Canvas::measure(dc, w(g.end_text), fonts_.small_bold.get(),
                                            rtl_) + px(20);
        g.state = {list.right - px(kRowPad) - state_w, mid - px(kStateH) / 2, list.right - px(kRowPad),
                   mid - px(kStateH) / 2 + px(kStateH)};
        std::vector<int> chip_w;
        chip_w.reserve(row.chips.size());
        for (const std::string& chip : row.chips) {
            chip_w.push_back(Canvas::measure(dc, w(chip), fonts_.caption.get(), false) + px(16));
        }
        std::size_t shown = chip_w.size();
        const auto chips_width = [&](std::size_t n) {
            int total = 0;
            for (std::size_t k = 0; k < n; ++k) {
                total += chip_w[k] + px(kRowPad);
            }
            return total;
        };
        while (shown > 0 && g.state.left - px(kRowPad) - chips_width(shown) - text_start < px(kMinTextW)) {
            --shown;
        }
        int x = g.state.left - px(kRowPad) - chips_width(shown) + px(kRowPad);
        for (std::size_t k = 0; k < shown; ++k) {
            g.chips.push_back({x, mid - px(kChipH) / 2, x + chip_w[k], mid - px(kChipH) / 2 + px(kChipH)});
            x += chip_w[k] + px(kRowPad);
        }
        const int text_end = (shown > 0 ? g.chips.front().left : g.state.left) - px(kRowPad);

        const int name_top = mid - px(19);
        g.name = {text_start, name_top, text_end, name_top + px(20)};
        g.line2 = {text_start, name_top + px(20), text_end, name_top + px(38)};
        if (!row.address.empty() && is_openable_address(row.address)) {
            const int aw = Canvas::measure(dc, w(row.address), fonts_.caption.get(), false);
            g.address = {text_start, g.line2.top, std::min(text_end, text_start + aw), g.line2.bottom};
        }
        out.push_back(std::move(g));
    }
    return out;
}

void MainWindow::layout() {
    if (search_ == nullptr || !fonts_.button) {
        return;
    }
    if (panel_open()) {
        ensure_editor();  // once a session, here rather than while painting
    }
    // The first-run page has no top bar: its search box goes too.
    if ((IsWindowVisible(search_) != FALSE) == first_run_open()) {
        ShowWindow(search_, first_run_open() ? SW_HIDE : SW_SHOWNA);
    }
    const RECT box = search_box();
    const int icon_area = px(10 + 14 + 8);
    const int h = px(20);
    const int edit_w = std::max(0, width_of(box) - icon_area - px(10));
    MoveWindow(search_, box.left + icon_area, box.top + (height_of(box) - h) / 2, edit_w, h, TRUE);

    // The New project name box sits in its outlined field.
    if (name_edit_ != nullptr && wizard_open() && wizard_.step == 1) {
        HDC dc = GetDC(hwnd_);
        const WizardGeometry g = wizard_geometry(dc);
        ReleaseDC(hwnd_, dc);
        const int eh = px(20);
        const RECT r = g.name_box;  // child positions use the same mirrored space as ours
        MoveWindow(name_edit_, r.left + px(10), r.top + (height_of(r) - eh) / 2, std::max(0, width_of(r) - px(20)), eh, TRUE);
    }
    if (!php_edits_.empty()) {
        place_project_php_edits();
    }
    for (const auto& [id, rect] : {std::pair{kTipNewProject, new_project_button()},
                                   std::pair{kTipSettings, settings_button()}}) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.hwnd = hwnd_;
        ti.uId = id;
        ti.rect = first_run_open() ? RECT{} : rect;
        SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

void MainWindow::update_visible() {
    visible_.clear();
    // Removed from the window: hidden at once, forgotten once the list agrees.
    std::erase_if(removed_, [this](const std::string& name) {
        return std::none_of(snap_.rows.begin(), snap_.rows.end(), [&](const ProjectRow& r) { return r.name == name; });
    });
    if (shows_rows()) {
        for (std::size_t i = 0; i < snap_.rows.size(); ++i) {
            if (row_matches(snap_.rows[i], query_) && !removed_.contains(snap_.rows[i].name)) {
                visible_.push_back(i);
            }
        }
    }
    if (focus_ >= static_cast<int>(visible_.size())) {
        focus_ = static_cast<int>(visible_.size()) - 1;
    }
    scroll_to(scroll_);
}

void MainWindow::scroll_to(int target) {
    const RECT list = list_rect();
    const int most = std::max(0, list_height() - height_of(list));
    const int value = std::clamp(target, 0, most);
    if (value != scroll_) {
        scroll_ = value;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::place_scroll_bars() {
    // The list's bar only while the list is what's shown under the mouse.
    const bool list_shown = !wizard_open() && !first_run_open();
    list_bar_.place(list_shown ? list_rect() : RECT{}, list_height(), scroll_, dpi_);
    panel_bar_.place(list_shown && panel_open() ? panel_geometry_.content : RECT{}, panel_content_h(), panel_scroll_,
                     dpi_);
    first_run_bar_.place(first_run_open() ? first_run_geometry_.content : RECT{}, first_run_geometry_.content_h,
                         first_run_.scroll, dpi_);
}

bool MainWindow::scroll_bar_mouse(UINT msg, int x, int y) {
    place_scroll_bars();
    // The panel lies over the list when they don't fit side by side.
    const bool in_panel = panel_open() && contains(panel_geometry_.panel, x, y);
    switch (msg) {
        case WM_LBUTTONDOWN:
            return first_run_bar_.press(hwnd_, x, y) || panel_bar_.press(hwnd_, x, y) ||
                   (!in_panel && list_bar_.press(hwnd_, x, y));
        case WM_LBUTTONUP:
            return first_run_bar_.release(hwnd_) || panel_bar_.release(hwnd_) || list_bar_.release(hwnd_);
        case WM_MOUSEMOVE: {
            const bool held = list_bar_.active() || panel_bar_.active() || first_run_bar_.active();
            bool changed = first_run_bar_.move(hwnd_, x, y);
            changed = panel_bar_.move(hwnd_, x, y) || changed;
            if (!panel_bar_.active()) {
                changed = list_bar_.move(hwnd_, in_panel ? -1 : x, in_panel ? -1 : y) || changed;
            }
            if (changed) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            // Otherwise the window's own hover goes on (it sees nothing under
            // the bar, and asks for the mouse leaving, which cools the bar).
            return held;
        }
        default:
            return false;
    }
}

void MainWindow::ensure_visible(int row) {
    if (row < 0) {
        return;
    }
    const int row_h = px(kRowH);
    const int top = row * row_h;
    const int view = height_of(list_rect());
    if (top < scroll_) {
        scroll_to(top);
    } else if (top + row_h > scroll_ + view) {
        scroll_to(top + row_h - view);
    }
}

bool MainWindow::shows_rows() const {
    return snap_.has_list &&
           (snap_.link == Link::Connected || snap_.link == Link::Lost || snap_.link == Link::Stopping);
}

MainWindow::BannerText MainWindow::status_line() const {
    switch (snap_.link) {
        case Link::Connecting:
            return {std::string(text(lang_, Str::Connecting)), false, false};
        case Link::Starting:
            return {std::string(text(lang_, Str::StartingService)), false, false};
        case Link::NotRunning:
            return {std::string(text(lang_, Str::ServiceStopped)), true, true};
        case Link::Failed:
            return {fill(lang_, Str::ServiceFailed, {snap_.error}), true, true};
        case Link::Lost:
            return {std::string(text(lang_, Str::ConnectionLost)), false, false};
        case Link::Stopping:
            return {std::string(text(lang_, Str::Stopping)), false, false};
        case Link::Connected:
            break;
    }
    if (!snap_.error.empty()) {
        // The service's own words (English for now).
        return {rtl_ ? isolate_ltr(snap_.error) : snap_.error, true, false};
    }
    return {};
}

bool MainWindow::switch_value(const ProjectRow& row) const {
    const auto it = pending_.find(row.name);
    return it != pending_.end() ? it->second : row.on;
}

MainWindow::Hit MainWindow::hit_test(int x, int y) const {
    if (first_run_open()) {
        // The page is all there is: no top bar, list or panel under it.
        if (first_run_bar_.over(x, y)) {
            return {};
        }
        if (contains(banner_rect(), x, y)) {
            return {Part::Banner, -1, {}};
        }
        return first_run_hit_test(x, y);
    }
    if (!status_line().text.empty() && contains(footer_rect(), x, y)) {
        return {Part::Banner, -1, {}};
    }
    if (panel_bar_.over(x, y) || (list_bar_.over(x, y) && !(panel_open() && contains(panel_geometry_.panel, x, y)))) {
        return {};  // a scroll bar's
    }
    if (contains(new_project_button(), x, y)) {
        return {Part::NewProject, -1, {}};
    }
    if (contains(settings_button(), x, y)) {
        return {Part::Settings, -1, {}};
    }
    if (contains(banner_rect(), x, y)) {
        return {Part::Banner, -1, {}};
    }
    if (contains(update_geometry_.banner, x, y)) {
        return update_hit_test(x, y);
    }
    if (wizard_open()) {
        return contains(wizard_geometry_.area, x, y) ? wizard_hit_test(x, y) : Hit{};
    }
    if (panel_open() && contains(panel_geometry_.panel, x, y)) {
        return panel_hit_test(x, y);
    }
    if (contains(shared_geometry_.row, x, y)) {
        return shared_hit_test(x, y);
    }
    if (!contains(list_rect(), x, y)) {
        return {};
    }
    for (std::size_t i = 0; i < geometry_.size(); ++i) {
        const RowGeometry& g = geometry_[i];
        const int row = static_cast<int>(i);
        if (contains(g.toggle, x, y)) {
            return {Part::Switch, row, g.project};
        }
        if (g.end == RowGeometry::End::Download && contains(g.state, x, y)) {
            return {Part::Download, row, g.project};
        }
        if (contains(g.address, x, y)) {
            return {Part::Address, row, g.project};
        }
        if (contains(g.row, x, y)) {
            return {Part::Row, row, g.project};
        }
    }
    return {};
}

// --- input ----------------------------------------------------------------

void MainWindow::on_mouse_move(int x, int y) {
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
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

int MainWindow::visible_index(const std::string& project) const {
    for (std::size_t i = 0; i < visible_.size(); ++i) {
        if (snap_.rows[visible_[i]].name == project) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void MainWindow::on_click(const Hit& clicked) {
    // The row is found again by its project: a new list may have arrived
    // since the screen the user clicked on was painted.
    Hit hit = clicked;
    if (!hit.project.empty() && !is_panel_part(hit.part)) {
        hit.row = visible_index(hit.project);
        if (hit.row < 0) {
            return;
        }
    }
    // Anywhere but the update banner, the keys leave its buttons.
    if (!is_update_part(hit.part)) {
        update_focus_ = -1;
    }
    // In the panel, the keys carry on from what was clicked.
    if (is_panel_part(hit.part)) {
        const std::vector<Hit> order = panel_focus_order();
        if (const auto it = std::find(order.begin(), order.end(), hit); it != order.end()) {
            panel_focus_ = static_cast<int>(it - order.begin());
            show_focus_ = false;
        }
    }
    switch (hit.part) {
        case Part::Switch:
            focus_ = hit.row;
            shared_focused_ = false;
            show_focus_ = false;
            toggle(hit.row);
            break;
        case Part::Address:
            open_address(hit.row);
            break;
        case Part::Download:
            start_downloads(hit.row);
            break;
        case Part::Row:
            focus_ = hit.row;
            shared_focused_ = false;
            show_focus_ = false;
            panel_focus_ = -1;
            select(hit.project);
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case Part::PanelClose:
        case Part::PanelAddress:
        case Part::PanelTab:
        case Part::PanelButton:
        case Part::PanelField:
        case Part::PanelCopyEnv:
        case Part::PanelPhpMyAdmin:
        case Part::PanelKeep:
        case Part::PanelRemove:
            on_panel_click(hit);
            break;
        case Part::PanelPhpSettings:
        case Part::PanelPhpChange:
        case Part::PanelPhpField:
        case Part::PanelPhpReset:
        case Part::PanelPhpDiscard:
        case Part::PanelPhpSave:
            on_php_panel_click(hit);
            break;
        case Part::Banner:
            if (status_line().clickable && worker_) {
                worker_->start_service();
            }
            break;
        case Part::NewProject:
            open_wizard();
            break;
        case Part::WizCard:
        case Part::WizField:
        case Part::WizBrowse:
        case Part::WizAddFolder:
        case Part::WizFromBackup:
        case Part::WizCancel:
        case Part::WizBack:
        case Part::WizNext:
            // A click isn't a keyboard stop: Enter mustn't press what was
            // clicked unseen. The keys go back to the step's first (a clicked
            // card is the cards' stop: on_wizard_click).
            wizard_.focus = {};
            on_wizard_click(hit);
            break;
        case Part::SharedRow:
        case Part::SharedSwitch:
        case Part::SharedEnd:
        case Part::SharedOpen:
        case Part::SharedGear: {
            // The keys follow the mouse to the row.
            focus_ = -1;
            panel_focus_ = -1;
            shared_focused_ = true;
            show_focus_ = false;
            const std::vector<Part> order = shared_focus_order();
            if (const auto it = std::find(order.begin(), order.end(), hit.part); it != order.end()) {
                shared_part_ = static_cast<int>(it - order.begin());
            }
            run_shared_part(hit.part);
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        case Part::Settings:
            open_settings();
            break;
        case Part::FrLang:
        case Part::FrTick:
        case Part::FrCancel:
        case Part::FrRetry:
        case Part::FrChange:
        case Part::FrCreate:
        case Part::FrAdd:
        case Part::FrBack:
        case Part::FrNext:
        case Part::FrSkipSetup:
        case Part::FrWithout:
            show_focus_ = false;
            on_first_run_click(hit);
            break;
        case Part::UpdateNotes:
        case Part::UpdateCancel:
        case Part::UpdateLater:
        case Part::UpdateNow: {
            // The keys follow the mouse to the banner.
            const std::vector<Part> order = update_focus_order();
            if (const auto it = std::find(order.begin(), order.end(), hit.part); it != order.end()) {
                update_focus_ = static_cast<int>(it - order.begin());
                focus_ = -1;
                shared_focused_ = false;
                panel_focus_ = -1;
                show_focus_ = false;
            }
            run_update_part(hit.part);
            break;
        }
        case Part::WizName:  // a real EDIT: it takes its own clicks
        case Part::None:
            break;
    }
}

void MainWindow::on_key(WPARAM key) {
    // The keys move from the focused row: a right-click's selection ends, so
    // the selected row and the focus frame never disagree.
    if (!highlighted_.empty() && (key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END ||
                                  key == VK_PRIOR || key == VK_NEXT || key == VK_TAB || key == VK_ESCAPE)) {
        highlighted_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    // Esc closes an error first (its close mark can't be reached with Tab);
    // information goes by itself and leaves Esc to the page.
    if (key == VK_ESCAPE && !toast_.text.empty() && (toast_.kind == ToastKind::Error || !toast_.action.empty())) {
        toast_event(ToastEvent::Close);
        return;
    }
    // Ctrl+, opens Settings from anywhere in the window.
    if (key == VK_OEM_COMMA && ctrl_only()) {
        open_settings();
        return;
    }
    // Ctrl+N: New project, the top bar's button (it isn't a Tab stop).
    if (key == 'N' && ctrl_only()) {
        if (!first_run_open() && !wizard_open()) {
            open_wizard();
        }
        return;
    }
    // F5 starts a stopped service from the two pages too (their status line
    // is clickable, and the screen reader's element for it says F5).
    if (key == VK_F5 && worker_ && status_line().clickable && (first_run_open() || wizard_open())) {
        worker_->start_service();
        return;
    }
    if (first_run_open()) {
        on_first_run_key(key);
        return;
    }
    if (wizard_open()) {
        on_wizard_key(key);
        return;
    }
    if (update_focus_ >= 0) {
        on_update_key(key);
        return;
    }
    if (panel_open() && (panel_focus_ >= 0 || !panel_beside_list())) {
        on_panel_key(key);
        return;
    }
    if (shared_focused_ && width_of(shared_geometry_.row) > 0) {
        on_shared_key(key);
        return;
    }
    shared_focused_ = false;
    const int count = static_cast<int>(visible_.size());
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const int page = std::max(1, height_of(list_rect()) / px(kRowH));
    int next = focus_;
    switch (key) {
        case VK_DOWN:
            // Past the last project: the Shared sites row.
            if (focus_ == count - 1 && width_of(shared_geometry_.row) > 0) {
                focus_ = -1;
                shared_focused_ = true;
                shared_part_ = 0;
                show_focus_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            next = std::min(count - 1, focus_ + 1);
            break;
        case VK_UP:
            next = std::max(0, focus_ - 1);
            break;
        case VK_HOME:
            next = 0;
            break;
        case VK_END:
            next = count - 1;
            break;
        case VK_NEXT:
            next = std::min(count - 1, focus_ + page);
            break;
        case VK_PRIOR:
            next = std::max(0, focus_ - page);
            break;
        case VK_SPACE:
            if (focus_ >= 0) {
                toggle(focus_);
            }
            break;
        case VK_RETURN:
            // Opens the project's panel; its first button opens the address.
            if (focus_ >= 0) {
                select(snap_.rows[visible_[static_cast<std::size_t>(focus_)]].name);
            }
            break;
        case VK_ESCAPE:
            close_panel();
            return;
        case VK_TAB:
            if ((GetKeyState(VK_SHIFT) & 0x8000) == 0 && panel_open() && !panel_focus_order().empty()) {
                panel_focus_ = 0;
                show_focus_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            // Shift+Tab: back to the update banner above the list, when it's there.
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0 && focus_update_banner(true)) {
                return;
            }
            SetFocus(search_);
            return;
        case VK_F5:
            // Also the keyboard's way to start a stopped service (the banner).
            if (worker_ && status_line().clickable) {
                worker_->start_service();
            } else if (worker_) {
                worker_->refresh();
            }
            return;
        case 'F':
            if (ctrl) {
                SetFocus(search_);
                SendMessageW(search_, EM_SETSEL, 0, -1);
            }
            return;
        default:
            return;
    }
    show_focus_ = true;
    if (count == 0) {
        next = -1;
    }
    focus_ = next;
    ensure_visible(focus_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK MainWindow::search_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                         DWORD_PTR self_ptr) {
    auto* self = win::int_to_ptr<MainWindow*>(self_ptr);
    switch (msg) {
        case WM_KEYDOWN:
            if (wparam == VK_OEM_COMMA && ctrl_only()) {
                self->open_settings();  // Ctrl+, works with the search box focused too
                return 0;
            }
            if (wparam == 'N' && ctrl_only()) {
                // Ctrl+N too; the keys leave the box only when the page opens.
                if (!self->first_run_open() && !self->wizard_open()) {
                    self->open_wizard();
                    if (self->wizard_open()) {
                        SetFocus(self->hwnd_);
                    }
                }
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                if (GetWindowTextLengthW(hwnd) > 0) {
                    SetWindowTextW(hwnd, L"");
                } else {
                    SetFocus(self->hwnd_);
                }
                return 0;
            }
            // Tab: the update banner's buttons first, when it's there.
            if (wparam == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000) == 0 && self->focus_update_banner(false)) {
                return 0;
            }
            if (wparam == VK_DOWN || wparam == VK_TAB || wparam == VK_RETURN) {
                SetFocus(self->hwnd_);
                if (self->focus_ < 0 && !self->visible_.empty()) {
                    self->focus_ = 0;
                    self->shared_focused_ = false;
                } else if (self->focus_ < 0 && width_of(self->shared_geometry_.row) > 0) {
                    self->shared_focused_ = true;  // no projects listed: the Shared sites row
                }
                self->show_focus_ = true;
                InvalidateRect(self->hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wparam == VK_ESCAPE || wparam == VK_TAB || wparam == VK_RETURN || wparam == 0x0E) {
                return 0;  // no beep (0x0E: Ctrl+N's character)
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        case WM_PAINT: {
            const LRESULT result = DefSubclassProc(hwnd, msg, wparam, lparam);
            if (GetWindowTextLengthW(hwnd) == 0 && GetFocus() != hwnd) {
                self->paint_search_cue(hwnd);
            }
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, search_proc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

void MainWindow::paint_search_cue(HWND edit) const {
    HDC dc = GetDC(edit);
    if (dc == nullptr) {
        return;
    }
    RECT r{};
    SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&r));
    auto* font = win::int_to_ptr<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colorref(palette(dark_).text_subtle));
    DrawTextW(dc, search_cue_.c_str(), static_cast<int>(search_cue_.size()), &r,
              DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | (rtl_ ? UINT{DT_RTLREADING} : 0U));
    SelectObject(dc, old);
    ReleaseDC(edit, dc);
}

void MainWindow::toggle(int visible_row) {
    if (visible_row >= 0 && visible_row < static_cast<int>(visible_.size())) {
        toggle_project(snap_.rows[visible_[static_cast<std::size_t>(visible_row)]].name);
    }
}

bool MainWindow::leaving(const std::string& project) const {
    return removing_.contains(project) || removed_.contains(project) || renaming_.contains(project);
}

void MainWindow::toggle_project(const std::string& project) {
    const auto found = std::find_if(snap_.rows.begin(), snap_.rows.end(),
                                    [&](const ProjectRow& r) { return r.name == project; });
    if (found == snap_.rows.end() || snap_.link != Link::Connected || !worker_ || leaving(project)) {
        return;
    }
    const ProjectRow& row = *found;
    const bool value = !switch_value(row);
    pending_[row.name] = value;
    ++outstanding_[ServiceWorker::on_key(row.name)];
    dismiss_error();
    worker_->set_on(row.name, value);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

MainWindow::RowGeometry::End MainWindow::row_end(const ProjectRow& row, std::string& end_text) const {
    if (const auto it = snap_.downloads.find(row.name); it != snap_.downloads.end() && !it->second.empty()) {
        end_text = it->second;
        return RowGeometry::End::Progress;
    }
    if (installing_.contains(row.name)) {
        const std::vector<Download> downloads = downloads_for(row, snap_.installed);
        const std::string first =
            downloads.empty() ? std::string() : runtime_label(downloads.front().name) +
                                                     (downloads.front().version.empty() ? "" : " " + downloads.front().version);
        end_text = fill(lang_, Str::Installing, {first});
        return RowGeometry::End::Progress;
    }
    // Not before the first runtimes.list answer: nginx would be counted as missing.
    if (row.state == RowState::NeedsDownload && snap_.link == Link::Connected && snap_.has_installed) {
        const std::vector<Download> downloads = downloads_for(row, snap_.installed);
        if (!downloads.empty()) {
            end_text = download_button_text(downloads, lang_);
            return RowGeometry::End::Download;
        }
    }
    end_text = std::string(text(lang_, state_label(row.state)));
    return RowGeometry::End::State;
}

void MainWindow::start_downloads(int visible_row) {
    if (visible_row >= 0 && visible_row < static_cast<int>(visible_.size())) {
        start_downloads_for(snap_.rows[visible_[static_cast<std::size_t>(visible_row)]].name);
    }
}

void MainWindow::start_downloads_for(const std::string& project) {
    const auto found = std::find_if(snap_.rows.begin(), snap_.rows.end(),
                                    [&](const ProjectRow& r) { return r.name == project; });
    if (found == snap_.rows.end() || snap_.link != Link::Connected || !worker_) {
        return;
    }
    const ProjectRow& row = *found;
    const std::vector<Download> downloads = downloads_for(row, snap_.installed);
    if (downloads.empty() || installing_.contains(row.name)) {
        return;
    }
    installing_[row.name] = true;
    ++outstanding_[ServiceWorker::install_key(row.name)];
    dismiss_error();
    worker_->install(row.name, downloads);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::is_settled(const std::string& key) const { return !outstanding_.contains(key); }

void MainWindow::settle(const std::vector<std::string>& keys) {
    for (const std::string& key : keys) {
        const auto it = outstanding_.find(key);
        if (it == outstanding_.end() || --it->second > 0) {
            continue;
        }
        outstanding_.erase(it);
        // The key is "<kind>\n<project>[\n<field>]".
        const std::size_t nl = key.find('\n');
        const std::string kind = key.substr(0, nl);
        const std::string rest = nl == std::string::npos ? std::string() : key.substr(nl + 1);
        if (kind == "on") {
            pending_.erase(rest);
        } else if (kind == "install") {
            installing_.erase(rest);
        } else if (kind == "keep") {
            pending_keep_.erase(rest);
        } else {
            pending_use_.erase(key);
        }
    }
}

void MainWindow::show_notice(std::string message, bool error) {
    toast_ = toast_with(toast_, std::move(message), error ? ToastKind::Error : ToastKind::Info);
    // Said by a screen reader as it comes, without moving the focus.
    access_.notify(toast_.text, error);
    sync_toast_timer();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::dismiss_error() {
    if (toast_.kind == ToastKind::Error) {
        toast_event(ToastEvent::Close);
    }
}

void MainWindow::toast_event(ToastEvent event) {
    const Toast before = toast_;
    toast_ = toast_after(toast_, event);
    if (!(toast_ == before)) {
        sync_toast_timer();
        if (toast_.text.empty()) {
            toast_popup_.hide();  // at once, minimised or not: no flash on restore
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::sync_toast_timer() {
    if (hwnd_ == nullptr) {
        return;
    }
    // One timer, only while information is shown and not being read; set again, it starts over.
    if (toast_timer_runs(toast_)) {
        SetTimer(hwnd_, kToastTimer, static_cast<UINT>(kToastInfoTime.count()), nullptr);
    } else {
        KillTimer(hwnd_, kToastTimer);
    }
}

int MainWindow::toast_bottom() const {
    if (first_run_open() && height_of(first_run_geometry_.content) > 0) {
        return first_run_geometry_.content.bottom - px(kGap);
    }
    return footer_rect().top - px(kGap);
}

// The .env lines hold the database password: they're kept out of Windows'
// clipboard history and cloud clipboard, and wiped from memory once copied.
bool MainWindow::put_on_clipboard(std::string& value) {
    const bool done = put_secret_on_clipboard(hwnd_, value);
    show_notice(done ? std::string(text(lang_, Str::EnvCopied))
                     : fill(lang_, Str::OpenFailed, {"clipboard"}),
                !done);
    return done;
}

void MainWindow::open_address(int visible_row) {
    if (visible_row < 0 || visible_row >= static_cast<int>(visible_.size())) {
        return;
    }
    const ProjectRow& row = snap_.rows[visible_[static_cast<std::size_t>(visible_row)]];
    // Only a project's own local address ever reaches the shell.
    if (!is_openable_address(row.address)) {
        return;
    }
    ShellExecuteW(hwnd_, L"open", w(row.address).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --- tray (D2) ------------------------------------------------------------

void MainWindow::add_tray_icon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = icon_small_.get();
    tray_tip_ = L"FloServer";
    const std::size_t n = std::min(tray_tip_.size(), std::size(nid.szTip) - 1);
    tray_tip_.copy(std::begin(nid.szTip), n);
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (tray_added_) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
    update_tray_tip();
}

void MainWindow::update_tray_tip() {
    if (!tray_added_) {
        return;
    }
    std::wstring tip = L"FloServer\n";
    if (shows_rows() && snap_.link == Link::Connected) {
        tip += w(snap_.footer.counts);
    } else {
        tip += w(status_line().text);
    }
    if (tip == tray_tip_) {
        return;
    }
    tray_tip_ = tip;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    const std::size_t n = std::min(tip.size(), std::size(nid.szTip) - 1);
    tip.copy(std::begin(nid.szTip), n);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void MainWindow::on_tray(LPARAM lparam, WPARAM wparam) {
    switch (LOWORD(lparam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            show_window();
            break;
        case WM_CONTEXTMENU:
            show_tray_menu({GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)});
            break;
        default:
            break;
    }
}

void MainWindow::show_tray_menu(POINT at) {
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    const std::wstring open = w(text(lang_, Str::TrayOpen));
    const std::wstring quit_text = w(text(lang_, Str::TrayQuit));
    AppendMenuW(menu.get(), MF_STRING, kMenuOpen, open.c_str());
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING | (quitting_ ? MF_GRAYED : 0U), kMenuQuit, quit_text.c_str());
    SetMenuDefaultItem(menu.get(), kMenuOpen, FALSE);

    SetForegroundWindow(hwnd_);  // so the menu closes when the user clicks elsewhere
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0 ? UINT{TPM_RIGHTALIGN} : UINT{TPM_LEFTALIGN};
    if (rtl_) {
        flags |= UINT{TPM_LAYOUTRTL};
    }
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    if (command == kMenuOpen) {
        show_window();
    } else if (command == kMenuQuit) {
        quit();
    }
}

// --- painting -------------------------------------------------------------

void MainWindow::on_paint() {
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
            geometry_ = row_geometry(mem.get());
            update_panel_geometry(mem.get());
            place_panel_tips();
            wizard_geometry_ = wizard_open() ? wizard_geometry(mem.get()) : WizardGeometry{};
            shared_geometry_ = !wizard_open() && !first_run_open() ? shared_geometry(mem.get()) : SharedGeometry{};
            update_geometry_ = update_geometry(mem.get());
            first_run_geometry_ = FirstRunGeometry{};
            if (first_run_open()) {
                first_run_geometry_ = first_run_geometry(mem.get(), first_run_.scroll);
                // Kept inside the content when it got shorter (or the window taller).
                const int most = std::max(0, first_run_geometry_.content_h - height_of(first_run_geometry_.content));
                if (first_run_.scroll > most) {
                    first_run_.scroll = most;
                    first_run_geometry_ = first_run_geometry(mem.get(), first_run_.scroll);
                }
                first_run_sync_focus();  // a stop that went (a finished download's Cancel) hands the keys on
            }
            place_scroll_bars();
            toast_geometry_ = toast_geometry(mem.get(), toast_, fonts_.button.get(), width_, toast_bottom(), dpi_, rtl_);
            // Its own popup over the content: nothing under it shows through.
            toast_popup_.show(toast_, toast_geometry_, fonts_.button.get(), dark_, dpi_, rtl_);
            {
                Canvas canvas(mem.get(), width_, rtl_);
                canvas.fill({0, 0, width_, height_}, palette(dark_).window);
                for (const bool text_pass : {false, true}) {
                    if (text_pass) {
                        canvas.begin_text();
                    }
                    if (first_run_open()) {
                        paint_first_run(canvas, mem.get(), text_pass);
                        paint_banner(canvas, text_pass);
                        continue;
                    }
                    paint_top_bar(canvas, mem.get(), text_pass);
                    paint_banner(canvas, text_pass);
                    paint_update_banner(canvas, text_pass);
                    if (wizard_open()) {
                        paint_wizard(canvas, mem.get(), text_pass);
                    } else {
                        paint_list(canvas, mem.get(), text_pass);
                        paint_shared_row(canvas, mem.get(), text_pass);
                        paint_panel(canvas, mem.get(), text_pass);
                    }
                    paint_footer(canvas, mem.get(), text_pass);
                }
            }
            // The bitmap is already laid out right to left; copy it as it is.
            SetLayout(hdc, 0);
            BitBlt(hdc, 0, 0, width_, height_, mem.get(), 0, 0, SRCCOPY);
        }
    }
    EndPaint(hwnd_, &ps);
    access_after_paint();
    if (first_paint_) {
        const std::function<void(HWND)> hook = std::exchange(first_paint_, nullptr);
        hook(hwnd_);
    }
}

void MainWindow::paint_top_bar(Canvas& c, HDC /*dc*/, bool text_pass) const {
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    const RECT search = search_box();
    const RECT add = new_project_button();
    const RECT settings = settings_button();
    if (!text_pass) {
        c.round_rect(search, pxf(4), p.input, p.input_border, border);
        // Magnifier, 14 px on a 16 grid (mirrored in right-to-left, as in the mockup).
        const float k = pxf(14) / 16.0F;
        const auto ix = static_cast<float>(search.left + px(10));
        const auto iy = static_cast<float>(search.top) + (static_cast<float>(height_of(search)) - pxf(14)) / 2.0F;
        c.circle(ix + 7 * k, iy + 7 * k, 5 * k, p.text_subtle, 1.4F * k);
        c.line(ix + 11 * k, iy + 11 * k, ix + 14.5F * k, iy + 14.5F * k, p.text_subtle, 1.4F * k);

        c.round_rect(add, pxf(4), p.accent);

        c.round_rect(settings, pxf(4), hover_.part == Part::Settings ? p.row_hover : p.button, p.button_border,
                     border);
        paint_gear(c, settings, p.text, false);
        return;
    }
    const RECT title{px(kPad), 0, search.left - px(kGap), px(kTopBarH)};
    c.text(title, w(text(lang_, Str::Projects)), fonts_.title.get(), p.text, Align::Start, false);
    c.text(add, w(text(lang_, Str::NewProject)), fonts_.button_bold.get(), p.accent_text, Align::Center, false);
    paint_gear(c, settings, p.text, true);
}

void MainWindow::paint_gear(Canvas& c, const RECT& r, Rgb color, bool text_pass) const {
    if (fonts_.icons) {
        if (text_pass) {
            c.text(r, std::wstring_view(&kIconSettings, 1), fonts_.icons.get(), color, Align::Center, true);
        }
        return;
    }
    if (text_pass) {
        return;
    }
    // No icon font: a gear, 16 px on a 16 grid.
    const float g = pxf(16) / 16.0F;
    const float gx = static_cast<float>(r.left + r.right) / 2.0F - 8 * g;
    const float gy = static_cast<float>(r.top + r.bottom) / 2.0F - 8 * g;
    c.circle(gx + 8 * g, gy + 8 * g, 2.5F * g, color, 1.3F * g);
    constexpr float kSpokes[][4] = {{8, 1.5F, 8, 3.5F},       {8, 12.5F, 8, 14.5F},     {1.5F, 8, 3.5F, 8},
                                    {12.5F, 8, 14.5F, 8},     {3.4F, 3.4F, 4.8F, 4.8F}, {11.2F, 11.2F, 12.6F, 12.6F},
                                    {3.4F, 12.6F, 4.8F, 11.2F}, {11.2F, 4.8F, 12.6F, 3.4F}};
    for (const auto& s : kSpokes) {
        c.line(gx + s[0] * g, gy + s[1] * g, gx + s[2] * g, gy + s[3] * g, color, 1.3F * g);
    }
}

void MainWindow::paint_banner(Canvas& c, bool text_pass) const {
    const BannerText b = status_line();
    if (b.text.empty() || height_of(banner_rect()) <= 0) {
        return;
    }
    const Palette& p = palette(dark_);
    const StateColors& colors = b.error ? p.banner_error : p.banner_info;
    const RECT r = banner_rect();
    if (!text_pass) {
        c.round_rect(r, pxf(4), colors.fill);
        return;
    }
    RECT inner = r;
    InflateRect(&inner, -px(12), 0);
    c.text(inner, w(b.text), fonts_.button.get(), colors.text, Align::Start, false);
}

void MainWindow::paint_list(Canvas& c, HDC /*dc*/, bool text_pass) const {
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    const RECT card = card_rect();
    const RECT list = list_rect();
    if (height_of(card) <= 0 || width_of(card) <= px(2)) {
        return;
    }
    if (!text_pass) {
        c.round_rect(card, pxf(8), p.card, p.card_border, border);
    }

    // Nothing to list: say why (the banner covers "not connected").
    if (geometry_.empty()) {
        if (text_pass && shows_rows()) {
            const std::string message =
                snap_.rows.empty() ? fill_pattern(text(lang_, Str::NoProjects), false, {text(lang_, Str::NewProject)})
                                   : fill_pattern(text(lang_, Str::NoMatches), false, {isolate_auto(query_)});
            c.text(list, w(message), fonts_.body.get(), p.text_subtle, Align::Center, false);
        }
        return;
    }

    const bool has_focus = GetFocus() == hwnd_;
    if (text_pass) {
        c.clip_text(list);
    } else {
        c.clip(list);
    }
    for (std::size_t i = 0; i < geometry_.size(); ++i) {
        const RowGeometry& g = geometry_[i];
        if (g.row.bottom <= list.top || g.row.top >= list.bottom) {
            continue;
        }
        const ProjectRow& row = snap_.rows[visible_[i]];
        const int index = static_cast<int>(i);
        const StateColors& sc = state_colors(p, row.state);
        const bool on = switch_value(row);

        if (!text_pass) {
            const bool selected = (row.name == selected_ && panel_open()) || row.name == highlighted_;
            if (selected) {
                c.fill(g.row, p.selected);
                c.fill({g.row.left, g.row.top, g.row.left + px(3), g.row.bottom}, p.accent);
            } else if (hover_.row == index && hover_.part != Part::None && !is_panel_part(hover_.part)) {
                c.fill(g.row, p.row_hover);
            }
            if (i + 1 < geometry_.size()) {
                c.fill({g.row.left, g.row.bottom - static_cast<int>(border), g.row.right, g.row.bottom}, p.divider);
            }
            paint_switch(c, g.toggle, on, hover_.row == index && hover_.part == Part::Switch);
            for (const RECT& chip : g.chips) {
                c.round_rect(chip, pxf(10), p.chip);
            }
            if (g.end == RowGeometry::End::Download) {
                // A button in the Needs download colours (mockup: "Download 2 runtimes").
                const bool hot = hover_.row == index && hover_.part == Part::Download;
                c.round_rect(g.state, pxf(4), hot ? p.row_hover : p.needs_download.fill, p.needs_download.border, border);
            } else if (g.end == RowGeometry::End::Progress) {
                c.round_rect(g.state, pxf(11), p.waking.fill);
            } else if (sc.border != sc.fill) {
                c.round_rect(g.state, pxf(11), sc.fill, sc.border, border);
            } else {
                c.round_rect(g.state, pxf(11), sc.fill);
            }
            if (show_focus_ && has_focus && focus_ == index && panel_focus_ < 0) {
                RECT ring = g.row;
                InflateRect(&ring, -px(2), -px(2));
                c.round_outline(ring, pxf(4), p.focus, pxf(2));
            }
            continue;
        }

        c.text(g.name, w(row.name), fonts_.body_bold.get(), p.text, Align::Start, true);
        if (!row.address.empty()) {
            const bool openable = g.address.right > g.address.left;
            c.text(g.line2, w(row.address), fonts_.caption.get(), openable ? p.link : p.text_subtle, Align::Start, true);
        } else {
            c.text(g.line2, w(row.detail), fonts_.caption.get(), p.text_subtle, Align::Start, false);
        }
        for (std::size_t k = 0; k < g.chips.size(); ++k) {
            c.text(g.chips[k], w(row.chips[k]), fonts_.caption.get(), p.chip_text, Align::Center, true);
        }
        const Rgb end_color = g.end == RowGeometry::End::Download   ? p.needs_download.text
                              : g.end == RowGeometry::End::Progress ? p.waking.text
                                                                    : sc.text;
        c.text(g.state, w(g.end_text), fonts_.small_bold.get(), end_color, Align::Center, false);
    }

    if (!text_pass) {
        // The scroll bar, when the list is taller than the card.
        list_bar_.paint(c, p);
        c.unclip();
    } else {
        c.unclip_text();
    }
}

void MainWindow::paint_footer(Canvas& c, HDC dc, bool text_pass) const {
    const Palette& p = palette(dark_);
    const RECT f = footer_rect();
    // The service's state (stopped, stopped answering, connecting...) takes
    // the line until it's connected and well; red while it's a problem.
    const BannerText status = status_line();
    const bool live = shows_rows() && snap_.link == Link::Connected && status.text.empty();
    const int mid = (f.top + f.bottom) / 2;
    if (!text_pass) {
        c.fill(f, p.footer);
        c.fill({f.left, f.top, f.right, f.top + std::max(1, px(1))}, p.footer_border);
        if (live || !status.text.empty()) {
            const Rgb dot = !status.text.empty() ? (status.error ? p.problem.text : p.text_subtle)
                                                 : (snap_.footer.any_running ? p.footer_dot : p.text_subtle);
            c.ellipse(static_cast<float>(px(kPad)) + pxf(4), static_cast<float>(mid), pxf(4), dot);
        }
        return;
    }
    if (!status.text.empty()) {
        const RECT line{px(kPad) + px(8) + px(6), f.top, f.right - px(kPad), f.bottom};
        c.text(line, w(status.text), status.error ? fonts_.button_bold.get() : fonts_.caption.get(),
               status.error ? p.problem.text : p.text_subtle, Align::Start, false);
        return;
    }
    if (!live) {
        return;
    }
    const FooterText& t = snap_.footer;
    int x = px(kPad) + px(8) + px(6);
    const std::wstring counts = w(t.counts);
    const int counts_w = Canvas::measure(dc, counts, fonts_.caption.get(), rtl_);
    c.text({x, f.top, x + counts_w + px(2), f.bottom}, counts, fonts_.caption.get(), p.text_subtle, Align::Start, false);
    x += counts_w + px(16);
    const std::wstring memory = w(t.memory);
    const int memory_w = Canvas::measure(dc, memory, fonts_.caption.get(), rtl_);
    const int memory_left = f.right - px(kPad) - memory_w;
    if (!t.nginx.empty() && x < memory_left - px(16)) {
        c.text({x, f.top, memory_left - px(16), f.bottom}, w(t.nginx), fonts_.caption.get(), p.text_subtle,
               Align::Start, false);
    }
    c.text({memory_left - px(2), f.top, f.right - px(kPad), f.bottom}, memory, fonts_.caption.get(), p.text_subtle,
           Align::End, false);
}

}  // namespace floserver::ui

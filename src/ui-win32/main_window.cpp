#include "main_window.h"

#include "main_window_layout.h"
#include "model/theme.h"
#include "model/view_model.h"
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
constexpr UINT_PTR kErrorTimer = 1;
constexpr UINT kErrorShownMs = 8000;
constexpr UINT_PTR kNoticeTimer = 2;
constexpr UINT kNoticeShownMs = 4000;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kSearchSubclass = 1;
constexpr UINT_PTR kTipNewProject = 1;
constexpr UINT_PTR kTipSettings = 2;
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuQuit = 2;

std::wstring w(std::string_view s) { return win::widen(s); }

}  // namespace

MainWindow::MainWindow(HINSTANCE instance, Lang lang, std::wstring class_name)
    : instance_(instance), lang_(lang), rtl_(is_rtl(lang)), class_name_(std::move(class_name)) {}

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
                // Settings belongs to this window; it would otherwise stay
                // on screen with no taskbar button.
                if (settings_ && !settings_->close()) {
                    return 0;  // PHP changes not saved, and the user chose to stay
                }
                ShowWindow(hwnd_, SW_HIDE);
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
            on_mouse_move(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            if (hover_.part != Part::None) {
                hover_ = {};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && win::int_to_ptr<HWND>(wparam) == hwnd_) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd_, &pt);
                const Hit hit = hit_test(pt.x, pt.y);
                const bool hand = hit.part == Part::Switch || hit.part == Part::Address || hit.part == Part::Download ||
                                  hit.part == Part::PanelAddress || hit.part == Part::PanelKeep ||
                                  hit.part == Part::SharedSwitch || hit.part == Part::SharedEnd ||
                                  hit.part == Part::WwwSwitch ||
                                  (hit.part == Part::Banner && banner().clickable);
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            pressed_ = hit_test(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            SetCapture(hwnd_);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const Hit hit = hit_test(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (hit == pressed_) {
                on_click(hit);
            }
            pressed_ = {};
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (wizard_open() || www_open()) {
                return 0;  // the list under the page stays put
            }
            scroll_to(scroll_ - GET_WHEEL_DELTA_WPARAM(wparam) * px(kRowH) / WHEEL_DELTA);
            return 0;
        case WM_KEYDOWN:
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
        case WM_TIMER:
            if (wparam == kErrorTimer || wparam == kNoticeTimer) {
                KillTimer(hwnd_, wparam);
                (wparam == kErrorTimer ? change_error_ : notice_.text).clear();
                layout();
                InvalidateRect(hwnd_, nullptr, FALSE);
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
        case kPhpBackMessage:
            if (leave_php_part(true)) {
                SetFocus(hwnd_);
            }
            return 0;
        case kSettingsClosedMessage:
            if (wparam == 1) {
                quit();  // asked while Settings' save question was open; it's answered now
                return 0;
            }
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

    make_fonts();
    make_icons();
    apply_theme();
    layout();
    add_tray_icon();
    worker_ = std::make_unique<ServiceWorker>(hwnd_, kSnapshotMessage, lang_);
}

void MainWindow::on_destroy() {
    KillTimer(hwnd_, kErrorTimer);
    KillTimer(hwnd_, kNoticeTimer);
    if (tray_added_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof nid;
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        tray_added_ = false;
    }
    settings_.reset();  // before the worker it uses
    worker_.reset();
    PostQuitMessage(0);
}

void MainWindow::on_snapshot() {
    if (!worker_) {
        return;
    }
    Snapshot s = worker_->take();
    // A switch or picker shows the user's choice until its last command has finished.
    settle(s.settled);
    if (!s.change_error.empty()) {
        change_error_ = s.change_error;
        SetTimer(hwnd_, kErrorTimer, kErrorShownMs, nullptr);
    }
    if (!s.clipboard.empty()) {
        put_on_clipboard(s.clipboard);
    }
    if (!s.added.empty()) {
        pending_added_ = s.added;
    }
    const bool quit_done = s.quit_done;
    const std::string quit_error = s.quit_error;
    // A project made from a template that hands over to FloCMS Studio (D9, D14).
    const bool just_made = wizard_running() && s.create.state == "done" && !s.create.project.empty();
    // Keyboard focus stays on the same project when the list changes.
    std::string focused;
    if (focus_ >= 0 && focus_ < static_cast<int>(visible_.size())) {
        focused = snap_.rows[visible_[static_cast<std::size_t>(focus_)]].name;
    }
    snap_ = std::move(s);
    if (settings_) {
        settings_->on_snapshot(snap_);
    }
    snap_.runtimes.reset();  // Settings' alone; not kept once it has it
    update_visible();
    // The panel's project may have been removed (flo remove).
    if (!selected_.empty() && shows_rows() && selected_row() == nullptr) {
        (void)leave_php_part(false);
        close_panel();
    }
    on_project_php_answers();
    sync_project_php();
    update_fields();
    // Shared sites: the row goes when the service stops answering shared.get.
    if (!snap_.shared.known) {
        shared_focused_ = false;
        close_www();
    } else if (www_open()) {
        www_fields_ = shared_fields(snap_.shared, snap_.installed, lang_);
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
    if (just_made && worker_ && wizard_.studio) {
        worker_->open_in_studio(snap_.create.project, true);
    }
    layout();
    update_tray_tip();
    InvalidateRect(hwnd_, nullptr, FALSE);

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
    if (quitting_ || !worker_) {
        return;
    }
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
    if (!worker_ || quitting_) {
        return;
    }
    // An earlier open's answers mustn't show before this one's arrive.
    snap_.runtimes_error.clear();
    snap_.runtimes_refresh = false;
    snap_.settings_error.clear();
    settings_ = std::make_unique<SettingsWindow>(instance_, lang_, *worker_, hwnd_, kSettingsClosedMessage);
    if (!settings_->create(settings_placement_)) {
        settings_.reset();
        return;
    }
    settings_->on_snapshot(snap_);  // the link, what's installed, the installs running
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
    if (banner().text.empty()) {
        return {};
    }
    return {px(kPad), px(kTopBarH), width_ - px(kPad), px(kTopBarH) + px(kBannerH)};
}

RECT MainWindow::card_rect() const {
    const RECT b = banner_rect();
    const int top = b.bottom > 0 ? b.bottom + px(8) : px(kTopBarH);
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
        HDC dc = GetDC(hwnd_);
        place_project_php_edits(dc);
        ReleaseDC(hwnd_, dc);
    }
    for (const auto& [id, rect] : {std::pair{kTipNewProject, new_project_button()},
                                   std::pair{kTipSettings, settings_button()}}) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.hwnd = hwnd_;
        ti.uId = id;
        ti.rect = rect;
        SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

void MainWindow::update_visible() {
    visible_.clear();
    if (shows_rows()) {
        for (std::size_t i = 0; i < snap_.rows.size(); ++i) {
            if (row_matches(snap_.rows[i], query_)) {
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

MainWindow::BannerText MainWindow::banner() const {
    if (!change_error_.empty()) {
        return {change_error_, true, false};
    }
    if (!notice_.text.empty()) {
        return {notice_.text, notice_.error, false};
    }
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
    if (contains(new_project_button(), x, y)) {
        return {Part::NewProject, -1, {}};
    }
    if (contains(settings_button(), x, y)) {
        return {Part::Settings, -1, {}};
    }
    if (contains(banner_rect(), x, y)) {
        return {Part::Banner, -1, {}};
    }
    if (wizard_open()) {
        return contains(wizard_geometry_.area, x, y) ? wizard_hit_test(x, y) : Hit{};
    }
    if (www_open()) {
        return contains(www_geometry_.area, x, y) ? www_hit_test(x, y) : Hit{};
    }
    if (panel_open() && php_open_ && contains(php_geometry_.panel, x, y)) {
        return php_panel_hit_test(x, y);
    }
    if (panel_open() && !php_open_ && contains(panel_geometry_.panel, x, y)) {
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
        case Part::PanelButton:
        case Part::PanelField:
        case Part::PanelCopyEnv:
        case Part::PanelKeep:
            on_panel_click(hit);
            break;
        case Part::PanelPhpBack:
        case Part::PanelPhpSettings:
        case Part::PanelPhpChange:
        case Part::PanelPhpField:
        case Part::PanelPhpReset:
        case Part::PanelPhpDiscard:
        case Part::PanelPhpSave:
            on_php_panel_click(hit);
            break;
        case Part::Banner:
            if (banner().clickable && worker_) {
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
        case Part::WizCancel:
        case Part::WizBack:
        case Part::WizNext:
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
        case Part::WwwSwitch:
        case Part::WwwChange:
        case Part::WwwOpen:
        case Part::WwwField:
        case Part::WwwCopyEnv:
        case Part::WwwClose:
            on_www_click(hit);
            break;
        case Part::Settings:
            open_settings();
            break;
        case Part::None:
            break;
    }
}

void MainWindow::on_key(WPARAM key) {
    // Ctrl+, opens Settings from anywhere in the window.
    if (key == VK_OEM_COMMA && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        open_settings();
        return;
    }
    if (wizard_open()) {
        on_wizard_key(key);
        return;
    }
    if (www_open()) {
        on_www_key(key);
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
            SetFocus(search_);
            return;
        case VK_F5:
            // Also the keyboard's way to start a stopped service (the banner).
            if (worker_ && banner().clickable) {
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
            if (wparam == VK_OEM_COMMA && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                self->open_settings();  // Ctrl+, works with the search box focused too
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
            if (wparam == VK_ESCAPE || wparam == VK_TAB || wparam == VK_RETURN) {
                return 0;  // no beep
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
    if (visible_row < 0 || visible_row >= static_cast<int>(visible_.size()) || snap_.link != Link::Connected ||
        !worker_) {
        return;
    }
    const ProjectRow& row = snap_.rows[visible_[static_cast<std::size_t>(visible_row)]];
    const bool value = !switch_value(row);
    pending_[row.name] = value;
    ++outstanding_[ServiceWorker::on_key(row.name)];
    change_error_.clear();
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
    change_error_.clear();
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
    notice_ = {std::move(message), error};
    SetTimer(hwnd_, kNoticeTimer, kNoticeShownMs, nullptr);
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// The .env lines hold the database password: they're kept out of Windows'
// clipboard history and cloud clipboard, and wiped from memory once copied.
void MainWindow::put_on_clipboard(std::string& value) {
    std::wstring wide = w(value);
    SecureZeroMemory(value.data(), value.size());
    value.clear();
    bool done = false;
    if (OpenClipboard(hwnd_) != FALSE) {
        EmptyClipboard();
        const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL text = GlobalAlloc(GMEM_MOVEABLE, bytes); text != nullptr) {
            if (void* to = GlobalLock(text); to != nullptr) {
                std::memcpy(to, wide.c_str(), bytes);
                GlobalUnlock(text);
                done = SetClipboardData(CF_UNICODETEXT, text) != nullptr;
            }
            if (!done) {
                GlobalFree(text);
            }
        }
        for (const wchar_t* name : {L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard",
                                    L"ExcludeClipboardContentFromMonitorProcessing"}) {
            const UINT format = RegisterClipboardFormatW(name);
            if (HGLOBAL zero = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD)); format != 0 && zero != nullptr) {
                if (auto* d = static_cast<DWORD*>(GlobalLock(zero)); d != nullptr) {
                    *d = 0;
                    GlobalUnlock(zero);
                }
                if (SetClipboardData(format, zero) == nullptr) {
                    GlobalFree(zero);
                }
            }
        }
        CloseClipboard();
    }
    SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
    show_notice(done ? std::string(text(lang_, Str::EnvCopied))
                     : fill(lang_, Str::OpenFailed, {"clipboard"}),
                !done);
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
        tip += w(banner().text);
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
            panel_geometry_ = panel_open() ? panel_geometry(mem.get()) : PanelGeometry{};
            php_geometry_ = panel_open() && php_open_ ? php_panel_geometry(mem.get()) : PhpPanelGeometry{};
            wizard_geometry_ = wizard_open() ? wizard_geometry(mem.get()) : WizardGeometry{};
            const bool list_shown = !wizard_open() && !www_open();
            shared_geometry_ = list_shown ? shared_geometry(mem.get()) : SharedGeometry{};
            www_geometry_ = www_open() && !wizard_open() ? www_geometry(mem.get()) : WwwGeometry{};
            {
                Canvas canvas(mem.get(), width_, rtl_);
                canvas.fill({0, 0, width_, height_}, palette(dark_).window);
                for (const bool text_pass : {false, true}) {
                    if (text_pass) {
                        canvas.begin_text();
                    }
                    paint_top_bar(canvas, mem.get(), text_pass);
                    paint_banner(canvas, text_pass);
                    if (wizard_open()) {
                        paint_wizard(canvas, mem.get(), text_pass);
                    } else if (www_open()) {
                        paint_www(canvas, mem.get(), text_pass);
                    } else {
                        paint_list(canvas, mem.get(), text_pass);
                        paint_shared_row(canvas, mem.get(), text_pass);
                        if (php_open_) {
                            paint_php_panel(canvas, text_pass);
                        } else {
                            paint_panel(canvas, mem.get(), text_pass);
                        }
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
    const BannerText b = banner();
    if (b.text.empty()) {
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
            const bool selected = row.name == selected_ && panel_open();
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
        // A thin scroll indicator when the list is taller than the card.
        const int view = height_of(list);
        const int content = list_height();
        if (content > view && view > 0) {
            const int thumb = std::max(px(24), view * view / content);
            const int top = list.top + (view - thumb) * scroll_ / (content - view);
            c.round_rect({list.right - px(6), top + px(2), list.right - px(2), top + thumb - px(2)}, pxf(2),
                         p.switch_off_border);
        }
        c.unclip();
    } else {
        c.unclip_text();
    }
}

void MainWindow::paint_footer(Canvas& c, HDC dc, bool text_pass) const {
    const Palette& p = palette(dark_);
    const RECT f = footer_rect();
    const bool live = shows_rows() && snap_.link == Link::Connected;
    const int mid = (f.top + f.bottom) / 2;
    if (!text_pass) {
        c.fill(f, p.footer);
        c.fill({f.left, f.top, f.right, f.top + std::max(1, px(1))}, p.footer_border);
        if (live) {
            c.ellipse(static_cast<float>(px(kPad)) + pxf(4), static_cast<float>(mid), pxf(4),
                      snap_.footer.any_running ? p.footer_dot : p.text_subtle);
        }
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

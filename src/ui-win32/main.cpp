// FloServer.exe: the Windows window (D1, D2). Plain Win32 with common
// controls v6 and per-monitor DPI v2 (both from the manifest). It's only a
// client of floserverd: everything it shows and changes goes through the
// service (main_window.h, service_worker.h).

#include "apply_update.h"
#include "canvas.h"
#include "main_window.h"
#include "model/ui_strings.h"
#include "win_util.h"
#include "window_prefs.h"
#include "window_style.h"

#include "floserver/core/ipc.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

using floserver::ui::win::widen;

// One window per user session and service instance: a second FloServer.exe
// brings the first one forward and ends.
bool hand_over_to_running_window(const std::wstring& class_name) {
    // The first copy may still be opening its window; give it a moment.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (HWND existing = FindWindowW(class_name.c_str(), nullptr); existing != nullptr) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, floserver::ui::kShowWindowMessage, 0, 0);
            return true;
        }
        Sleep(100);
    }
    return false;
}

// FloServer.exe's arguments, without the program: [--tray] [--instance
// <suffix>] as the Run entry writes them, or --apply-update <msi> ... for
// the copy that installs an update.
std::vector<std::string> command_args() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return {};
    }
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(floserver::ui::win::narrow(argv[i]));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv from Windows
    }
    LocalFree(argv);
    return args;
}

// COM for this thread, for as long as the window lives.
class ComSession {
public:
    ComSession() : ok_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {}
    ComSession(const ComSession&) = delete;
    ComSession& operator=(const ComSession&) = delete;
    ComSession(ComSession&&) = delete;
    ComSession& operator=(ComSession&&) = delete;
    ~ComSession() {
        if (ok_) {
            CoUninitialize();
        }
    }

private:
    bool ok_;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev*/, PWSTR /*cmd*/, int show) {
    // Started with Windows (Settings > General's Run entry): --tray makes the
    // window but doesn't show it (the service starts as it always does), and
    // --instance stands for FLOSERVER_INSTANCE, which floserverd inherits.
    const std::vector<std::string> command = command_args();
    // Update now (Q5): this is the copy in %TEMP% that installs the update.
    // Not one of the windows: it never takes their single-instance mark, so
    // the FloServer it opens at the end can.
    if (const auto update = floserver::ui::parse_apply_update_args(command)) {
        if (!update->instance.empty()) {
            SetEnvironmentVariableW(L"FLOSERVER_INSTANCE", widen(update->instance).c_str());
        }
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof icc;
        icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
        const floserver::ui::Lang lang =
            update->lang.value_or(floserver::ui::window_lang(floserver::ui::load_window_prefs()));
        return floserver::ui::run_apply_update(instance, lang, *update);
    }
    const floserver::ui::StartArgs args = floserver::ui::parse_start_args(command);
    const bool tray = args.tray;
    if (!args.instance.empty()) {
        SetEnvironmentVariableW(L"FLOSERVER_INSTANCE", widen(args.instance).c_str());
    }
    const std::string service_instance = floserver::core::ipc::default_instance();
    const std::wstring class_name = L"FloServerWindow." + widen(service_instance);
    const std::wstring mutex_name = floserver::ui::window_mark_name(service_instance);

    // An update is being installed (apply_update.h): no window now, or it
    // would start floserverd again in the middle. The new one opens by itself.
    if (const floserver::ui::win::UniqueHandle updating(
            OpenMutexW(SYNCHRONIZE, FALSE, floserver::ui::updating_mark_name(service_instance).c_str()));
        updating) {
        if (!tray) {
            const floserver::ui::Lang lang = floserver::ui::window_lang(floserver::ui::load_window_prefs());
            MessageBoxW(nullptr, widen(floserver::ui::text(lang, floserver::ui::Str::UpdateInProgress)).c_str(),
                        L"FloServer",
                        MB_OK | MB_ICONINFORMATION |
                            (floserver::ui::is_rtl(lang) ? UINT{MB_RTLREADING | MB_RIGHT} : 0U));
        }
        return 0;
    }

    const floserver::ui::win::UniqueHandle single(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (tray) {
            return 0;  // already running: nothing to bring forward at sign-in
        }
        return hand_over_to_running_window(class_name) ? 0 : 1;
    }

    // Opening an address goes through the shell, which may use COM.
    const ComSession com;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    const floserver::ui::GdiplusSession gdiplus;
    if (!gdiplus.ok()) {
        return 1;
    }

    // Settings > General's choices (window.json): the language (else
    // FLOSERVER_LANG, else Windows' display language) and the theme.
    const floserver::ui::WindowPrefs prefs = floserver::ui::load_window_prefs();
    floserver::ui::set_theme_choice(prefs.theme);
    floserver::ui::MainWindow window(instance, floserver::ui::window_lang(prefs), prefs, class_name);
    if (!window.create(tray ? SW_HIDE : show)) {
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

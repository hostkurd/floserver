// FloServer.exe: the Windows window (D1, D2). Plain Win32 with common
// controls v6 and per-monitor DPI v2 (both from the manifest). It's only a
// client of floserverd: everything it shows and changes goes through the
// service (main_window.h, service_worker.h).

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

// FloServer.exe --tray (the Run entry's "Start in the tray").
bool started_in_tray() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return false;
    }
    const std::wstring flag = widen(floserver::ui::kTrayFlag);
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (flag == argv[i]) {  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv from Windows
            found = true;
        }
    }
    LocalFree(argv);
    return found;
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
    const std::string service_instance = floserver::core::ipc::default_instance();
    const std::wstring class_name = L"FloServerWindow." + widen(service_instance);
    const std::wstring mutex_name = L"Local\\FloServerWindow." + widen(service_instance);
    // Started with Windows in the tray (Settings > General's Run entry): the
    // window is made but not shown; the service starts as it always does.
    const bool tray = started_in_tray();

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

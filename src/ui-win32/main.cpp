// FloServer.exe: the Windows window (D1, D2). Plain Win32 with common
// controls v6 and per-monitor DPI v2 (both from the manifest). It's only a
// client of floserverd: everything it shows and changes goes through the
// service (main_window.h, service_worker.h).

#include "canvas.h"
#include "main_window.h"
#include "model/ui_strings.h"
#include "win_util.h"

#include "floserver/core/ipc.h"
#include "floserver/core/process.h"

#include <windows.h>
#include <commctrl.h>
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

    const floserver::ui::win::UniqueHandle single(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
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

    // The language setting moves to the service later; for now FLOSERVER_LANG (en, ar, ku).
    const auto lang_setting = floserver::core::get_env("FLOSERVER_LANG");
    floserver::ui::MainWindow window(instance, floserver::ui::lang_from_setting(lang_setting ? lang_setting->c_str() : nullptr),
                                     class_name);
    if (!window.create(show)) {
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

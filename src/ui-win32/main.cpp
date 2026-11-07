// FloServer.exe: the Windows window (D1, D2). Plain Win32 with common
// controls v6 and per-monitor DPI v2 (both from the manifest). It's only a
// client of floserverd: everything it shows and changes goes through the
// service (main_window.h, service_worker.h).

#include "apply_update.h"
#include "canvas.h"
#include "main_window.h"
#include "model/start_measure.h"
#include "model/ui_strings.h"
#include "model/uninstall.h"
#include "setup_dll.h"
#include "win_util.h"
#include "window_prefs.h"
#include "window_style.h"

#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <psapi.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using floserver::ui::win::widen;

// One window per user session and service instance: a second FloServer.exe
// brings the first one forward and ends, handing over a .zip it was opened
// with (New project from a backup there).
bool hand_over_to_running_window(const std::wstring& class_name, const std::string& open, bool uninstall) {
    // The first copy may still be opening its window; give it a moment.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (HWND existing = FindWindowW(class_name.c_str(), nullptr); existing != nullptr) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, uninstall ? floserver::ui::kShowUninstallMessage : floserver::ui::kShowWindowMessage, 0,
                         0);
            if (!open.empty()) {
                COPYDATASTRUCT data{};
                data.dwData = floserver::ui::MainWindow::kOpenBackupData;
                data.cbData = static_cast<DWORD>(open.size());
                data.lpData = const_cast<char*>(open.data());  // NOLINT(cppcoreguidelines-pro-type-const-cast): only read
                DWORD_PTR ignored = 0;
                SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000,
                                    &ignored);
            }
            return true;
        }
        Sleep(100);
    }
    return false;
}

bool mark_exists(const std::wstring& name) {
    return floserver::ui::win::UniqueHandle(OpenMutexW(SYNCHRONIZE, FALSE, name.c_str())) != nullptr;
}

bool quitting_mark_exists(const std::string& service_instance) {
    return floserver::ui::win::UniqueHandle(
               OpenEventW(SYNCHRONIZE, FALSE, floserver::ui::quitting_mark_name(service_instance).c_str())) != nullptr;
}

// How waiting for a FloServer that's quitting ended.
enum class QuitWait : std::uint8_t { Gone, Updating, TooLong };

// Started again while the open FloServer quits (its projects and the
// service stopping can take a while): it would find that one's mark and
// end, and nothing would open. Instead this one waits, with nothing shown,
// for that process to end, which hands it the mark (`single`, a mutex the
// process with the window owns), then opens as usual. An update that
// begins meanwhile wins: its installer waits for the mark to go.
QuitWait wait_for_quitting_window(HANDLE single, const std::string& service_instance) {
    constexpr DWORD kStepMs = 100;
    constexpr DWORD kLimitMs = 30000;
    const std::wstring updating = floserver::ui::updating_mark_name(service_instance);
    for (DWORD waited = 0; waited < kLimitMs; waited += kStepMs) {
        if (mark_exists(updating)) {
            return QuitWait::Updating;
        }
        const DWORD got = WaitForSingleObject(single, kStepMs);
        if (got == WAIT_OBJECT_0 || got == WAIT_ABANDONED) {
            // The mark is this process's now. An update that started just
            // before still comes first.
            if (mark_exists(updating)) {
                ReleaseMutex(single);
                return QuitWait::Updating;
            }
            return QuitWait::Gone;
        }
        if (got != WAIT_TIMEOUT) {
            break;
        }
    }
    return QuitWait::TooLong;
}

void say_update_in_progress() {
    const floserver::ui::Lang lang = floserver::ui::window_lang(floserver::ui::load_window_prefs());
    MessageBoxW(nullptr, widen(floserver::ui::text(lang, floserver::ui::Str::UpdateInProgress)).c_str(), L"FloServer",
                MB_OK | MB_ICONINFORMATION | (floserver::ui::is_rtl(lang) ? UINT{MB_RTLREADING | MB_RIGHT} : 0U));
}

// FloServer.exe's arguments, without the program: [--tray] [--instance
// <suffix>] as the Run entry writes them, --measure <file> for CI (D15), or
// --apply-update <msi> ... for the copy that installs an update.
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

std::uint64_t ticks(const FILETIME& t) { return (std::uint64_t{t.dwHighDateTime} << 32U) | t.dwLowDateTime; }

// FloServer.exe --measure <file> (D15, checked in CI): the time from this
// process's start to the window's first finished paint, then, a moment
// later, its memory, written to <file>. Then the window closes; the service
// keeps running (this isn't the tray's Quit). Nothing of it on a normal start.
class StartMeter {
public:
    explicit StartMeter(std::string file) : file_(std::move(file)) {}

    void painted(HWND window) {
        window_ = window;
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        FILETIME now{};
        GetSystemTimePreciseAsFileTime(&now);
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE) {
            measure_.first_window_ms = floserver::ui::filetime_ms_between(ticks(created), ticks(now));
        }
        // A thread timer: no window of its own, so it can't reach the window's.
        timer_ = SetTimer(nullptr, 0, static_cast<UINT>(floserver::ui::kMeasureSettle.count()), nullptr);
        if (timer_ == 0) {
            finish();
        }
    }

    // The timer's WM_TIMER, which has no window: the message loop hands it here.
    [[nodiscard]] bool takes(const MSG& msg) const {
        return timer_ != 0 && msg.hwnd == nullptr && msg.message == WM_TIMER && msg.wParam == timer_;
    }

    void finish() {
        if (timer_ != 0) {
            KillTimer(nullptr, timer_);
            timer_ = 0;
        }
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof counters;
        if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): the _EX form, as its cb says
                                    sizeof counters) != FALSE) {
            measure_.private_bytes = counters.PrivateUsage;
            measure_.working_set_bytes = counters.WorkingSetSize;
            std::string error;
            written_ = floserver::core::write_file_atomically(floserver::core::path_from_utf8(file_),
                                                              floserver::ui::measure_text(measure_), error);
        }
        // DestroyWindow, not WM_CLOSE: closing may be set to quit, service and all.
        if (IsWindow(window_) != FALSE) {
            DestroyWindow(window_);  // its WM_DESTROY ends the message loop
        } else {
            PostQuitMessage(1);
        }
    }

    [[nodiscard]] bool written() const { return written_; }

private:
    std::string file_;
    HWND window_ = nullptr;
    UINT_PTR timer_ = 0;
    floserver::ui::StartMeasure measure_;
    bool written_ = false;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev*/, PWSTR /*cmd*/, int show) {
    // Started with Windows (Settings > General's Run entry): --tray makes the
    // window but doesn't show it (the service starts as it always does), and
    // --instance stands for FLOSERVER_INSTANCE, which floserverd inherits.
    const std::vector<std::string> command = command_args();
    // Remove FloServer (D25): this is the copy in %TEMP% that removes the program.
    if (const auto plan = floserver::ui::parse_remove_program_args(command)) {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof icc;
        icc.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
        return floserver::ui::run_remove_program(*plan);
    }
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
    std::optional<StartMeter> meter;
    if (auto file = floserver::ui::parse_measure_args(command)) {
        meter.emplace(std::move(*file));
    }
    // Windows' Settings -> Apps -> Uninstall (D25): only the Uninstall window shows.
    const bool uninstall = args.uninstall && !meter;
    const bool tray = (args.tray || uninstall) && !meter;  // measured: always shown
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
            say_update_in_progress();
        }
        return 0;
    }

    // Owned by this process's thread for as long as it runs: when it ends,
    // a copy waiting for it (wait_for_quitting_window) is handed the mark.
    const floserver::ui::win::UniqueHandle single(CreateMutexW(nullptr, TRUE, mutex_name.c_str()));
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        bool wait = quitting_mark_exists(service_instance);
        if (!wait) {
            if (tray && !uninstall) {
                return 0;  // already running: nothing to bring forward at sign-in
            }
            if (hand_over_to_running_window(class_name, args.open, uninstall)) {
                return 0;
            }
            wait = quitting_mark_exists(service_instance);  // it began quitting meanwhile (its window went)
            if (!wait) {
                return 1;
            }
        }
        switch (wait_for_quitting_window(single.get(), service_instance)) {
            case QuitWait::Gone:
                break;  // opens as usual
            case QuitWait::Updating:
                if (!tray) {
                    say_update_in_progress();
                }
                return 0;
            case QuitWait::TooLong:
                return 1;  // still quitting: starting FloServer again later opens it
        }
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
    if (meter) {
        window.on_first_paint([&meter](HWND hwnd) { meter->painted(hwnd); });
    }
    if (!window.create(tray ? SW_HIDE : show)) {
        return 1;
    }
    if (!args.open.empty()) {
        window.open_backup(args.open);
    }
    if (uninstall) {
        window.open_uninstall(true);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (meter && meter->takes(msg)) {
            meter->finish();
            continue;
        }
        if (window.dialog_message(msg)) {
            continue;  // Tab, Enter and Esc in the Uninstall window
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (meter && !meter->written()) {
        return 1;
    }
    return static_cast<int>(msg.wParam);
}

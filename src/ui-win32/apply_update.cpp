#include "apply_update.h"

#include "access_provider.h"
#include "form_parts.h"
#include "model/general_page.h"
#include "win_util.h"

#include "floserver/core/downloader.h"
#include "floserver/core/encoding.h"
#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/service_client.h"
#include "floserver/core/sha256.h"

#include <commctrl.h>
#include <msi.h>
#include <msiquery.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

#if defined(__MINGW32__)
// In msi.lib, but not in MinGW-w64's msi.h (Windows Installer 3.1 and later).
extern "C" UINT WINAPI MsiSetExternalUIRecord(INSTALLUI_HANDLER_RECORD handler, DWORD filter, LPVOID context,
                                              PINSTALLUI_HANDLER_RECORD previous);
#endif

namespace floserver::ui {

namespace {

namespace core = floserver::core;
namespace fs = std::filesystem;

// The dialog's controls.
constexpr int kTitle = 300;
constexpr int kText = 301;
constexpr int kMarkFirst = 310;   // a step's tick, + its index
constexpr int kLabelFirst = 320;  // a step's words, + its index
constexpr int kBar = 330;
constexpr int kError = 331;
constexpr int kOpenInstaller = 332;
// From the work's thread: a step began (wparam its index), the bar moved
// (wparam per mille), it failed (the message is in the box), it's done.
constexpr UINT kStepMessage = WM_APP + 60;
constexpr UINT kProgressMessage = WM_APP + 61;
constexpr UINT kFailedMessage = WM_APP + 62;
constexpr UINT kDoneMessage = WM_APP + 63;
constexpr UINT kRestartMessage = WM_APP + 64;  // installed; Windows must restart to finish

// Glyphs in Windows' icon font (Segoe Fluent Icons, Segoe MDL2 Assets).
constexpr wchar_t kIconCheck = L'';
constexpr wchar_t kIconSync = L'';
constexpr wchar_t kIconCancel = L'';

std::wstring w(std::string_view s) { return win::widen(s); }

// What the dialog and the work's thread share. The thread may outlive the
// dialog by a moment: it posts only while `dialog` is set.
struct Box {
    std::mutex mutex;
    HWND dialog = nullptr;
    // The window's Update-now mark (update_quitting_mark_name), held from the
    // start until the updating mark is made, or the update fails before it.
    win::UniqueHandle quit_mark;
    std::string error;
    MsiProgress progress;
    int last_permille = -1;
    std::optional<UpdateStatus> checked;  // what the service said the installer is
    bool verified = false;                // it was that, hashed again before installing

    void post(UINT message, WPARAM wparam = 0) {
        const std::lock_guard lock(mutex);
        if (dialog != nullptr) {
            PostMessageW(dialog, message, wparam, 0);
        }
    }
};

struct Dialog {
    Lang lang = Lang::En;
    ApplyUpdateArgs args;
    std::string version;
    std::shared_ptr<Box> box = std::make_shared<Box>();
    ApplyStep step = ApplyStep::Checked;
    bool failed = false;
    bool restart = false;  // installed; Windows must restart to finish
    bool running = true;
    HFONT bold = nullptr;
    HFONT icons = nullptr;
    HWND bar = nullptr;
};

// The installed FloServer.exe: %LOCALAPPDATA%\Programs\FloServer, as the MSI puts it.
std::optional<fs::path> installed_exe() {
    const auto local = core::get_env("LOCALAPPDATA");
    if (!local || local->empty()) {
        return std::nullopt;
    }
    return core::path_from_utf8(*local) / L"Programs" / L"FloServer" / L"FloServer.exe";
}

// Starts the installed FloServer (it starts the service and the projects that were on).
bool open_floserver(const std::string& instance, std::string& error) {
    const auto exe = installed_exe();
    std::error_code ec;
    if (!exe || !fs::is_regular_file(*exe, ec)) {
        error = exe ? core::path_to_utf8(*exe) : std::string("LOCALAPPDATA");
        return false;
    }
    std::wstring command = L"\"" + exe->wstring() + L"\"";
    if (!instance.empty()) {
        command += L" " + w(kInstanceFlag) + L" " + w(instance);
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const std::wstring dir = exe->parent_path().wstring();
    if (CreateProcessW(exe->c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi) ==
        FALSE) {
        error = core::error_text({static_cast<int>(GetLastError()), std::system_category()});
        return false;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Windows Installer's messages while it installs: its progress, and the
// words of an error (said if the install fails). On the work's thread.
int WINAPI msi_handler(LPVOID context, UINT type, MSIHANDLE record) {
    auto* box = static_cast<Box*>(context);
    const UINT kind = type & 0xFF000000U;
    if (kind == static_cast<UINT>(INSTALLMESSAGE_PROGRESS) || kind == static_cast<UINT>(INSTALLMESSAGE_ACTIONDATA)) {
        int permille = 0;
        {
            const std::lock_guard lock(box->mutex);
            if (kind == static_cast<UINT>(INSTALLMESSAGE_PROGRESS)) {
                box->progress.on_record(MsiRecordGetInteger(record, 1), MsiRecordGetInteger(record, 2),
                                        MsiRecordGetInteger(record, 3), MsiRecordGetInteger(record, 4));
            } else {
                box->progress.on_action_data();
            }
            permille = static_cast<int>(box->progress.fraction() * 1000);
            if (permille == box->last_permille) {
                return 0;
            }
            box->last_permille = permille;
        }
        box->post(kProgressMessage, static_cast<WPARAM>(permille));
        return 0;
    }
    if (kind == static_cast<UINT>(INSTALLMESSAGE_ERROR) || kind == static_cast<UINT>(INSTALLMESSAGE_FATALEXIT)) {
        std::array<wchar_t, 1024> buf{};
        DWORD size = static_cast<DWORD>(buf.size());
        if (MsiFormatRecordW(0, record, buf.data(), &size) == ERROR_SUCCESS) {
            const std::lock_guard lock(box->mutex);
            box->error = win::narrow(std::wstring_view(buf.data(), std::min<std::size_t>(size, buf.size() - 1)));
        }
        return 0;  // Windows Installer decides; nothing is asked
    }
    return 0;
}

// The installer opened so that nothing can change or delete it while it's
// open (others may only read it, as Windows Installer does); empty when it
// can't be.
win::UniqueHandle open_installer(const fs::path& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    return win::UniqueHandle(h == INVALID_HANDLE_VALUE ? nullptr : h);
}

// The size and SHA-256 (hex) of what `file` holds, read through it from the start.
std::optional<std::pair<std::uint64_t, std::string>> hash_installer(HANDLE file) {
    LARGE_INTEGER zero{};
    if (file == nullptr || SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) == FALSE) {
        return std::nullopt;
    }
    core::Sha256 hash;
    std::uint64_t size = 0;
    std::vector<std::uint8_t> buf(std::size_t{256} * 1024);
    while (true) {
        DWORD got = 0;
        if (ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) == FALSE) {
            return std::nullopt;
        }
        if (got == 0) {
            break;
        }
        hash.update(std::span<const std::uint8_t>(buf.data(), got));
        size += got;
    }
    const core::Sha256Digest digest = hash.finish();
    return std::pair{size, core::to_hex(digest)};
}

// The installer is the service's file and still what the service checked:
// opened (and held, while `held` lives) and hashed again now.
bool still_checked(const UpdateStatus& checked, const std::string& msi, win::UniqueHandle& held) {
    std::error_code ec;
    if (checked.file.empty() || !fs::equivalent(core::path_from_utf8(checked.file), core::path_from_utf8(msi), ec)) {
        return false;
    }
    held = open_installer(core::path_from_utf8(msi));
    const auto now = hash_installer(held.get());
    return now && installer_matches(checked, now->first, now->second);
}

// The steps, on their own thread; each said to the dialog as it begins.
void apply(const std::shared_ptr<Box>& box, const ApplyUpdateArgs& args, Lang lang) {
    const std::string instance = core::ipc::default_instance();
    const auto fail = [&](std::string message) {
        box->quit_mark.reset();  // only this thread uses it once it runs
        {
            const std::lock_guard lock(box->mutex);
            box->error = std::move(message);
        }
        box->post(kFailedMessage);
    };

    // 1. The service says it's the update it downloaded and checked, and what it should be.
    box->post(kStepMessage, static_cast<WPARAM>(ApplyStep::Checked));
    std::string error;
    std::optional<UpdateStatus> status;
    if (auto client = core::ServiceClient::connect(instance, error)) {
        if (const auto reply = client->call("updates.status"); reply && reply->ok) {
            status = update_status_from(reply->result);
        }
    }
    std::error_code ec;
    if (!status || status->state != UpdateState::Ready || status->sha256.size() != 64 || status->size == 0 ||
        !fs::equivalent(core::path_from_utf8(status->file), core::path_from_utf8(args.msi), ec)) {
        fail(std::string(text(lang, Str::UpdateErrNotReady)));
        return;
    }
    {
        const std::lock_guard lock(box->mutex);
        box->checked = *status;
    }

    // 2. Marked as updating first, so no FloServer window opens (and starts
    //    the service again) meanwhile; then, once the old window has gone
    //    (its single-instance mark goes with it), the service and
    //    everything it runs stop.
    box->post(kStepMessage, static_cast<WPARAM>(ApplyStep::Stopped));
    win::UniqueHandle updating(CreateMutexW(nullptr, FALSE, updating_mark_name(instance).c_str()));
    if (!updating || GetLastError() == ERROR_ALREADY_EXISTS) {
        fail(std::string(text(lang, Str::UpdateErrBusy)));
        return;
    }
    box->quit_mark.reset();  // the updating mark keeps FloServer from opening now
    const std::wstring mark = window_mark_name(instance);
    bool window_gone = false;
    for (int i = 0; i < 100 && !window_gone; ++i) {
        const win::UniqueHandle still(OpenMutexW(SYNCHRONIZE, FALSE, mark.c_str()));
        window_gone = !still;
        if (!window_gone) {
            Sleep(100);
        }
    }
    if (!window_gone) {
        fail(std::string(text(lang, Str::UpdateErrWindow)));
        return;
    }
    if (!core::stop_service(instance, error)) {
        fail(fill(lang, Str::UpdateErrStop, {error}));
        return;
    }

    // 3. Checked once more, right before Windows Installer reads it, through
    //    a handle held until it's done: nothing can change it in between.
    box->post(kStepMessage, static_cast<WPARAM>(ApplyStep::Installing));
    win::UniqueHandle held;
    if (!still_checked(*status, args.msi, held)) {
        fail(std::string(text(lang, Str::UpdateErrChanged)));
        return;
    }
    {
        const std::lock_guard lock(box->mutex);
        box->verified = true;
        box->error.clear();
    }
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    constexpr auto kMessages = static_cast<DWORD>(INSTALLLOGMODE_PROGRESS) | static_cast<DWORD>(INSTALLLOGMODE_ACTIONDATA) |
                               static_cast<DWORD>(INSTALLLOGMODE_ERROR) | static_cast<DWORD>(INSTALLLOGMODE_FATALEXIT);
    MsiSetExternalUIRecord(msi_handler, kMessages, box.get(), nullptr);
    const UINT result = MsiInstallProductW(w(args.msi).c_str(), L"REBOOT=ReallySuppress");
    MsiSetExternalUIRecord(nullptr, 0, nullptr, nullptr);
    held.reset();
    if (result == ERROR_SUCCESS_REBOOT_REQUIRED) {
        // Installed, but Windows finishes it at a restart: FloServer isn't
        // opened now (some of its files are still to be replaced).
        box->post(kProgressMessage, 1000);
        box->post(kRestartMessage);
        return;
    }
    if (result != ERROR_SUCCESS) {
        std::string said;
        {
            const std::lock_guard lock(box->mutex);
            said = box->error;
        }
        std::string message = fill(lang, Str::UpdateErrInstall, {std::to_string(result)});
        if (!said.empty()) {
            message += ": " + said;
        }
        fail(std::move(message));
        return;
    }
    box->post(kProgressMessage, 1000);
    // Done installing: the new FloServer may start the service again.
    updating.reset();

    // 4. The new FloServer, which starts the service and the projects.
    box->post(kStepMessage, static_cast<WPARAM>(ApplyStep::Opening));
    if (!open_floserver(args.instance, error)) {
        fail(fill(lang, Str::UpdateErrOpen, {error}));
        return;
    }
    // The installer it used is this version's now: the service deletes it at its first check.
    box->post(kDoneMessage);
}

void show_marks(HWND dialog, const Dialog& d) {
    const std::vector<StepMark> marks = d.restart ? apply_marks_restart() : apply_marks(d.step, d.failed);
    for (std::size_t i = 0; i < marks.size(); ++i) {
        HWND mark = GetDlgItem(dialog, kMarkFirst + static_cast<int>(i));
        wchar_t glyph = 0;
        const wchar_t* fallback = L"";
        switch (marks[i]) {
            case StepMark::Done:
                glyph = kIconCheck;
                fallback = L"✓";
                break;
            case StepMark::Now:
                glyph = kIconSync;
                fallback = L"…";
                break;
            case StepMark::Failed:
                glyph = kIconCancel;
                fallback = L"×";
                break;
            case StepMark::Waiting:
                break;
        }
        const std::wstring shown = glyph == 0 ? std::wstring() : d.icons != nullptr ? std::wstring(1, glyph) : fallback;
        SetWindowTextW(mark, shown.c_str());
        // The mark's meaning for screen readers: "Done", "In progress"...
        set_accessible_name(mark, text(d.lang, step_mark_label(marks[i])));
        // The step going now in bold, the others as they are.
        SendDlgItemMessageW(dialog, kLabelFirst + static_cast<int>(i), WM_SETFONT,
                            reinterpret_cast<WPARAM>(marks[i] == StepMark::Now || marks[i] == StepMark::Failed
                                                         ? d.bold
                                                         : reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0))),
                            TRUE);
    }
}

INT_PTR CALLBACK dialog_proc(HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* d = win::int_to_ptr<Dialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            d = win::int_to_ptr<Dialog*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            SetWindowTextW(dialog, w(text(d->lang, Str::UpdatingWindowTitle)).c_str());
            const std::string title = d->version.empty() ? std::string(text(d->lang, Str::UpdatingWindowTitle))
                                                         : fill(d->lang, Str::UpdatingTitle, {d->version});
            SetDlgItemTextW(dialog, kTitle, w(title).c_str());
            SetDlgItemTextW(dialog, kText, w(text(d->lang, Str::UpdatingText)).c_str());
            for (std::size_t i = 0; i < kApplySteps; ++i) {
                SetDlgItemTextW(dialog, kLabelFirst + static_cast<int>(i),
                                w(text(d->lang, apply_step_label(static_cast<ApplyStep>(i)))).c_str());
            }
            SetDlgItemTextW(dialog, kOpenInstaller, w(text(d->lang, Str::UpdateOpenInstaller)).c_str());
            SetDlgItemTextW(dialog, IDCANCEL, w(text(d->lang, Str::UpdateClose)).c_str());
            // The title in bold, a little larger, as a task dialog's main line; the step going now in bold.
            LOGFONTW lf{};
            if (const auto font = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
                font != nullptr && GetObjectW(font, sizeof lf, &lf) != 0) {
                LOGFONTW large = lf;
                large.lfWeight = FW_SEMIBOLD;
                large.lfHeight = lf.lfHeight * 4 / 3;
                SendDlgItemMessageW(dialog, kTitle, WM_SETFONT, reinterpret_cast<WPARAM>(CreateFontIndirectW(&large)),
                                    FALSE);
                lf.lfWeight = FW_SEMIBOLD;
                d->bold = CreateFontIndirectW(&lf);
                // Windows' icon font for the ticks, when it's there.
                for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
                    LOGFONTW icon{};
                    icon.lfHeight = lf.lfHeight;
                    icon.lfCharSet = DEFAULT_CHARSET;
                    wcsncpy_s(icon.lfFaceName, face, _TRUNCATE);
                    HFONT made = CreateFontIndirectW(&icon);
                    HDC dc = GetDC(dialog);
                    const HGDIOBJ old = SelectObject(dc, made);
                    std::array<wchar_t, LF_FACESIZE> got{};
                    GetTextFaceW(dc, static_cast<int>(got.size()), got.data());
                    SelectObject(dc, old);
                    ReleaseDC(dialog, dc);
                    if (std::wstring_view(got.data()) == face) {
                        d->icons = made;
                        break;
                    }
                    DeleteObject(made);
                }
            }
            for (std::size_t i = 0; i < kApplySteps && d->icons != nullptr; ++i) {
                SendDlgItemMessageW(dialog, kMarkFirst + static_cast<int>(i), WM_SETFONT,
                                    reinterpret_cast<WPARAM>(d->icons), FALSE);
            }
            // The progress bar where its place was kept (a real one: screen readers say how far it is).
            if (HWND slot = GetDlgItem(dialog, kBar); slot != nullptr) {
                RECT r{};
                GetWindowRect(slot, &r);
                MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&r), 2);
                DestroyWindow(slot);
                d->bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, r.left, r.top,
                                         r.right - r.left, r.bottom - r.top, dialog,
                                         win::int_to_ptr<HMENU>(static_cast<std::uintptr_t>(kBar)),
                                         GetModuleHandleW(nullptr), nullptr);
                SendMessageW(d->bar, PBM_SETRANGE32, 0, 1000);
                set_accessible_name(d->bar, text(d->lang, Str::UpdateStepInstalling));
            }
            // Only once something went wrong: the error and its two buttons.
            for (const int id : {kError, kOpenInstaller, static_cast<int>(IDCANCEL)}) {
                ShowWindow(GetDlgItem(dialog, id), SW_HIDE);
            }
            show_marks(dialog, *d);
            {
                const std::lock_guard lock(d->box->mutex);
                d->box->dialog = dialog;
            }
            try {
                std::thread([box = d->box, args = d->args, lang = d->lang] {
                    try {
                        apply(box, args, lang);
                    } catch (const std::exception& e) {
                        {
                            const std::lock_guard lock(box->mutex);
                            box->error = e.what();
                        }
                        box->post(kFailedMessage);
                    }
                }).detach();
            } catch (const std::system_error& e) {
                {
                    const std::lock_guard lock(d->box->mutex);
                    d->box->error = e.what();
                }
                PostMessageW(dialog, kFailedMessage, 0, 0);
            }
            return TRUE;
        }
        case kStepMessage:
            if (d != nullptr) {
                d->step = static_cast<ApplyStep>(std::min<WPARAM>(wparam, kApplySteps - 1));
                show_marks(dialog, *d);
            }
            return TRUE;
        case kProgressMessage:
            if (d != nullptr && d->bar != nullptr) {
                SendMessageW(d->bar, PBM_SETPOS, wparam, 0);
            }
            return TRUE;
        case kFailedMessage:
            if (d != nullptr) {
                d->running = false;
                d->failed = true;
                show_marks(dialog, *d);
                std::string error;
                {
                    const std::lock_guard lock(d->box->mutex);
                    error = d->box->error;
                }
                bool verified = false;
                {
                    const std::lock_guard lock(d->box->mutex);
                    verified = d->box->verified;
                }
                // Open the installer only for a file that passed its checks.
                const bool offer = offers_installer(d->step, verified);
                std::string message = fill(d->lang, Str::UpdateFailed, {error});
                if (offer) {
                    message += "\r\n\r\n" + std::string(text(d->lang, Str::UpdateFailedNote));
                }
                SetDlgItemTextW(dialog, kError, w(message).c_str());
                for (const int id : {kError, static_cast<int>(IDCANCEL)}) {
                    ShowWindow(GetDlgItem(dialog, id), SW_SHOW);
                }
                ShowWindow(GetDlgItem(dialog, kOpenInstaller), offer ? SW_SHOW : SW_HIDE);
                EnableWindow(GetDlgItem(dialog, kOpenInstaller), offer ? TRUE : FALSE);
                if (d->bar != nullptr) {
                    SendMessageW(d->bar, PBM_SETSTATE, PBST_ERROR, 0);
                }
                SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
                SetFocus(GetDlgItem(dialog, IDCANCEL));
                MessageBeep(MB_ICONWARNING);
            }
            return TRUE;
        case kDoneMessage:
            if (d != nullptr) {
                d->running = false;
            }
            EndDialog(dialog, 0);
            return TRUE;
        case kRestartMessage:
            if (d != nullptr) {
                d->running = false;
                d->restart = true;
                show_marks(dialog, *d);
                SetDlgItemTextW(dialog, kError, w(fill(d->lang, Str::UpdateRestartNeeded, {d->version})).c_str());
                for (const int id : {kError, static_cast<int>(IDCANCEL)}) {
                    ShowWindow(GetDlgItem(dialog, id), SW_SHOW);
                }
                SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
                SetFocus(GetDlgItem(dialog, IDCANCEL));
            }
            return TRUE;
        case WM_COMMAND:
            if (d == nullptr || d->running) {
                return TRUE;  // nothing stops it half-way: Esc and the close box do nothing meanwhile
            }
            if (LOWORD(wparam) == kOpenInstaller) {
                std::optional<UpdateStatus> checked;
                bool verified = false;
                {
                    const std::lock_guard lock(d->box->mutex);
                    checked = d->box->checked;
                    verified = d->box->verified;
                }
                // Windows' usual install of the same file, with its own screens:
                // only one that passed its checks, and is still the service's
                // file as it was checked (hashed again, and held meanwhile).
                win::UniqueHandle held;
                if (!offers_installer(d->step, verified) || !checked || !still_checked(*checked, d->args.msi, held)) {
                    ShowWindow(GetDlgItem(dialog, kOpenInstaller), SW_HIDE);
                    SetDlgItemTextW(dialog, kError, w(fill(d->lang, Str::UpdateFailed,
                                                           {text(d->lang, Str::UpdateErrChanged)}))
                                                        .c_str());
                    SetFocus(GetDlgItem(dialog, IDCANCEL));
                    return TRUE;
                }
                ShellExecuteW(nullptr, L"open", w(d->args.msi).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                EndDialog(dialog, 1);
                return TRUE;
            }
            if (LOWORD(wparam) == IDCANCEL && d->restart) {
                EndDialog(dialog, 0);  // Windows' restart opens nothing by itself; the user opens FloServer after
                return TRUE;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                // FloServer as it is now (the old one when the install rolled back).
                std::string ignored;
                (void)open_floserver(d->args.instance, ignored);
                EndDialog(dialog, 1);
                return TRUE;
            }
            break;
        case WM_DESTROY:
            if (d != nullptr) {
                {
                    const std::lock_guard lock(d->box->mutex);
                    d->box->dialog = nullptr;
                }
                for (std::size_t i = 0; i < kApplySteps; ++i) {
                    set_accessible_name(GetDlgItem(dialog, kMarkFirst + static_cast<int>(i)), {});
                }
                set_accessible_name(d->bar, {});
                if (const auto title = reinterpret_cast<HFONT>(SendDlgItemMessageW(dialog, kTitle, WM_GETFONT, 0, 0));
                    title != nullptr && title != d->bold) {
                    DeleteObject(title);
                }
                for (HFONT* font : {&d->bold, &d->icons}) {
                    if (*font != nullptr) {
                        DeleteObject(*font);
                        *font = nullptr;
                    }
                }
            }
            break;
        default:
            break;
    }
    return FALSE;
}

}  // namespace

std::wstring window_mark_name(const std::string& service_instance) {
    return L"Local\\FloServerWindow." + w(service_instance);
}

std::wstring updating_mark_name(const std::string& service_instance) {
    return L"Local\\FloServerUpdating." + w(service_instance);
}

std::wstring quitting_mark_name(const std::string& service_instance) {
    return L"Local\\FloServerQuitting." + w(service_instance);
}

std::wstring update_quitting_mark_name(const std::string& service_instance) {
    return L"Local\\FloServerQuitting." + w(service_instance) + L".update";
}

int run_apply_update(HINSTANCE instance, Lang lang, const ApplyUpdateArgs& args) {
    Dialog d;
    d.lang = lang;
    d.args = args;
    // Taken before the dialog shows: the window waits for that (WaitForInputIdle) before it goes.
    d.box->quit_mark.reset(
        OpenEventW(SYNCHRONIZE, FALSE, update_quitting_mark_name(core::ipc::default_instance()).c_str()));
    // The version for the title, from the installer's name (FloServer-<x.y.z>-x64.msi).
    const std::string name = core::path_to_utf8(core::path_from_utf8(args.msi).filename());
    if (name.starts_with("FloServer-") && name.ends_with("-x64.msi")) {
        d.version = name.substr(10, name.size() - 10 - 8);
    }
    // In dialog units: the title, what happens, the four steps with their
    // ticks, the bar under the install's step, then (when it fails) why,
    // with Open the installer and Close.
    constexpr short kWidth = 280;
    constexpr short kMargin = 10;
    constexpr short kInner = kWidth - 2 * kMargin;
    constexpr short kMark = 12;
    constexpr WORD kButton = 0x0080;
    constexpr WORD kStatic = 0x0082;
    short y = kMargin;
    std::vector<std::tuple<DWORD, short, short, short, short, WORD, WORD>> items;
    items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 14, kTitle, kStatic);
    y += 18;
    items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 18, kText, kStatic);
    y += 24;
    for (std::size_t i = 0; i < kApplySteps; ++i) {
        items.emplace_back(SS_CENTER | SS_NOPREFIX, kMargin, y, kMark, 10, static_cast<WORD>(kMarkFirst + i), kStatic);
        items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin + kMark + 4, y, kInner - kMark - 4, 10,
                           static_cast<WORD>(kLabelFirst + i), kStatic);
        y += 14;
        if (static_cast<ApplyStep>(i) == ApplyStep::Installing) {
            items.emplace_back(SS_LEFT, kMargin + kMark + 4, y, kInner - kMark - 4, 6, kBar, kStatic);
            y += 10;
        }
    }
    y += 4;
    items.emplace_back(SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, kMargin, y, kInner, 36, kError, kStatic);
    y += 40;
    constexpr short kOpenW = 84;
    constexpr short kCloseW = 60;
    constexpr short kButtonH = 14;
    items.emplace_back(BS_PUSHBUTTON | WS_TABSTOP, kWidth - kMargin - kCloseW - 6 - kOpenW, y, kOpenW, kButtonH,
                       kOpenInstaller, kButton);
    items.emplace_back(BS_DEFPUSHBUTTON | WS_TABSTOP, kWidth - kMargin - kCloseW, y, kCloseW, kButtonH, IDCANCEL,
                       kButton);
    y += kButtonH + kMargin;
    // Mirrored in Arabic and Kurdish, the bar filling from the right too.
    DialogTemplate dialog(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                          is_rtl(lang) ? WS_EX_LAYOUTRTL | WS_EX_RTLREADING | WS_EX_APPWINDOW : WS_EX_APPWINDOW,
                          kWidth, y);
    for (const auto& [style, x, iy, cx, cy, id, kind] : items) {
        dialog.add(style, x, iy, cx, cy, id, kind);
    }
    const INT_PTR answer =
        DialogBoxIndirectParamW(instance, dialog.get(), nullptr, dialog_proc, reinterpret_cast<LPARAM>(&d));
    return answer == 0 ? 0 : 1;
}

}  // namespace floserver::ui

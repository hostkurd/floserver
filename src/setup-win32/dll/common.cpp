#include "common.h"

#include "model/setup_model.h"
#include "win_util.h"

#include <msi.h>
#include <msiquery.h>

#include <array>

#if defined(__MINGW32__)
// In msi.lib, but not in MinGW-w64's msi.h (Windows Installer 3.1 and later).
extern "C" UINT WINAPI MsiSetExternalUIRecord(INSTALLUI_HANDLER_RECORD handler, DWORD filter, LPVOID context,
                                              PINSTALLUI_HANDLER_RECORD previous);
#endif
#ifndef INSTALLLOGMODE_RMFILESINUSE
#define INSTALLLOGMODE_RMFILESINUSE (1 << (INSTALLMESSAGE_RMFILESINUSE >> 24))
#endif

extern "C" IMAGE_DOS_HEADER __ImageBase;  // NOLINT(bugprone-reserved-identifier): the linker's name for this module

namespace floserver::setup {

namespace {

int WINAPI msi_handler(LPVOID context, UINT type, MSIHANDLE record) {
    auto* run = static_cast<MsiRun*>(context);
    const UINT kind = type & 0xFF000000U;
    if (kind == static_cast<UINT>(INSTALLMESSAGE_RMFILESINUSE)) {
        return IDOK;  // FloServer's window (or anything else holding its files) is asked to close
    }
    if (kind == static_cast<UINT>(INSTALLMESSAGE_PROGRESS) || kind == static_cast<UINT>(INSTALLMESSAGE_ACTIONDATA)) {
        int permille = 0;
        bool changed = false;
        {
            const std::lock_guard lock(run->mutex);
            if (kind == static_cast<UINT>(INSTALLMESSAGE_PROGRESS)) {
                run->progress.on_record(MsiRecordGetInteger(record, 1), MsiRecordGetInteger(record, 2),
                                        MsiRecordGetInteger(record, 3), MsiRecordGetInteger(record, 4));
            } else {
                run->progress.on_action_data();
            }
            permille = static_cast<int>(run->progress.fraction() * 1000);
            changed = permille != run->last_permille;
            run->last_permille = permille;
        }
        if (changed) {
            run->post(kProgressMessage, static_cast<WPARAM>(permille));
        }
        return run->cancel ? IDCANCEL : 0;
    }
    if (kind == static_cast<UINT>(INSTALLMESSAGE_ACTIONSTART)) {
        std::array<wchar_t, 128> name{};
        DWORD size = static_cast<DWORD>(name.size());
        int index = -1;
        {
            const std::lock_guard lock(run->mutex);
            // While Windows Installer only writes its script, nothing is done yet.
            if (!run->progress.writing_script() &&
                MsiRecordGetStringW(record, 1, name.data(), &size) == ERROR_SUCCESS) {
                if (const auto step = step_for_action(ui::win::narrow(name.data())); step) {
                    if (static_cast<int>(*step) > run->step) {
                        run->step = static_cast<int>(*step);
                        index = run->step;
                    }
                }
            }
        }
        if (index >= 0) {
            run->post(kStepMessage, static_cast<WPARAM>(index));
        }
        return run->cancel ? IDCANCEL : 0;
    }
    if (kind == static_cast<UINT>(INSTALLMESSAGE_ERROR) || kind == static_cast<UINT>(INSTALLMESSAGE_FATALEXIT)) {
        std::array<wchar_t, 1024> buf{};
        DWORD size = static_cast<DWORD>(buf.size());
        if (MsiFormatRecordW(0, record, buf.data(), &size) == ERROR_SUCCESS) {
            const std::lock_guard lock(run->mutex);
            run->error = buf.data();
        }
        return 0;  // nothing is asked: Windows Installer decides
    }
    return 0;
}

constexpr DWORD kFilter = INSTALLLOGMODE_PROGRESS | INSTALLLOGMODE_ACTIONDATA | INSTALLLOGMODE_ACTIONSTART |
                          INSTALLLOGMODE_ERROR | INSTALLLOGMODE_FATALEXIT | INSTALLLOGMODE_RMFILESINUSE;

}  // namespace

HINSTANCE dll_instance() { return reinterpret_cast<HINSTANCE>(&__ImageBase); }  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): this module's base is its handle

std::span<const std::uint8_t> dll_resource(const wchar_t* name) {
    HRSRC res = FindResourceW(dll_instance(), name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (res == nullptr) {
        return {};
    }
    HGLOBAL loaded = LoadResource(dll_instance(), res);
    const DWORD size = SizeofResource(dll_instance(), res);
    const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (data == nullptr || size == 0) {
        return {};
    }
    return {static_cast<const std::uint8_t*>(data), size};
}

std::wstring error_words(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): FORMAT_MESSAGE_ALLOCATE_BUFFER's convention
    std::wstring text = n > 0 && buf != nullptr ? std::wstring(buf, n) : std::wstring();
    if (buf != nullptr) {
        LocalFree(buf);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return text.empty() ? L"Error " + std::to_wstring(code) + L"." : text;
}

std::wstring install_folder() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) {
        return {};
    }
    buf.resize(n);
    return buf + L"\\Programs\\FloServer";
}

void MsiRun::post(UINT message, WPARAM wparam) {
    const std::lock_guard lock(mutex);
    if (window != nullptr) {
        PostMessageW(window, message, wparam, 0);
    }
}

void MsiRun::detach() {
    const std::lock_guard lock(mutex);
    window = nullptr;
}

UINT msi_install(const std::wstring& msi, const std::wstring& properties, const std::wstring& log, MsiRun& run) {
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    if (!log.empty()) {
        MsiEnableLogW(INSTALLLOGMODE_VERBOSE, log.c_str(), 0);
    }
    MsiSetExternalUIRecord(&msi_handler, kFilter, &run, nullptr);
    const UINT result = MsiInstallProductW(msi.c_str(), properties.c_str());
    MsiSetExternalUIRecord(nullptr, 0, nullptr, nullptr);
    return result;
}

UINT msi_remove(const std::wstring& product, MsiRun& run) {
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    MsiSetExternalUIRecord(&msi_handler, kFilter, &run, nullptr);
    const UINT result =
        MsiConfigureProductExW(product.c_str(), INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT, L"REBOOT=ReallySuppress");
    MsiSetExternalUIRecord(nullptr, 0, nullptr, nullptr);
    return result;
}

}  // namespace floserver::setup

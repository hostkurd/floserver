// FloServer-Setup-<version>.exe (D25): the download. A small launcher that
// carries floserver-setup.dll (the screens, their pictures and the licence)
// and FloServer's MSI as resources. It writes both into a new folder of its
// own under %TEMP%, loads the DLL from there and lets it run the setup, then
// deletes them again.
//
// Built with the static C runtime and only Windows' own DLLs, so it runs on
// a PC that has never had the Visual C++ runtime. DLLs are looked for in
// System32 only (a planted DLL next to the download is never loaded), and
// floserver-setup.dll by its full path.
//
//   FloServer-Setup-<version>.exe [--quiet] [--log <file>]
//
// --quiet (or /quiet, /qn) installs with no window, accepting the licence as
// msiexec /qn does; the exit code is Windows Installer's.

#include "floserver/setup_api.h"
#include "model/setup_model.h"
#include "win_util.h"

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {

using floserver::ui::win::narrow;
using floserver::ui::win::UniqueHandle;
using floserver::ui::win::widen;

std::span<const std::uint8_t> resource(const wchar_t* name) {
    HRSRC res = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (res == nullptr) {
        return {};
    }
    HGLOBAL loaded = LoadResource(nullptr, res);
    const DWORD size = SizeofResource(nullptr, res);
    const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (data == nullptr || size == 0) {
        return {};
    }
    return {static_cast<const std::uint8_t*>(data), size};
}

// A new folder under %TEMP% holding the DLL and the MSI, each read-only
// for everyone while the setup runs. Everything goes when this does.
class Unpacked {
public:
    Unpacked() = default;
    Unpacked(const Unpacked&) = delete;
    Unpacked& operator=(const Unpacked&) = delete;
    Unpacked(Unpacked&&) = delete;
    Unpacked& operator=(Unpacked&&) = delete;
    ~Unpacked() {
        guards_.clear();
        for (const std::wstring& f : files_) {
            DeleteFileW(f.c_str());
        }
        if (!folder_.empty()) {
            RemoveDirectoryW(folder_.c_str());
        }
    }

    bool make_folder() {
        std::array<wchar_t, MAX_PATH + 1> temp{};
        const DWORD n = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
        if (n == 0 || n > MAX_PATH) {
            return false;
        }
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::array<wchar_t, 40> name{};
            swprintf_s(name.data(), name.size(), L"FloServer-setup-%08x%08x", random(), random());
            std::wstring candidate = std::wstring(temp.data()) + name.data();
            if (CreateDirectoryW(candidate.c_str(), nullptr) != FALSE) {
                folder_ = std::move(candidate);
                return true;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS) {
                return false;
            }
        }
        return false;
    }

    // Writes `data` as `name` in the folder (never over anything), then keeps
    // it open so that it can only be read until it's deleted. Its full path.
    std::wstring write(const wchar_t* name, std::span<const std::uint8_t> data) {
        const std::wstring file = folder_ + L"\\" + name;
        {
            const UniqueHandle out(
                CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!out || out.get() == INVALID_HANDLE_VALUE) {
                return {};
            }
            files_.push_back(file);
            DWORD written = 0;
            if (WriteFile(out.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr) == FALSE ||
                written != data.size()) {
                return {};
            }
        }
        HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return {};
        }
        guards_.emplace_back(h);
        return file;
    }

private:
    std::wstring folder_;
    std::vector<std::wstring> files_;
    std::vector<UniqueHandle> guards_;
};

std::vector<std::string> command_args() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return {};
    }
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(narrow(argv[i]));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv from Windows
    }
    LocalFree(argv);
    return args;
}

int fail(bool quiet, const wchar_t* text, UINT code) {
    if (!quiet) {
        MessageBoxW(nullptr, text, L"FloServer Setup", MB_OK | MB_ICONERROR);
    }
    return static_cast<int>(code);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*prev*/, PWSTR /*cmd*/, int show) {
    // DLLs from System32 only, before anything else is loaded.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    const floserver::setup::SetupArgs args = floserver::setup::parse_setup_args(command_args());

    // One setup at a time: a second one brings the first forward.
    const UniqueHandle single(CreateMutexW(nullptr, FALSE, L"Local\\FloServerSetup"));
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!args.quiet) {
            if (HWND other = FindWindowW(FLOSERVER_SETUP_WINDOW_CLASS, nullptr); other != nullptr) {
                if (IsIconic(other) != FALSE) {
                    ShowWindow(other, SW_RESTORE);
                }
                SetForegroundWindow(other);
            }
        }
        return static_cast<int>(ERROR_INSTALL_ALREADY_RUNNING);
    }

    const auto dll_data = resource(L"FLOSERVER_SETUP_DLL");
    const auto msi_data = resource(L"FLOSERVER_MSI");
    if (dll_data.empty() || msi_data.empty()) {
        return fail(args.quiet, L"This setup is incomplete. Download FloServer again from floserver.org.",
                    ERROR_INSTALL_PACKAGE_INVALID);
    }
    int code = 0;
    {
        Unpacked unpacked;
        if (!unpacked.make_folder()) {
            return fail(args.quiet, L"FloServer's setup couldn't write to the folder for temporary files.",
                        ERROR_INSTALL_FAILURE);
        }
        const std::wstring dll_file = unpacked.write(FLOSERVER_SETUP_DLL_NAME, dll_data);
        const std::wstring msi_file = dll_file.empty() ? std::wstring() : unpacked.write(L"FloServer.msi", msi_data);
        if (msi_file.empty()) {
            return fail(args.quiet, L"FloServer's setup couldn't write to the folder for temporary files.",
                        ERROR_INSTALL_FAILURE);
        }
        HMODULE dll = LoadLibraryExW(dll_file.c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto run = dll != nullptr ? floserver_setup_proc<FloSetupRunFn>(dll, FLOSERVER_SETUP_RUN) : nullptr;
        if (run == nullptr) {
            if (dll != nullptr) {
                FreeLibrary(dll);
            }
            return fail(args.quiet, L"This setup is damaged. Download FloServer again from floserver.org.",
                        ERROR_INSTALL_PACKAGE_INVALID);
        }
        const std::wstring log = widen(args.log);
        FloSetupRunArgs run_args{};
        run_args.size = sizeof run_args;
        run_args.msi = msi_file.c_str();
        run_args.log = log.empty() ? nullptr : log.c_str();
        run_args.quiet = args.quiet ? 1 : 0;
        run_args.show = show;
        code = run(&run_args);
        FreeLibrary(dll);
    }
    return code;
}

// FloServer.exe --remove-program <plan> (D25): the copy of FloServer.exe in
// %TEMP%\FloServer-update-<hex>\ that goes on once the clean-up is done.
// floserver-setup.dll (copied beside it) shows the Uninstall window at its
// last step while Windows removes the program, then Done. Afterwards the
// folder is deleted by a hidden cmd.exe once this process has ended.

#include "setup_dll.h"

#include "uninstall.h"

#include "model/app_update.h"
#include "model/uninstall.h"
#include "win_util.h"

#include "floserver/core/build_info.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"

#include <windows.h>

#include <array>
#include <filesystem>
#include <string>

namespace floserver::ui {

namespace {

namespace fs = std::filesystem;

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        if (!out.empty()) {
            out += '\n';
        }
        out += line;
    }
    return out;
}

// This copy's own folder goes once it has ended: only one of FloServer's
// own (FloServer-update-<hex>) under %TEMP%, and only when its path can be
// given to cmd.exe as it is.
void delete_own_folder_later() {
    const auto exe = core::executable_path();
    if (!exe) {
        return;
    }
    const fs::path folder = exe->parent_path();
    std::error_code ec;
    const fs::path temp = fs::temp_directory_path(ec);
    const std::wstring path = folder.wstring();
    std::error_code same;
    if (ec || !is_updater_folder(core::path_to_utf8(folder.filename())) ||
        !fs::equivalent(folder.parent_path(), temp, same) || same ||
        path.find_first_of(L"\"%!^&|<>\r\n") != std::wstring::npos) {
        return;
    }
    std::array<wchar_t, MAX_PATH + 1> system{};
    const UINT n = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (n == 0 || n > MAX_PATH) {
        return;
    }
    const std::wstring cmd = std::wstring(system.data(), n) + L"\\cmd.exe";
    // Up to ten tries a second apart, so this process (and its open files)
    // is gone first. /v:off: a "!" can't be expanded (none is let through anyway).
    const std::wstring quoted = L"\"" + path + L"\"";
    std::wstring command = L"\"" + cmd + L"\" /d /v:off /c for /l %i in (1,1,10) do @(ping -n 2 127.0.0.1 >nul & if exist " +
                           quoted + L" rd /s /q " + quoted + L")";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(cmd.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       std::wstring(system.data(), n).c_str(), &si, &pi) != FALSE) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

}  // namespace

int run_remove_program(const std::string& plan_file) {
    const auto text = core::read_small_file(core::path_from_utf8(plan_file), std::size_t{256} * 1024);
    auto plan = text ? parse_remove_program_json(*text) : std::nullopt;
    // Only FloServer's own MSI, as installed now: never another product named in the notes.
    if (plan && plan->product != installed_product_code()) {
        plan.reset();
    }
    // The FloServer.exe that started this copy has its files open until it
    // has gone; Windows Installer would find them in use.
    if (plan && plan->parent != 0) {
        if (const win::UniqueHandle parent(OpenProcess(SYNCHRONIZE, FALSE, plan->parent)); parent) {
            WaitForSingleObject(parent.get(), 20000);
        }
    }
    std::string error;
    const SetupDll* dll = plan ? setup_dll(error) : nullptr;
    int result = 1;
    if (!plan) {
        MessageBoxW(nullptr, L"FloServer's uninstall couldn't go on: its notes are missing or damaged.", L"FloServer",
                    MB_OK | MB_ICONWARNING);
    } else if (dll == nullptr) {
        // Without its screens: Windows' standard uninstall, with its own window.
        std::string start_error;
        if (!start_msi_uninstall(nullptr, plan->product, start_error)) {
            const std::wstring message = L"FloServer's program couldn't be removed (" + win::widen(start_error) + L").";
            MessageBoxW(nullptr, message.c_str(), L"FloServer", MB_OK | MB_ICONWARNING);
        } else {
            result = 0;
        }
    } else {
        const std::wstring product = win::widen(plan->product);
        const std::wstring deleted = win::widen(plan->deleted);
        const std::wstring kept = win::widen(join_lines(plan->kept));
        const std::wstring problems = win::widen(join_lines(plan->problems));
        const std::wstring data_dir = win::widen(plan->data_dir);
        const std::wstring version = win::widen(core::kVersion);
        FloRemoveProgramArgs args{};
        args.size = sizeof args;
        args.product = product.c_str();
        args.top_left = {plan->x, plan->y};
        args.deleted = deleted.c_str();
        args.kept = kept.c_str();
        args.problems = problems.c_str();
        args.data_dir = data_dir.c_str();
        args.version = version.c_str();
        result = dll->remove_program(&args);
    }
    delete_own_folder_later();
    return result;
}

}  // namespace floserver::ui

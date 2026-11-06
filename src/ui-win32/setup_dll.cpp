#include "setup_dll.h"

#include "win_util.h"

#include <windows.h>

#include <system_error>

namespace floserver::ui {

const SetupDll* setup_dll(std::string& error) {
    static SetupDll dll;
    static bool loaded = false;
    if (loaded) {
        return &dll;
    }
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0 || n >= path.size()) {
        error = std::system_category().message(static_cast<int>(GetLastError()));
        return nullptr;
    }
    path.resize(n);
    path = path.substr(0, path.find_last_of(L'\\') + 1) + FLOSERVER_SETUP_DLL_NAME;
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        error = win::narrow(path) + ": " + std::system_category().message(static_cast<int>(GetLastError()));
        return nullptr;
    }
    SetupDll d;
    d.open = floserver_setup_proc<FloUninstallOpenFn>(module, FLOSERVER_UNINSTALL_OPEN);
    d.choices = floserver_setup_proc<FloUninstallChoicesFn>(module, FLOSERVER_UNINSTALL_CHOICES);
    d.step = floserver_setup_proc<FloUninstallStepFn>(module, FLOSERVER_UNINSTALL_STEP);
    d.message = floserver_setup_proc<FloUninstallMessageFn>(module, FLOSERVER_UNINSTALL_MESSAGE);
    d.place = floserver_setup_proc<FloUninstallPlaceFn>(module, FLOSERVER_UNINSTALL_PLACE);
    d.dialog_message = floserver_setup_proc<FloUninstallDialogMessageFn>(module, FLOSERVER_UNINSTALL_DIALOG_MESSAGE);
    d.forward = floserver_setup_proc<FloUninstallForwardFn>(module, FLOSERVER_UNINSTALL_FORWARD);
    d.close = floserver_setup_proc<FloUninstallCloseFn>(module, FLOSERVER_UNINSTALL_CLOSE);
    d.remove_program = floserver_setup_proc<FloRemoveProgramFn>(module, FLOSERVER_REMOVE_PROGRAM);
    if (d.open == nullptr || d.choices == nullptr || d.step == nullptr || d.message == nullptr || d.place == nullptr ||
        d.dialog_message == nullptr || d.forward == nullptr || d.close == nullptr || d.remove_program == nullptr) {
        FreeLibrary(module);
        error = win::narrow(path) + ": not the floserver-setup.dll this FloServer needs";
        return nullptr;
    }
    dll = d;
    loaded = true;  // never freed: its window class stays registered for the process
    return &dll;
}

}  // namespace floserver::ui

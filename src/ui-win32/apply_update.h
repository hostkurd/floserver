#pragma once

// FloServer.exe --apply-update <msi> (Q5, mockup approved 5 Nov): the copy
// of FloServer.exe in %TEMP%\FloServer-update-<random>\ that installs an update.
// Windows Installer replaces the installed FloServer.exe, so the window
// that was running starts this copy and quits (main_window_update.cpp).
//
// It shows only the updating dialog, "Updating FloServer to <v>", and its
// four steps:
//   1. Downloaded and checked against the signed list: the service says the
//      MSI is ready (updates.status), and which size and SHA-256 it has;
//   2. Stopped your projects and databases: marked as updating (no window
//      opens meanwhile), then, once the old window has gone (or it fails),
//      core::stop_service(), which waits for floserverd to end;
//   3. Installing the new version…: the MSI opened so it can't change, its
//      SHA-256 checked again through that handle, then MsiInstallProductW at
//      INSTALLUILEVEL_NONE with REBOOT=ReallySuppress, Windows Installer's
//      progress messages driving the bar (the MSI's own StopFloServer finds
//      nothing running). When Windows must restart to finish, it says so
//      and stops there;
//   4. Open FloServer and start your projects again: the installed
//      %LOCALAPPDATA%\Programs\FloServer\FloServer.exe, which starts the
//      service and the projects that were on.
// If a step fails, the dialog says why, with Close (FloServer opens again as
// it is) and, only when the installer had passed its checks, Open the
// installer (Windows' usual install of the same MSI, hashed once more first).

#include "model/app_update.h"
#include "model/ui_strings.h"

#include <windows.h>

#include <filesystem>
#include <string>

namespace floserver::ui {

// The marks the windows and the updater leave for each other (per service
// instance): a FloServer window is open; an update is being installed (no
// window opens meanwhile, so nothing starts floserverd mid-install).
[[nodiscard]] std::wstring window_mark_name(const std::string& service_instance);
[[nodiscard]] std::wstring updating_mark_name(const std::string& service_instance);

// FloServer.exe and the DLLs beside it (floserver-setup.dll among them),
// copied into a new folder of their own under %TEMP%
// ("FloServer-update-<hex>"; earlier ones are cleaned up first). The copy's
// path; false with `error`. Update now and Remove FloServer use it.
[[nodiscard]] bool copy_floserver_to_temp(std::filesystem::path& copy, std::string& error);

// Runs the dialog to its end; the process's exit code.
[[nodiscard]] int run_apply_update(HINSTANCE instance, Lang lang, const ApplyUpdateArgs& args);

}  // namespace floserver::ui

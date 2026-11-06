#pragma once

// FloServer.exe --apply-update <msi> (Q5, mockup approved 5 Nov): the copy
// of FloServer.exe in %TEMP%\FloServer-update\ that installs an update.
// Windows Installer replaces the installed FloServer.exe, so the window
// that was running starts this copy and quits (main_window_update.cpp).
//
// It shows only the updating dialog, "Updating FloServer to <v>", and its
// four steps:
//   1. Downloaded and checked against the signed list: the service says the
//      MSI is ready (updates.status), and which size and SHA-256 it has;
//   2. Stopped your projects and databases: once the old window has gone,
//      core::stop_service(), which waits for floserverd to end;
//   3. Installing the new version…: the MSI's SHA-256 checked again, then
//      MsiInstallProductW at INSTALLUILEVEL_NONE with REBOOT=ReallySuppress,
//      Windows Installer's progress messages driving the bar (the MSI's own
//      StopFloServer finds nothing running);
//   4. Open FloServer and start your projects again: the installed
//      %LOCALAPPDATA%\Programs\FloServer\FloServer.exe, which starts the
//      service and the projects that were on.
// If a step fails, the dialog says why, with Open the installer (Windows'
// usual install of the same MSI) and Close (FloServer opens again as it is).

#include "model/app_update.h"
#include "model/ui_strings.h"

#include <windows.h>

namespace floserver::ui {

// Runs the dialog to its end; the process's exit code.
[[nodiscard]] int run_apply_update(HINSTANCE instance, Lang lang, const ApplyUpdateArgs& args);

}  // namespace floserver::ui

#pragma once

// floserver-setup.dll (D25), loaded by FloServer.exe only when it's needed:
// the Uninstall window, and removing the program from the copy in %TEMP%.
// Loaded once, by its full path next to FloServer.exe (never through the
// search path), and kept loaded: its window class stays registered.

#include "floserver/setup_api.h"

#include <string>

namespace floserver::ui {

struct SetupDll {
    FloUninstallOpenFn open = nullptr;
    FloUninstallChoicesFn choices = nullptr;
    FloUninstallStepFn step = nullptr;
    FloUninstallMessageFn message = nullptr;
    FloUninstallPlaceFn place = nullptr;
    FloUninstallDialogMessageFn dialog_message = nullptr;
    FloUninstallForwardFn forward = nullptr;
    FloUninstallCloseFn close = nullptr;
    FloRemoveProgramFn remove_program = nullptr;
};

// The DLL's functions; null with `error` (Windows' words, UTF-8) when it
// isn't there or isn't the one this FloServer.exe expects.
[[nodiscard]] const SetupDll* setup_dll(std::string& error);

// FloServer.exe --remove-program <plan>: the copy in %TEMP% removing the
// program (remove_program.cpp). The process's exit code.
[[nodiscard]] int run_remove_program(const std::string& plan_file);

}  // namespace floserver::ui

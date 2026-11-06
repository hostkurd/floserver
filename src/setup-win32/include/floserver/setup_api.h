#pragma once

// floserver-setup.dll (D25): every setup and uninstall screen, with their
// pictures (the feather, the icon) and the licence, in one DLL, so
// FloServer.exe stays small. Two programs use it:
//
//   - FloServer-Setup-<version>.exe, the download: a small launcher that
//     carries this DLL and the MSI, writes both into a new folder under
//     %TEMP% and calls floserver_setup_run();
//   - FloServer.exe, which loads it from its own folder only to uninstall
//     (setup_dll.h): the Uninstall window, then floserver_remove_program()
//     from FloServer.exe's copy in %TEMP% while Windows removes the program.
//
// A plain C interface (UTF-16 strings, plain structs, nothing allocated on
// one side and freed on the other), because the DLL is built with its own
// copy of the C runtime: the launcher must run on a PC without the Visual
// C++ runtime. Each struct starts with its size, so an older caller still
// works with a newer DLL.

#include <windows.h>

#include <cstdint>

#if defined(FLOSERVER_SETUP_DLL_BUILD)
#define FLOSERVER_SETUP_API extern "C" __declspec(dllexport)
#else
#define FLOSERVER_SETUP_API extern "C" __declspec(dllimport)
#endif

// A function of the DLL by its name (FLOSERVER_SETUP_RUN, ...) as its type;
// null when it isn't there.
template <typename F>
F floserver_setup_proc(HMODULE dll, const char* name) {
    // Through void(*)(): the one function type any other may be cast from.
    return reinterpret_cast<F>(reinterpret_cast<void (*)()>(GetProcAddress(dll, name)));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): GetProcAddress's convention
}

// The DLL's file name, next to FloServer.exe (and in the launcher's folder).
#define FLOSERVER_SETUP_DLL_NAME L"floserver-setup.dll"

// --- the setup (from the launcher) ---

struct FloSetupRunArgs {
    std::uint32_t size;      // sizeof(FloSetupRunArgs)
    const wchar_t* msi;      // the MSI, already written out (read-only while this runs)
    const wchar_t* log;      // Windows Installer's log file, or null
    int quiet;               // nonzero: install with no window, licence accepted (msiexec /qn)
    int show;                // ShowWindow's first value for the window
};
// Runs the setup to its end. Windows Installer's result (0 done, 3010 done
// but Windows must restart, 1602 cancelled, else an error).
using FloSetupRunFn = int(__stdcall*)(const FloSetupRunArgs* args);
#define FLOSERVER_SETUP_RUN "floserver_setup_run"

// --- the Uninstall window (from FloServer.exe) ---

struct FloUninstall;  // the window, owned by the DLL

// One "Also delete" box.
struct FloUninstallChoice {
    unsigned bit;          // FloServer's own meaning (uninstall_ticks_bits)
    const wchar_t* label;
    const wchar_t* note;   // under it, in grey
    int checked;
};

struct FloUninstallHost {
    std::uint32_t size;    // sizeof(FloUninstallHost)
    void* context;
    // Remove was pressed with these boxes ticked (their bits, or'ed).
    void(__stdcall* remove)(void* context, unsigned ticks);
    // The window has closed (Cancel, ✕, or Close after a problem). Called
    // while it's being destroyed: free it later (post yourself a message),
    // never from here.
    void(__stdcall* closed)(void* context);
    const wchar_t* version;  // shown in the panel
};

// Opens the window on its first page (Choose); null if it couldn't be made.
using FloUninstallOpenFn = FloUninstall*(__stdcall*)(const FloUninstallHost* host, int show);
// The boxes, and again when their notes change (sizes counted).
using FloUninstallChoicesFn = void(__stdcall*)(FloUninstall* window, const FloUninstallChoice* choices,
                                               std::uint32_t count);
// The Removing page: the step under way (0 the certificate, 1 stopping
// FloServer, 2 deleting what was ticked, 3 removing the program).
using FloUninstallStepFn = void(__stdcall*)(FloUninstall* window, int step);
// A page saying what went wrong (or, with `done` nonzero, the Done page of
// a copy that wasn't installed by the MSI), with Close.
using FloUninstallMessageFn = void(__stdcall*)(FloUninstall* window, const wchar_t* heading, const wchar_t* text,
                                               int done);
// Where the window is (its top-left), for the copy that goes on with it.
using FloUninstallPlaceFn = int(__stdcall*)(FloUninstall* window, POINT* top_left);
// Tab, Enter and Esc for the window: call from the message loop.
using FloUninstallDialogMessageFn = int(__stdcall*)(FloUninstall* window, MSG* msg);
// Brings it to the front.
using FloUninstallForwardFn = void(__stdcall*)(FloUninstall* window);
// Closes it (no `closed` call) and frees it.
using FloUninstallCloseFn = void(__stdcall*)(FloUninstall* window);
#define FLOSERVER_UNINSTALL_OPEN "floserver_uninstall_open"
#define FLOSERVER_UNINSTALL_CHOICES "floserver_uninstall_choices"
#define FLOSERVER_UNINSTALL_STEP "floserver_uninstall_step"
#define FLOSERVER_UNINSTALL_MESSAGE "floserver_uninstall_message"
#define FLOSERVER_UNINSTALL_PLACE "floserver_uninstall_place"
#define FLOSERVER_UNINSTALL_DIALOG_MESSAGE "floserver_uninstall_dialog_message"
#define FLOSERVER_UNINSTALL_FORWARD "floserver_uninstall_forward"
#define FLOSERVER_UNINSTALL_CLOSE "floserver_uninstall_close"

// --- removing the program (from FloServer.exe's copy in %TEMP%) ---

struct FloRemoveProgramArgs {
    std::uint32_t size;        // sizeof(FloRemoveProgramArgs)
    const wchar_t* product;    // the MSI's product code, "{...}"
    POINT top_left;            // where the Uninstall window was
    const wchar_t* deleted;    // the Done page's line on what was deleted too
    const wchar_t* kept;       // its "Kept, as you chose" lines (one per line), or empty
    const wchar_t* problems;   // what couldn't be done, or empty
    const wchar_t* data_dir;   // "Open the data folder" opens it; empty: no link
    const wchar_t* version;
};
// Shows the Removing page at its last step and runs Windows' uninstall of
// `product` with Windows Installer's screens off, then the Done page.
// Windows Installer's result.
using FloRemoveProgramFn = int(__stdcall*)(const FloRemoveProgramArgs* args);
#define FLOSERVER_REMOVE_PROGRAM "floserver_remove_program"

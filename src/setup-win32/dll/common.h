#pragma once

// What the setup and uninstall screens in floserver-setup.dll share:
// the DLL's own resources, Windows' error words, and running Windows
// Installer with its screens off and its progress on ours.

#include "model/msi_progress.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>

namespace floserver::setup {

// The DLL itself (its icon, the licence).
[[nodiscard]] HINSTANCE dll_instance();
// One of the DLL's RCDATA resources; empty when it isn't there.
[[nodiscard]] std::span<const std::uint8_t> dll_resource(const wchar_t* name);
// Windows' words for an error code.
[[nodiscard]] std::wstring error_words(DWORD code);
// %LOCALAPPDATA%\Programs\FloServer, where the MSI installs (D21).
[[nodiscard]] std::wstring install_folder();

// Where the setup notes the licence the user accepted (its SHA-256, hex), so
// an update asks again only when it changed. Removed on uninstall.
inline constexpr wchar_t kRegistryKey[] = L"Software\\HostKurd\\FloServer";
inline constexpr wchar_t kAcceptedLicenceValue[] = L"AcceptedLicence";

// From Windows Installer's thread to a window.
inline constexpr UINT kProgressMessage = WM_APP + 1;  // wparam: per mille
inline constexpr UINT kStepMessage = WM_APP + 2;      // wparam: the install step under way
inline constexpr UINT kDoneMessage = WM_APP + 3;      // wparam: Windows Installer's result

// What a window and Windows Installer's thread share while it runs. The
// thread may outlive the window by a moment: it posts only while `window` is set.
struct MsiRun {
    std::mutex mutex;
    HWND window = nullptr;
    ui::MsiProgress progress;
    int last_permille = -1;
    int step = -1;
    std::wstring error;  // Windows Installer's words for what failed
    std::atomic<bool> cancel{false};

    void post(UINT message, WPARAM wparam);
    void detach();  // the window is going: nothing more is posted
};

// Installs (or updates) from `msi`: Windows Installer's result. `properties`
// as setup_model.h's msi_properties(); `log` may be empty.
[[nodiscard]] UINT msi_install(const std::wstring& msi, const std::wstring& properties, const std::wstring& log,
                               MsiRun& run);
// Uninstalls the product with this code: Windows Installer's result.
[[nodiscard]] UINT msi_remove(const std::wstring& product, MsiRun& run);

}  // namespace floserver::setup

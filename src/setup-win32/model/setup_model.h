#pragma once

// FloServer's setup program (D25): what it decides, as plain C++ so it's
// tested on every platform. The Win32 side (setup_main.cpp) reads Windows
// Installer and the registry, and draws the screens.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::setup {

// "1.2.3" or "1.2.3.4" as numbers; nullopt when it isn't one.
[[nodiscard]] std::optional<std::vector<unsigned>> parse_version(std::string_view text);
// <0, 0, >0 as `a` is older than, the same as, or newer than `b`; missing
// parts count as 0 ("1.2" == "1.2.0"). Unreadable versions compare as equal.
[[nodiscard]] int compare_versions(std::string_view a, std::string_view b);

// A FloServer MSI: the one inside the setup program, or the one installed.
struct Product {
    std::string code;     // "{...}"
    std::string version;  // "0.2.0"
};

// What the setup does, from what's installed (found by the upgrade code).
enum class SetupMode : std::uint8_t {
    Install,    // nothing installed: Licence, Install, Done
    Update,     // an older version: Update (the licence again only if it changed)
    Reinstall,  // this very version (same product code): installed again
    Newer,      // a newer version is installed: nothing to do, says so
};
[[nodiscard]] SetupMode decide_setup_mode(const std::optional<Product>& installed, const Product& ours);

// The licence screen first: always for a new install; for an update or a
// reinstall only when the licence accepted last time (its SHA-256, hex, as
// the setup wrote it) isn't this one.
[[nodiscard]] bool licence_needed(SetupMode mode, std::string_view accepted_sha256, std::string_view ours_sha256);

// The properties MsiInstallProduct gets: Windows never restarts by itself.
[[nodiscard]] std::wstring msi_properties(SetupMode mode);

// The steps on the Installing screen, in order.
enum class InstallStep : std::uint8_t { Checked, Copying, Path, StartMenu };
inline constexpr int kInstallSteps = 4;
// The step an action Windows Installer starts belongs to; nullopt for the
// rest (they don't move the steps).
[[nodiscard]] std::optional<InstallStep> step_for_action(std::string_view action);

// "9.8 MB", "640 KB": the installed size on the Licence screen.
[[nodiscard]] std::string format_size(std::uint64_t bytes);

// FloServer-Setup-<version>.exe [--quiet] [--log <file>]. /quiet, /q, /qn
// and /log work too, as msiexec's do. --quiet installs (or updates) without
// a window, accepting the licence as msiexec /qn does; the exit code is
// Windows Installer's (0 done, 3010 done but Windows must restart).
struct SetupArgs {
    bool quiet = false;
    std::string log;
};
[[nodiscard]] SetupArgs parse_setup_args(const std::vector<std::string>& args);

}  // namespace floserver::setup

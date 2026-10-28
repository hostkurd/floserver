#pragma once

// Installed runtimes (D8, D10): one shared copy per version per computer,
// under <data>/runtimes/<name>/<folder>, where <folder> is the version, with
// "-<variant>" added for any variant other than PHP's usual "nts".
//
// An install unpacks into a hidden ".staging-..." folder next to the final
// one, adds the Microsoft Visual C++ runtime DLLs where the runtime's
// programs are (app-local, so no system-wide redistributable is needed),
// writes a ".floserver-runtime.json" marker last, then renames the folder
// into place. A folder without a marker isn't an install.

#include "floserver/core/manifest.h"

#include <filesystem>
#include <string>
#include <vector>

namespace floserver::core {

inline constexpr std::string_view kRuntimeMarker = ".floserver-runtime.json";

struct InstalledRuntime {
    std::string name;
    std::string version;
    std::string platform;
    std::string variant;
    std::string license;
    std::string sha256;  // hex, of the archive it came from
    std::filesystem::path dir;
};

// The folder name for an entry, or empty when the entry's fields aren't safe
// as a folder name (open_manifest() already refuses those).
[[nodiscard]] std::string runtime_folder(const RuntimeFile& entry);

// Every complete install, sorted by name then newest version first.
[[nodiscard]] std::vector<InstalledRuntime> list_installed(const std::filesystem::path& runtimes_root);

[[nodiscard]] bool is_installed(const std::filesystem::path& runtimes_root, const RuntimeFile& entry);

struct InstallResult {
    bool ok = false;
    bool already_installed = false;
    std::string message;
    std::filesystem::path dir;
};

// Installs from a downloaded archive that already matched the list. With a
// non-empty `vc_runtime_dir`, its *.dll files are copied next to the
// runtime's programs where those DLLs aren't already present.
[[nodiscard]] InstallResult install_runtime(const RuntimeFile& entry, const std::filesystem::path& archive,
                                            const std::filesystem::path& runtimes_root,
                                            const std::filesystem::path& vc_runtime_dir);

// Removes an install. False (with `error`) if it couldn't.
[[nodiscard]] bool remove_runtime(const std::filesystem::path& runtimes_root, const std::string& name,
                                  const std::string& folder, std::string& error);

// Deletes staging folders left by an install that was interrupted (a crash
// or power cut). Only call while no install is running.
void remove_leftovers(const std::filesystem::path& runtimes_root);

}  // namespace floserver::core

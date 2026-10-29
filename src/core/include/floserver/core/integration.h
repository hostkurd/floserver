#pragma once

// Working with FloCMS Studio (D14).
//
// FloServer writes integration.json at the top of its data folder so Studio
// can find it: the `flo` program, the shims folder (D10), what's installed and
// the database ports. It never holds a secret; projects and passwords go
// through `flo` (flo projects --json, flo add --json, flo env --write).
//
//   {"schema": 1, "floserver_version": "0.1.0",
//    "cli": "C:\\...\\flo.exe", "shims": "C:\\...\\FloServer\\shims",
//    "runtimes": [{"name": "php", "version": "8.5.12"}, ...],
//    "databases": [{"series": "11.4", "version": "11.4.13", "port": 3306}]}
//
// The other way, FloServer opens a FloCMS project in Studio: Studio records
// where its program is, and FloServer starts it with --open <folder>.
//
//   Windows  HKCU (then HKLM) \Software\HostKurd\FloCMS Studio, value "Path"
//   macOS    FloCMS Studio.app in /Applications or ~/Applications
//   Linux    flocms-studio on the PATH

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace floserver::core {

inline constexpr int kIntegrationSchema = 1;

struct IntegrationRuntime {
    std::string name;
    std::string version;
};

struct IntegrationDatabase {
    std::string series;
    std::string version;
    int port = 0;
};

struct IntegrationInfo {
    std::filesystem::path cli;    // empty when it can't be found
    std::filesystem::path shims;
    std::vector<IntegrationRuntime> runtimes;
    std::vector<IntegrationDatabase> databases;
};

[[nodiscard]] std::filesystem::path integration_file(const std::filesystem::path& data);

// The file's text, always the same for the same info (so it is rewritten only
// when something changed).
[[nodiscard]] std::string integration_json(const IntegrationInfo& info);

// Writes `text` unless the file already holds exactly that. True when the
// file holds it afterwards.
[[nodiscard]] bool write_integration(const std::filesystem::path& data, const std::string& text, std::string& error);

// What FloServer has now: installed runtimes from the data folder, `flo` next
// to this program, and the database servers from `database_status` (the
// reply of DatabaseService::status()).
[[nodiscard]] IntegrationInfo collect_integration(const std::filesystem::path& data,
                                                  const std::string& database_status_json);

// A FloCMS project, by the same rule as Studio: its composer.json is the
// skeleton (hostkurd/flocms) or requires hostkurd/flocms-core.
[[nodiscard]] bool is_flocms_project(const std::filesystem::path& dir);

// Studio's program, or nothing when it isn't installed.
[[nodiscard]] std::optional<std::filesystem::path> find_studio();

// Starts Studio with --open <folder> (a running Studio takes it over).
[[nodiscard]] bool open_in_studio(const std::filesystem::path& studio, const std::filesystem::path& folder,
                                  std::string& error);

}  // namespace floserver::core

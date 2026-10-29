#pragma once

// Database passwords (D12): kept in the system's own secure store, never in
// logs, arguments or shown text.
//
//   Windows   Credential Manager, generic credentials named "FloServer:<name>"
//             (only this Windows user can read them).
//   elsewhere for now a file per secret in <data>/secrets, owner-only (0600,
//             folder 0700). The macOS Keychain and Linux Secret Service come
//             with their own stage (D3).
//
// Names are short ASCII ("mariadb-11.4/root", "mariadb-11.4/shop").

#include <filesystem>
#include <optional>
#include <string>

namespace floserver::core {

class SecretStore {
public:
    explicit SecretStore(const std::filesystem::path& data_dir);

    [[nodiscard]] bool save(const std::string& name, const std::string& secret, std::string& error) const;
    // Nothing when none is saved under that name (or it can't be read).
    [[nodiscard]] std::optional<std::string> read(const std::string& name) const;
    // True when it's gone (or was never there).
    bool remove(const std::string& name) const;  // NOLINT(modernize-use-nodiscard): callers may ignore it

    // Names may use a-z, 0-9, '.', '-', '_' and '/'.
    [[nodiscard]] static bool valid_name(const std::string& name);

private:
    std::filesystem::path dir_;
};

// A password of letters and digits only (no quoting needed anywhere: SQL,
// .env files, connection strings), from the system's secure random source.
// Nothing when that source fails.
[[nodiscard]] std::optional<std::string> generate_password(std::size_t length = 24);

}  // namespace floserver::core

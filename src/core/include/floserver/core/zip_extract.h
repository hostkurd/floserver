#pragma once

// Unpacks a runtime's .zip (D5). The archive has already matched the SHA-256
// in the signed list, so its contents are the project's official release;
// the checks here make sure that even a bad archive can only write inside
// the target folder:
//   - names are plain relative paths: no "..", no absolute or drive paths,
//     no ':' (alternate data streams), no Windows device names (CON, NUL...),
//     no trailing dots or spaces, printable ASCII only;
//   - symbolic links and encrypted entries are refused;
//   - entry count and total size are capped, and each entry must unpack to
//     exactly the size its header claims (with miniz's CRC-32 check).
// When every entry sits under one top folder (node-v24.22.1-win-x64/...),
// that folder is dropped, so a runtime's files land directly in its folder.

#include <cstdint>
#include <filesystem>
#include <string>

namespace floserver::core {

struct ZipLimits {
    std::uint64_t max_entries = 200'000;
    std::uint64_t max_total_bytes = std::uint64_t{8} * 1024 * 1024 * 1024;
};

struct ZipResult {
    bool ok = false;
    std::string message;  // why it failed, plain English
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};

// Unpacks `archive` into `target`, which must not exist yet. On failure,
// whatever was written is left for the caller to remove.
[[nodiscard]] ZipResult extract_zip(const std::filesystem::path& archive, const std::filesystem::path& target,
                                    const ZipLimits& limits = {});

// The relative path an entry name is unpacked to, or empty when the name is
// refused. Exposed for tests.
[[nodiscard]] std::filesystem::path safe_entry_path(std::string_view name);

}  // namespace floserver::core

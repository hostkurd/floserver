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
//
// A project's backup (D22, backup.h) is unpacked by the same rules, with
// ZipOptions: its layout kept as it is, names in any language (still no
// control characters or the characters Windows refuses), its own files
// (database.sql, floserver-backup.json) left in the archive, and a progress
// callback that can stop it.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

struct ZipLimits {
    std::uint64_t max_entries = 200'000;
    std::uint64_t max_total_bytes = std::uint64_t{8} * 1024 * 1024 * 1024;
};

struct ZipOptions {
    // Drop a single top folder every entry shares (a runtime's archive).
    bool drop_top_folder = true;
    // Names may be any UTF-8 (a backup of a project with Arabic file
    // names); otherwise printable ASCII only.
    bool unicode_names = false;
    // Files left in the archive, by their names there (a backup's own:
    // "floserver-backup.json", ".floserver/database.sql").
    std::vector<std::string> skip;
    // The free space where `target` is made; set, an archive that needs
    // more than it holds, and a tenth more, is refused before anything is
    // written (ZipResult::no_space). Nothing: it can't be told.
    std::function<std::optional<std::uint64_t>(const std::filesystem::path& folder)> free_space;
    // After each file: how many are done and how many there are. False
    // stops the unpacking (ZipResult::cancelled).
    std::function<bool(std::uint64_t done, std::uint64_t total)> progress;
};

struct ZipResult {
    bool ok = false;
    bool cancelled = false;
    bool no_space = false;
    std::string message;  // why it failed, plain English
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};

// Unpacks `archive` into `target`, which must not exist yet. On failure,
// whatever was written is left for the caller to remove.
[[nodiscard]] ZipResult extract_zip(const std::filesystem::path& archive, const std::filesystem::path& target,
                                    const ZipLimits& limits = {}, const ZipOptions& options = {});

// One file of the archive, by its exact name there, when it's there, is a
// plain file and is at most `max_bytes`; else nothing, with `error` saying
// why (empty when it just isn't there).
[[nodiscard]] std::optional<std::string> read_zip_entry(const std::filesystem::path& archive, std::string_view name,
                                                        std::size_t max_bytes, std::string& error);
// The same, written to `file` (a backup's database.sql, which may be large).
[[nodiscard]] bool extract_zip_entry(const std::filesystem::path& archive, std::string_view name,
                                     const std::filesystem::path& file, std::string& error);
// The same, handed to `sink` piece by piece as it unpacks (false from it
// stops); `size`, when given, is told the entry's size first.
[[nodiscard]] bool stream_zip_entry(const std::filesystem::path& archive, std::string_view name,
                                    const std::function<bool(std::string_view piece)>& sink, std::string& error,
                                    std::uint64_t* size = nullptr);
// The unpacked size of a file in the archive, or nothing when it isn't there.
[[nodiscard]] std::optional<std::uint64_t> zip_entry_size(const std::filesystem::path& archive, std::string_view name,
                                                          std::string& error);

// The relative path an entry name is unpacked to, or empty when the name is
// refused. Exposed for tests.
[[nodiscard]] std::filesystem::path safe_entry_path(std::string_view name, bool unicode_names = false);

}  // namespace floserver::core

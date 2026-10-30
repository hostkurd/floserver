#pragma once

// Where FloServer keeps its own files (D8), and UTF-8 <-> path conversion.
//
//   Windows  %LOCALAPPDATA%\FloServer
//   macOS    ~/Library/Application Support/FloServer
//   Linux    $XDG_DATA_HOME/floserver, else ~/.local/share/floserver
//
// FLOSERVER_DATA_DIR replaces the folder, for tests and dev copies (as
// FLOSERVER_INSTANCE does for the channel).
//
// Inside it:
//   runtimes/<name>/<version>   one shared copy per version (D8, D10)
//   downloads/                  archives while they download, deleted after install
//   state/manifest-state.json   last runtime-list sequence accepted, per key

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace floserver::core {

// Nothing when the system gives no usable folder (no LOCALAPPDATA or HOME).
[[nodiscard]] std::optional<std::filesystem::path> data_dir();

[[nodiscard]] std::filesystem::path runtimes_dir(const std::filesystem::path& data);
[[nodiscard]] std::filesystem::path downloads_dir(const std::filesystem::path& data);
[[nodiscard]] std::filesystem::path manifest_state_file(const std::filesystem::path& data);
// The last signed runtime list with templates, kept for offline use (D9).
[[nodiscard]] std::filesystem::path kept_manifest_file(const std::filesystem::path& data);
[[nodiscard]] std::filesystem::path kept_signature_file(const std::filesystem::path& data);

// path(std::string) reads the ANSI code page on Windows; these always use UTF-8.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

// A system error as UTF-8. std::error_code::message() gives the ANSI code
// page on Windows, which isn't UTF-8 on an Arabic or Greek system.
[[nodiscard]] std::string error_text(const std::error_code& ec);

// The platform name used in the manifest: "windows-x64", "linux-x64"...
[[nodiscard]] std::string_view current_platform();

// Writes `bytes` to `file` through a temporary file in the same folder and a
// rename, so a crash leaves the old file or the new one, never half of one.
[[nodiscard]] bool write_file_atomically(const std::filesystem::path& file, std::string_view bytes,
                                         std::string& error);

// The same for secrets (private keys): on macOS and Linux the file is
// created owner-only (0600, O_EXCL) and its folder is made owner-only (0700).
// On Windows the per-user app folder already keeps other users out.
[[nodiscard]] bool write_private_file_atomically(const std::filesystem::path& file, std::string_view bytes,
                                                 std::string& error);

// The whole file, or nothing if it can't be read or is larger than `max_bytes`.
[[nodiscard]] std::optional<std::string> read_small_file(const std::filesystem::path& file,
                                                         std::size_t max_bytes);

}  // namespace floserver::core

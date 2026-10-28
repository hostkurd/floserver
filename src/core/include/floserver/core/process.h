#pragma once

// Small process helpers shared by the service, the CLI and the window.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace floserver::core {

[[nodiscard]] std::uint32_t current_pid();

// Memory this process uses: private bytes on Windows, resident memory
// elsewhere (what D15's budgets measure). Nothing if the system won't say.
[[nodiscard]] std::optional<std::uint64_t> memory_in_use();

// The running program's own file.
[[nodiscard]] std::optional<std::filesystem::path> executable_path();

// Starts `program` with `args`, detached from this console and process group,
// with no window and standard input and output closed. It keeps running after
// the caller exits.
[[nodiscard]] bool spawn_detached(const std::filesystem::path& program,
                                  const std::vector<std::string>& args, std::string& error);

// The name of a sibling program in the same folder as this one, with ".exe" on
// Windows: sibling_program("floserverd").
[[nodiscard]] std::optional<std::filesystem::path> sibling_program(const std::string& name);

}  // namespace floserver::core

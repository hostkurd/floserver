#pragma once

// Small process helpers shared by the service, the CLI and the window.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace floserver::core {

[[nodiscard]] std::uint32_t current_pid();

// An environment variable, or nothing when it isn't set.
[[nodiscard]] std::optional<std::string> get_env(const char* name);

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

// Runs `program` without a window and waits up to `limit` for it to finish.
// Its exit code, or nothing (with `error`) if it couldn't start or ran too
// long; then it's killed (it may be stuck, holding files open) and
// `timed_out`, when given, is set.
[[nodiscard]] std::optional<int> run_and_wait(const std::filesystem::path& program,
                                              const std::vector<std::string>& args,
                                              std::chrono::milliseconds limit, std::string& error,
                                              bool* timed_out = nullptr);

// The name of a sibling program in the same folder as this one, with ".exe" on
// Windows: sibling_program("floserverd").
[[nodiscard]] std::optional<std::filesystem::path> sibling_program(const std::string& name);

}  // namespace floserver::core

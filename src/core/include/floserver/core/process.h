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

// Starts a program the user sees (a code editor, FloCMS Studio) with `args`,
// never through a shell: its own windows, and a console of its own if it's a
// console program (spawn_detached's "no window" would hide one for good).
// It runs in `folder` (the program's own folder when empty) and, when the
// caller's job allows it, outside the caller's job, so it outlives the caller.
// On Windows only an .exe is started (a .bat or .cmd would go through cmd.exe).
[[nodiscard]] bool start_program(const std::filesystem::path& program, const std::vector<std::string>& args,
                                 const std::filesystem::path& folder, std::string& error);

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

// Waits for another process to end, by its id. Made before that process is
// asked to quit, so its id can't be reused meanwhile (on Windows it holds the
// process open; elsewhere it checks the id is still in use).
class ProcessExitWatch {
public:
    explicit ProcessExitWatch(std::uint32_t pid);
    ProcessExitWatch(const ProcessExitWatch&) = delete;
    ProcessExitWatch& operator=(const ProcessExitWatch&) = delete;
    ProcessExitWatch(ProcessExitWatch&&) = delete;
    ProcessExitWatch& operator=(ProcessExitWatch&&) = delete;
    ~ProcessExitWatch();

    // True once the process has ended (or there was none to watch); false if
    // it's still running after `limit`.
    [[nodiscard]] bool wait(std::chrono::milliseconds limit);

private:
    std::uint32_t pid_ = 0;
    void* handle_ = nullptr;  // Windows: the process, opened for waiting
};

}  // namespace floserver::core

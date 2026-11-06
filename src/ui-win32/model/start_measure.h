#pragma once

// D15's budgets, checked by CI on every PR (as FloCMS Studio's --memory-test):
// the window open under 20 MB, the service with nothing running under 10 MB,
// the first window shown under 300 ms. FloServer.exe --measure <file> starts
// as usual, writes what it measured to <file> and closes its window (the
// service keeps running). Plain C++ so the file's shape and the budgets are
// tested on every platform; main.cpp does the measuring.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// FloServer.exe --measure <file> [--instance <suffix>]
inline constexpr std::string_view kMeasureFlag = "--measure";
// How long after the first paint the memory is read: the service's first answers are in by then.
inline constexpr std::chrono::milliseconds kMeasureSettle{2000};

inline constexpr std::uint64_t kMiB = std::uint64_t{1024} * 1024U;
inline constexpr std::uint64_t kWindowBudgetBytes = 20 * kMiB;   // the window's private bytes
inline constexpr std::uint64_t kServiceBudgetBytes = 10 * kMiB;  // the idle service's private bytes
inline constexpr std::int64_t kFirstWindowBudgetMs = 300;       // process created to first paint

// What --measure writes:
// {"first_window_ms": 142, "private_bytes": 9437184, "working_set_bytes": 15728640}
struct StartMeasure {
    std::int64_t first_window_ms = 0;
    std::uint64_t private_bytes = 0;
    std::uint64_t working_set_bytes = 0;
    bool operator==(const StartMeasure&) const = default;
};

// The file after --measure; nullopt when it isn't asked for (or has no file).
[[nodiscard]] std::optional<std::string> parse_measure_args(const std::vector<std::string>& args);

[[nodiscard]] std::string measure_text(const StartMeasure& measure);
// nullopt unless all three numbers are there, none negative.
[[nodiscard]] std::optional<StartMeasure> parse_measure(std::string_view text);

// Milliseconds between two FILETIMEs (100 ns since 1601); never below 0.
[[nodiscard]] std::int64_t filetime_ms_between(std::uint64_t from, std::uint64_t to);

// One line for each budget broken, empty when all are kept. `service_bytes`
// is flo status's memory_bytes; nullopt when it wasn't measured.
[[nodiscard]] std::vector<std::string> budgets_broken(const StartMeasure& window,
                                                      std::optional<std::uint64_t> service_bytes);

}  // namespace floserver::ui

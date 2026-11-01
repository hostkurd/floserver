#pragma once

// Settings → Folders: how much FloServer's data folder (paths.h) holds, and
// its two clean-ups. Only ever run for a request (when the section opens or
// a button is pressed), never on a timer.
//
// What the clean-ups may touch, and nothing else:
//
//   Clear old logs (each emptied to nothing, never deleted, so a running
//   server's open handle writes on at the new start; one held without
//   sharing is skipped):
//     <data>/web/logs/*.log         nginx, Apache, PHP pools
//     <data>/web/logs/sites/*.log   each project's request log
//     <data>/web/apps/*.log         Node.js and Python servers
//     <data>/cli/php/*.log          PHP run from the command line
//
//   Remove leftover downloads and partial installs (deleted):
//     <data>/downloads/*.part, *.download        cut-off or unused downloads
//     <data>/runtimes/<name>/.staging-*, .removing-*   half-made or half-removed installs
//     <data>/templates/job-*                      a New project's work folder
//   The first two only while no install is running, the last only while no
//   project is being made.
//
// Never: an installed runtime, a project's files (they're outside the data
// folder), databases (<data>/data), config.json and the other settings,
// certificates, secrets, the MariaDB logs. Links and junctions are removed
// as links: nothing they point to is read or deleted.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace floserver::core {

struct DiskUsage {
    std::uintmax_t bytes = 0;
    std::uintmax_t files = 0;
    bool complete = true;  // false: the walk stopped at the deadline (bytes is then "at least")
};

// The files under `dir`, not following links or junctions; it stops at
// `deadline` (checked every few entries). A missing folder is empty.
[[nodiscard]] DiskUsage disk_usage(const std::filesystem::path& dir, std::chrono::steady_clock::time_point deadline);

struct CleanItem {
    std::filesystem::path path;
    std::uintmax_t bytes = 0;
    bool folder = false;  // a leftover install or work folder
};

// The logs "Clear old logs" empties (see above), with their sizes; empty ones are left out.
[[nodiscard]] std::vector<CleanItem> log_files(const std::filesystem::path& data);

// The leftovers "Remove leftover downloads" deletes (see above). With
// `runtimes_busy` (an install or removal running) the downloads and
// install folders are left out; with `templates_busy` the work folders.
// A folder's size stops counting at `deadline`.
[[nodiscard]] std::vector<CleanItem> leftovers(const std::filesystem::path& data, bool runtimes_busy,
                                               bool templates_busy, std::chrono::steady_clock::time_point deadline);

[[nodiscard]] std::uintmax_t total_bytes(const std::vector<CleanItem>& items);

struct CleanResult {
    std::uintmax_t freed = 0;
    std::size_t done = 0;
    std::size_t skipped = 0;  // in use, or gone already
};

// Empties each of log_files(data) to length 0. Anything not a log there is
// refused (it's checked again here, whatever the list says).
[[nodiscard]] CleanResult empty_logs(const std::filesystem::path& data, const std::vector<CleanItem>& logs);

// Deletes each of leftovers(data, ...), checked again the same way; inside a
// folder, links and junctions go as links.
[[nodiscard]] CleanResult remove_leftovers(const std::filesystem::path& data, const std::vector<CleanItem>& items);

// Whether `path` is one the clean-ups may touch (the patterns above).
[[nodiscard]] bool is_log_file(const std::filesystem::path& data, const std::filesystem::path& path);
[[nodiscard]] bool is_leftover(const std::filesystem::path& data, const std::filesystem::path& path);

}  // namespace floserver::core

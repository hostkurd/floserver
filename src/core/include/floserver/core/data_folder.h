#pragma once

// Settings → Folders: how much FloServer's data folder (paths.h) holds, and
// its two clean-ups. Only ever run for a request (when the section opens or
// a button is pressed), never on a timer.
//
// The clean-ups delete files, so they work from an allow-list of places and
// name patterns, never from a list of things to avoid. What they may touch,
// and nothing else:
//
//   Clear old logs, in these places only (their own files, no sub-folder):
//     <data>/web/logs/     nginx, Apache, PHP pools
//     <data>/web/apps/     Node.js and Python servers
//     <data>/cli/php/      PHP run from the command line
//   - an older copy of a log ("x.log.1", "x.log.old", "x.log.bak",
//     "x.log.gz", "x.log.2.gz", "x.log.20261101"): deleted;
//   - a current "x.log" nothing has written to for kLogQuietFor (a day):
//     emptied, not deleted, so a program that still has it open writes on
//     at the new start. One written to within the day belongs to something
//     running (or that ran just now) and is left as it is.
//   Never: each project's request log (<data>/web/logs/sites/, the panel's
//   Log tab), a PHP pool's "*.access.log" (the service measures how long a
//   pool has slept from it and empties it itself), MariaDB's files (they're
//   in <data>/data), anything in a sub-folder of a place.
//
//   Remove leftover downloads and partial installs (deleted):
//     <data>/downloads/*.part, *.download        cut-off or unused downloads
//     <data>/runtimes/<name>/.staging-*, .removing-*   half-made or half-removed installs
//     <data>/templates/job-*                      a New project's work folder
//   The first two only while no install is running, the last only while no
//   project is being made.
//
// Never: an installed runtime, a project's files, databases (<data>/data),
// config.json and the other settings, certificates, secrets.
//
// Links: every part of the path from the data folder down to a place and to
// each file in it is looked at; a symlink, junction or other reparse point
// there (a linked web, web/logs, downloads, runtimes, templates, cli...) is
// never gone into, and is reported. A leftover that is itself a link goes as
// the link (what it points to is left alone); links inside a leftover folder
// go as links too. A registered project's folder, the projects folder and
// the www folder are skipped (with anything inside them, and any leftover
// holding one) should one lie in a clean-up place, which only setting it by
// hand can do. A file another program holds is skipped and named; one file
// never stops the rest. A current log with more than one hard link (the same
// file under another name, maybe outside the data folder) is never emptied.
//
// Each file is opened without following a link (Windows:
// FILE_FLAG_OPEN_REPARSE_POINT; POSIX: O_NOFOLLOW), checked again on that
// handle (not a link, a plain file, one link, and for a current log its
// last-write time), and emptied or deleted through the same handle (POSIX
// deletes by name in its folder, opened O_NOFOLLOW, after an lstat there),
// so a file swapped for a link after it was listed is never written through.
// What's left: a folder above it swapped for a link between the check and
// the open. Only a program of the same user can do that, and it can already
// write anywhere the user can.
//
// The protected folders are compared both as given and as the file system
// names them (real_folder(): Windows' long names, so an 8.3 short
// LOCALAPPDATA still matches a project's full path).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace floserver::core {

// A current log is only emptied once nothing has written to it for this long.
inline constexpr std::chrono::hours kLogQuietFor{24};

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
    bool remove = false;  // a log's older copy: deleted (a current log is emptied)
};

// Why something was left: a short code the window words in its language, and
// the same in plain English for anything else that reads the reply.
struct CleanSkip {
    std::string path;  // relative to the data folder, '/' separated
    std::string why;   // "in_use", "link", "hard_link", "protected", "not_allowed", "unreadable"
    std::string reason;
    // "protected" for lying inside one: which ("project:shop", "projects",
    // "www"; CleanOptions::protected_kinds). Empty for one that holds a
    // protected folder, or when it isn't named.
    std::string inside;
};

struct CleanOptions {
    // Never touched, nor anything inside them, nor a leftover holding one:
    // every project's folder, the projects folder, the www folder.
    std::vector<std::filesystem::path> protected_dirs;
    // What each is, beside it: "project:<name>", "projects" or "www"; a skip
    // inside one says which (CleanSkip::inside). Shorter or empty: unnamed.
    std::vector<std::string> protected_kinds;
    std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now();
    // The file operations, replaceable so tests can play a file another
    // program holds; the real ones when empty. Each sets `ec` when it
    // couldn't be done.
    std::function<void(const std::filesystem::path&, std::error_code&)> remove_file;
    std::function<void(const std::filesystem::path&, std::error_code&)> empty_file;
};

// The ways a file operation is refused once the file is open (the real
// operations set these; replacements may too).
inline constexpr int kCleanIsLink = 1;      // it turned out to be a link
inline constexpr int kCleanHardLinked = 2;  // more than one hard link
inline constexpr int kCleanWritten = 3;     // a current log written to within the day
inline constexpr int kCleanGone = 4;        // not there any more
[[nodiscard]] const std::error_category& clean_category();

// The logs "Clear old logs" deletes or empties (see above), with their sizes;
// empty ones are left out. What can't be gone into (a link, a protected
// folder) is added to `skipped` when it's given.
[[nodiscard]] std::vector<CleanItem> log_files(const std::filesystem::path& data, const CleanOptions& options = {},
                                               std::vector<CleanSkip>* skipped = nullptr);

// The leftovers "Remove leftover downloads" deletes (see above). With
// `runtimes_busy` (an install or removal running) the downloads and
// install folders are left out; with `templates_busy` the work folders.
// A folder's size stops counting at `deadline`.
[[nodiscard]] std::vector<CleanItem> leftovers(const std::filesystem::path& data, bool runtimes_busy,
                                               bool templates_busy, std::chrono::steady_clock::time_point deadline,
                                               const CleanOptions& options = {},
                                               std::vector<CleanSkip>* skipped = nullptr);

[[nodiscard]] std::uintmax_t total_bytes(const std::vector<CleanItem>& items);

struct CleanResult {
    std::uintmax_t freed = 0;
    std::size_t done = 0;
    std::size_t skipped = 0;          // how many were left (skipped_items.size(), kept for older windows)
    std::vector<CleanSkip> skipped_items;
};

// Deletes or empties each of log_files(data). Anything not a log there is
// refused (it's checked again here, whatever the list says), and so is a
// current log written to within the day.
[[nodiscard]] CleanResult empty_logs(const std::filesystem::path& data, const std::vector<CleanItem>& logs,
                                     const CleanOptions& options = {});

// Deletes each of leftovers(data, ...), checked again the same way; inside a
// folder, links and junctions go as links.
[[nodiscard]] CleanResult remove_leftovers(const std::filesystem::path& data, const std::vector<CleanItem>& items,
                                           const CleanOptions& options = {});

// Whether `path` is one the clean-ups may touch (the places and patterns
// above; by name only: links and protected folders are checked apart).
[[nodiscard]] bool is_log_file(const std::filesystem::path& data, const std::filesystem::path& path);
[[nodiscard]] bool is_leftover(const std::filesystem::path& data, const std::filesystem::path& path);
// "php.log.1", "nginx-error.log.old", "x.log.bak", "x.log.gz", "x.log.3.gz": a log's older copy.
[[nodiscard]] bool is_rotated_log_name(std::string_view name);

// Every folder from below `data` down to `path`, and `path`, is a plain one
// (no symlink, junction or other reparse point); false for a path outside it.
// `linked` is set to the first part that is a link, when there is one.
[[nodiscard]] bool plain_path(const std::filesystem::path& data, const std::filesystem::path& path,
                              std::filesystem::path* linked = nullptr);

}  // namespace floserver::core

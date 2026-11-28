#pragma once

// D37: Clean up the CDN. Which files on R2 no list uses any more, when they
// left the list (from the lists the Publisher saved), and which are safe to
// delete. Planning only: deleting is R2Bucket::delete_object, purging is
// purge_cache, both run by the Publisher's window.

#include "golive.h"
#include "r2.h"

#include "floserver/core/http.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::manifest_tool {

// D37.3: a file is ticked when it left the list longer ago than this.
inline constexpr std::chrono::days kCleanupWait{7};

// The CDN paths a list uses (its runtimes' and app's first address under
// `cdn`), from the list's JSON. Nothing when the JSON isn't a list.
[[nodiscard]] std::optional<std::vector<std::string>> list_paths(std::string_view manifest_json, std::string_view cdn);

// D37.2: a list the Publisher signed and saved (Documents\FloServer
// Publisher\<date> sequence N\manifest.json).
struct SavedList {
    std::int64_t sequence = 0;
    std::chrono::sys_seconds made{};  // when it was signed
    std::vector<std::string> paths;   // list_paths() of it
};

// Every saved list in `folder` (Documents\FloServer Publisher): each
// "<date> sequence N" folder with a manifest.json. Its time is the "made"
// line of its upload-steps.txt, else the folder's date. Unreadable folders
// are skipped.
[[nodiscard]] std::vector<SavedList> read_saved_lists(const std::filesystem::path& folder, std::string_view cdn);

enum class CleanupKind : std::uint8_t {
    Ready,    // ticked: left the list more than kCleanupWait ago
    Wait,     // left less than kCleanupWait ago
    Unknown,  // in no saved list: when it left isn't known
    Kept,     // shown greyed: a GPL source whose binary is still listed
};

struct CleanupRow {
    std::string path;
    std::string title;  // "PHP 8.5.12 (Linux)", "MariaDB 11.4.13 source"
    std::uint64_t size = 0;
    CleanupKind kind = CleanupKind::Unknown;
    std::optional<std::chrono::sys_seconds> left;  // when it left the list
    std::string note;   // why it waits or is kept
    bool ticked = false;
};

struct CleanupInput {
    std::vector<R2Object> objects;        // the bucket
    std::vector<std::string> live_paths;  // the live list's
    std::vector<std::string> next_paths;  // the main window's files
    std::vector<SavedList> saved;
    std::chrono::sys_seconds now{};
    std::chrono::days wait = kCleanupWait;
};

// D37.1: one row per object no list uses that is a runtime file, a source
// archive or a FloServer installer; anything else in the bucket isn't
// shown. Ready rows first (by path), then Wait, Unknown, Kept.
[[nodiscard]] std::vector<CleanupRow> plan_cleanup(const CleanupInput& input);

// "left 12 days ago", "left today", "left: unknown", "—".
[[nodiscard]] std::string left_text(const CleanupRow& row, std::chrono::sys_seconds now);

// D37.5: purges `paths` (under `cdn`) from Cloudflare's cache, 30 at a time
// (the free plan's limit). Empty when every batch was purged.
[[nodiscard]] std::string purge_deleted(core::HttpTransport& http, const PurgeToken& token,
                                        const std::vector<std::string>& paths, std::string_view cdn,
                                        const std::atomic<bool>* cancel = nullptr);

// D37.6: what the record file says, one line per row deleted or failed.
struct CleanupDone {
    std::string path;
    std::uint64_t size = 0;
    std::string problem;  // empty: deleted
};
[[nodiscard]] std::string cleanup_record(const std::vector<CleanupDone>& done, std::chrono::sys_seconds when,
                                         std::string_view purge_note);

}  // namespace floserver::manifest_tool

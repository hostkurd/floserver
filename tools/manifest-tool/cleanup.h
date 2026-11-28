#pragma once

// D37: Clean up the CDN. Which files on R2 no list uses any more, when they
// left the list (from the lists the Publisher saved), and which are safe to
// delete. Planning only: deleting is R2Bucket::delete_object, purging is
// purge_cache, both run by the Publisher's window.

#include "golive.h"
#include "publish.h"
#include "r2.h"

#include "floserver/core/http.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::manifest_tool {

// D37.3: a file is ticked when it left the list longer ago than this.
inline constexpr std::chrono::days kCleanupWait{7};

// The CDN paths a list uses: every address under `cdn` of its runtimes and
// its app, from the list's JSON. Nothing when the JSON isn't a list, or when
// a runtime or the app has no address under `cdn` (then what it uses can't
// be told, so nothing may be deleted).
[[nodiscard]] std::optional<std::vector<std::string>> list_paths(std::string_view manifest_json, std::string_view cdn);

// The same for the live list as read (fetch_live_list).
[[nodiscard]] std::optional<std::vector<std::string>> list_paths(const LiveList& live, std::string_view cdn);

// D37.2: a list the Publisher signed and saved (Documents\FloServer
// Publisher\<date> sequence N\manifest.json).
struct SavedList {
    std::int64_t sequence = 0;
    std::chrono::sys_seconds made{};  // when it was signed
    // When the Publisher first saw it live (live-at.txt): no earlier than
    // it went live. Missing for lists from before D37.
    std::optional<std::chrono::sys_seconds> live_at;
    std::vector<std::string> paths;   // list_paths() of it
    std::map<std::string, std::string> sha256;  // path -> its SHA-256 in the list (D37.6's record)
};

// Every saved list in `folder` (Documents\FloServer Publisher): each
// "<date> sequence N" folder with manifest.json and manifest.json.sig. Its
// time is the "made" line of its upload-steps.txt, else the end of the
// folder's date. Unreadable folders are skipped.
[[nodiscard]] std::vector<SavedList> read_saved_lists(const std::filesystem::path& folder, std::string_view cdn);

// Notes when a saved list was seen live: writes "live at <utc>" to
// live-at.txt in `list_folder`, unless it's there already (the first time
// seen stays). False when it couldn't be written.
bool mark_live(const std::filesystem::path& list_folder, std::chrono::sys_seconds when);

// The same for each saved list in `folder` with the live list's sequence
// and files (an abandoned list of the same sequence isn't marked).
void mark_live_lists(const std::filesystem::path& folder, const LiveList& live, std::string_view cdn,
                     std::chrono::sys_seconds when);

enum class CleanupKind : std::uint8_t {
    Ready,    // ticked: left the list more than kCleanupWait ago
    Wait,     // left less than kCleanupWait ago
    Unknown,  // in no saved list: when it left isn't known
    Kept,     // shown greyed: a GPL source whose binary stays on the CDN
};

struct CleanupRow {
    std::string path;
    std::string title;  // "PHP 8.5.12 (Linux)", "MariaDB 11.4.13 source"
    std::uint64_t size = 0;
    CleanupKind kind = CleanupKind::Unknown;
    std::optional<std::chrono::sys_seconds> left;  // when it left the list
    std::string note;   // why it waits or is kept
    bool ticked = false;
    bool source = false;  // a GPL source archive (see tie_sources)
    std::string sha256;   // from the last saved list that had it, when known
};

struct CleanupInput {
    std::vector<R2Object> objects;        // the bucket (manifest.json's time: when the live list went up)
    std::int64_t live_sequence = 0;       // the live list's
    std::vector<std::string> live_paths;  // the live list's
    std::vector<std::string> next_paths;  // the main window's files
    std::vector<SavedList> saved;
    std::chrono::sys_seconds now{};
    std::chrono::days wait = kCleanupWait;
};

// D37.1: one row per object no list uses that is a runtime file, a source
// archive or a FloServer installer; anything else in the bucket isn't
// shown. Ready rows first (by path), then Wait, Unknown, Kept. Nothing at
// all without the live list (live_sequence and live_paths).
//
// When a file left: when the first saved list without it went live. That
// is the earliest live-at.txt time of that list or a later one, or the
// time manifest.json went up for the live list, and never before the list
// was signed, nor before the file itself was uploaded. Saved lists newer
// than the live one never went live, so they don't count; lists of one
// sequence count as one.
[[nodiscard]] std::vector<CleanupRow> plan_cleanup(const CleanupInput& input);

// D37 (GPL): a source archive goes only with every binary in its folder.
// Unticks a ticked source whose folder has a binary that isn't ticked.
// plan_cleanup calls it; the window calls it again after each change.
void tie_sources(std::vector<CleanupRow>& rows);

// The last guard before a delete: true only for a runtime file, a source
// archive or a FloServer installer, never manifest.json or anything else.
[[nodiscard]] bool may_delete(std::string_view key);

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
    std::string sha256;   // when known
    std::string problem;  // empty: deleted
    bool skipped = false;  // not tried (problem says why)
};
[[nodiscard]] std::string cleanup_record(const std::vector<CleanupDone>& done, std::chrono::sys_seconds when,
                                         std::string_view purge_note);

}  // namespace floserver::manifest_tool

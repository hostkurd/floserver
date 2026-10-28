#pragma once

// The downloader (D5): fetches the signed runtime list and the files it names.
//
// The runtime list: manifest.json and manifest.json.sig are fetched from the
// CDN and checked with open_manifest(). The two files are uploaded one after
// the other, so for a moment a cache may hold a new list with the old
// signature: a bad signature is retried once after a short wait before it
// counts. A list that has expired gets a clock hint when the server's Date
// header shows the computer's clock is off.
//
// Files: each URL of the entry is tried in order (our CDN first, then the
// project's own server). The body streams to "<file>.part" while its SHA-256
// is computed; more bytes than the list says stop the transfer at once. Only
// a file of the exact size and hash is renamed into place. Anything else is
// deleted and the next URL is tried.

#include "floserver/core/http.h"
#include "floserver/core/manifest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace floserver::core {

// The runtime list is small; anything bigger is refused before it is read.
inline constexpr std::uint64_t kMaxManifestBytes = std::uint64_t{1024} * 1024;
inline constexpr std::uint64_t kMaxSignatureBytes = 4096;

struct ManifestSource {
    std::string manifest_url;
    std::string signature_url;
};

enum class FetchError : std::uint8_t {
    None,
    Network,   // no reply: offline, DNS, TLS, timeout
    Http,      // a reply, but not 200
    TooBig,    // more bytes than any real list
    Manifest,  // fetched, but open_manifest() refused it: see manifest_error
};

struct ManifestFetch {
    std::optional<Manifest> manifest;
    std::string key_id;
    FetchError error = FetchError::None;
    ManifestError manifest_error = ManifestError::None;
    std::string message;                // plain English for logs and the UI
    std::vector<std::string> warnings;  // entries open_manifest() skipped
    bool clock_wrong = false;           // the server's clock disagrees with ours by more than an hour
};

struct ManifestFetchOptions {
    ManifestCheck check;
    std::chrono::milliseconds retry_delay{3000};
    // Waits between attempts; tests pass one that returns at once.
    std::function<void(std::chrono::milliseconds)> sleep;
    // When set and true, the fetch stops as soon as possible.
    const std::atomic<bool>* cancel = nullptr;
};

// Each of the two files must arrive within this time.
inline constexpr std::chrono::seconds kManifestMaxTime{60};

[[nodiscard]] ManifestFetch fetch_manifest(HttpTransport& transport, const ManifestSource& source,
                                           const ManifestFetchOptions& options);

// The highest sequence accepted per signing key (manifest.h, ManifestCheck),
// kept across restarts. A missing or unreadable file reads as empty: that
// only relaxes the rollback check back to what a fresh install has.
[[nodiscard]] std::map<std::string, std::int64_t, std::less<>> load_last_sequences(
    const std::filesystem::path& file);
// Raises the stored value for `key_id` to `sequence` (never lowers it).
[[nodiscard]] bool store_last_sequence(const std::filesystem::path& file, const std::string& key_id,
                                       std::int64_t sequence, std::string& error);

struct DownloadProgress {
    std::uint64_t received = 0;  // bytes of the current attempt
    std::uint64_t total = 0;     // the size in the list
    std::size_t attempt = 0;     // index into entry.urls
};

struct DownloadOptions {
    // Called as bytes arrive; return false to cancel.
    std::function<bool(const DownloadProgress&)> progress;
    // When set and true, the download stops even while waiting for data.
    const std::atomic<bool>* cancel = nullptr;
};

// How long one attempt at a file may take: two minutes plus the time at
// 16 KiB/s, so a slow but honest line finishes and a stalling server doesn't
// hold the install for ever.
[[nodiscard]] std::chrono::seconds download_time_limit(std::uint64_t size);

struct DownloadResult {
    bool ok = false;
    bool cancelled = false;
    std::string message;                // why it failed, plain English
    std::vector<std::string> attempts;  // one line per URL tried, for the log
};

// Downloads `entry` to `destination`. A file already there with the right
// size and hash is kept without downloading again.
[[nodiscard]] DownloadResult download_file(HttpTransport& transport, const RuntimeFile& entry,
                                           const std::filesystem::path& destination,
                                           const DownloadOptions& options = {});

// The SHA-256 and size of a file on disk, or nothing if it can't be read.
struct FileDigest {
    std::uint64_t size = 0;
    Sha256Digest sha256{};
};
[[nodiscard]] std::optional<FileDigest> digest_file(const std::filesystem::path& file);

}  // namespace floserver::core

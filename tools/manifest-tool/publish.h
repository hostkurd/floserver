#pragma once

// What FloServer Publisher (D18) does, without its window, so it's tested
// with a fake transport. The steps, as on tools/cdn-check and
// floserver-publishing-routine:
//   1. read the live list from the CDN (signed with a key FloServer trusts);
//   2. check the files: those already in the live list are still served at
//      their size (the app checks every byte against the signed SHA-256 when
//      it downloads them); new ones are downloaded from the CDN and compared
//      with the checksum their project publishes;
//   3. build the next list (live sequence + 1, 90 days) from those results;
//   4. sign it (signing.h).
// Putting the signed list live (R2 upload, cache purge, Check live) is golive.h.
//
// FloServer's own installer (app/<x.y.z>/FloServer-<x.y.z>-x64.msi, the
// list's "app", Q5) is one of the files: checked by hand, as Apache is
// (compare its SHA-256 with the CI artifact's and tick it), with its "What's
// new" page typed after its path when it's added.

#include "floserver/core/http.h"
#include "floserver/core/manifest.h"
#include "floserver/core/zip_extract.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::manifest_tool {

// Cloudflare R2 bucket floserver-cdn, served through its custom domain.
inline constexpr std::string_view kCdn = "https://cdn.floserver.org";

struct LiveList {
    std::int64_t sequence = 0;
    std::chrono::sys_seconds expires{};
    bool expired = false;  // still read (the signature is checked), so it can be re-signed
    std::string key_id;
    std::vector<core::RuntimeFile> runtimes;
    std::size_t template_count = 0;
    std::string templates_json;  // the live "templates", compact JSON
    std::optional<core::AppRelease> app;  // the live "app"
};

struct LiveResult {
    std::optional<LiveList> list;
    std::string error;
};

[[nodiscard]] LiveResult fetch_live_list(core::HttpTransport& http, std::string_view cdn,
                                         const std::vector<core::TrustedKey>& keys,
                                         std::chrono::sys_seconds now,
                                         const std::atomic<bool>* cancel = nullptr);

enum class CheckState : std::uint8_t {
    NotRun,
    Ok,      // still served (published) or matches the official file (new)
    ByHand,  // no official checksum to compare with (Apache Lounge): compare by hand
    Failed,
};

struct PublishFile {
    std::string path;        // <runtime>/<version>/<file> on the CDN
    std::string title;       // "PHP 8.5.13"
    bool published = false;  // in the live list
    std::uint64_t size = 0;  // known for published files; measured for new ones
    std::string sha256;      // hex; from the live list, or measured
    CheckState state = CheckState::NotRun;
    std::string detail;      // what the check found, for the window
    bool accepted = false;   // a ByHand file the user compared and ticked
    std::string notes_url;   // FloServer's installer: its "What's new" page, or empty
};

// The live list's files, in its order, then its FloServer installer. One
// whose first address isn't on `cdn` can't be carried over and is left out
// with a note.
[[nodiscard]] std::vector<PublishFile> files_from_live(const LiveList& live, std::string_view cdn,
                                                       std::vector<std::string>& notes);

// "PHP 8.5.13" for php/8.5.13/..., "FloServer 1.2.0" for app/1.2.0/...; a
// variant other than PHP's "nts" is added ("Bun 1.3.14 baseline"), and
// php-redis says its PHP series ("phpredis 6.3.0 for PHP 8.4").
[[nodiscard]] std::string runtime_title(std::string_view runtime, std::string_view version,
                                        std::string_view variant = {});
// A file's title in the Publisher: runtime_title plus the platform for one
// that isn't Windows' ("PHP 8.5.12 (Linux)" beside "PHP 8.5.12").
[[nodiscard]] std::string file_title(std::string_view runtime, std::string_view version, std::string_view variant,
                                     std::string_view platform);

// A new file to add: trims spaces, a leading '/' and a CDN address the user
// pasted with it. FloServer's installer may have its "What's new" address
// after it, past a space. Fails when the catalog can't carry it or it's
// already there (one installer only: the one listed goes first).
struct AddResult {
    std::optional<PublishFile> file;
    std::string error;
};
[[nodiscard]] AddResult new_file(std::string_view text, std::string_view cdn,
                                 const std::vector<PublishFile>& existing);

// Where the official checksum comes from.
struct OfficialSource {
    // Unknown: a runtime with no source set here yet; its files fail.
    // MadeFrom (D29): our zip is a part of the original zip, so every file in
    // ours must be byte-identical to the same path in the original's.
    enum class Kind : std::uint8_t { Unknown, Sums, Original, ByHand, MadeFrom } kind = Kind::Unknown;
    std::vector<std::string> urls;  // checksum lists to try in order, or the original file
    std::string name;               // the file's name in the checksum list
    bool powershell_print = false;  // the list is a Get-FileHash print (Deno's)
    std::vector<std::string> required;  // MadeFrom: paths ours must have
    // MadeFrom: folders whose files in the original must all be in ours too
    // (the same list as the script that makes our zip).
    std::vector<std::string> kept;
};
[[nodiscard]] OfficialSource official_source(std::string_view path);

// The checksum for `name` in a checksum list: the line naming the file as a
// whole word, and the 64-hex value on it; or a list holding just one hash.
// `powershell_print` (Deno's lists only) also accepts a Get-FileHash print: a
// "Hash :" line, the file named, and exactly one distinct hash in the text.
[[nodiscard]] std::optional<std::string> find_official_hash(std::string_view sums, std::string_view name,
                                                            bool powershell_print = false);

// MadeFrom: what's wrong with `ours` against `original` (entries as
// zip_file_hashes gives them), or empty when every file in ours is the same
// file at the same path in the original, every folder entry in ours holds
// some of those files, ours has every file the original has under `kept`,
// and every one of `required`. Paths compare with '\\' read as '/'.
[[nodiscard]] std::string made_from_problem(const std::vector<std::pair<std::string, core::ZipFileHash>>& ours,
                                            const std::vector<std::pair<std::string, core::ZipFileHash>>& original,
                                            const std::vector<std::string>& required,
                                            const std::vector<std::string>& kept = {});

// What check_file is doing, for the window: a MadeFrom check downloads a
// second file and then compares them.
enum class CheckPhase : std::uint8_t { Download, Original, Compare };

struct CheckContext {
    CheckContext(core::HttpTransport& transport, std::string_view cdn_base)
        : http(transport), cdn(cdn_base) {}
    core::HttpTransport& http;
    std::string cdn;
    const std::atomic<bool>* cancel = nullptr;
    // Bytes done of the current download, for the window.
    std::function<void(std::uint64_t done)> progress;
    // Each phase after the first download (bytes start again from 0).
    std::function<void(CheckPhase)> phase;
    std::map<std::string, std::optional<std::string>> sums_cache;  // url -> text
    // MadeFrom downloads both zips here for a moment; empty: the system's temp folder.
    std::filesystem::path scratch;
    std::string sums_from;  // the checksum list official_checksum() last found a file in
};

// The official SHA-256 of the file at `cdn_path`, for a runtime whose project
// publishes checksum lists (OfficialSource::Kind::Sums: php.net, MariaDB,
// nodejs.org...). Nothing with `why_not` empty for the others (Publish
// compares those with the original); nothing with `why_not` set when the
// list can't be read or doesn't name the file.
[[nodiscard]] std::optional<std::string> official_checksum(CheckContext& ctx, const std::string& cdn_path,
                                                           std::string& why_not);

// Checks one file and sets its state, detail, and (new files) size and sha256.
void check_file(CheckContext& ctx, PublishFile& file);

// The time for the clock check: the CDN's Date header on a request no cache
// can answer (a query string used once), so it's Cloudflare's current time.
struct CdnClock {
    std::optional<std::chrono::sys_seconds> server_date;
    std::string error;  // set when the CDN didn't answer 200
};
[[nodiscard]] CdnClock read_cdn_clock(core::HttpTransport& http, std::string_view cdn,
                                      std::chrono::sys_seconds now, const std::atomic<bool>* cancel = nullptr);

// Before building on `live`: what stops publishing, and what needs the
// user's yes. `templates_json` is what the new list will carry.
struct SuccessorCheck {
    std::vector<std::string> stop;
    std::vector<std::string> confirm;
};
[[nodiscard]] SuccessorCheck check_successor(const LiveList& live, std::string_view templates_json,
                                             std::chrono::sys_seconds now);

// The clock check: empty when `server_date` is within 10 minutes of `now`.
[[nodiscard]] std::string clock_problem(std::optional<std::chrono::sys_seconds> server_date,
                                        std::chrono::sys_seconds now);

struct BuiltList {
    std::optional<std::string> manifest_json;
    std::vector<std::string> errors;
    std::vector<std::string> notes;
    std::int64_t sequence = 0;
    std::size_t runtime_count = 0;
    std::size_t template_count = 0;
    std::chrono::sys_seconds expires{};
};

// The next list from checked files: every file must be Ok, or ByHand and
// accepted. Uses the built-in templates.
[[nodiscard]] BuiltList build_next_list(std::string_view cdn, const std::vector<PublishFile>& files,
                                        std::int64_t sequence, std::chrono::sys_seconds now,
                                        int days = 90);

// "36.4 MB"
[[nodiscard]] std::string format_size(std::uint64_t bytes);

}  // namespace floserver::manifest_tool

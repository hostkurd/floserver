#pragma once

// What FloServer Publisher (D18) does, without its window, so it's tested
// with a fake transport. The steps, as on tools/cdn-check and
// floserver-publishing-routine:
//   1. read the live list from the CDN (signed with a key FloServer trusts);
//   2. check the files: those already in the live list are still served at
//      their size (the app checks every byte against the signed SHA-256 when
//      it downloads them); new ones are downloaded from the CDN and compared
//      with the checksum their project publishes; the S3 bucket must refuse
//      direct reads;
//   3. build the next list (live sequence + 1, 90 days) from those results;
//   4. sign it (signing.h).
// Uploading and the CloudFront invalidation stay by hand.

#include "floserver/core/http.h"
#include "floserver/core/manifest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::manifest_tool {

inline constexpr std::string_view kCdn = "https://cdn.floserver.org";
// The bucket behind the CDN; it must refuse direct reads (only CloudFront may read it).
inline constexpr std::string_view kDirectS3 = "https://floserver-cdn-origin.s3.eu-central-1.amazonaws.com";

struct LiveList {
    std::int64_t sequence = 0;
    std::chrono::sys_seconds expires{};
    bool expired = false;  // still read (the signature is checked), so it can be re-signed
    std::string key_id;
    std::vector<core::RuntimeFile> runtimes;
    std::size_t template_count = 0;
    std::string templates_json;  // the live "templates", compact JSON
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
};

// The live list's files, in its order. A runtime whose first address isn't
// on `cdn` can't be carried over and is left out with a note.
[[nodiscard]] std::vector<PublishFile> files_from_live(const LiveList& live, std::string_view cdn,
                                                       std::vector<std::string>& notes);

// "PHP 8.5.13" for php/8.5.13/...
[[nodiscard]] std::string runtime_title(std::string_view runtime, std::string_view version);

// A new file to add: trims spaces, a leading '/' and a CDN address the user
// pasted with it. Fails when the catalog can't carry it or it's already there.
struct AddResult {
    std::optional<PublishFile> file;
    std::string error;
};
[[nodiscard]] AddResult new_file(std::string_view text, std::string_view cdn,
                                 const std::vector<PublishFile>& existing);

// Where the official checksum comes from.
struct OfficialSource {
    enum class Kind : std::uint8_t { Sums, Original, ByHand } kind = Kind::ByHand;
    std::vector<std::string> urls;  // checksum lists to try in order, or the original file
    std::string name;               // the file's name in the checksum list
};
[[nodiscard]] OfficialSource official_source(std::string_view path);

// The checksum for `name` in a checksum list: the line naming the file as a
// whole word, and the 64-hex value on it; or a list holding just one hash.
[[nodiscard]] std::optional<std::string> find_official_hash(std::string_view sums,
                                                            std::string_view name);

struct CheckContext {
    CheckContext(core::HttpTransport& transport, std::string_view cdn_base)
        : http(transport), cdn(cdn_base) {}
    core::HttpTransport& http;
    std::string cdn;
    const std::atomic<bool>* cancel = nullptr;
    // Bytes done of the current download, for the window.
    std::function<void(std::uint64_t done)> progress;
    std::map<std::string, std::optional<std::string>> sums_cache;  // url -> text
};

// Checks one file and sets its state, detail, and (new files) size and sha256.
void check_file(CheckContext& ctx, PublishFile& file);

// Direct reads of the bucket must give 403. `problem` is empty when they do.
// S3's Date header is fresh (CloudFront's may be cached), so it also serves
// as the clock check before signing.
struct S3Check {
    std::string problem;
    std::optional<std::chrono::sys_seconds> server_date;
};
[[nodiscard]] S3Check check_direct_s3(core::HttpTransport& http, std::string_view s3_base,
                                      std::string_view path, const std::atomic<bool>* cancel = nullptr);

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

#pragma once

// D34.6: after signing, the Publisher puts the new list live itself:
//   1. reads the list on R2, which must still be the one it replaces (or
//      already the new one, when an earlier try got half way);
//   2. uploads manifest.json, then manifest.json.sig; if the second fails,
//      the old manifest.json goes back, so R2 never keeps a mixed pair;
//   3. purges both from Cloudflare's cache, with a token allowed to purge
//      the floserver.org zone only (Q2), when one is saved;
//   4. reads the live list from the CDN until it shows the new sequence.
// Without an R2 key the two files are uploaded by hand, as before.

#include "r2.h"
#include "publish.h"

#include "floserver/core/http.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::manifest_tool {

// A Cloudflare API token with Zone -> Cache Purge -> Purge on floserver.org
// only, and that zone's ID (it can't look the zone up itself).
struct PurgeToken {
    std::string zone_id;  // 32 characters, 0-9 a-f, on the zone's overview page
    std::string token;
};

// What's wrong with a token typed in, or empty.
[[nodiscard]] std::string purge_token_problem(const PurgeToken& token);

inline constexpr std::string_view kCloudflareApi = "https://api.cloudflare.com/client/v4";

// https://cdn.floserver.org/manifest.json and .sig.
[[nodiscard]] std::vector<std::string> list_urls(std::string_view cdn);

// Purges `urls` from the zone's cache. Empty when Cloudflare said yes;
// otherwise why not. One retry on no answer or 5xx.
[[nodiscard]] std::string purge_cache(core::HttpTransport& http, const PurgeToken& token,
                                      const std::vector<std::string>& urls, const std::atomic<bool>* cancel = nullptr);

// The "sequence" in a list's JSON, without checking its signature: only to
// see which list R2 has before replacing it.
[[nodiscard]] std::optional<std::int64_t> list_sequence(std::string_view manifest_json);

struct GoLiveInput {
    std::string manifest_json;
    std::string signature;
    std::int64_t sequence = 0;       // the new list's
    std::int64_t live_sequence = 0;  // the list it replaces
};

struct GoLiveOptions {
    // Purged only when set.
    std::optional<PurgeToken> purge;
    std::string cdn = std::string(kCdn);
    // Reads the live list as FloServer does (signature checked).
    std::function<LiveResult()> read_live;
    // How often the CDN is asked, and how long between.
    int checks = 12;
    std::chrono::seconds wait{5};
    std::function<void(std::chrono::seconds)> sleep;  // tests don't wait
    // What it's doing, for the window.
    std::function<void(const std::string&)> say;
    // Stops before the upload or while checking; never between the two files.
    const std::atomic<bool>* cancel = nullptr;
};

struct GoLiveResult {
    bool uploaded = false;  // both files are on R2
    bool purged = false;
    bool live = false;      // the CDN shows the new sequence
    // The signature failed and the old manifest.json couldn't go back:
    // R2 has the new list with the old signature. Upload both by hand now.
    bool mixed = false;
    bool rolled_back = false;  // the signature failed; R2 has the old pair again
    std::int64_t seen_sequence = 0;
    std::string problem;  // why it stopped short of "live"
    std::string note;     // something that didn't stop it (the purge)
};

[[nodiscard]] GoLiveResult go_live(core::HttpTransport& http, R2Bucket& bucket, const GoLiveInput& input,
                                   const GoLiveOptions& options);

}  // namespace floserver::manifest_tool

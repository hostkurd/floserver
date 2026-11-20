#include "golive.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <regex>
#include <thread>

namespace floserver::manifest_tool {

namespace {

using json = nlohmann::json;

constexpr const char* kList = "manifest.json";
constexpr const char* kSig = "manifest.json.sig";
constexpr std::size_t kMaxList = std::size_t{8} * 1024 * 1024;  // as publish.cpp reads it
constexpr std::size_t kMaxSig = std::size_t{64} * 1024;

bool stopped(const std::atomic<bool>* cancel) { return cancel != nullptr && cancel->load(); }

// Cloudflare's reason: the first of "errors", with its code.
std::string cloudflare_error(const core::HttpResponse& r) {
    if (r.status == 0) {
        return r.error.empty() ? "no answer" : r.error;
    }
    std::string out = "HTTP " + std::to_string(r.status);
    const json doc = json::parse(r.body, nullptr, false);
    if (doc.is_object() && doc.contains("errors") && doc["errors"].is_array() && !doc["errors"].empty()) {
        const json& e = doc["errors"].front();
        if (e.is_object() && e.contains("message") && e["message"].is_string()) {
            out += ": " + e["message"].get<std::string>();
        }
        if (e.is_object() && e.contains("code") && e["code"].is_number_integer()) {
            out += " (code " + std::to_string(e["code"].get<std::int64_t>()) + ")";
        }
    }
    if (r.status == 401 || r.status == 403) {
        out += ". Is it the token for Cache Purge on floserver.org, and that zone's ID?";
    }
    return out;
}

}  // namespace

std::string purge_token_problem(const PurgeToken& t) {
    static const std::regex zone("^[0-9a-f]{32}$");
    // Cloudflare's API tokens are 40 characters today; allow for longer ones.
    static const std::regex token("^[A-Za-z0-9_-]{20,200}$");
    if (!std::regex_match(t.zone_id, zone)) {
        return "the Zone ID is 32 characters, 0-9 and a-f (floserver.org's overview page shows it, under API)";
    }
    if (!std::regex_match(t.token, token)) {
        return "the API token is letters, digits, - and _ (shown once when the token is made)";
    }
    return {};
}

std::vector<std::string> list_urls(std::string_view cdn) {
    const std::string base(cdn);
    return {base + "/" + kList, base + "/" + kSig};
}

std::string purge_cache(core::HttpTransport& http, const PurgeToken& token, const std::vector<std::string>& urls,
                        const std::atomic<bool>* cancel) {
    const std::string body = json{{"files", urls}}.dump();
    core::HttpResponse r;
    for (int attempt = 0; attempt < 2; ++attempt) {
        core::HttpRequest req;
        req.method = "POST";
        req.url = std::string(kCloudflareApi) + "/zones/" + token.zone_id + "/purge_cache";
        req.headers = {{"Authorization", "Bearer " + token.token}, {"Content-Type", "application/json"}};
        req.body_size = body.size();
        std::size_t done = 0;
        req.source = [&](std::span<std::uint8_t> out) -> std::size_t {
            const std::size_t n = std::min(out.size(), body.size() - done);
            std::copy_n(body.begin() + static_cast<std::ptrdiff_t>(done), n, out.begin());
            done += n;
            return n;
        };
        core::HttpLimits limits;
        limits.max_time = std::chrono::seconds{30};
        limits.cancel = cancel;
        r = http.send(req, limits);
        if ((r.status != 0 && r.status < 500) || stopped(cancel)) {
            break;
        }
    }
    if (stopped(cancel)) {
        return "stopped";
    }
    if (r.status == 200) {
        const json doc = json::parse(r.body, nullptr, false);
        if (doc.is_object() && doc.value("success", false)) {
            return {};
        }
        r.status = 500;  // shown as Cloudflare's own trouble, with its reason
    }
    return cloudflare_error(r);
}

std::optional<std::int64_t> list_sequence(std::string_view manifest_json) {
    const json doc = json::parse(manifest_json, nullptr, false);
    if (!doc.is_object() || !doc.contains("sequence") || !doc["sequence"].is_number_integer()) {
        return std::nullopt;
    }
    return doc["sequence"].get<std::int64_t>();
}

GoLiveResult go_live(core::HttpTransport& http, R2Bucket& bucket, const GoLiveInput& in, const GoLiveOptions& o) {
    GoLiveResult out;
    const auto say = [&](const std::string& s) {
        if (o.say) {
            o.say(s);
        }
    };
    const auto sleep = [&](std::chrono::seconds s) {
        if (o.sleep) {
            o.sleep(s);
        } else {
            // In short steps, so Stop is quick.
            for (auto left = std::chrono::milliseconds(s); left.count() > 0 && !stopped(o.cancel);
                 left -= std::chrono::milliseconds{200}) {
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
            }
        }
    };

    // 1. Which list R2 has: the one being replaced, or this one already.
    say("Reading the list on R2…");
    std::string error;
    const auto old_list = bucket.get_text(kList, kMaxList, error, o.cancel);
    if (stopped(o.cancel)) {
        out.problem = "Stopped; nothing was uploaded.";
        return out;
    }
    if (!old_list) {
        out.problem = error.empty() ? "R2 has no manifest.json, so this isn't the bucket the live list comes from. "
                                      "Nothing was uploaded."
                                    : "Couldn't read the list on R2 (" + error + "). Nothing was uploaded.";
        return out;
    }
    const bool list_there = *old_list == in.manifest_json;
    if (!list_there) {
        const auto seq = list_sequence(*old_list);
        if (!seq || *seq != in.live_sequence) {
            out.problem = "R2's manifest.json is " +
                          (seq ? "sequence " + std::to_string(*seq) : std::string("not a list")) + ", not sequence " +
                          std::to_string(in.live_sequence) +
                          " as the live list was. Was a list published from somewhere else? Nothing was uploaded; "
                          "press Check live.";
            return out;
        }
    }
    const auto old_sig = bucket.get_text(kSig, kMaxSig, error, o.cancel);
    if (!error.empty()) {
        out.problem = "Couldn't read the signature on R2 (" + error + "). Nothing was uploaded.";
        return out;
    }
    if (stopped(o.cancel)) {
        out.problem = "Stopped; nothing was uploaded.";
        return out;
    }
    const bool sig_there = old_sig && *old_sig == in.signature;

    // 2. The pair: the list, then its signature. A PUT that seems to fail
    // may still have landed (the answer was lost), so what R2 holds decides.
    const auto holds = [&](const char* object, const std::string& bytes) -> std::optional<bool> {
        std::string ignored;
        const auto now_there = bucket.get_text(object, kMaxList, ignored, nullptr);
        if (!ignored.empty()) {
            return std::nullopt;  // can't tell
        }
        return now_there && *now_there == bytes;
    };
    if (!list_there) {
        say("Uploading manifest.json…");
        if (!bucket.put_text(kList, in.manifest_json, error)) {
            const auto landed = holds(kList, in.manifest_json);
            if (!landed) {
                // The new list may be on R2 with the old signature; sending
                // the new signature over a list that may be the old one could
                // be the mismatch itself.
                out.mixed = true;
                out.problem = "manifest.json may have been uploaded (" + error +
                              "), so R2 may have the new list with the old signature, which FloServer refuses. "
                              "Press Go live again now, or upload both by hand (upload-steps.txt).";
                return out;
            }
            if (!*landed) {
                out.problem = "manifest.json wasn't uploaded (" + error + "). R2 still has the old list.";
                return out;
            }
            // It landed; the answer was lost. On to the signature.
        }
    }
    if (!sig_there) {
        say("Uploading manifest.json.sig…");
        if (!bucket.put_text(kSig, in.signature, error)) {
            const std::string why = error;
            const auto landed = holds(kSig, in.signature);
            if (landed != true) {
                // Surely not there: the old list goes back, so its old
                // signature matches it again. If that can't be told, putting
                // the old list back could be the mismatch, so it stays.
                if (landed == false && !list_there &&
                    (bucket.put_text(kList, *old_list, error) || holds(kList, *old_list) == true)) {
                    out.rolled_back = true;
                    out.problem = "manifest.json.sig wasn't uploaded (" + why +
                                  "), so the old manifest.json was put back: R2 has the old list, as before.";
                    if (o.purge) {
                        (void)purge_cache(http, *o.purge, list_urls(o.cdn), o.cancel);
                    }
                } else {
                    out.mixed = true;
                    out.problem = "manifest.json.sig wasn't uploaded (" + why +
                                  "). R2 may have the new manifest.json with the old signature, which FloServer "
                                  "refuses. Press Go live again now, or upload manifest.json.sig by hand "
                                  "(upload-steps.txt).";
                }
                return out;
            }
        }
    }
    out.uploaded = true;

    // 3. Out of Cloudflare's cache.
    if (o.purge) {
        say("Purging the cache…");
        const std::string why = purge_cache(http, *o.purge, list_urls(o.cdn), o.cancel);
        out.purged = why.empty();
        if (!out.purged) {
            out.note = "The cache wasn't purged (" + why + ").";
        }
    } else {
        out.note = "No purge token saved, so the cache wasn't purged.";
    }

    // 4. What FloServer users get.
    for (int i = 0; i < o.checks && o.read_live; ++i) {
        if (stopped(o.cancel)) {
            break;
        }
        if (i > 0) {
            sleep(o.wait);
            if (stopped(o.cancel)) {
                break;
            }
        }
        say(i == 0 ? "Checking the live list…" : "Checking the live list again (" + std::to_string(i + 1) + ")…");
        const LiveResult live = o.read_live();
        if (live.list) {
            out.seen_sequence = live.list->sequence;
            if (live.list->sequence == in.sequence) {
                out.live = true;
                return out;
            }
        }
    }
    if (stopped(o.cancel)) {
        out.problem = "Uploaded; stopped before the CDN showed sequence " + std::to_string(in.sequence) +
                      ". Press Check live.";
        return out;
    }
    const auto waited = std::chrono::duration_cast<std::chrono::seconds>(o.wait * std::max(o.checks - 1, 0)).count();
    out.problem = "Uploaded, but the CDN " +
                  (out.seen_sequence > 0 ? "still shows sequence " + std::to_string(out.seen_sequence)
                                         : std::string("didn't give a list that opens")) +
                  " after " + std::to_string(waited) +
                  " seconds. Purge manifest.json and manifest.json.sig from the cache by hand (Copy purge URLs), "
                  "then press Check live.";
    return out;
}

}  // namespace floserver::manifest_tool

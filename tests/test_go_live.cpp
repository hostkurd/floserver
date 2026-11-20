// D34.6: putting a signed list live. R2 and Cloudflare's API are fakes that
// keep the objects in memory; the CDN serves its cached copy until a purge.
// (SigV4 itself is tested in test_r2.cpp.)

#include "golive.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <map>

using namespace floserver::manifest_tool;
namespace core = floserver::core;

namespace {

const R2Key kKey{std::string(32, 'a'), std::string(32, 'b'), std::string(64, 'c')};
const PurgeToken kToken{std::string(32, 'd'), std::string(40, 'T')};

std::string list(std::int64_t sequence) { return R"({"format":1,"sequence":)" + std::to_string(sequence) + "}"; }
std::string sig(std::int64_t sequence) { return R"({"key":"k","signature":"s)" + std::to_string(sequence) + R"("})"; }

class FakeCloud final : public core::HttpTransport {
public:
    std::map<std::string, std::string> objects;  // "manifest.json" -> body
    std::map<std::string, int> fail_puts;        // object -> PUTs to answer 503 first
    std::map<std::string, int> refuse_puts;      // object -> PUTs to answer 403 first
    int list_puts_allowed = -1;                  // refuse manifest.json PUTs after this many (-1: never)
    std::vector<std::string> log;                // "PUT manifest.json", "PURGE"
    int purge_status = 200;
    std::string purge_reply = R"({"success":true,"errors":[],"messages":[],"result":{"id":"x"}})";
    std::string purge_body;
    std::string purge_auth;
    bool cache_stuck = false;  // purges don't take
    std::map<std::string, std::string> cached;  // what the CDN serves

    core::HttpReply get(const std::string&, const core::HttpLimits&, const core::HttpBody&) override { return {}; }

    core::HttpResponse send(const core::HttpRequest& req, const core::HttpLimits&) override {
        std::string body;
        std::vector<std::uint8_t> buf(1000);
        while (body.size() < req.body_size) {
            const std::size_t n = req.source(std::span<std::uint8_t>(buf.data(), buf.size()));
            REQUIRE(n > 0);
            body.append(reinterpret_cast<const char*>(buf.data()), n);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        }
        core::HttpResponse r;
        const std::string zone = std::string(kCloudflareApi) + "/zones/" + kToken.zone_id + "/purge_cache";
        if (req.url == zone) {
            log.emplace_back("PURGE");
            REQUIRE(req.method == "POST");
            purge_body = body;
            for (const auto& [k, v] : req.headers) {
                purge_auth = k == "Authorization" ? v : purge_auth;
            }
            r.status = purge_status;
            r.body = purge_reply;
            if (r.status == 200 && !cache_stuck) {
                cached = objects;
            }
            return r;
        }
        const std::string prefix = "https://" + kKey.account_id + ".r2.cloudflarestorage.com/floserver-cdn/";
        REQUIRE(req.url.starts_with(prefix));
        const std::string object = req.url.substr(prefix.size());
        log.push_back(req.method + " " + object);
        if (req.method == "GET") {
            const auto it = objects.find(object);
            r.status = it == objects.end() ? 404 : 200;
            r.body = it == objects.end() ? "<Error><Code>NoSuchKey</Code></Error>" : it->second;
            if (r.body.size() > req.max_reply) {
                r.body.resize(req.max_reply);
            }
        } else if (req.method == "PUT") {
            if (fail_puts[object] > 0) {
                --fail_puts[object];
                r.status = 503;
                return r;
            }
            if (object == "manifest.json" && list_puts_allowed == 0) {
                refuse_puts[object] = 1;
            }
            if (refuse_puts[object] > 0) {
                --refuse_puts[object];
                r.status = 403;
                r.body = "<Error><Code>AccessDenied</Code><Message>Access Denied</Message></Error>";
                return r;
            }
            if (object == "manifest.json" && list_puts_allowed > 0) {
                --list_puts_allowed;
            }
            objects[object] = body;
            r.status = 200;
        } else {
            r.status = 400;
        }
        return r;
    }

    // As FloServer reads it: the cached pair, opened only when it matches.
    [[nodiscard]] LiveResult read_live() const {
        LiveResult out;
        const auto l = cached.find("manifest.json");
        const auto s = cached.find("manifest.json.sig");
        if (l == cached.end() || s == cached.end()) {
            out.error = "missing";
            return out;
        }
        const auto seq = list_sequence(l->second);
        if (!seq || s->second != sig(*seq)) {
            out.error = "The live list doesn't open";
            return out;
        }
        out.list = LiveList{};
        out.list->sequence = *seq;
        return out;
    }

    void publish(std::int64_t sequence) {
        objects["manifest.json"] = list(sequence);
        objects["manifest.json.sig"] = sig(sequence);
        cached = objects;
    }
};

struct Run {
    FakeCloud cloud;
    R2Bucket bucket{cloud, kKey, kR2Bucket, [] { return std::chrono::sys_seconds{std::chrono::seconds{1'700'000'000}}; }};
    GoLiveOptions options;
    std::vector<std::string> said;
    int slept = 0;

    Run() {
        cloud.publish(10);
        options.purge = kToken;
        options.read_live = [this] { return cloud.read_live(); };
        options.sleep = [this](std::chrono::seconds) { ++slept; };
        options.say = [this](const std::string& s) { said.push_back(s); };
    }
    GoLiveResult go(std::int64_t sequence = 11, std::int64_t live = 10) {
        return go_live(cloud, bucket, {list(sequence), sig(sequence), sequence, live}, options);
    }
};

}  // namespace

TEST_CASE("go live: uploads the list, then its signature, purges both and sees the new sequence") {
    Run run;
    const GoLiveResult r = run.go();
    CHECK(r.problem.empty());
    CHECK(r.note.empty());
    CHECK(r.uploaded);
    CHECK(r.purged);
    CHECK(r.live);
    CHECK(r.seen_sequence == 11);
    CHECK(run.cloud.objects["manifest.json"] == list(11));
    CHECK(run.cloud.objects["manifest.json.sig"] == sig(11));
    CHECK(run.cloud.log == std::vector<std::string>{"GET manifest.json", "GET manifest.json.sig", "PUT manifest.json",
                                                    "PUT manifest.json.sig", "PURGE"});
    const auto body = nlohmann::json::parse(run.cloud.purge_body);
    CHECK(body["files"] == nlohmann::json::array({"https://cdn.floserver.org/manifest.json",
                                                  "https://cdn.floserver.org/manifest.json.sig"}));
    CHECK(run.cloud.purge_auth == "Bearer " + kToken.token);
    CHECK(run.slept == 0);
    CHECK_FALSE(run.said.empty());
}

TEST_CASE("go live: nothing is uploaded when R2 has another list than the live one") {
    Run run;
    run.cloud.publish(12);  // published from somewhere else meanwhile
    const GoLiveResult r = run.go(11, 10);
    CHECK_FALSE(r.uploaded);
    CHECK(r.problem.find("sequence 12, not sequence 10") != std::string::npos);
    CHECK(run.cloud.objects["manifest.json"] == list(12));
    CHECK(std::none_of(run.cloud.log.begin(), run.cloud.log.end(), [](const std::string& l) { return l.starts_with("PUT"); }));

    Run empty;
    empty.cloud.objects.clear();
    const GoLiveResult e = empty.go();
    CHECK_FALSE(e.uploaded);
    CHECK(e.problem.find("no manifest.json") != std::string::npos);

    Run junk;
    junk.cloud.objects["manifest.json"] = "<html>";
    CHECK(junk.go().problem.find("not a list") != std::string::npos);
}

TEST_CASE("go live: a failed signature puts the old list back") {
    Run run;
    run.cloud.refuse_puts["manifest.json.sig"] = 1;
    const GoLiveResult r = run.go();
    CHECK_FALSE(r.uploaded);
    CHECK(r.rolled_back);
    CHECK_FALSE(r.mixed);
    CHECK(r.problem.find("old manifest.json was put back") != std::string::npos);
    CHECK(run.cloud.objects["manifest.json"] == list(10));
    CHECK(run.cloud.objects["manifest.json.sig"] == sig(10));
    CHECK(std::count(run.cloud.log.begin(), run.cloud.log.end(), "PURGE") == 0);
}

TEST_CASE("go live: when the old list can't go back either, it says the pair is mixed") {
    Run run;
    run.cloud.fail_puts["manifest.json.sig"] = 2;  // the retry fails too
    run.cloud.list_puts_allowed = 1;               // the new list goes up; putting the old one back is refused
    const GoLiveResult r = run.go();
    CHECK(r.mixed);
    CHECK_FALSE(r.rolled_back);
    CHECK_FALSE(r.uploaded);
    CHECK(r.problem.find("by hand now") != std::string::npos);
    CHECK(run.cloud.objects["manifest.json"] == list(11));
    CHECK(run.cloud.objects["manifest.json.sig"] == sig(10));
}

TEST_CASE("go live: one retry on no answer or 5xx, none on a refusal") {
    Run run;
    run.cloud.fail_puts["manifest.json"] = 1;
    CHECK(run.go().live);

    Run refused;
    refused.cloud.refuse_puts["manifest.json"] = 1;
    const GoLiveResult r = refused.go();
    CHECK_FALSE(r.uploaded);
    CHECK(r.problem.find("R2 still has the old list") != std::string::npos);
    CHECK(r.problem.find("Access Denied") != std::string::npos);
    CHECK(std::count(refused.cloud.log.begin(), refused.cloud.log.end(), "PUT manifest.json") == 1);
}

TEST_CASE("go live: a second try after a half upload sends only what's missing") {
    Run run;
    run.cloud.objects["manifest.json"] = list(11);  // the list went up, the signature didn't
    const GoLiveResult r = run.go();
    CHECK(r.live);
    CHECK(std::count(run.cloud.log.begin(), run.cloud.log.end(), "PUT manifest.json") == 0);
    CHECK(std::count(run.cloud.log.begin(), run.cloud.log.end(), "PUT manifest.json.sig") == 1);

    Run both;
    both.cloud.objects["manifest.json"] = list(11);
    both.cloud.objects["manifest.json.sig"] = sig(11);
    CHECK(both.go().live);
    CHECK(std::none_of(both.cloud.log.begin(), both.cloud.log.end(), [](const std::string& l) { return l.starts_with("PUT"); }));

    // Half up and the signature fails again: nothing to put back.
    Run again;
    again.cloud.objects["manifest.json"] = list(11);
    again.cloud.refuse_puts["manifest.json.sig"] = 1;
    const GoLiveResult m = again.go();
    CHECK(m.mixed);
    CHECK_FALSE(m.rolled_back);
}

TEST_CASE("go live: without a purge it keeps asking the CDN, then says what to do") {
    Run run;
    run.options.purge.reset();
    run.options.checks = 4;
    const GoLiveResult r = run.go();
    CHECK(r.uploaded);
    CHECK_FALSE(r.purged);
    CHECK_FALSE(r.live);
    CHECK(r.seen_sequence == 10);
    CHECK(run.slept == 3);
    CHECK(r.note.find("No purge token") != std::string::npos);
    CHECK(r.problem.find("still shows sequence 10 after 15 seconds") != std::string::npos);
    CHECK(std::count(run.cloud.log.begin(), run.cloud.log.end(), "PURGE") == 0);

    // The cache rule lets the new pair through a little later.
    Run later;
    later.options.purge.reset();
    int asked = 0;
    later.options.read_live = [&] {
        if (++asked == 3) {
            later.cloud.cached = later.cloud.objects;
        }
        return later.cloud.read_live();
    };
    const GoLiveResult l = later.go();
    CHECK(l.live);
    CHECK(asked == 3);
}

TEST_CASE("go live: a refused purge is a note, and the CDN is still checked") {
    Run run;
    run.cloud.purge_status = 403;
    run.cloud.purge_reply = R"({"success":false,"errors":[{"code":10000,"message":"Authentication error"}]})";
    run.cloud.cache_stuck = true;
    run.options.checks = 2;
    const GoLiveResult r = run.go();
    CHECK(r.uploaded);
    CHECK_FALSE(r.purged);
    CHECK(r.note.find("Authentication error (code 10000)") != std::string::npos);
    CHECK(r.note.find("Cache Purge on floserver.org") != std::string::npos);
    CHECK(r.problem.find("by hand") != std::string::npos);
}

TEST_CASE("go live: stopping before the upload uploads nothing") {
    Run run;
    const std::atomic<bool> cancel{true};
    run.options.cancel = &cancel;
    const GoLiveResult r = run.go();
    CHECK_FALSE(r.uploaded);
    CHECK(r.problem.find("Stopped") != std::string::npos);
    CHECK(run.cloud.objects["manifest.json"] == list(10));
}

TEST_CASE("go live: purge_cache's answers") {
    FakeCloud cloud;
    cloud.objects["a"] = "1";
    CHECK(purge_cache(cloud, kToken, {"https://cdn.floserver.org/a"}).empty());
    CHECK(cloud.cached["a"] == "1");

    cloud.purge_reply = R"({"success":false,"errors":[{"code":1234,"message":"nope"}]})";
    CHECK(purge_cache(cloud, kToken, {"u"}).find("nope (code 1234)") != std::string::npos);

    cloud.purge_status = 502;
    cloud.purge_reply = "<html>bad gateway</html>";
    cloud.log.clear();
    CHECK(purge_cache(cloud, kToken, {"u"}) == "HTTP 502");
    CHECK(cloud.log.size() == 2);  // one retry
}

TEST_CASE("go live: purge token and list checks") {
    CHECK(purge_token_problem(kToken).empty());
    CHECK(purge_token_problem({std::string(31, 'd'), kToken.token}).find("Zone ID") != std::string::npos);
    CHECK(purge_token_problem({std::string(32, 'D'), kToken.token}).find("Zone ID") != std::string::npos);
    CHECK(purge_token_problem({kToken.zone_id, "short"}).find("API token") != std::string::npos);
    CHECK(purge_token_problem({kToken.zone_id, std::string(39, 'x') + "!"}).find("API token") != std::string::npos);
    CHECK(purge_token_problem({kToken.zone_id, "Ab-_09" + std::string(34, 'z')}).empty());

    CHECK(list_sequence(list(42)) == 42);
    CHECK_FALSE(list_sequence("[]"));
    CHECK_FALSE(list_sequence(R"({"sequence":"4"})"));
    CHECK(list_urls("https://cdn.floserver.org") ==
          std::vector<std::string>{"https://cdn.floserver.org/manifest.json",
                                   "https://cdn.floserver.org/manifest.json.sig"});
}

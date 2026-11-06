// App updates (Q5): the service's check, download and cancel, with the
// runtimes' own list and downloader over a fake transport.

#include "floserver/core/paths.h"
#include "floserver/core/runtime_service.h"
#include "floserver/core/update_service.h"

#include "test_util.h"

#include <doctest/doctest.h>

#include <atomic>
#include <functional>
#include <thread>

using namespace floserver::core;
using namespace test_util;
using namespace std::chrono;

namespace {

const SigningKey kKey(0x46);
const std::string kKeyId = "update-test";
const ManifestSource kSource{"https://cdn.example.org/manifest.json", "https://cdn.example.org/manifest.json.sig"};

std::string msi_url(const std::string& version) {
    return "https://cdn.example.org/app/" + version + "/FloServer-" + version + "-x64.msi";
}

// The fake, except that an installer marked held sends its first bytes and
// then waits until the download is cancelled (or let go), as a slow line would.
class HoldingTransport final : public HttpTransport {
public:
    FakeTransport fake;
    std::atomic<bool> hold{false};
    std::atomic<bool> holding{false};  // a held download is waiting now

    HttpReply get(const std::string& url, const HttpLimits& limits, const HttpBody& body) override {
        if (!hold || !url.ends_with(".msi")) {
            return fake.get(url, limits, body);
        }
        bool sent = false;
        return fake.get(url, limits, [&](std::span<const std::uint8_t> chunk) {
            if (!sent) {
                sent = true;
                return body(chunk);
            }
            holding = true;
            while (hold && (limits.cancel == nullptr || !limits.cancel->load())) {
                std::this_thread::sleep_for(milliseconds(5));
            }
            holding = false;
            return body(chunk);
        });
    }
};

struct World {
    TempDir tmp;
    std::shared_ptr<HoldingTransport> transport = std::make_shared<HoldingTransport>();
    std::string msi = std::string("MSI installer bytes, ") + std::string(200, 'x');
    std::unique_ptr<RuntimeService> runtimes;
    std::int64_t sequence = 1;

    // The list, with `app` as its "app" entry (none when null).
    void publish(const nlohmann::json& app) {
        nlohmann::json doc = {{"schema", 1},
                              {"sequence", sequence++},
                              {"expires", "2026-12-01T00:00:00Z"},
                              {"runtimes", nlohmann::json::array()}};
        if (!app.is_null()) {
            doc["app"] = app;
        }
        const std::string bytes = doc.dump();
        transport->fake.serve(kSource.manifest_url, {200, bytes});
        transport->fake.serve(kSource.signature_url, {200, kKey.sign(bytes, kKeyId)});
    }

    [[nodiscard]] nlohmann::json app(const std::string& version, const std::string& bytes) const {
        return {{"version", version},
                {"platform", "windows-x86_64"},
                {"urls", {msi_url(version)}},
                {"size", bytes.size()},
                {"sha256", sha256_hex(bytes)},
                {"notes_url", "https://floserver.org/news/" + version}};
    }

    void serve_msi(const std::string& version, const std::string& bytes) {
        transport->fake.serve(msi_url(version), {200, bytes});
    }

    World() {
        RuntimeServiceConfig c;
        c.data_dir = tmp.path();
        c.platform = "windows-x64";
        c.keys = {{kKeyId, kKey.pub}};
        c.source = kSource;
        c.now = [] { return *parse_utc_time("2026-11-05T09:00:00Z"); };
        c.sleep = [](milliseconds) {};
        runtimes = std::make_unique<RuntimeService>(c, transport);
    }

    [[nodiscard]] UpdateServiceConfig config(milliseconds first_delay = hours{1}) const {
        UpdateServiceConfig c;
        c.data_dir = tmp.path();
        c.current_version = "1.1.0";
        RuntimeService* r = runtimes.get();
        c.fetch_list = [r](std::string& error) { return r->fresh_list(error); };
        c.now = [] { return *parse_utc_time("2026-11-05T09:12:00Z"); };
        c.first_delay = first_delay;
        return c;
    }

    [[nodiscard]] fs::path updates() const { return updates_dir(tmp.path()); }
};

// The status once `done` says so (within 5 s).
nlohmann::json wait_until(const UpdateService& u, const std::function<bool(const nlohmann::json&)>& done) {
    for (int i = 0; i < 500; ++i) {
        nlohmann::json s = u.status();
        if (done(s)) {
            return s;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    FAIL("the update service didn't get there: " << u.status().dump());
    return {};
}

nlohmann::json wait_for_state(const UpdateService& u, const char* state) {
    return wait_until(u, [state](const nlohmann::json& s) { return s["state"] == state; });
}

// Checks, then waits for the check to end.
nlohmann::json check_and_wait(UpdateService& u, const char* state) {
    std::string error;
    REQUIRE(u.check(error));
    // A check that's asked is under way at once or within a moment.
    return wait_until(u, [state](const nlohmann::json& s) {
        return s["state"] == state && s.contains("last_checked");
    });
}

}  // namespace

TEST_CASE("updates: a newer app is downloaded, checked and kept ready") {
    World w;
    w.publish(w.app("1.2.0", w.msi));
    w.serve_msi("1.2.0", w.msi);
    // Left from before: an older installer, and one cut off mid-way.
    write_file(w.updates() / "FloServer-1.1.0-x64.msi", "old");
    write_file(w.updates() / "FloServer-1.2.0-x64.msi.part", "cut");

    UpdateService u(w.config(), w.transport);
    CHECK_FALSE(fs::exists(w.updates() / "FloServer-1.2.0-x64.msi.part"));  // gone at once
    nlohmann::json s = u.status();
    CHECK(s["state"] == "idle");
    CHECK(s["available"] == true);
    CHECK(s["current"] == "1.1.0");
    CHECK_FALSE(s.contains("last_checked"));

    u.start();
    s = check_and_wait(u, "ready");
    INFO(s.dump(2));
    CHECK(s["version"] == "1.2.0");
    CHECK(s["size"] == w.msi.size());
    CHECK(s["done"] == w.msi.size());
    CHECK(s["sha256"] == sha256_hex(w.msi));
    CHECK(s["notes_url"] == "https://floserver.org/news/1.2.0");
    CHECK(s["last_checked"] == "2026-11-05T09:12:00Z");
    CHECK_FALSE(s.contains("error"));
    const fs::path ready = w.updates() / "FloServer-1.2.0-x64.msi";
    CHECK(s["file"] == path_to_utf8(ready));
    CHECK(read_file(ready) == w.msi);
    // The stale installer went; only the ready one is there.
    CHECK_FALSE(fs::exists(w.updates() / "FloServer-1.1.0-x64.msi"));
    CHECK(std::distance(fs::directory_iterator(w.updates()), fs::directory_iterator()) == 1);
    CHECK(w.transport->fake.requests(msi_url("1.2.0")) == 1);

    // Checked again: the ready file is kept, not fetched again.
    s = check_and_wait(u, "ready");
    CHECK(w.transport->fake.requests(msi_url("1.2.0")) == 1);
    CHECK(read_file(ready) == w.msi);

    // Nothing to cancel once it's ready.
    CHECK_FALSE(u.cancel());
}

TEST_CASE("updates: nothing newer leaves nothing behind") {
    World w;
    UpdateService u(w.config(), w.transport);
    u.start();

    SUBCASE("a list without an app") {
        w.publish(nullptr);
    }
    SUBCASE("the same version") {
        w.publish(w.app("1.1.0", w.msi));
    }
    SUBCASE("an older version") {
        w.publish(w.app("1.0.9", w.msi));
    }
    SUBCASE("another platform's app") {
        nlohmann::json app = w.app("1.2.0", w.msi);
        app["platform"] = "windows-arm64";
        w.publish(app);
    }
    w.serve_msi("1.2.0", w.msi);
    // The installer this version came from, kept from before the update.
    write_file(w.updates() / "FloServer-1.1.0-x64.msi", w.msi);

    const nlohmann::json s = check_and_wait(u, "idle");
    INFO(s.dump(2));
    CHECK_FALSE(s.contains("version"));
    CHECK_FALSE(s.contains("file"));
    CHECK(s["last_checked"] == "2026-11-05T09:12:00Z");
    CHECK(fs::is_empty(w.updates()));
    CHECK(w.transport->fake.requests(msi_url("1.2.0")) == 0);
}

TEST_CASE("updates: a file that doesn't match the signed list isn't kept") {
    World w;
    w.publish(w.app("1.2.0", w.msi));
    w.serve_msi("1.2.0", w.msi + "tampered");
    UpdateService u(w.config(), w.transport);
    u.start();

    const nlohmann::json s = check_and_wait(u, "failed");
    INFO(s.dump(2));
    CHECK_FALSE(s.value("error", std::string()).empty());
    CHECK(s["version"] == "1.2.0");
    CHECK_FALSE(s.contains("file"));
    CHECK(fs::is_empty(w.updates()));
}

TEST_CASE("updates: a list that can't be had is a failed check, said plainly") {
    World w;  // nothing served
    UpdateService u(w.config(), w.transport);
    u.start();
    std::string error;
    REQUIRE(u.check(error));
    const nlohmann::json s = wait_for_state(u, "failed");
    CHECK_FALSE(s.value("error", std::string()).empty());
    CHECK_FALSE(s.contains("last_checked"));  // never read
}

TEST_CASE("updates: cancel stops the download; only a check starts it again") {
    World w;
    w.publish(w.app("1.2.0", w.msi));
    w.serve_msi("1.2.0", w.msi);
    w.transport->hold = true;
    UpdateService u(w.config(), w.transport);
    u.start();
    std::string error;
    REQUIRE(u.check(error));

    nlohmann::json s = wait_until(u, [&](const nlohmann::json& j) { return j["state"] == "downloading" && w.transport->holding; });
    INFO(s.dump(2));
    CHECK(s["version"] == "1.2.0");
    CHECK(s["size"] == w.msi.size());
    CHECK(s["done"].get<std::uint64_t>() > 0);
    CHECK(s["done"].get<std::uint64_t>() < w.msi.size());
    CHECK_FALSE(s.contains("file"));
    // Asking again while it downloads changes nothing.
    REQUIRE(u.check(error));

    CHECK(u.cancel());
    s = wait_for_state(u, "idle");
    CHECK(s["version"] == "1.2.0");  // still offered, not downloaded
    CHECK_FALSE(s.contains("done"));
    CHECK(fs::is_empty(w.updates()));  // the .part went with it
    // Nothing starts again by itself.
    std::this_thread::sleep_for(milliseconds(100));
    CHECK(u.status()["state"] == "idle");
    CHECK(w.transport->fake.requests(msi_url("1.2.0")) == 1);
    CHECK_FALSE(u.cancel());

    // updates.check does.
    w.transport->hold = false;
    s = check_and_wait(u, "ready");
    CHECK(read_file(w.updates() / "FloServer-1.2.0-x64.msi") == w.msi);
    CHECK(w.transport->fake.requests(msi_url("1.2.0")) == 2);
}

TEST_CASE("updates: the first check comes by itself after the delay") {
    World w;
    w.publish(w.app("1.2.0", w.msi));
    w.serve_msi("1.2.0", w.msi);
    UpdateService u(w.config(milliseconds(0)), w.transport);
    u.start();
    const nlohmann::json s = wait_for_state(u, "ready");
    CHECK(s["version"] == "1.2.0");
}

TEST_CASE("updates: stopping during a download ends it at once") {
    World w;
    w.publish(w.app("1.2.0", w.msi));
    w.serve_msi("1.2.0", w.msi);
    w.transport->hold = true;
    auto u = std::make_unique<UpdateService>(w.config(), w.transport);
    u->start();
    std::string error;
    REQUIRE(u->check(error));
    wait_until(*u, [&](const nlohmann::json&) { return w.transport->holding.load(); });
    u.reset();  // stops and waits: the held transfer sees the cancel
    CHECK(fs::is_empty(w.updates()));
}

TEST_CASE("updates: none where there's no transport") {
    World w;
    UpdateService u(w.config(), nullptr);
    u.start();
    const nlohmann::json s = u.status();
    CHECK(s["available"] == false);
    CHECK(s["state"] == "failed");
    CHECK(s["error"] == "updates aren't available on this platform yet");
    std::string error;
    CHECK_FALSE(u.check(error));
    CHECK(error == "updates aren't available on this platform yet");
    CHECK_FALSE(u.cancel());
}

TEST_CASE("updates: the installer's name") {
    CHECK(UpdateService::file_name("1.2.0") == "FloServer-1.2.0-x64.msi");
}

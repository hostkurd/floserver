#include "floserver/core/builtin_templates.h"
#include "floserver/core/paths.h"
#include "floserver/core/runtime_service.h"

#include "test_util.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <atomic>
#include <thread>

using namespace floserver::core;
using namespace test_util;
using namespace std::chrono;

namespace {

const SigningKey kKey(0x44);
const std::string kKeyId = "svc-test";
const ManifestSource kSource{"https://cdn.example.org/manifest.json", "https://cdn.example.org/manifest.json.sig"};

std::string make_zip(const std::string& name, const std::string& content) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(),
                                     static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)));
    void* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
    std::string out(static_cast<const char*>(buffer), size);
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return out;
}

struct World {
    TempDir tmp;
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::string php85 = make_zip("php-cgi.exe", "MZ 8.5");
    std::string php84 = make_zip("php-cgi.exe", "MZ 8.4");
    std::string phar = "<?php // composer";

    nlohmann::json entry(const char* name, const char* version, const std::string& bytes,
                         const char* platform = "windows-x64") const {
        return {{"name", name},
                {"version", version},
                {"platform", platform},
                {"archive", "zip"},
                {"size", bytes.size()},
                {"sha256", sha256_hex(bytes)},
                {"license", "PHP-3.01"},
                {"urls", {"https://cdn.example.org/" + std::string(name) + "/" + version + ".zip"}}};
    }

    [[nodiscard]] nlohmann::json composer() const {
        auto e = entry("composer", "2.10.3", phar, "any");
        e["archive"] = "file";
        e["urls"] = {"https://cdn.example.org/composer/2.10.3/composer.phar"};
        return e;
    }

    void publish(std::int64_t sequence, const nlohmann::json& templates = nullptr) {
        nlohmann::json doc = {
            {"schema", 1},
            {"sequence", sequence},
            {"expires", "2026-12-01T00:00:00Z"},
            {"runtimes",
             {entry("php", "8.5.12", php85), entry("php", "8.4.27", php84),
              entry("php", "8.5.12", php85, "linux-x64"), composer()}}};
        if (!templates.is_null()) {
            doc["templates"] = templates;
        }
        const std::string bytes = doc.dump();
        transport->serve(kSource.manifest_url, {200, bytes});
        transport->serve(kSource.signature_url, {200, kKey.sign(bytes, kKeyId)});
        transport->serve("https://cdn.example.org/php/8.5.12.zip", {200, php85});
        transport->serve("https://cdn.example.org/php/8.4.27.zip", {200, php84});
        transport->serve("https://cdn.example.org/composer/2.10.3/composer.phar", {200, phar});
    }

    [[nodiscard]] RuntimeServiceConfig config() const {
        RuntimeServiceConfig c;
        c.data_dir = tmp.path();
        c.platform = "windows-x64";
        c.keys = {{kKeyId, kKey.pub}};
        c.source = kSource;
        c.now = [] { return *parse_utc_time("2026-10-28T12:00:00Z"); };
        c.sleep = [](milliseconds) {};
        return c;
    }
};

nlohmann::json wait_for(RuntimeService& s, std::int64_t id) {
    for (int i = 0; i < 1000; ++i) {
        const auto j = s.job(id);
        REQUIRE(j.has_value());
        const std::string state = (*j)["state"];
        if (state == "done" || state == "failed" || state == "cancelled") {
            return *j;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    FAIL("the job didn't finish");
    return {};
}

}  // namespace

TEST_CASE("the service lists, installs the newest match, and remembers the sequence") {
    World w;
    w.publish(7);
    RuntimeService s(w.config(), w.transport);

    auto listed = s.list(false);
    INFO(listed.dump(2));
    REQUIRE(listed["list"].is_object());
    CHECK(listed["list"]["sequence"] == 7);
    CHECK(listed["available"].size() == 3);  // windows-x64 and "any", not linux-x64
    CHECK(listed["installed"].empty());

    ServiceRefusal refusal;
    const auto id = s.install("php", "8", refusal);
    REQUIRE(id.has_value());
    auto job = wait_for(s, *id);
    INFO(job.dump(2));
    CHECK(job["state"] == "done");
    CHECK(job["version"] == "8.5.12");
    CHECK(job["received"] == w.php85.size());

    listed = s.list(false);
    REQUIRE(listed["installed"].size() == 1);
    CHECK(listed["installed"][0]["version"] == "8.5.12");
    // The window's frequent ask: what's installed, without the list.
    const auto installed_only = s.list(false, true);
    CHECK(installed_only["installed"].size() == 1);
    CHECK_FALSE(installed_only.contains("list"));
    CHECK(installed_only["available"].empty());
    CHECK(read_file(w.tmp.path() / "runtimes" / "php" / "8.5.12" / "php-cgi.exe") == "MZ 8.5");
    CHECK(fs::is_empty(downloads_dir(w.tmp.path())));
    CHECK(read_file(manifest_state_file(w.tmp.path())).find("\"svc-test\": 7") != std::string::npos);

    // An exact version, then one that's already there.
    job = wait_for(s, *s.install("php", "8.4.27", refusal));
    CHECK(job["state"] == "done");
    job = wait_for(s, *s.install("php", "8.5", refusal));
    CHECK(job["state"] == "done");
    CHECK(job["message"].get<std::string>().find("already installed") != std::string::npos);
    CHECK(w.transport->requests("https://cdn.example.org/php/8.5.12.zip") == 1);

    // Composer is listed for any platform.
    job = wait_for(s, *s.install("composer", "", refusal));
    INFO(job.dump(2));
    CHECK(job["state"] == "done");
    CHECK(read_file(w.tmp.path() / "runtimes" / "composer" / "2.10.3" / "composer.phar") == w.phar);
}

TEST_CASE("a list older than the stored sequence is refused") {
    World w;
    w.publish(7);
    {
        RuntimeService s(w.config(), w.transport);
        REQUIRE(s.list(false)["list"].is_object());
    }
    w.publish(6);  // a mirror serving an older, correctly signed list
    RuntimeService s(w.config(), w.transport);
    const auto listed = s.list(false);
    CHECK(listed["list"].is_null());
    CHECK(listed["list_error"].get<std::string>().find("older") != std::string::npos);

    ServiceRefusal refusal;
    const auto job = wait_for(s, *s.install("php", "8.5", refusal));
    CHECK(job["state"] == "failed");
}

TEST_CASE("bad requests and missing versions are explained") {
    World w;
    w.publish(1);
    RuntimeService s(w.config(), w.transport);
    ServiceRefusal refusal;
    CHECK_FALSE(s.install("../php", "8.5", refusal).has_value());
    CHECK(refusal.code == "bad_params");
    CHECK_FALSE(s.install("php", "eight", refusal).has_value());
    CHECK(refusal.code == "bad_params");

    const auto job = wait_for(s, *s.install("php", "7.4", refusal));
    CHECK(job["state"] == "failed");
    CHECK(job["message"].get<std::string>().find("8.5.12 8.4.27") != std::string::npos);

    CHECK_FALSE(s.job(9999).has_value());
    CHECK_FALSE(s.remove("php", "8.5.12", refusal));
    CHECK(refusal.code == "not_removed");
}

TEST_CASE("without a transport, installs are refused and the list says why") {
    const World w;
    RuntimeService s(w.config(), nullptr);
    ServiceRefusal refusal;
    CHECK_FALSE(s.install("php", "8.5", refusal).has_value());
    CHECK(refusal.code == "downloads_unavailable");
    CHECK(s.list(false)["list_error"].get<std::string>().find("platform") != std::string::npos);
}

TEST_CASE("an install can be cancelled, and the same request isn't queued twice") {
    World w;
    w.publish(1);
    // A transport that waits until told to go on, so the job is caught mid-download.
    struct Slow final : HttpTransport {
        std::shared_ptr<FakeTransport> inner;
        std::atomic<bool> go{false};
        HttpReply get(const std::string& url, const HttpLimits& limits, const HttpBody& body) override {
            if (url.ends_with(".zip")) {
                return inner->get(url, limits, [&](std::span<const std::uint8_t> chunk) {
                    while (!go) {
                        std::this_thread::sleep_for(milliseconds(1));
                    }
                    return body(chunk);
                });
            }
            return inner->get(url, limits, body);
        }
    };
    auto slow = std::make_shared<Slow>();
    slow->inner = w.transport;
    RuntimeService s(w.config(), slow);

    ServiceRefusal refusal;
    const auto id = s.install("php", "8.5", refusal);
    REQUIRE(id.has_value());
    CHECK(s.install("php", "8.5", refusal) == id);
    for (int i = 0; i < 500 && (*s.job(*id))["state"] != "downloading"; ++i) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE((*s.job(*id))["state"] == "downloading");
    CHECK(s.cancel(*id));
    slow->go = true;
    const auto job = wait_for(s, *id);
    CHECK(job["state"] == "cancelled");
    CHECK_FALSE(s.cancel(*id));
    CHECK(s.list(false)["installed"].empty());
    CHECK(fs::is_empty(downloads_dir(w.tmp.path())));
}

TEST_CASE("stopping the service cancels a running install") {
    World w;
    w.publish(1);
    struct Blocking final : HttpTransport {
        std::shared_ptr<FakeTransport> inner;
        HttpReply get(const std::string& url, const HttpLimits& limits, const HttpBody& body) override {
            if (url.ends_with(".zip")) {
                // Keeps sending until the body callback says stop.
                const std::array<std::uint8_t, 1> byte{0};
                HttpReply r;
                r.status = 200;
                // Ignores the body's answer: only the cancel flag ends it, as
                // closing the handle does in the WinHTTP transport.
                for (int i = 0; i < 100000; ++i) {
                    if (limits.cancel != nullptr && limits.cancel->load()) {
                        r.status = 0;
                        r.error = "cancelled";
                        return r;
                    }
                    if (!body(byte)) {
                        r.stopped = true;
                        return r;
                    }
                    std::this_thread::sleep_for(milliseconds(20));
                }
                return r;
            }
            return inner->get(url, limits, body);
        }
    };
    auto blocking = std::make_shared<Blocking>();
    blocking->inner = w.transport;
    {
        RuntimeService s(w.config(), blocking);
        ServiceRefusal refusal;
        const auto id = s.install("php", "8.5", refusal);
        REQUIRE(id.has_value());
        for (int i = 0; i < 500 && (*s.job(*id))["state"] != "downloading"; ++i) {
            std::this_thread::sleep_for(milliseconds(5));
        }
    }  // the destructor must return promptly
    CHECK(fs::is_empty(downloads_dir(w.tmp.path())));
}

TEST_CASE("waiting installs are capped") {
    World w;
    w.publish(1);
    struct Stuck final : HttpTransport {
        std::shared_ptr<FakeTransport> inner;
        HttpReply get(const std::string& url, const HttpLimits& limits, const HttpBody& body) override {
            if (url.ends_with(".zip")) {
                while (limits.cancel == nullptr || !limits.cancel->load()) {
                    std::this_thread::sleep_for(milliseconds(2));
                }
                HttpReply r;
                r.error = "cancelled";
                return r;
            }
            return inner->get(url, limits, body);
        }
    };
    auto stuck = std::make_shared<Stuck>();
    stuck->inner = w.transport;
    RuntimeService s(w.config(), stuck);
    ServiceRefusal refusal;
    int accepted = 0;
    for (int patch = 0; patch < 20; ++patch) {
        if (s.install("php", "8.5." + std::to_string(patch), refusal)) {
            ++accepted;
        }
    }
    CHECK(accepted == 16);
    CHECK(refusal.code == "busy");
}

TEST_CASE("templates work offline: the kept signed list, else the built-in ones") {
    World w;
    // The signed list carries only the static-site template, so it can be
    // told apart from the eight built-in ones.
    const auto builtin = nlohmann::json::parse(kBuiltInTemplatesJson);
    nlohmann::json one = nlohmann::json::array();
    for (const auto& t : builtin) {
        if (t["id"] == "static-site") {
            one.push_back(t);
        }
    }
    REQUIRE(one.size() == 1);
    auto config = w.config();
    config.builtin_templates_json = std::string(kBuiltInTemplatesJson);

    SUBCASE("first run offline: the built-in templates") {
        RuntimeService s(config, w.transport);  // the transport serves nothing
        std::string error;
        const auto list = s.templates(error);
        REQUIRE_MESSAGE(list, error);
        CHECK(list->size() == builtin.size());
        CHECK(s.templates(error, false)->size() == builtin.size());
    }

    SUBCASE("online once, then offline: the kept list") {
        w.publish(3, one);
        {
            RuntimeService s(config, w.transport);
            std::string error;
            const auto list = s.templates(error);
            REQUIRE_MESSAGE(list, error);
            CHECK(list->size() == 1);
        }
        CHECK(fs::exists(kept_manifest_file(w.tmp.path())));
        CHECK(fs::exists(kept_signature_file(w.tmp.path())));

        auto offline = std::make_shared<FakeTransport>();
        auto later = config;
        // Long after the kept list expired: its templates still serve.
        later.now = [] { return *parse_utc_time("2027-06-01T00:00:00Z"); };
        RuntimeService s(later, offline);
        std::string error;
        auto list = s.templates(error);
        REQUIRE_MESSAGE(list, error);
        CHECK(list->size() == 1);
        CHECK(list->front().id == "static-site");
        // A project job (no fetch) gets the same.
        list = s.templates(error, false);
        REQUIRE(list);
        CHECK(list->size() == 1);

        // Settings (D20) lists runtimes from the kept list without going
        // online; asking for the network still fails offline.
        const int before = offline->total_requests();
        const auto cached = s.list(false, false, true);
        CHECK(cached["list"]["sequence"] == 3);
        CHECK_FALSE(cached["available"].empty());
        CHECK(offline->total_requests() == before);
        CHECK(s.list(true, false, true)["list"].is_null());  // refresh wins: tried online
        CHECK(offline->total_requests() > before);
    }

    SUBCASE("cached only, nothing kept: no list and no request") {
        auto quiet = std::make_shared<FakeTransport>();
        RuntimeService s(config, quiet);
        const auto out = s.list(false, false, true);
        CHECK(out["list"].is_null());
        CHECK(out["list_error"] == "there's no runtime list on this computer yet");
        CHECK(quiet->total_requests() == 0);
    }

    SUBCASE("a damaged kept pair is ignored") {
        w.publish(3, one);
        {
            RuntimeService s(config, w.transport);
            std::string error;
            REQUIRE(s.templates(error));
        }
        std::string error;
        REQUIRE(write_file_atomically(kept_signature_file(w.tmp.path()), "{}", error));
        RuntimeService s(config, std::make_shared<FakeTransport>());
        const auto list = s.templates(error);
        REQUIRE(list);
        CHECK(list->size() == builtin.size());
    }

    SUBCASE("a list without templates falls back too") {
        w.publish(2);
        RuntimeService s(config, w.transport);
        std::string error;
        const auto list = s.templates(error);
        REQUIRE(list);
        CHECK(list->size() == builtin.size());
    }

    SUBCASE("a list without templates is kept too: Settings has it offline") {
        w.publish(2);
        {
            RuntimeService s(config, w.transport);
            CHECK(s.list(true, false, false)["list"]["sequence"] == 2);
        }
        auto offline = std::make_shared<FakeTransport>();
        RuntimeService s(config, offline);
        const auto cached = s.list(false, false, true);
        CHECK(cached["list"]["sequence"] == 2);
        CHECK(offline->total_requests() == 0);
        std::string error;
        const auto list = s.templates(error, false);  // the built-in ones still
        REQUIRE(list);
        CHECK(list->size() == builtin.size());
    }

    SUBCASE("offline after a fetch: the list in hand, as a project job sees it") {
        w.publish(3, one);
        auto fresh_always = config;
        fresh_always.list_max_age = minutes{0};  // every call asks the CDN again
        RuntimeService s(fresh_always, w.transport);
        std::string error;
        REQUIRE(s.templates(error)->size() == 1);
        // The CDN goes away; the kept pair goes too, so only the list in
        // hand can give the signed templates.
        w.transport->serve(kSource.manifest_url, {0, "", "offline"});
        fs::remove(kept_manifest_file(w.tmp.path()));
        const auto wizard = s.templates(error, true);
        const auto job = s.templates(error, false);
        REQUIRE(wizard);
        REQUIRE(job);
        CHECK(wizard->size() == 1);
        CHECK(job->size() == 1);
    }

    SUBCASE("nothing at all: an error") {
        RuntimeService s(w.config(), nullptr);
        std::string error;
        CHECK_FALSE(s.templates(error));
        CHECK(error.find("internet") != std::string::npos);
    }
}

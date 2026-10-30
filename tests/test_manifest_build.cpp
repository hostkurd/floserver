#include "build.h"
#include "templates_json.h"

#include "floserver/core/manifest.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

using namespace floserver::manifest_tool;
using nlohmann::json;

namespace {

// The CDN check of 28 Oct 2026 (values from the user's report).
json real_report() {
    const auto file = [](const char* path, std::uint64_t size, const char* sha, const char* result) {
        return json{{"path", path}, {"size", size}, {"sha256", sha}, {"result", result}};
    };
    return {{"cdn", "https://cdn.floserver.org"},
            {"files",
             {file("php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip", 36310917,
                   "2a36a2edcb7a5b9252f638fd5e4cf84d7f6e066092654220dc0b63e5cf4e9723", "ok"),
              file("php/8.4.27/php-8.4.27-1-nts-Win32-vs17-x64.zip", 35296081,
                   "dbae154a0a89791f36c3d48e68bae797878333e7d6cb5721e74617de59b63ab7", "ok"),
              file("mariadb/11.4.13/mariadb-11.4.13-winx64.zip", 95024126,
                   "d62986d433eeebfde218560b276103831604a61e929e87f1a17f5aebd80257e2", "ok"),
              file("mariadb/11.4.13/mariadb-11.4.13.tar.gz", 221118336,
                   "1bb254b106d0a7ca871cfa18fa6e18d4b80a7430f9ec9d1571ec4271a13def96", "ok"),
              file("node/24.22.1/node-v24.22.1-win-x64.zip", 37748064,
                   "d2046d28c2c3dcd211ac5145c0bf30e2047ef9dfb306693c8c6e2eccd8b95fa5", "ok"),
              file("composer/2.10.3/composer.phar", 3642137,
                   "7a2d379d5b8ffdaa028580ef26494c36d2feef4b178d3dd1473a4dbc5e17c8d6", "ok"),
              file("nginx/1.30.6/nginx-1.30.6.zip", 2773293,
                   "c6ee35edb0a861063323f709192ae50d1eefaf2ea7d00678bd2b22d8542dc61e", "ok"),
              file("apache/2.4.69/httpd-2.4.69-261002-Win64-VS18.zip", 14495691,
                   "77d715d0d9cee731e2251e7ddc2090a7a8e6d41fedef2e9827cd918593c5a652", "unchecked")}}};
}

BuildOptions options() {
    BuildOptions o;
    o.sequence = 1;
    o.days = 30;
    o.now = *floserver::core::parse_utc_time("2026-10-28T12:00:00Z");
    o.accepted = {"apache/2.4.69/httpd-2.4.69-261002-Win64-VS18.zip"};
    return o;
}

bool has_error_with(const BuildResult& r, std::string_view text) {
    for (const auto& e : r.errors) {
        if (e.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("the real report builds a list the app accepts") {
    const auto r = build_manifest(real_report().dump(), options());
    REQUIRE(r.errors.empty());
    REQUIRE(r.manifest_json.has_value());
    CHECK(r.runtime_count == 7);
    REQUIRE(r.notes.size() == 1);  // the MariaDB source archive
    CHECK(r.notes[0].find("source archive, not listed") != std::string::npos);

    const auto doc = json::parse(*r.manifest_json);
    CHECK(doc["schema"] == 1);
    CHECK(doc["sequence"] == 1);
    CHECK(doc["expires"] == "2026-11-27T12:00:00Z");
    const auto& rt = doc["runtimes"];
    REQUIRE(rt.size() == 7);

    const auto& php = rt[0];
    CHECK(php["name"] == "php");
    CHECK(php["version"] == "8.5.12");  // not "8.5.12-1"
    CHECK(php["platform"] == "windows-x64");
    CHECK(php["variant"] == "nts");
    CHECK(php["archive"] == "zip");
    CHECK(php["license"] == "PHP-3.01");
    CHECK(php["urls"] ==
          json::array({"https://cdn.floserver.org/php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip",
                       "https://downloads.php.net/~windows/releases/archives/"
                       "php-8.5.12-1-nts-Win32-vs17-x64.zip"}));

    const auto find = [&](const char* name) {
        for (const auto& e : rt) {
            if (e["name"] == name) {
                return e;
            }
        }
        FAIL("missing " << name);
        return json{};
    };
    CHECK(find("composer")["archive"] == "file");
    CHECK(find("composer")["platform"] == "any");
    CHECK(find("mariadb")["urls"][1] ==
          "https://archive.mariadb.org/mariadb-11.4.13/winx64-packages/mariadb-11.4.13-winx64.zip");
    CHECK(find("node")["urls"][1] == "https://nodejs.org/dist/v24.22.1/node-v24.22.1-win-x64.zip");
    CHECK(find("nginx")["license"] == "BSD-2-Clause");
    CHECK(find("apache")["urls"].size() == 1);  // no stable upstream link
}

TEST_CASE("an unchecked file needs --accept, and a failed one is refused") {
    auto o = options();
    o.accepted.clear();
    auto r = build_manifest(real_report().dump(), o);
    CHECK_FALSE(r.manifest_json.has_value());
    CHECK(has_error_with(r, "--accept apache/2.4.69/httpd-2.4.69-261002-Win64-VS18.zip"));

    auto report = real_report();
    report["files"][0]["result"] = "fail";
    r = build_manifest(report.dump(), options());
    CHECK_FALSE(r.manifest_json.has_value());
    CHECK(has_error_with(r, "does NOT match"));

    o = options();
    o.accepted.insert("php/9.9.9/typo.zip");
    r = build_manifest(real_report().dump(), o);
    CHECK(has_error_with(r, "no unchecked file with that path"));
}

TEST_CASE("files that don't fit the catalog are refused or left out") {
    auto report = real_report();
    report["files"].push_back(
        {{"path", "ruby/3.4.0/ruby.zip"}, {"size", 1}, {"sha256", std::string(64, 'a')}, {"result", "ok"}});
    CHECK(has_error_with(build_manifest(report.dump(), options()), "isn't a runtime FloServer knows"));

    // Python (D17): python-build-standalone's Windows build, a .tar.gz, CDN only.
    report = real_report();
    report["files"].push_back(
        {{"path", "python/3.13.18/cpython-3.13.18-20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"},
         {"size", 22023022}, {"sha256", std::string(64, 'a')}, {"result", "ok"}});
    const auto py = build_manifest(report.dump(), options());
    REQUIRE(py.manifest_json.has_value());
    const json py_doc = json::parse(*py.manifest_json);
    bool found = false;
    for (const auto& rt : py_doc["runtimes"]) {
        if (rt["name"] == "python") {
            found = true;
            CHECK(rt["version"] == "3.13.18");
            CHECK(rt["archive"] == "tar.gz");
            CHECK(rt["license"] == "PSF-2.0");
            CHECK(rt["urls"].size() == 1);
        }
    }
    CHECK(found);
    report = real_report();
    report["files"].push_back(
        {{"path", "python/3.13.18/cpython-3.13.18-20261026-x86_64-pc-windows-msvc-pgo-full.tar.zst"},
         {"size", 1}, {"sha256", std::string(64, 'a')}, {"result", "ok"}});
    CHECK_FALSE(build_manifest(report.dump(), options()).manifest_json.has_value());

    report = real_report();
    report["files"].push_back(
        {{"path", "php/8.5.x/php.zip"}, {"size", 1}, {"sha256", std::string(64, 'a')}, {"result", "ok"}});
    CHECK(has_error_with(build_manifest(report.dump(), options()), "isn't a version folder"));

    report = real_report();
    report["files"].push_back(
        {{"path", "php.zip"}, {"size", 1}, {"sha256", std::string(64, 'a')}, {"result", "ok"}});
    CHECK(has_error_with(build_manifest(report.dump(), options()), "expected <runtime>/<version>/<file>"));

    // A file whose name says another version than its folder stops the build.
    report = real_report();
    report["files"][0]["path"] = "php/8.5.11/php-8.5.12-1-nts-Win32-vs17-x64.zip";
    auto r = build_manifest(report.dump(), options());
    CHECK_FALSE(r.manifest_json.has_value());
    CHECK(has_error_with(r, "doesn't match a php 8.5.11 Windows download"));

    // So does a misspelt name, such as a lower-case "win64".
    report = real_report();
    report["files"][7]["path"] = "apache/2.4.69/httpd-2.4.69-261002-win64-VS18.zip";
    auto o = options();
    o.accepted = {"apache/2.4.69/httpd-2.4.69-261002-win64-VS18.zip"};
    CHECK(has_error_with(build_manifest(report.dump(), o), "doesn't match a apache 2.4.69"));

    // Thread-safe PHP is another build of the release: noted, not listed.
    report = real_report();
    report["files"][0]["path"] = "php/8.5.12/php-8.5.12-1-Win32-vs17-x64.zip";
    r = build_manifest(report.dump(), options());
    REQUIRE(r.manifest_json.has_value());
    CHECK(r.runtime_count == 6);
    CHECK(r.notes.size() == 2);
}

TEST_CASE("the CDN address comes from the report and must be plain https") {
    auto report = real_report();
    report["cdn"] = "https://cdn.floserver.org/";  // a trailing slash is fine
    auto r = build_manifest(report.dump(), options());
    REQUIRE(r.manifest_json.has_value());
    CHECK(json::parse(*r.manifest_json)["runtimes"][0]["urls"][0] ==
          "https://cdn.floserver.org/php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip");

    report["cdn"] = "https://mirror.example.net/floserver";
    r = build_manifest(report.dump(), options());
    REQUIRE(r.manifest_json.has_value());
    CHECK(json::parse(*r.manifest_json)["runtimes"][0]["urls"][0] ==
          "https://mirror.example.net/floserver/php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip");

    for (const char* bad : {"http://cdn.floserver.org", "https://CDN.floserver.org", "https://x/../y",
                            "https://x/./y", "https://x/a%2e", "https://x/?q=1", "https://x:443",
                            "https://x//y", "", "https://"}) {
        CAPTURE(bad);
        report["cdn"] = bad;
        CHECK(has_error_with(build_manifest(report.dump(), options()), "plain https address"));
    }
    report.erase("cdn");
    CHECK(has_error_with(build_manifest(report.dump(), options()), "plain https address"));
}

TEST_CASE("a partial check (-Only) can't be published") {
    auto report = real_report();
    report["only"] = "php/";
    CHECK(has_error_with(build_manifest(report.dump(), options()), "only some files"));
    report["only"] = "";
    CHECK(build_manifest(report.dump(), options()).manifest_json.has_value());
    report["only"] = nullptr;
    CHECK(has_error_with(build_manifest(report.dump(), options()), "only some files"));
}

TEST_CASE("null or missing fields are errors, not crashes") {
    for (const char* key : {"result", "sha256", "size"}) {
        CAPTURE(key);
        auto report = real_report();
        report["files"][0][key] = nullptr;
        const auto r = build_manifest(report.dump(), options());
        CHECK_FALSE(r.manifest_json.has_value());
        CHECK_FALSE(r.errors.empty());
    }
}

TEST_CASE("sequence, days and the report itself are checked") {
    auto o = options();
    o.sequence = 0;
    CHECK(has_error_with(build_manifest(real_report().dump(), o), "--sequence"));
    o = options();
    o.days = 91;
    CHECK(has_error_with(build_manifest(real_report().dump(), o), "--days"));
    o.days = 90;
    CHECK(build_manifest(real_report().dump(), o).manifest_json.has_value());

    CHECK(has_error_with(build_manifest("not json", options()), "isn't a FloServer CDN check report"));
    CHECK(has_error_with(build_manifest("{}", options()), "isn't a FloServer CDN check report"));

    auto report = real_report();
    report["files"].push_back(report["files"][0]);
    CHECK(has_error_with(build_manifest(report.dump(), options()), "listed twice"));

    report = real_report();
    report["files"][0]["sha256"] = "abc";
    CHECK(has_error_with(build_manifest(report.dump(), options()), "no valid sha256"));
    report = real_report();
    report["files"][0]["size"] = 0;
    CHECK(has_error_with(build_manifest(report.dump(), options()), "no size"));
}

TEST_CASE("UTC times are written in the manifest's format") {
    CHECK(format_utc_time(*floserver::core::parse_utc_time("2028-02-29T23:59:59Z")) ==
          "2028-02-29T23:59:59Z");
    CHECK(format_utc_time(*floserver::core::parse_utc_time("2026-01-05T03:04:05Z")) ==
          "2026-01-05T03:04:05Z");
}

TEST_CASE("the built-in templates go into the list, and their runtimes must be there") {
    auto report = real_report();
    report["files"].push_back({{"path", "wordpress/7.1.4/wordpress-7.1.4.zip"},
                               {"size", 27000000},
                               {"sha256", std::string(64, 'a')},
                               {"result", "ok"}});
    BuildOptions o = options();
    o.templates_json = std::string(kBuiltInTemplates);
    const auto r = build_manifest(report.dump(), o);
    INFO((r.errors.empty() ? std::string() : r.errors.front()));
    REQUIRE(r.manifest_json);
    CHECK(r.template_count == 8);
    const json doc = json::parse(*r.manifest_json);
    REQUIRE(doc["templates"].size() == 8);
    CHECK(doc["templates"][0]["id"] == "php-app");
    // WordPress is listed for every platform, from the CDN with wordpress.org behind it.
    bool wordpress = false;
    for (const json& rt : doc["runtimes"]) {
        if (rt["name"] == "wordpress") {
            wordpress = true;
            CHECK(rt["platform"] == "any");
            CHECK(rt["urls"][1] == "https://wordpress.org/wordpress-7.1.4.zip");
        }
    }
    CHECK(wordpress);

    SUBCASE("a template whose runtime isn't listed is refused") {
        const auto without = build_manifest(real_report().dump(), o);  // no WordPress file
        CHECK_FALSE(without.manifest_json);
        CHECK(has_error_with(without, "template wordpress needs wordpress"));
    }
    SUBCASE("a template the app would skip is refused") {
        o.templates_json = R"([{"id": "Bad Id"}])";
        const auto bad = build_manifest(report.dump(), o);
        CHECK_FALSE(bad.manifest_json);
        CHECK(has_error_with(bad, "template entry 0"));
        o.templates_json = "{}";
        CHECK(has_error_with(build_manifest(report.dump(), o), "aren't a JSON array"));
    }
}

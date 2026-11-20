// D34.3: getting a version's files from the official sites (Download only).
// Fake sites stand in for the real ones.

#include "fetch.h"
#include "test_util.h"

#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/sha256.h"
#include "floserver/core/zip_extract.h"
#include "floserver/core/zip_write.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <fstream>
#include <sstream>

using namespace floserver::manifest_tool;
using test_util::FakeTransport;
namespace fs = std::filesystem;

namespace {

UpdateRow row(std::string runtime, std::string latest) {
    UpdateRow r;
    r.runtime = std::move(runtime);
    r.latest = std::move(latest);
    r.kind = UpdateKind::Update;
    return r;
}

std::string hex(std::string_view text) { return floserver::core::to_hex(floserver::core::sha256(text)); }

std::string slurp(const fs::path& p) {
    const std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

const std::vector<std::string> kEdbNames = {
    "pgsql/bin/postgres.exe", "pgsql/bin/initdb.exe",   "pgsql/bin/pg_ctl.exe",        "pgsql/bin/psql.exe",
    "pgsql/bin/pg_dump.exe",  "pgsql/bin/libpq.dll",    "pgsql/share/postgres.bki",    "pgsql/lib/plpgsql.dll",
    "pgsql/include/libpq-fe.h", "pgsql/pgAdmin 4/runtime/pgAdmin4.exe"};

// EDB's zip as zip64 (miniz copies such entries only into a zip64 archive).
void write_zip64(const fs::path& file, const std::vector<std::string>& names) {
    mz_zip_archive z{};
    REQUIRE(mz_zip_writer_init_heap_v2(&z, 0, 0, MZ_ZIP_FLAG_WRITE_ZIP64) != MZ_FALSE);
    for (const std::string& n : names) {
        const std::string body = "contents of " + n;
        REQUIRE(mz_zip_writer_add_mem(&z, n.c_str(), body.data(), body.size(), static_cast<mz_uint>(MZ_BEST_SPEED)) != MZ_FALSE);
    }
    void* buf = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&z, &buf, &size) != MZ_FALSE);
    {
        std::ofstream out(file, std::ios::binary);
        out.write(static_cast<const char*>(buf), static_cast<std::streamsize>(size));
    }
    mz_free(buf);
    mz_zip_writer_end(&z);
}

// A zip's first bytes, as zip_there() looks for.
const std::string kZip = std::string("PK\3\4") + "rest of a zip";

}  // namespace

TEST_CASE("fetch: plan_fetch names each runtime's official download and its CDN path") {
    FakeTransport http;
    const std::map<std::string, std::vector<std::string>> none;

    SUBCASE("fixed names") {
        const FetchPlan p = plan_fetch(http, row("node", "24.23.0"), none);
        REQUIRE(p.problem.empty());
        REQUIRE(p.files.size() == 1);
        CHECK(p.files[0].url == "https://nodejs.org/dist/v24.23.0/node-v24.23.0-win-x64.zip");
        CHECK(p.files[0].cdn_path == "node/24.23.0/node-v24.23.0-win-x64.zip");

        const FetchPlan b = plan_fetch(http, row("bun", "1.3.14"), none);
        REQUIRE(b.files.size() == 2);
        CHECK(b.files[1].cdn_path == "bun/1.3.14/bun-windows-x64-baseline.zip");
        CHECK(http.total_requests() == 0);
    }

    SUBCASE("PHP: the name with its rebuild number, from the checksum list") {
        http.serve("https://downloads.php.net/~windows/releases/sha256sum.txt",
                   {200, "aa *php-8.5.13-Win32-vs17-x64.zip\nbb *php-8.5.13-1-nts-Win32-vs17-x64.zip\n"});
        const FetchPlan p = plan_fetch(http, row("php", "8.5.13"), none);
        REQUIRE(p.files.size() == 1);
        CHECK(p.files[0].cdn_path == "php/8.5.13/php-8.5.13-1-nts-Win32-vs17-x64.zip");
        CHECK(plan_fetch(http, row("php", "8.5.1"), none).problem.find("don't name") != std::string::npos);
    }

    SUBCASE("Apache: the link on Apache Lounge's page") {
        http.serve("https://www.apachelounge.com/download/",
                   {200, R"(<a href="/download/VS18/binaries/httpd-2.4.66-251206-Win64-VS18.zip">x</a>)"});
        const FetchPlan p = plan_fetch(http, row("apache", "2.4.66"), none);
        REQUIRE(p.files.size() == 1);
        CHECK(p.files[0].url == "https://www.apachelounge.com/download/VS18/binaries/httpd-2.4.66-251206-Win64-VS18.zip");
        CHECK(p.files[0].cdn_path == "apache/2.4.66/httpd-2.4.66-251206-Win64-VS18.zip");
    }

    SUBCASE("Python: '+' in the release's name becomes '-' on the CDN") {
        http.serve("https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest",
                   {200, R"({"assets":[
                     {"name":"cpython-3.13.9+20251120-x86_64-pc-windows-msvc-install_only.tar.gz","browser_download_url":"https://x/a"},
                     {"name":"cpython-3.13.9+20251120-x86_64-pc-windows-msvc-install_only_stripped.tar.gz","browser_download_url":"https://x/b"}]})"});
        const FetchPlan p = plan_fetch(http, row("python", "3.13.9"), none);
        REQUIRE(p.files.size() == 1);
        CHECK(p.files[0].url == "https://x/b");
        CHECK(p.files[0].cdn_path ==
              "python/3.13.9/cpython-3.13.9-20251120-x86_64-pc-windows-msvc-install_only_stripped.tar.gz");
    }

    SUBCASE("PostgreSQL: EDB's highest build, made into the server zip") {
        const std::string base = "https://get.enterprisedb.com/postgresql/postgresql-17.12-";
        http.serve(base + "1-windows-x64-binaries.zip", {200, kZip});
        http.serve(base + "2-windows-x64-binaries.zip", {200, kZip});
        http.serve(base + "3-windows-x64-binaries.zip", {404, ""});
        http.serve(base + "4-windows-x64-binaries.zip", {200, "<html>not a zip</html>"});
        const FetchPlan p = plan_fetch(http, row("postgresql", "17.12"), none);
        REQUIRE(p.files.size() == 1);
        CHECK(p.files[0].url == base + "2-windows-x64-binaries.zip");
        CHECK(p.files[0].cdn_path == "postgresql/17.12/postgresql-17.12-2-windows-x64-server.zip");
        CHECK(p.files[0].prepare == FetchFile::Prepare::PostgresServerZip);

        CHECK(!plan_fetch(http, row("postgresql", "19.0"), none).problem.empty());
    }

    SUBCASE("phpredis: one zip per PHP series in the list, missing ones noted") {
        const std::map<std::string, std::vector<std::string>> listed{{"php", {"8.3.28", "8.4.16", "8.5.13"}}};
        const std::string base = "https://downloads.php.net/~windows/pecl/releases/redis/6.3.0/php_redis-6.3.0-";
        http.serve(base + "8.3-nts-vs16-x64.zip", {200, kZip});
        http.serve(base + "8.4-nts-vs17-x64.zip", {200, kZip});
        http.serve(base + "8.5-nts-vs17-x64.zip", {404, ""});
        const FetchPlan p = plan_fetch(http, row("php-redis", "6.3.0"), listed);
        REQUIRE(p.files.size() == 2);
        CHECK(p.files[0].cdn_path == "php-redis/6.3.0/php_redis-6.3.0-8.3-nts-vs16-x64.zip");
        CHECK(p.files[1].cdn_path == "php-redis/6.3.0/php_redis-6.3.0-8.4-nts-vs17-x64.zip");
        CHECK(p.note.find("8.5") != std::string::npos);
    }
}

TEST_CASE("fetch: fetch_files writes each file at its CDN path, checked against its official checksum") {
    FakeTransport http;
    const test_util::TempDir dir;
    const std::string body = "node zip bytes";
    FetchPlan plan;
    plan.title = "Node.js 24.23.0";
    plan.files = {{"https://nodejs.org/dist/v24.23.0/node-v24.23.0-win-x64.zip", "node/24.23.0/node-v24.23.0-win-x64.zip"}};
    http.serve(plan.files[0].url, {200, body});
    const fs::path file = dir.path() / "node" / "24.23.0" / "node-v24.23.0-win-x64.zip";

    SUBCASE("the same as the official checksum: kept") {
        http.serve("https://nodejs.org/dist/v24.23.0/SHASUMS256.txt", {200, hex(body) + "  node-v24.23.0-win-x64.zip\n"});
        const FetchResult r = fetch_files(http, plan, dir.path());
        CHECK(r.problem.empty());
        CHECK(r.cdn_paths == std::vector<std::string>{"node/24.23.0/node-v24.23.0-win-x64.zip"});
        CHECK(slurp(file) == body);
        CHECK(!fs::exists(fs::path(file) += ".part"));
    }
    SUBCASE("different: deleted, never left to upload") {
        http.serve("https://nodejs.org/dist/v24.23.0/SHASUMS256.txt", {200, hex("other") + "  node-v24.23.0-win-x64.zip\n"});
        const FetchResult r = fetch_files(http, plan, dir.path());
        CHECK(r.problem.find("DIFFERENT") != std::string::npos);
        CHECK(r.cdn_paths.empty());
        CHECK(!fs::exists(file));
    }
    SUBCASE("the list can't be read: deleted too") {
        http.serve("https://nodejs.org/dist/v24.23.0/SHASUMS256.txt", {404, ""});
        const FetchResult r = fetch_files(http, plan, dir.path());
        CHECK(!r.problem.empty());
        CHECK(!fs::exists(file));
    }
    SUBCASE("the download fails: no file, no .part") {
        http.serve(plan.files[0].url, {500, ""});
        const FetchResult r = fetch_files(http, plan, dir.path());
        CHECK(r.problem.find("HTTP 500") != std::string::npos);
        CHECK(!fs::exists(file));
        CHECK(!fs::exists(fs::path(file) += ".part"));
    }
}

TEST_CASE("fetch: a version is fetched whole or not at all") {
    FakeTransport http;
    const test_util::TempDir dir;
    FetchPlan plan;
    plan.title = "Deno 2.6.0";
    const std::string base = "https://github.com/denoland/deno/releases/download/v2.6.0/";
    // Two files of one version (as Bun's two builds); the second one fails.
    plan.files = {{base + "deno-x86_64-pc-windows-msvc.zip", "deno/2.6.0/deno-x86_64-pc-windows-msvc.zip"},
                  {base + "missing.zip", "deno/2.6.0/missing.zip"}};
    http.serve(plan.files[0].url, {200, "deno"});
    http.serve(base + "deno-x86_64-pc-windows-msvc.zip.sha256sum", {200, hex("deno") + "  deno-x86_64-pc-windows-msvc.zip\n"});
    http.serve(plan.files[1].url, {404, ""});
    const FetchResult r = fetch_files(http, plan, dir.path());
    CHECK(!r.problem.empty());
    CHECK(r.cdn_paths.empty());
    CHECK(!fs::exists(dir.path() / "deno"));  // the first file and its folders went too
}

TEST_CASE("fetch: PHP's zip found in the archives once a newer patch is out") {
    FakeTransport http;
    http.serve("https://downloads.php.net/~windows/releases/sha256sum.txt", {200, "aa *php-8.4.16-nts-Win32-vs17-x64.zip\n"});
    http.serve("https://downloads.php.net/~windows/releases/archives/sha256sum.txt",
               {200, "bb *php-8.4.15-nts-Win32-vs17-x64.zip\n"});
    const FetchPlan p = plan_fetch(http, row("php", "8.4.15"), {});
    REQUIRE(p.problem.empty());
    REQUIRE(p.files.size() == 1);
    CHECK(p.files[0].url == "https://downloads.php.net/~windows/releases/archives/php-8.4.15-nts-Win32-vs17-x64.zip");
}

TEST_CASE("fetch: the PostgreSQL server zip holds EDB's server files, copied as they are") {
    const test_util::TempDir dir;
    const fs::path edb = dir.path() / "edb.zip";
    {
        floserver::core::ZipWriter z;
        std::string e;
        REQUIRE(z.open(edb, e));
        for (const char* name : {"pgsql/bin/postgres.exe", "pgsql/bin/initdb.exe", "pgsql/bin/pg_ctl.exe",
                                 "pgsql/bin/psql.exe", "pgsql/bin/pg_dump.exe", "pgsql/bin/libpq.dll",
                                 "pgsql/share/postgres.bki", "pgsql/lib/plpgsql.dll", "pgsql/include/libpq-fe.h",
                                 "pgsql/pgAdmin 4/runtime/pgAdmin4.exe", "pgsql/StackBuilder/bin/stackbuilder.exe",
                                 "pgsql/doc/postgresql/html/index.html"}) {
            REQUIRE(z.add_text(name, std::string("contents of ") + name, e));
        }
        REQUIRE(z.finish(e));
    }
    const fs::path out = dir.path() / "server.zip";
    std::string error;
    REQUIRE(make_postgresql_server_zip(edb, out, error));
    CHECK(error.empty());

    std::string e1;
    std::string e2;
    const auto ours = floserver::core::zip_file_hashes(out, e1);
    const auto theirs = floserver::core::zip_file_hashes(edb, e2);
    REQUIRE(ours);
    REQUIRE(theirs);
    CHECK(ours->size() == 9);
    // What Publish checks: the same files at the same paths, every server one.
    const OfficialSource rules = official_source("postgresql/17.12/postgresql-17.12-1-windows-x64-server.zip");
    CHECK(made_from_problem(*ours, *theirs, rules.required, rules.kept).empty());
    for (const auto& [name, h] : *ours) {
        CHECK(name.find("pgAdmin") == std::string::npos);
        CHECK(name.find("/doc/") == std::string::npos);
    }

    SUBCASE("a zip64 zip from EDB") {
        const fs::path z64 = dir.path() / "edb64.zip";
        write_zip64(z64, kEdbNames);
        const fs::path out64 = dir.path() / "server64.zip";
        REQUIRE(make_postgresql_server_zip(z64, out64, error));
        std::string e3;
        std::string e4;
        const auto a = floserver::core::zip_file_hashes(out64, e3);
        const auto b = floserver::core::zip_file_hashes(z64, e4);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a->size() == 9);
        CHECK(made_from_problem(*a, *b, rules.required, rules.kept).empty());
    }

    SUBCASE("the server zip can't be written: a reason, not a quiet failure") {
        const fs::path blocker = dir.path() / "a-file";
        std::ofstream(blocker) << "x";
        error.clear();
        CHECK(!make_postgresql_server_zip(edb, blocker / "server.zip", error));
        CHECK(!error.empty());
    }

    SUBCASE("not EDB's binaries zip: refused, nothing left") {
        const fs::path other = dir.path() / "other.zip";
        floserver::core::ZipWriter z;
        std::string e;
        REQUIRE(z.open(other, e));
        REQUIRE(z.add_text("pgsql/bin/postgres.exe", "x", e));
        REQUIRE(z.finish(e));
        const fs::path out2 = dir.path() / "server2.zip";
        CHECK(!make_postgresql_server_zip(other, out2, error));
        CHECK(error.find("has no") != std::string::npos);
        CHECK(!fs::exists(out2));
        CHECK(!fs::exists(fs::path(out2) += ".part"));
    }
}

TEST_CASE("fetch: upload_steps gives a Wrangler command per file") {
    const std::string s = upload_steps({"node/24.23.0/node-v24.23.0-win-x64.zip"});
    CHECK(s.find("npx wrangler r2 object put floserver-cdn/node/24.23.0/node-v24.23.0-win-x64.zip "
                 "--file \"node\\24.23.0\\node-v24.23.0-win-x64.zip\" --remote") != std::string::npos);
}

TEST_CASE("fetch: add_to_list replaces the older versions of the same series") {
    std::vector<PublishFile> files;
    for (const char* p : {"php/8.4.15/php-8.4.15-nts-Win32-vs17-x64.zip", "php/8.3.28/php-8.3.28-nts-Win32-vs16-x64.zip",
                          "bun/1.3.13/bun-windows-x64.zip", "bun/1.3.13/bun-windows-x64-baseline.zip",
                          "postgresql/18.7/postgresql-18.7-1-windows-x64-server.zip"}) {
        PublishFile f;
        f.path = p;
        f.published = true;
        files.push_back(f);
    }
    const ListChange c = add_to_list(files, {"php/8.4.16/php-8.4.16-nts-Win32-vs17-x64.zip",
                                             "bun/1.3.14/bun-windows-x64-baseline.zip",
                                             "postgresql/17.12/postgresql-17.12-1-windows-x64-server.zip"});
    CHECK(c.problems.empty());
    CHECK(c.added.size() == 3);
    CHECK(c.left_out == std::vector<std::string>{"php/8.4.15/php-8.4.15-nts-Win32-vs17-x64.zip",
                                                 "bun/1.3.13/bun-windows-x64-baseline.zip"});
    std::vector<std::string> paths;
    paths.reserve(c.files.size());
    for (const PublishFile& f : c.files) {
        paths.push_back(f.path);
    }
    // PHP 8.3, Bun's regular build and PostgreSQL 18 stay.
    CHECK(paths == std::vector<std::string>{"php/8.3.28/php-8.3.28-nts-Win32-vs16-x64.zip", "bun/1.3.13/bun-windows-x64.zip",
                                            "postgresql/18.7/postgresql-18.7-1-windows-x64-server.zip",
                                            "php/8.4.16/php-8.4.16-nts-Win32-vs17-x64.zip",
                                            "bun/1.3.14/bun-windows-x64-baseline.zip",
                                            "postgresql/17.12/postgresql-17.12-1-windows-x64-server.zip"});
    CHECK(add_to_list(c.files, {"nonsense"}).problems.size() == 1);
}

TEST_CASE("fetch: a path the list can't carry leaves the older version in it") {
    PublishFile old;
    old.path = "node/24.22.1/node-v24.22.1-win-x64.zip";
    old.published = true;
    const ListChange c = add_to_list({old}, {"node/24.23.0/wrong-name.zip"});
    CHECK(c.problems.size() == 1);
    CHECK(c.left_out.empty());
    REQUIRE(c.files.size() == 1);
    CHECK(c.files[0].path == old.path);
}

TEST_CASE("fetch: upload_steps lists a file over 300 MiB apart") {
    const test_util::TempDir dir;
    const fs::path big = dir.path() / "mysql" / "8.4.8" / "mysql-8.4.8-winx64.zip";
    fs::create_directories(big.parent_path());
    std::ofstream(big) << "x";
    fs::resize_file(big, std::uintmax_t{301} * 1024 * 1024);  // sparse: no real 301 MiB written
    const std::string s = upload_steps({"mysql/8.4.8/mysql-8.4.8-winx64.zip"}, dir.path());
    CHECK(s.find("object put floserver-cdn/mysql") == std::string::npos);
    CHECK(s.find("Over 300 MiB") != std::string::npos);
    CHECK(s.find("  mysql/8.4.8/mysql-8.4.8-winx64.zip") != std::string::npos);
}

#include "floserver/core/runtime_store.h"

#include "test_util.h"

#include <doctest/doctest.h>
#include <miniz.h>

using namespace floserver::core;
using namespace test_util;

namespace {

std::string make_zip(const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, content] : entries) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(),
                                     static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)));
    }
    void* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
    std::string out(static_cast<const char*>(buffer), size);
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return out;
}

RuntimeFile entry(const char* name, const char* version, const char* archive = "zip") {
    RuntimeFile e;
    e.name = name;
    e.version = version;
    e.platform = "windows-x64";
    e.archive = archive;
    e.license = "MIT";
    e.urls = {"https://cdn.example.org/" + std::string(name) + "/" + version + "/file.bin"};
    return e;
}

}  // namespace

TEST_CASE("runtime folders are the version, plus any unusual variant") {
    auto e = entry("php", "8.5.12");
    CHECK(runtime_folder(e) == "8.5.12");
    e.variant = "nts";
    CHECK(runtime_folder(e) == "8.5.12");
    e.variant = "ts";
    CHECK(runtime_folder(e) == "8.5.12-ts");
    e.variant = "../x";
    CHECK(runtime_folder(e).empty());
    CHECK(runtime_folder(entry("../php", "8.5.12")).empty());
    CHECK(runtime_folder(entry("php", "8.5.12/..")).empty());
}

TEST_CASE("a runtime installs, with the VC++ DLLs next to its programs, and is listed") {
    const TempDir tmp;
    const auto root = tmp.path() / "runtimes";
    const auto vc = tmp.path() / "vcruntime";
    write_file(vc / "vcruntime140.dll", "vc");
    write_file(vc / "msvcp140.dll", "cpp");
    write_file(vc / "readme.txt", "not a dll");
    const auto archive = tmp.path() / "php.zip";
    write_file(archive, make_zip({{"php-cgi.exe", "MZ cgi"},
                                  {"msvcp140.dll", "php's own copy"},
                                  {"ext/php_intl.dll", "intl"},
                                  {"dev/php8.lib", "lib"}}));

    const auto e = entry("php", "8.5.12");
    CHECK_FALSE(is_installed(root, e));
    const auto r = install_runtime(e, archive, root, vc);
    INFO(r.message);
    REQUIRE(r.ok);
    CHECK_FALSE(r.already_installed);
    CHECK(r.dir == root / "php" / "8.5.12");
    CHECK(read_file(r.dir / "php-cgi.exe") == "MZ cgi");
    CHECK(read_file(r.dir / "vcruntime140.dll") == "vc");
    CHECK(read_file(r.dir / "msvcp140.dll") == "php's own copy");
    CHECK_FALSE(fs::exists(r.dir / "readme.txt"));
    CHECK_FALSE(fs::exists(r.dir / "ext" / "vcruntime140.dll"));  // no programs there
    CHECK(is_installed(root, e));

    const auto again = install_runtime(e, archive, root, vc);
    CHECK(again.ok);
    CHECK(again.already_installed);

    const auto listed = list_installed(root);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].name == "php");
    CHECK(listed[0].version == "8.5.12");
    CHECK(listed[0].license == "MIT");
    CHECK(listed[0].dir == r.dir);
}

TEST_CASE("Apache's DLLs go into Apache24/bin; a plain file is copied as is") {
    const TempDir tmp;
    const auto root = tmp.path() / "runtimes";
    const auto vc = tmp.path() / "vc";
    write_file(vc / "vcruntime140.dll", "vc");

    const auto apache_zip = tmp.path() / "apache.zip";
    write_file(apache_zip, make_zip({{"Apache24/bin/httpd.exe", "MZ"}, {"ReadMe.txt", "r"}}));
    const auto a = install_runtime(entry("apache", "2.4.69"), apache_zip, root, vc);
    REQUIRE(a.ok);
    CHECK(fs::exists(a.dir / "Apache24" / "bin" / "vcruntime140.dll"));
    CHECK_FALSE(fs::exists(a.dir / "vcruntime140.dll"));

    const auto phar = tmp.path() / "download.tmp";
    write_file(phar, "<?php composer");
    auto c = entry("composer", "2.10.3", "file");
    c.urls = {"https://cdn.example.org/composer/2.10.3/composer.phar"};
    const auto r = install_runtime(c, phar, root, vc);
    REQUIRE(r.ok);
    CHECK(read_file(r.dir / "composer.phar") == "<?php composer");
    CHECK_FALSE(fs::exists(r.dir / "vcruntime140.dll"));

    const auto listed = list_installed(root);
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].name == "apache");
    CHECK(listed[1].name == "composer");
}

TEST_CASE("a failed install leaves nothing; leftovers and broken folders are cleaned") {
    const TempDir tmp;
    const auto root = tmp.path() / "runtimes";
    const auto bad = tmp.path() / "bad.zip";
    write_file(bad, "not a zip");
    const auto r = install_runtime(entry("node", "24.22.1"), bad, root, {});
    CHECK_FALSE(r.ok);
    CHECK(fs::is_empty(root / "node"));

    // A crash mid-install leaves a staging folder; a folder without a marker
    // isn't an install.
    fs::create_directories(root / "node" / ".staging-24.22.1-123");
    write_file(root / "node" / "24.22.1" / "node.exe", "half");
    CHECK(list_installed(root).empty());
    remove_leftovers(root);
    CHECK_FALSE(fs::exists(root / "node" / ".staging-24.22.1-123"));

    const auto good = tmp.path() / "node.zip";
    write_file(good, make_zip({{"node.exe", "MZ"}}));
    const auto ok = install_runtime(entry("node", "24.22.1"), good, root, {});
    INFO(ok.message);
    REQUIRE(ok.ok);
    CHECK(read_file(ok.dir / "node.exe") == "MZ");
}

TEST_CASE("an installed runtime can be removed; nothing else can") {
    const TempDir tmp;
    const auto root = tmp.path() / "runtimes";
    const auto zip = tmp.path() / "n.zip";
    write_file(zip, make_zip({{"nginx.exe", "MZ"}}));
    REQUIRE(install_runtime(entry("nginx", "1.30.6"), zip, root, {}).ok);
    write_file(tmp.path() / "precious.txt", "keep");

    std::string error;
    CHECK_FALSE(remove_runtime(root, "nginx", "1.30.5", error));
    CHECK_FALSE(remove_runtime(root, "nginx", "..", error));
    CHECK_FALSE(remove_runtime(root, "..", "runtimes", error));
    CHECK(remove_runtime(root, "nginx", "1.30.6", error));
    CHECK_FALSE(fs::exists(root / "nginx" / "1.30.6"));
    CHECK(list_installed(root).empty());
    CHECK(fs::exists(tmp.path() / "precious.txt"));
}

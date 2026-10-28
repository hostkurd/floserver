#include "floserver/core/zip_extract.h"

#include "test_util.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <cstring>
#include <utility>

using namespace floserver::core;
using namespace test_util;

namespace {

using Entries = std::vector<std::pair<std::string, std::string>>;

// A zip built with miniz. A name ending in '/' is a folder.
std::string make_zip(const Entries& entries, mz_uint level = static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, content] : entries) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), level));
    }
    void* buffer = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
    std::string out(static_cast<const char*>(buffer), size);
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return out;
}

// Renames an entry in place (both headers), for names miniz won't write.
void rename_entry(std::string& zip, const std::string& from, const std::string& to) {
    REQUIRE(from.size() == to.size());
    int found = 0;
    for (std::size_t at = zip.find(from); at != std::string::npos; at = zip.find(from, at + 1)) {
        zip.replace(at, from.size(), to);
        ++found;
    }
    REQUIRE(found == 2);
}

void put16(std::string& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<char>(v & 0xffU);
    b[at + 1] = static_cast<char>(v >> 8U);
}

void put32(std::string& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<char>((v >> (8U * static_cast<unsigned>(i))) & 0xffU);
    }
}

// Marks an entry as a Unix symbolic link in the central directory.
void make_symlink(std::string& zip, const std::string& name) {
    const std::string sig("PK\x01\x02", 4);
    for (std::size_t at = zip.find(sig); at != std::string::npos; at = zip.find(sig, at + 1)) {
        const auto len = static_cast<std::size_t>(static_cast<unsigned char>(zip[at + 28]) |
                                                  (static_cast<unsigned char>(zip[at + 29]) << 8U));
        if (zip.compare(at + 46, len, name) == 0) {
            put16(zip, at + 4, (3U << 8U) | 20U);
            put32(zip, at + 38, 0120777U << 16U);
            return;
        }
    }
    FAIL("entry not found");
}

fs::path save(const TempDir& tmp, const std::string& bytes) {
    const auto p = tmp.path() / "archive.zip";
    write_file(p, bytes);
    return p;
}

}  // namespace

TEST_CASE("entry names: plain relative paths only") {
    CHECK(safe_entry_path("php.exe") == fs::path("php.exe"));
    CHECK(safe_entry_path("ext/php_intl.dll") == fs::path("ext") / "php_intl.dll");
    CHECK(safe_entry_path("ext\\php_intl.dll") == fs::path("ext") / "php_intl.dll");
    CHECK(safe_entry_path("dev/") == fs::path("dev"));
    CHECK(safe_entry_path("node_modules/npm/bin/npm-cli.js") ==
          fs::path("node_modules") / "npm" / "bin" / "npm-cli.js");

    for (const char* bad : {"", "/etc/passwd", "\\Windows\\x", "../evil", "a/../../evil", "a\\..\\evil", "./a",
                            "a//b", "C:evil", "C:/evil", "a/b:stream", "nul", "NUL.txt", "con", "Aux.dll",
                            "lpt1", "COM9.log", "conin$", "dir./x", "file ", "a/b.", "x\ty", "caf\xc3\xa9",
                            "a*b", "a?b", "a|b", "a<b", "a\"b"}) {
        INFO(bad);
        CHECK(safe_entry_path(bad).empty());
    }
    // Look-alikes that are fine.
    CHECK_FALSE(safe_entry_path("console.log").empty());
    CHECK_FALSE(safe_entry_path("com10").empty());
    CHECK_FALSE(safe_entry_path("..a").empty());
    CHECK_FALSE(safe_entry_path("a..b/c").empty());
    CHECK(safe_entry_path(std::string(300, 'a')).empty());
}

TEST_CASE("a zip unpacks, dropping a single top folder") {
    const TempDir tmp;
    const auto zip = save(tmp, make_zip({{"node-v24/", ""},
                                         {"node-v24/node.exe", "MZ node"},
                                         {"node-v24/node_modules/npm/package.json", "{}"},
                                         {"node-v24/empty/", ""}}));
    const auto target = tmp.path() / "out";
    const auto r = extract_zip(zip, target);
    INFO(r.message);
    REQUIRE(r.ok);
    CHECK(r.files == 2);
    CHECK(read_file(target / "node.exe") == "MZ node");
    CHECK(read_file(target / "node_modules" / "npm" / "package.json") == "{}");
    CHECK(fs::is_directory(target / "empty"));
    CHECK_FALSE(fs::exists(target / "node-v24"));
}

TEST_CASE("a zip with several top entries keeps its layout") {
    const TempDir tmp;
    const auto zip = save(tmp, make_zip({{"Apache24/bin/httpd.exe", "MZ"}, {"ReadMe.txt", "hi"}}));
    const auto target = tmp.path() / "out";
    REQUIRE(extract_zip(zip, target).ok);
    CHECK(read_file(target / "Apache24" / "bin" / "httpd.exe") == "MZ");
    CHECK(read_file(target / "ReadMe.txt") == "hi");
}

TEST_CASE("a file at the top stops the top folder from being dropped") {
    const TempDir tmp;
    const auto zip = save(tmp, make_zip({{"php", "a file named like the folder"}, {"php/php.exe", "MZ"}}));
    // "php" is both a file and a folder: refused rather than guessed.
    CHECK_FALSE(extract_zip(zip, tmp.path() / "out").ok);
}

TEST_CASE("unsafe archives write nothing outside the target") {
    const TempDir tmp;
    std::string bytes;
    std::string why;
    SUBCASE("parent folder") {
        bytes = make_zip({{"ok.txt", "1"}, {"zz/evil.txt", "2"}});
        rename_entry(bytes, "zz/evil.txt", "../evil.txt");
        why = "unsafe file name";
    }
    SUBCASE("absolute path") {
        bytes = make_zip({{"ok.txt", "1"}, {"zevil.txt", "2"}});
        rename_entry(bytes, "zevil.txt", "/evil.txt");
        why = "unsafe file name";
    }
    SUBCASE("backslash parent folder") {
        bytes = make_zip({{"ok.txt", "1"}, {"zz\\evil.txt", "2"}});
        why = "unsafe file name";
        // miniz may refuse backslashes when writing; patch if it wrote something else.
        if (bytes.find("zz\\evil.txt") != std::string::npos) {
            rename_entry(bytes, "zz\\evil.txt", "..\\evil.txt");
        }
    }
    SUBCASE("drive letter") {
        bytes = make_zip({{"ok.txt", "1"}, {"zzevil.txt", "2"}});
        rename_entry(bytes, "zzevil.txt", "C:evil.txt");
        why = "unsafe file name";
    }
    SUBCASE("device name") {
        bytes = make_zip({{"ok.txt", "1"}, {"nul.txt", "2"}});
        why = "unsafe file name";
    }
    SUBCASE("symbolic link") {
        bytes = make_zip({{"ok.txt", "1"}, {"link", "../../etc"}});
        make_symlink(bytes, "link");
        why = "symbolic link";
    }
    const auto zip = save(tmp, bytes);
    const auto target = tmp.path() / "deep" / "out";
    const auto r = extract_zip(zip, target);
    INFO(r.message);
    CHECK_FALSE(r.ok);
    CHECK(r.message.find(why) != std::string::npos);
    // Every entry is checked before the first is written.
    CHECK_FALSE(fs::exists(target));
    CHECK_FALSE(fs::exists(tmp.path() / "deep" / "evil.txt"));
    CHECK_FALSE(fs::exists(tmp.path() / "evil.txt"));
}

TEST_CASE("damaged or oversized archives are refused") {
    const TempDir tmp;
    SUBCASE("corrupted data fails its CRC-32") {
        std::string bytes = make_zip({{"data.bin", std::string(200, 'q')}}, 0);  // stored
        const auto at = bytes.find(std::string(200, 'q'));
        REQUIRE(at != std::string::npos);
        bytes[at + 100] = 'r';
        const auto r = extract_zip(save(tmp, bytes), tmp.path() / "out");
        CHECK_FALSE(r.ok);
    }
    SUBCASE("too big") {
        ZipLimits limits;
        limits.max_total_bytes = 100;
        const auto r = extract_zip(save(tmp, make_zip({{"a", std::string(60, 'a')}, {"b", std::string(60, 'b')}})),
                                   tmp.path() / "out", limits);
        CHECK_FALSE(r.ok);
        CHECK(r.message.find("more than") != std::string::npos);
        CHECK_FALSE(fs::exists(tmp.path() / "out"));
    }
    SUBCASE("too many files") {
        ZipLimits limits;
        limits.max_entries = 1;
        CHECK_FALSE(extract_zip(save(tmp, make_zip({{"a", "1"}, {"b", "2"}})), tmp.path() / "out", limits).ok);
    }
    SUBCASE("not a zip") {
        CHECK_FALSE(extract_zip(save(tmp, "this is not a zip file at all"), tmp.path() / "out").ok);
    }
    SUBCASE("the target already exists") {
        fs::create_directories(tmp.path() / "out");
        CHECK_FALSE(extract_zip(save(tmp, make_zip({{"a", "1"}})), tmp.path() / "out").ok);
    }
}

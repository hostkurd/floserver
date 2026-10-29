// .tar.gz runtimes (Python, D17): unpacked like a .zip, and refused when an
// archive could write outside its folder or isn't what it says.

#include "floserver/core/paths.h"
#include "floserver/core/tar_extract.h"
#include "test_util.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <algorithm>
#include <cstring>

using namespace floserver::core;
namespace fs = std::filesystem;

namespace {

struct TarItem {
    TarItem(std::string n, std::string d, char t = '0', unsigned m = 0644, std::string l = {})
        : name(std::move(n)), data(std::move(d)), type(t), mode(m), link(std::move(l)) {}
    std::string name;
    std::string data;
    char type;
    unsigned mode;
    std::string link;  // for links
};

void put_octal(std::string& h, std::size_t at, std::size_t len, std::uint64_t v) {
    std::string digits;
    do {
        digits.insert(digits.begin(), static_cast<char>('0' + (v & 7U)));
        v >>= 3U;
    } while (v != 0);
    digits = std::string(len - 1 - digits.size(), '0') + digits;
    h.replace(at, len - 1, digits);
    h[at + len - 1] = '\0';
}

std::string header(const std::string& name, std::uint64_t size, char type, unsigned mode, const std::string& link,
                   bool bad_checksum = false) {
    std::string h(512, '\0');
    h.replace(0, std::min<std::size_t>(name.size(), 100), name.substr(0, 100));
    put_octal(h, 100, 8, mode);
    put_octal(h, 108, 8, 0);
    put_octal(h, 116, 8, 0);
    put_octal(h, 124, 12, size);
    put_octal(h, 136, 12, 0);
    h[156] = type;
    h.replace(157, std::min<std::size_t>(link.size(), 100), link.substr(0, 100));
    h.replace(257, 6, std::string("ustar\0", 6));
    h.replace(263, 2, "00");
    h.replace(148, 8, "        ");
    unsigned sum = 0;
    for (const char c : h) {
        sum += static_cast<unsigned char>(c);
    }
    put_octal(h, 148, 7, sum + (bad_checksum ? 1 : 0));
    h[155] = ' ';
    return h;
}

std::string pad(std::string data) {
    data.append((512 - data.size() % 512) % 512, '\0');
    return data;
}

std::string make_tar(const std::vector<TarItem>& items, bool bad_checksum = false) {
    std::string out;
    for (const TarItem& it : items) {
        out += header(it.name, it.data.size(), it.type, it.mode, it.link, bad_checksum);
        out += pad(it.data);
    }
    out += std::string(1024, '\0');
    return out;
}

// RFC 1952: a header, raw deflate, CRC-32 and length.
std::string gzip(const std::string& data, bool bad_crc = false) {
    mz_stream z{};
    REQUIRE(mz_deflateInit2(&z, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9,
                            MZ_DEFAULT_STRATEGY) == MZ_OK);
    std::string body(mz_deflateBound(&z, static_cast<mz_ulong>(data.size())) + 64, '\0');
    z.next_in = reinterpret_cast<const unsigned char*>(data.data());
    z.avail_in = static_cast<unsigned>(data.size());
    z.next_out = reinterpret_cast<unsigned char*>(body.data());
    z.avail_out = static_cast<unsigned>(body.size());
    REQUIRE(mz_deflate(&z, MZ_FINISH) == MZ_STREAM_END);
    body.resize(z.total_out);
    mz_deflateEnd(&z);
    std::string out = {'\x1f', '\x8b', '\x08', '\x08', 0, 0, 0, 0, 0, '\x03'};
    out += "runtime.tar";
    out += '\0';
    out += body;
    auto crc = static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.data()), data.size()));
    if (bad_crc) {
        crc ^= 1U;
    }
    const auto size = static_cast<std::uint32_t>(data.size());
    for (const std::uint32_t v : {crc, size}) {
        for (int i = 0; i < 4; ++i) {
            out += static_cast<char>((v >> (8 * i)) & 0xFFU);
        }
    }
    return out;
}

fs::path write_archive(const fs::path& dir, const std::string& bytes) {
    const fs::path p = dir / "runtime.tar.gz";
    test_util::write_file(p, bytes);
    return p;
}

std::string read(const fs::path& p) { return read_small_file(p, std::size_t{16} * 1024 * 1024).value_or("<none>"); }

std::string pax(const std::string& key, const std::string& value) {
    const std::string record = " " + key + "=" + value + "\n";
    std::size_t len = record.size() + 1;
    while (std::to_string(len).size() + record.size() != len) {
        ++len;
    }
    return std::to_string(len) + record;
}

}  // namespace

TEST_CASE("a tar.gz unpacks into its folder, without the top folder") {
    const test_util::TempDir tmp;
    const std::string big(200'000, 'x');
    const std::string long_name = "python/Lib/" + std::string(120, 'a') + ".py";
    const auto archive = write_archive(
        tmp.path(), gzip(make_tar({{"./python/", "", '5'},
                                   {"./python/python.exe", "MZ", '0', 0755},
                                   {"python/Lib/os.py", "import sys\n"},
                                   {"python/Lib/big.bin", big},
                                   {"././@LongLink", long_name + std::string(1, '\0'), 'L'},
                                   {"truncated-name", "long one"},
                                   {"PaxHeader", pax("path", "python/Lib/pax-" + std::string(110, 'b') + ".txt"), 'x'},
                                   {"short", "pax one"}})));
    const fs::path target = tmp.path() / "out";
    const ZipResult r = extract_tar_gz(archive, target);
    INFO(r.message);
    REQUIRE(r.ok);
    CHECK(r.files == 5);
    CHECK(r.bytes == 2 + 11 + big.size() + 8 + 7);
    CHECK(read(target / "python.exe") == "MZ");
    CHECK(read(target / "Lib" / "os.py") == "import sys\n");
    CHECK(read(target / "Lib" / "big.bin") == big);
    CHECK(read(target / "Lib" / (std::string(120, 'a') + ".py")) == "long one");
    CHECK(read(target / "Lib" / ("pax-" + std::string(110, 'b') + ".txt")) == "pax one");
#ifndef _WIN32
    CHECK((fs::status(target / "python.exe").permissions() & fs::perms::owner_exec) != fs::perms::none);
    CHECK((fs::status(target / "Lib" / "os.py").permissions() & fs::perms::owner_exec) == fs::perms::none);
#endif
}

TEST_CASE("unsafe or damaged tar.gz archives are refused") {
    test_util::TempDir tmp;
    const auto refused = [&](const std::string& bytes, const std::string& why) {
        const fs::path dir = tmp.path() / std::to_string(std::hash<std::string>{}(bytes));
        fs::create_directories(dir);
        const ZipResult r = extract_tar_gz(write_archive(dir, bytes), dir / "out");
        CAPTURE(why);
        CHECK_FALSE(r.ok);
        CHECK_MESSAGE(r.message.find(why) != std::string::npos, r.message);
        CHECK_FALSE(fs::exists(dir / "evil.txt"));
        CHECK_FALSE(fs::exists(tmp.path() / "evil.txt"));
    };
    refused(gzip(make_tar({{"python/../../evil.txt", "x"}})), "unsafe file name");
    refused(gzip(make_tar({{"/etc/evil.txt", "x"}})), "unsafe file name");
    refused(gzip(make_tar({{"C:/evil.txt", "x"}})), "unsafe file name");
    refused(gzip(make_tar({{"python/nul.txt", "x"}})), "unsafe file name");
    refused(gzip(make_tar({{"python/a.txt", "x"}, {"PaxHeader", pax("path", "../evil.txt"), 'x'}, {"ok", "x"}})),
            "unsafe file name");
    refused(gzip(make_tar({{"python/link", "", '2', 0777, "../../evil.txt"}})), "link");
    refused(gzip(make_tar({{"python/hard", "", '1', 0644, "python/a"}})), "link");
    refused(gzip(make_tar({{"python/fifo", "", '6'}})), "kind FloServer doesn't unpack");
    refused(gzip(make_tar({{"python/a", "x"}}, true)), "damaged header");
    refused(gzip(make_tar({{"python/a", "x"}, {"python/a", "y"}})), "same file twice");
    refused(gzip(make_tar({{"python/a", "x"}}), true), "checksum doesn't match");
    const std::string whole = gzip(make_tar({{"python/a", std::string(5000, 'q')}}));
    refused(whole.substr(0, whole.size() / 2), "cut short");
    refused(whole + gzip("more"), "more than one gzip part");
    refused(make_tar({{"python/a", "x"}}), "isn't a gzip file");
    refused(gzip(make_tar({})), "no files");
    // Nothing is written when one name is bad, even after good ones.
    const fs::path dir = tmp.path() / "partial";
    fs::create_directories(dir);
    const ZipResult r = extract_tar_gz(write_archive(dir, gzip(make_tar({{"python/good", "x"}, {"../bad", "x"}}))),
                                       dir / "out");
    CHECK_FALSE(r.ok);
    CHECK_FALSE(fs::exists(dir / "out"));
}

TEST_CASE("tar.gz limits and an existing target") {
    const test_util::TempDir tmp;
    const auto archive = write_archive(tmp.path(), gzip(make_tar({{"a", "1234"}, {"b", "5678"}})));
    ZipLimits few;
    few.max_entries = 1;
    CHECK(extract_tar_gz(archive, tmp.path() / "x", few).message == "the archive has too many files");
    ZipLimits small;
    small.max_total_bytes = 6;
    CHECK(extract_tar_gz(archive, tmp.path() / "y", small).message ==
          "the archive unpacks to more than FloServer allows");
    fs::create_directories(tmp.path() / "z");
    CHECK(extract_tar_gz(archive, tmp.path() / "z").message.ends_with("already exists"));
    const ZipResult ok = extract_tar_gz(archive, tmp.path() / "w");
    REQUIRE(ok.ok);
    CHECK(read(tmp.path() / "w" / "a") == "1234");  // two top-level files: nothing dropped
}

TEST_CASE("tar.gz: repeated names, pax rules and what follows the end") {
    const test_util::TempDir tmp;
    int n = 0;
    const auto run = [&](const std::string& bytes, const ZipLimits& limits = {}) {
        const fs::path dir = tmp.path() / std::to_string(++n);
        fs::create_directories(dir);
        return std::pair{extract_tar_gz(write_archive(dir, bytes), dir / "out", limits), dir / "out"};
    };
    // The same name twice, also differing only in case (one file on
    // Windows), is refused before anything is written; a folder listed
    // again is fine.
    auto [r1, out1] = run(gzip(make_tar({{"p/A.txt", "1"}, {"p/a.txt", "2"}})));
    CHECK(r1.message.find("same file twice") != std::string::npos);
    CHECK_FALSE(fs::exists(out1));
    auto [r2, out2] = run(gzip(make_tar({{"p/d/", "", '5'}, {"p/d/x", "1"}, {"p/d", "", '5'}})));
    CHECK(r2.ok);
    auto [r3, out3] = run(gzip(make_tar({{"p/d/", "", '5'}, {"p/d", "file"}})));
    CHECK(r3.message.find("same file twice") != std::string::npos);
    // An old-style folder: no type, a trailing '/'.
    auto [r4, out4] = run(gzip(make_tar({{"p/old/", "", '\0'}, {"p/f", "1"}})));
    REQUIRE(r4.ok);
    CHECK(fs::is_directory(out4 / "old"));

    // pax: the last record wins, a malformed one is an error, sparse files are refused.
    auto [r5, out5] =
        run(gzip(make_tar({{"PaxHeader", pax("path", "p/first.txt") + pax("path", "p/second.txt"), 'x'}, {"x", "1"}})));
    REQUIRE(r5.ok);
    CHECK(fs::exists(out5 / "second.txt"));
    auto [r6, out6] = run(gzip(make_tar({{"PaxHeader", "9 path=p/x\n", 'x'}, {"p/y", "1"}})));
    CHECK(r6.message.find("damaged extended header") != std::string::npos);
    auto [r7, out7] = run(gzip(make_tar({{"PaxHeader", pax("size", "1junk"), 'x'}, {"p/y", "1"}})));
    CHECK(r7.message.find("unreadable size") != std::string::npos);
    auto [r8, out8] = run(gzip(make_tar({{"PaxHeader", pax("GNU.sparse.major", "1"), 'x'}, {"p/y", "1"}})));
    CHECK(r8.message.find("sparse") != std::string::npos);
    // pax wins over a GNU long name.
    auto [r9, out9] = run(gzip(make_tar(
        {{"PaxHeader", pax("path", "p/pax.txt"), 'x'}, {"././@LongLink", "p/gnu.txt", 'L'}, {"x", "1"}, {"p/z", "2"}})));
    REQUIRE(r9.ok);
    CHECK(fs::exists(out9 / "pax.txt"));

    // After the end only zeros: a second tar after a lone zero block is refused.
    const std::string one = make_tar({{"p/a", "1"}});
    auto [r10, out10] = run(gzip(one.substr(0, one.size() - 512) + make_tar({{"p/hidden", "2"}})));
    CHECK(r10.message.find("more after its end") != std::string::npos);
    // Extended headers count towards what may be unpacked.
    std::vector<TarItem> many;
    many.reserve(41);
    for (int i = 0; i < 40; ++i) {
        many.emplace_back("GlobalHead", std::string(100'000, 'g'), 'g');
    }
    many.emplace_back("p/a", "1");
    ZipLimits small;
    small.max_total_bytes = 10;
    CHECK(extract_tar_gz(write_archive(tmp.path(), gzip(make_tar(many))), tmp.path() / "many", small).ok);  // 4 MB
    many.insert(many.begin(), 700, TarItem("GlobalHead", std::string(100'000, 'g'), 'g'));  // 74 MB
    CHECK(extract_tar_gz(write_archive(tmp.path() / "m2", gzip(make_tar(many))), tmp.path() / "many2").message ==
          "the archive has extended headers that are too long");
    // Messages show names printable and short.
    auto [r11, out11] = run(gzip(make_tar({{"PaxHeader", pax("path", "p/\x1b[31m" + std::string(500, 'n')), 'x'},
                                           {"x", "1", '2', 0777, "t"}})));
    CHECK(r11.message.find('\x1b') == std::string::npos);
    CHECK(r11.message.size() < 300);
}

TEST_CASE("tar.gz: the bytes unpacked are the bytes whose SHA-256 was checked") {
    const test_util::TempDir tmp;
    const std::string bytes = gzip(make_tar({{"p/a", "1"}}));
    const auto archive = write_archive(tmp.path(), bytes);
    CHECK(extract_tar_gz(archive, tmp.path() / "ok", {}, sha256(bytes)).ok);
    auto wrong = sha256(bytes);
    wrong[0] ^= 1U;
    const ZipResult r = extract_tar_gz(archive, tmp.path() / "bad", {}, wrong);
    CHECK_FALSE(r.ok);
    CHECK(r.message == "the archive changed while it was unpacked");
}

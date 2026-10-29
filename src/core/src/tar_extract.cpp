#include "floserver/core/tar_extract.h"

#include "floserver/core/paths.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <system_error>
#include <vector>

namespace floserver::core {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBlock = 512;
constexpr std::size_t kBuffer = std::size_t{64} * 1024;
constexpr std::uint64_t kMaxExtendedHeader = std::uint64_t{1024} * 1024;
constexpr std::uint64_t kMaxLongName = 4096;
constexpr std::uint64_t kMaxExtendedTotal = std::uint64_t{64} * 1024 * 1024;  // all extended headers together
// Headers, padding and extended headers on top of the files themselves.
constexpr std::uint64_t kHeaderAllowance = std::uint64_t{256} * 1024 * 1024;
// What may follow the end of the tar (zeros: tar pads to whole records).
constexpr std::uint64_t kMaxTrailingZeros = std::uint64_t{1024} * 1024;
constexpr std::uint64_t kMaxNameBytes = std::uint64_t{64} * 1024 * 1024;
constexpr std::size_t kShownName = 200;

// The gzip flags (RFC 1952).
constexpr unsigned char kHeaderCrc = 0x02;
constexpr unsigned char kExtra = 0x04;
constexpr unsigned char kName = 0x08;
constexpr unsigned char kComment = 0x10;
constexpr unsigned char kReserved = 0xE0;

// A name from the archive as it may appear in a message: printable, short.
std::string shown(std::string_view name) {
    std::string out;
    for (const char c : name.substr(0, kShownName)) {
        const auto u = static_cast<unsigned char>(c);
        out += (u < 0x20 || u == 0x7F) ? '?' : c;
    }
    return name.size() > kShownName ? out + "..." : out;
}

// One gzip member, inflated as it's read, from one open file that can be
// read again from the start. On the second reading every byte can pass
// through the SHA-256, tying what's written to the checked download.
class GzipReader {
public:
    explicit GzipReader(std::uint64_t budget) : budget_(budget) {}
    GzipReader(const GzipReader&) = delete;
    GzipReader& operator=(const GzipReader&) = delete;
    GzipReader(GzipReader&&) = delete;
    GzipReader& operator=(GzipReader&&) = delete;
    ~GzipReader() {
        if (started_) {
            mz_inflateEnd(&z_);
        }
    }

    bool open(const fs::path& file) {
        in_.open(file, std::ios::binary);
        if (!in_) {
            return fail("can't read " + path_to_utf8(file));
        }
        return start();
    }

    // From the start again, through the same open file; `hash`: pass every
    // byte through the SHA-256 this time.
    bool rewind(bool hash) {
        hashing_ = hash;
        in_.clear();
        in_.seekg(0);
        if (started_) {
            mz_inflateEnd(&z_);
            started_ = false;
        }
        z_ = mz_stream{};
        ended_ = false;
        crc_ = MZ_CRC32_INIT;
        total_ = 0;
        sha_ = Sha256();
        error_.clear();
        return in_ && start();
    }

    // Exactly `n` bytes of the unpacked stream; false at its end or on damage.
    bool read(char* out, std::size_t n) {
        if (n > budget_ - std::min(budget_, total_)) {
            return fail("the archive unpacks to more than FloServer allows");
        }
        std::size_t got = 0;
        while (got < n) {
            if (ended_) {
                return fail("the archive is cut short");
            }
            const std::size_t produced = inflate_some(out + got, n - got);
            if (produced == 0 && !error_.empty()) {
                return false;
            }
            got += produced;
        }
        return true;
    }

    bool skip(std::uint64_t n) {
        while (n > 0) {
            const auto step = static_cast<std::size_t>(std::min<std::uint64_t>(n, scratch_.size()));
            if (!read(scratch_.data(), step)) {
                return false;
            }
            n -= step;
        }
        return true;
    }

    // After the tar's end: the rest of the stream may only be zeros (tar
    // pads to whole records), then the gzip trailer's CRC-32 and length,
    // then nothing but zeros to the end of the file.
    bool finish() {
        std::uint64_t after = 0;
        while (!ended_) {
            const std::size_t produced = inflate_some(scratch_.data(), scratch_.size());
            if (produced == 0 && !error_.empty()) {
                return false;
            }
            after += produced;
            if (after > kMaxTrailingZeros ||
                std::any_of(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(produced),
                            [](char c) { return c != '\0'; })) {
                return fail("the archive has more after its end, which FloServer doesn't unpack");
            }
        }
        std::array<unsigned char, 8> trailer{};
        for (unsigned char& b : trailer) {
            if (!next_byte(b)) {
                return fail("the gzip trailer is missing");
            }
        }
        const auto le32 = [&trailer](std::size_t at) {
            return static_cast<std::uint32_t>(trailer[at]) | (static_cast<std::uint32_t>(trailer[at + 1]) << 8U) |
                   (static_cast<std::uint32_t>(trailer[at + 2]) << 16U) |
                   (static_cast<std::uint32_t>(trailer[at + 3]) << 24U);
        };
        if (le32(0) != static_cast<std::uint32_t>(crc_) || le32(4) != static_cast<std::uint32_t>(total_)) {
            return fail("the archive's checksum doesn't match");
        }
        for (unsigned char b = 0; next_byte(b);) {
            if (b != 0) {
                return fail("the archive has more than one gzip part, which FloServer doesn't unpack");
            }
        }
        return true;
    }

    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] mz_ulong crc() const { return crc_; }
    [[nodiscard]] std::uint64_t total() const { return total_; }
    // The whole file's SHA-256, once finish() has read it to the end.
    [[nodiscard]] Sha256Digest digest() { return sha_.finish(); }

private:
    bool fail(std::string why) {
        if (error_.empty()) {
            error_ = std::move(why);
        }
        return false;
    }

    bool start() {
        std::array<unsigned char, 10> h{};
        if (!raw(h.data(), h.size()) || h[0] != 0x1f || h[1] != 0x8b || h[2] != 8 || (h[3] & kReserved) != 0) {
            return fail("the archive isn't a gzip file");
        }
        const unsigned char flags = h[3];
        if ((flags & kExtra) != 0) {
            std::array<unsigned char, 2> len{};
            if (!raw(len.data(), 2) || !skip_raw(static_cast<std::size_t>(len[0] | (len[1] << 8U)))) {
                return fail("the gzip header is cut short");
            }
        }
        for (const unsigned char text : {kName, kComment}) {
            if ((flags & text) != 0) {
                for (unsigned char c = 1; c != 0;) {
                    if (!raw(&c, 1)) {
                        return fail("the gzip header is cut short");
                    }
                }
            }
        }
        if ((flags & kHeaderCrc) != 0 && !skip_raw(2)) {
            return fail("the gzip header is cut short");
        }
        if (mz_inflateInit2(&z_, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
            return fail("can't start unpacking");
        }
        started_ = true;
        return true;
    }

    // Some of the unpacked stream into `out`; 0 with error_ set on damage.
    std::size_t inflate_some(char* out, std::size_t n) {
        if (z_.avail_in == 0) {
            in_.read(reinterpret_cast<char*>(input_.data()), static_cast<std::streamsize>(input_.size()));
            const auto got = static_cast<std::size_t>(in_.gcount());
            if (hashing_) {
                sha_.update(std::span<const std::uint8_t>(input_.data(), got));
            }
            z_.next_in = input_.data();
            z_.avail_in = static_cast<unsigned>(got);
        }
        const bool no_input = z_.avail_in == 0;
        z_.next_out = reinterpret_cast<unsigned char*>(out);
        z_.avail_out = static_cast<unsigned>(n);
        const int ret = mz_inflate(&z_, MZ_NO_FLUSH);
        const std::size_t produced = n - z_.avail_out;
        crc_ = mz_crc32(crc_, reinterpret_cast<const unsigned char*>(out), produced);
        total_ += produced;
        if (ret == MZ_STREAM_END) {
            ended_ = true;
        } else if (ret == MZ_BUF_ERROR && no_input) {
            fail("the archive is cut short");
            return 0;
        } else if (ret != MZ_OK && ret != MZ_BUF_ERROR) {
            fail("the archive is damaged");
            return 0;
        }
        return produced;
    }

    bool raw(unsigned char* out, std::size_t n) {
        in_.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
        const auto got = static_cast<std::size_t>(in_.gcount());
        if (hashing_) {
            sha_.update(std::span<const std::uint8_t>(out, got));
        }
        return got == n;
    }
    bool skip_raw(std::size_t n) {
        std::vector<unsigned char> buf(n);
        return raw(buf.data(), n);
    }
    // A byte after the deflate data: what inflate didn't use, then the file.
    bool next_byte(unsigned char& b) {
        if (z_.avail_in > 0) {
            b = *z_.next_in;
            ++z_.next_in;
            --z_.avail_in;
            return true;
        }
        return raw(&b, 1);
    }

    std::uint64_t budget_;
    std::ifstream in_;
    mz_stream z_{};
    bool started_ = false;
    bool ended_ = false;
    std::vector<unsigned char> input_ = std::vector<unsigned char>(kBuffer);
    std::vector<char> scratch_ = std::vector<char>(kBuffer);
    mz_ulong crc_ = MZ_CRC32_INIT;
    std::uint64_t total_ = 0;
    bool hashing_ = false;
    Sha256 sha_;
    std::string error_;
};

// A number in a header field: octal digits after optional spaces.
std::optional<std::uint64_t> octal(const char* field, std::size_t len) {
    if ((static_cast<unsigned char>(field[0]) & 0x80U) != 0) {
        return std::nullopt;  // base-256: only for sizes far beyond any runtime
    }
    std::size_t i = 0;
    while (i < len && field[i] == ' ') {
        ++i;
    }
    std::uint64_t v = 0;
    bool any = false;
    for (; i < len && field[i] >= '0' && field[i] <= '7'; ++i) {
        if (v > (std::uint64_t{1} << 60U)) {
            return std::nullopt;
        }
        v = v * 8 + static_cast<std::uint64_t>(field[i] - '0');
        any = true;
    }
    if (i < len && field[i] != ' ' && field[i] != '\0') {
        return std::nullopt;
    }
    return any ? std::optional<std::uint64_t>(v) : std::nullopt;
}

// Decimal digits only (a pax size).
std::optional<std::uint64_t> decimal(std::string_view s) {
    if (s.empty() || s.size() > 19) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

std::string field(const char* f, std::size_t len) { return {f, strnlen(f, len)}; }

bool checksum_ok(const std::array<char, kBlock>& h) {
    const auto want = octal(h.data() + 148, 8);
    std::uint64_t sum_unsigned = 0;
    std::int64_t sum_signed = 0;
    for (std::size_t i = 0; i < kBlock; ++i) {
        const char c = (i >= 148 && i < 156) ? ' ' : h[i];
        sum_unsigned += static_cast<unsigned char>(c);
        sum_signed += static_cast<signed char>(c);
    }
    return want && (*want == sum_unsigned || static_cast<std::int64_t>(*want) == sum_signed);
}

// A pax extended header's records ("<len> key=value\n"); a later record
// replaces an earlier one, as POSIX says. Nothing when it's malformed.
std::optional<std::map<std::string, std::string, std::less<>>> pax_records(const std::string& data) {
    std::map<std::string, std::string, std::less<>> out;
    std::size_t at = 0;
    while (at < data.size()) {
        const auto space = data.find(' ', at);
        if (space == std::string::npos || space == at) {
            return std::nullopt;
        }
        const auto len = decimal(std::string_view(data).substr(at, space - at));
        if (!len || *len <= space - at + 1 || *len > data.size() - at || data[at + *len - 1] != '\n') {
            return std::nullopt;
        }
        const std::string_view record(data.data() + space + 1, at + *len - 1 - (space + 1));
        const auto eq = record.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            return std::nullopt;
        }
        out[std::string(record.substr(0, eq))] = std::string(record.substr(eq + 1));
        at += *len;
    }
    return out;
}

struct TarEntry {
    std::string name;
    bool folder = false;
    std::uint64_t size = 0;
    std::uint64_t mode = 0;
};

// Calls `on_entry` for each file and folder, in order. For a file it must
// read exactly `size` bytes from the reader; padding is skipped here.
bool walk(GzipReader& gz, std::string& error, const std::function<bool(const TarEntry&)>& on_entry) {
    std::optional<std::string> pax_name;
    std::optional<std::string> gnu_name;
    std::optional<std::uint64_t> pax_size;
    std::uint64_t extended_total = 0;
    std::array<char, kBlock> h{};
    std::string data;
    const auto read_data = [&gz, &data](std::uint64_t size) {
        data.assign(static_cast<std::size_t>(size), '\0');
        return gz.read(data.data(), data.size()) && gz.skip((kBlock - size % kBlock) % kBlock);
    };
    while (true) {
        if (!gz.read(h.data(), h.size())) {
            error = gz.error();
            return false;
        }
        if (std::all_of(h.begin(), h.end(), [](char c) { return c == '\0'; })) {
            return true;  // the end
        }
        if (!checksum_ok(h)) {
            error = "the archive has a damaged header";
            return false;
        }
        const auto size = octal(h.data() + 124, 12);
        if (!size) {
            error = "the archive has an entry with a size FloServer can't read";
            return false;
        }
        const char type = h[156];
        if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
            extended_total += *size;
            if (*size > (type == 'L' || type == 'K' ? kMaxLongName : kMaxExtendedHeader) ||
                extended_total > kMaxExtendedTotal) {
                error = "the archive has extended headers that are too long";
                return false;
            }
            if (!read_data(*size)) {
                error = gz.error();
                return false;
            }
            if (type == 'x') {
                const auto records = pax_records(data);
                if (!records) {
                    error = "the archive has a damaged extended header";
                    return false;
                }
                for (const auto& [key, value] : *records) {
                    if (key == "path") {
                        pax_name = value;
                    } else if (key == "size") {
                        pax_size = decimal(value);
                        if (!pax_size) {
                            error = "the archive has an unreadable size";
                            return false;
                        }
                    } else if (key.starts_with("GNU.sparse.")) {
                        error = "the archive has a sparse file, which FloServer doesn't unpack";
                        return false;
                    }
                }
            } else if (type == 'L') {
                gnu_name = data.substr(0, data.find('\0'));
            }
            continue;  // 'g' and 'K' carry nothing used here
        }
        TarEntry e;
        if (pax_name) {
            e.name = std::move(*pax_name);  // pax wins over a GNU long name, as in GNU tar
        } else if (gnu_name) {
            e.name = std::move(*gnu_name);
        } else {
            e.name = field(h.data(), 100);
            const std::string prefix = std::string_view(h.data() + 257, 6) == std::string_view("ustar\0", 6)
                                           ? field(h.data() + 345, 155)
                                           : std::string();
            if (!prefix.empty()) {
                e.name = prefix + "/" + e.name;
            }
        }
        pax_name.reset();
        gnu_name.reset();
        e.size = pax_size.value_or(*size);
        pax_size.reset();
        e.mode = octal(h.data() + 100, 8).value_or(0);
        if (type == '1' || type == '2') {
            error = "the archive has a link, which isn't allowed: " + shown(e.name);
            return false;
        }
        if (type == '5' || (type == '\0' && e.name.ends_with('/'))) {  // old tars mark folders with a '/'
            e.folder = true;
        } else if (type != '0' && type != '\0' && type != '7') {
            error = "the archive has an entry of a kind FloServer doesn't unpack: " + shown(e.name);
            return false;
        }
        if (!on_entry(e)) {
            return false;
        }
        if (!gz.skip((kBlock - e.size % kBlock) % kBlock)) {
            error = gz.error();
            return false;
        }
    }
}

// "./python/bin/" → "python/bin" checked; nothing when refused; "" for the
// archive's own "./".
std::optional<std::string> entry_name(std::string name) {
    while (name.starts_with("./")) {
        name.erase(0, 2);
    }
    while (name.size() > 1 && name.ends_with('/')) {
        name.pop_back();
    }
    if (name.empty() || name == "." || name == "/") {
        return std::string();
    }
    const fs::path rel = safe_entry_path(name);
    if (rel.empty()) {
        return std::nullopt;
    }
    const std::u8string u = rel.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::string first_part(const std::string& name) { return name.substr(0, name.find('/')); }

std::string folded(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return s;
}

struct Planned {
    std::string name;  // checked, '/'-separated
    bool folder = false;
    std::uint64_t size = 0;
};

}  // namespace

ZipResult extract_tar_gz(const fs::path& archive, const fs::path& target, const ZipLimits& limits,
                         const std::optional<Sha256Digest>& expected_sha256) {
    ZipResult result;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        result.message = path_to_utf8(target) + " already exists";
        return result;
    }
    const std::uint64_t budget = limits.max_total_bytes > UINT64_MAX - kHeaderAllowance
                                     ? UINT64_MAX
                                     : limits.max_total_bytes + kHeaderAllowance;
    GzipReader gz(budget);

    // First pass: names and sizes, nothing written. A name already seen
    // (ignoring case, as Windows does) is refused here, except a folder
    // listed again.
    std::vector<Planned> planned;
    std::map<std::string, bool> seen;  // folded name -> is a folder
    std::uint64_t total = 0;
    std::uint64_t name_bytes = 0;
    if (!gz.open(archive)) {
        result.message = gz.error();
        return result;
    }
    const bool checked = walk(gz, result.message, [&](const TarEntry& e) {
        auto name = entry_name(e.name);
        if (!name) {
            result.message = "the archive has an unsafe file name: " + shown(e.name);
            return false;
        }
        if (planned.size() >= limits.max_entries) {
            result.message = "the archive has too many files";
            return false;
        }
        if (e.size > limits.max_total_bytes - total) {
            result.message = "the archive unpacks to more than FloServer allows";
            return false;
        }
        name_bytes += name->size();
        if (name_bytes > kMaxNameBytes) {
            result.message = "the archive's file names are too long altogether";
            return false;
        }
        if (!name->empty()) {
            const auto [it, added] = seen.emplace(folded(*name), e.folder);
            if (!added && !(e.folder && it->second)) {
                result.message = "the archive names the same file twice: " + shown(*name);
                return false;
            }
        }
        total += e.size;
        planned.push_back({std::move(*name), e.folder, e.size});
        if (!gz.skip(e.size)) {
            result.message = gz.error();
            return false;
        }
        return true;
    });
    if (!checked || !gz.finish()) {
        if (result.message.empty()) {
            result.message = gz.error();
        }
        return result;
    }
    const auto first_crc = gz.crc();
    const auto first_total = gz.total();

    // Drop a single top folder shared by every entry.
    bool drop = false;
    std::string top;
    for (const Planned& p : planned) {
        if (p.name.empty()) {
            continue;
        }
        if (top.empty()) {
            top = first_part(p.name);
            drop = true;
        }
        if (first_part(p.name) != top || (!p.folder && p.name.find('/') == std::string::npos)) {
            drop = false;
            break;
        }
    }

    fs::create_directories(target, ec);
    if (ec) {
        result.message = "can't create " + path_to_utf8(target) + ": " + error_text(ec);
        return result;
    }
    // Second pass: the same file, read again through the same handle; it
    // must give the same entries.
    if (!gz.rewind(expected_sha256.has_value())) {
        result.message = gz.error();
        return result;
    }
    std::size_t index = 0;
    std::vector<char> buf(kBuffer);
    const bool written = walk(gz, result.message, [&](const TarEntry& e) {
        const auto name = entry_name(e.name);
        if (index >= planned.size() || !name || planned[index].name != *name || planned[index].folder != e.folder ||
            planned[index].size != e.size) {
            result.message = "the archive changed while it was unpacked";
            return false;
        }
        ++index;
        if (name->empty() || (drop && *name == top)) {
            return gz.skip(e.size);  // the archive's "./" or the dropped top folder
        }
        const std::string rel = drop ? name->substr(top.size() + 1) : *name;
        const fs::path out = target / path_from_utf8(rel);
        if (e.folder) {
            fs::create_directories(out, ec);
            if (ec) {
                result.message = "can't create " + path_to_utf8(out) + ": " + error_text(ec);
                return false;
            }
            return true;
        }
        fs::create_directories(out.parent_path(), ec);
        if (ec) {
            result.message = "can't create " + path_to_utf8(out.parent_path()) + ": " + error_text(ec);
            return false;
        }
        if (fs::exists(fs::symlink_status(out, ec))) {
            result.message = "the archive names the same file twice: " + path_to_utf8(out);
            return false;
        }
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f) {
            result.message = "can't write " + path_to_utf8(out);
            return false;
        }
        for (std::uint64_t left = e.size; left > 0;) {
            const auto step = static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()));
            if (!gz.read(buf.data(), step)) {
                result.message = gz.error();
                return false;
            }
            f.write(buf.data(), static_cast<std::streamsize>(step));
            left -= step;
        }
        f.close();
        if (f.fail()) {
            result.message = "can't write " + path_to_utf8(out) + " (is the disk full?)";
            return false;
        }
#ifndef _WIN32
        if ((e.mode & 0111U) != 0) {
            fs::permissions(out, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                            fs::perm_options::add, ec);
        }
#endif
        ++result.files;
        result.bytes += e.size;
        return true;
    });
    if (!written || !gz.finish()) {
        if (result.message.empty()) {
            result.message = gz.error();
        }
        return result;
    }
    if (index != planned.size() || gz.crc() != first_crc || gz.total() != first_total ||
        (expected_sha256 && gz.digest() != *expected_sha256)) {
        result.message = "the archive changed while it was unpacked";
        return result;
    }
    if (result.files == 0) {
        result.message = "the archive has no files";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace floserver::core

#include "floserver/core/tar_extract.h"

#include "floserver/core/paths.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
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

// The gzip flags (RFC 1952).
constexpr unsigned char kHeaderCrc = 0x02;
constexpr unsigned char kExtra = 0x04;
constexpr unsigned char kName = 0x08;
constexpr unsigned char kComment = 0x10;
constexpr unsigned char kReserved = 0xE0;

// One gzip member, inflated as it's read.
class GzipReader {
public:
    GzipReader() = default;
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

    // Exactly `n` bytes of the unpacked stream; false at its end or on damage.
    bool read(char* out, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            if (ended_) {
                return fail("the archive is cut short");
            }
            if (z_.avail_in == 0) {
                in_.read(reinterpret_cast<char*>(input_.data()), static_cast<std::streamsize>(input_.size()));
                z_.next_in = input_.data();
                z_.avail_in = static_cast<unsigned>(in_.gcount());
            }
            const bool no_input = z_.avail_in == 0;
            z_.next_out = reinterpret_cast<unsigned char*>(out + got);
            z_.avail_out = static_cast<unsigned>(n - got);
            const int ret = mz_inflate(&z_, MZ_NO_FLUSH);
            const std::size_t produced = (n - got) - z_.avail_out;
            crc_ = mz_crc32(crc_, reinterpret_cast<const unsigned char*>(out + got), produced);
            total_ += produced;
            got += produced;
            if (ret == MZ_STREAM_END) {
                ended_ = true;
            } else if (ret == MZ_BUF_ERROR && no_input) {
                return fail("the archive is cut short");
            } else if (ret != MZ_OK && ret != MZ_BUF_ERROR) {
                return fail("the archive is damaged");
            }
        }
        return true;
    }

    bool skip(std::uint64_t n) {
        std::array<char, kBuffer> buf{};
        while (n > 0) {
            const auto step = static_cast<std::size_t>(std::min<std::uint64_t>(n, buf.size()));
            if (!read(buf.data(), step)) {
                return false;
            }
            n -= step;
        }
        return true;
    }

    // After the tar's end: the rest of the stream (padding), then the gzip
    // trailer's CRC-32 and length, and nothing more but zeros.
    bool finish() {
        std::array<char, kBuffer> buf{};
        while (!ended_) {
            if (z_.avail_in == 0) {
                in_.read(reinterpret_cast<char*>(input_.data()), static_cast<std::streamsize>(input_.size()));
                z_.next_in = input_.data();
                z_.avail_in = static_cast<unsigned>(in_.gcount());
            }
            const bool no_input = z_.avail_in == 0;
            z_.next_out = reinterpret_cast<unsigned char*>(buf.data());
            z_.avail_out = static_cast<unsigned>(buf.size());
            const int ret = mz_inflate(&z_, MZ_NO_FLUSH);
            const std::size_t produced = buf.size() - z_.avail_out;
            crc_ = mz_crc32(crc_, reinterpret_cast<const unsigned char*>(buf.data()), produced);
            total_ += produced;
            if (ret == MZ_STREAM_END) {
                ended_ = true;
            } else if ((ret == MZ_BUF_ERROR && no_input) || (ret != MZ_OK && ret != MZ_BUF_ERROR)) {
                return fail("the archive is cut short or damaged");
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

private:
    bool fail(std::string why) {
        if (error_.empty()) {
            error_ = std::move(why);
        }
        return false;
    }
    bool raw(unsigned char* out, std::size_t n) {
        in_.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
        return static_cast<std::size_t>(in_.gcount()) == n;
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

    std::ifstream in_;
    mz_stream z_{};
    bool started_ = false;
    bool ended_ = false;
    std::vector<unsigned char> input_ = std::vector<unsigned char>(kBuffer);
    mz_ulong crc_ = MZ_CRC32_INIT;
    std::uint64_t total_ = 0;
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

// The value of `key` in a pax extended header ("<len> key=value\n" records).
std::optional<std::string> pax_value(const std::string& data, std::string_view key) {
    std::size_t at = 0;
    while (at < data.size()) {
        const auto space = data.find(' ', at);
        if (space == std::string::npos) {
            return std::nullopt;
        }
        std::size_t len = 0;
        for (std::size_t i = at; i < space; ++i) {
            if (data[i] < '0' || data[i] > '9' || len > data.size()) {
                return std::nullopt;
            }
            len = len * 10 + static_cast<std::size_t>(data[i] - '0');
        }
        if (len <= space - at + 1 || at + len > data.size() || data[at + len - 1] != '\n') {
            return std::nullopt;
        }
        const std::string_view record(data.data() + space + 1, at + len - 1 - (space + 1));
        if (record.starts_with(key) && record.size() > key.size() && record[key.size()] == '=') {
            return std::string(record.substr(key.size() + 1));
        }
        at += len;
    }
    return std::nullopt;
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
    std::optional<std::string> long_name;
    std::optional<std::uint64_t> pax_size;
    std::array<char, kBlock> h{};
    const auto data_of = [&gz](std::uint64_t size, std::string& out) {
        out.assign(static_cast<std::size_t>(size), '\0');
        return gz.read(out.data(), out.size()) && gz.skip((kBlock - size % kBlock) % kBlock);
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
        std::string data;
        if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
            if (*size > (type == 'L' || type == 'K' ? kMaxLongName : kMaxExtendedHeader)) {
                error = "the archive has an extended header that's too long";
                return false;
            }
            if (!data_of(*size, data)) {
                error = gz.error();
                return false;
            }
            if (type == 'x') {
                if (auto p = pax_value(data, "path")) {
                    long_name = std::move(*p);
                }
                if (auto s = pax_value(data, "size")) {
                    try {
                        pax_size = std::stoull(*s);
                    } catch (const std::exception&) {
                        error = "the archive has an unreadable size";
                        return false;
                    }
                }
            } else if (type == 'L') {
                long_name = data.substr(0, data.find('\0'));
            }
            continue;  // 'g' and 'K' carry nothing used here
        }
        TarEntry e;
        if (long_name) {
            e.name = std::move(*long_name);
        } else {
            e.name = field(h.data(), 100);
            const std::string prefix = std::string_view(h.data() + 257, 6) == std::string_view("ustar\0", 6)
                                           ? field(h.data() + 345, 155)
                                           : std::string();
            if (!prefix.empty()) {
                e.name = prefix + "/" + e.name;
            }
        }
        long_name.reset();
        e.size = pax_size.value_or(*size);
        pax_size.reset();
        e.mode = octal(h.data() + 100, 8).value_or(0);
        if (type == '1' || type == '2') {
            error = "the archive has a link, which isn't allowed: " + e.name;
            return false;
        }
        if (type == '5') {
            e.folder = true;
            e.size = 0;
        } else if (type != '0' && type != '\0' && type != '7') {
            error = "the archive has an entry of a kind FloServer doesn't unpack: " + e.name;
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

struct Planned {
    std::vector<std::string> parts;
    bool folder = false;
    std::uint64_t size = 0;
};

// "./python/bin/" → {"python", "bin"}, folder; nothing when refused; an
// empty list for the archive's own "./".
std::optional<std::vector<std::string>> entry_parts(std::string name) {
    while (name.starts_with("./")) {
        name.erase(0, 2);
    }
    while (name.size() > 1 && name.ends_with('/')) {
        name.pop_back();
    }
    if (name.empty() || name == "." || name == "/") {
        return std::vector<std::string>{};
    }
    const fs::path rel = safe_entry_path(name);
    if (rel.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> parts;
    for (const fs::path& p : rel) {
        parts.push_back(path_to_utf8(p));
    }
    return parts;
}

}  // namespace

ZipResult extract_tar_gz(const fs::path& archive, const fs::path& target, const ZipLimits& limits) {
    ZipResult result;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        result.message = path_to_utf8(target) + " already exists";
        return result;
    }

    // First pass: names and sizes, nothing written.
    std::vector<Planned> planned;
    std::uint64_t total = 0;
    {
        GzipReader gz;
        if (!gz.open(archive)) {
            result.message = gz.error();
            return result;
        }
        const bool ok = walk(gz, result.message, [&](const TarEntry& e) {
            auto parts = entry_parts(e.name);
            if (!parts) {
                result.message = "the archive has an unsafe file name: " + e.name;
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
            total += e.size;
            planned.push_back({std::move(*parts), e.folder, e.size});
            if (!gz.skip(e.size)) {
                result.message = gz.error();
                return false;
            }
            return true;
        });
        if (!ok || !gz.finish()) {
            if (result.message.empty()) {
                result.message = gz.error();
            }
            return result;
        }
    }

    // Drop a single top folder shared by every entry.
    std::erase_if(planned, [](const Planned& p) { return p.parts.empty(); });
    std::size_t drop = 0;
    if (!planned.empty()) {
        const std::string& top = planned.front().parts.front();
        const bool shared = std::all_of(planned.begin(), planned.end(), [&](const Planned& p) {
            return p.parts.front() == top && (p.folder || p.parts.size() >= 2);
        });
        drop = shared ? 1 : 0;
    }

    fs::create_directories(target, ec);
    if (ec) {
        result.message = "can't create " + path_to_utf8(target) + ": " + error_text(ec);
        return result;
    }
    // Second pass: the same entries, written.
    GzipReader gz;
    if (!gz.open(archive)) {
        result.message = gz.error();
        return result;
    }
    std::size_t index = 0;
    std::array<char, kBuffer> buf{};
    const bool ok = walk(gz, result.message, [&](const TarEntry& e) {
        const auto parts = entry_parts(e.name);
        if (!parts || parts->empty()) {
            return gz.skip(e.size);
        }
        if (index >= planned.size() || planned[index].parts != *parts || planned[index].size != e.size) {
            result.message = "the archive changed while it was unpacked";
            return false;
        }
        ++index;
        if (parts->size() <= drop) {
            return true;  // the dropped top folder itself
        }
        fs::path out = target;
        for (std::size_t i = drop; i < parts->size(); ++i) {
            out /= path_from_utf8((*parts)[i]);
        }
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
    if (!ok || !gz.finish()) {
        if (result.message.empty()) {
            result.message = gz.error();
        }
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

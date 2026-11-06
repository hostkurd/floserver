#include "floserver/core/zip_extract.h"

#include "floserver/core/paths.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace floserver::core {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxNameLength = 1024;
constexpr std::size_t kMaxComponentLength = 255;
constexpr unsigned kUnixHost = 3;
constexpr std::uint32_t kUnixTypeMask = 0170000;
constexpr std::uint32_t kUnixSymlink = 0120000;

bool is_reserved_device(std::string_view component) {
    // Windows treats these as devices whatever the extension: "nul.txt" too.
    std::string base(component.substr(0, component.find('.')));
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    std::transform(base.begin(), base.end(), base.begin(),
                   [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    static constexpr std::array<std::string_view, 6> kNames = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    if (std::find(kNames.begin(), kNames.end(), base) != kNames.end()) {
        return true;
    }
    if (!base.starts_with("COM") && !base.starts_with("LPT")) {
        return false;
    }
    // COM1, and COM¹ COM² COM³ (U+00B9, U+00B2, U+00B3), which Windows takes as devices too.
    const std::string_view digit = std::string_view(base).substr(3);
    return (digit.size() == 1 && digit[0] >= '0' && digit[0] <= '9') || digit == "\xC2\xB9" ||
           digit == "\xC2\xB2" || digit == "\xC2\xB3";
}

// The code point starting at `i` in well-formed UTF-8 (shortest form, no
// surrogates), moving `i` past it; nothing when it isn't.
std::optional<char32_t> next_code_point(std::string_view s, std::size_t& i) {
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t len = 0;
    char32_t cp = 0;
    if (b < 0x80) {
        ++i;
        return b;
    }
    if ((b & 0xE0U) == 0xC0U) {
        len = 2;
        cp = b & 0x1FU;
    } else if ((b & 0xF0U) == 0xE0U) {
        len = 3;
        cp = b & 0x0FU;
    } else if ((b & 0xF8U) == 0xF0U) {
        len = 4;
        cp = b & 0x07U;
    } else {
        return std::nullopt;
    }
    if (i + len > s.size()) {
        return std::nullopt;
    }
    for (std::size_t k = 1; k < len; ++k) {
        const auto c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0U) != 0x80U) {
            return std::nullopt;
        }
        cp = (cp << 6U) | (c & 0x3FU);
    }
    constexpr std::array<char32_t, 5> kLeast = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kLeast.at(len) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return std::nullopt;
    }
    i += len;
    return cp;
}

bool is_safe_component(std::string_view c, bool unicode) {
    if (c.empty() || c.size() > kMaxComponentLength || c == "." || c == "..") {
        return false;
    }
    if (c.back() == '.' || c.back() == ' ') {
        return false;  // Windows drops them, so two names could reach one file
    }
    for (std::size_t i = 0; i < c.size();) {
        const auto u = static_cast<unsigned char>(c[i]);
        if (u >= 0x80) {
            // A backup's names: any letter, but no control characters and no
            // direction marks that would show a name other than it is.
            const auto cp = unicode ? next_code_point(c, i) : std::nullopt;
            if (!cp || *cp < 0xA0 || (*cp >= 0x200E && *cp <= 0x200F) || (*cp >= 0x202A && *cp <= 0x202E) ||
                (*cp >= 0x2066 && *cp <= 0x2069) || *cp == 0xFEFF) {
                return false;
            }
            continue;
        }
        if (u < 0x20 || u > 0x7e) {
            return false;
        }
        if (std::string_view("<>:\"|?*").find(c[i]) != std::string_view::npos) {
            return false;
        }
        ++i;
    }
    return !is_reserved_device(c);
}

// The entry's path components, or nothing when the name is refused.
// A trailing slash (a folder entry) is allowed and dropped.
std::optional<std::vector<std::string>> split_name(std::string_view name, bool unicode) {
    if (name.empty() || name.size() > kMaxNameLength || name.front() == '/' || name.front() == '\\') {
        return std::nullopt;
    }
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < name.size()) {
        std::size_t end = name.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = name.size();
        }
        const std::string_view part = name.substr(start, end - start);
        if (!is_safe_component(part, unicode)) {
            return std::nullopt;
        }
        parts.emplace_back(part);
        start = end + 1;
    }
    if (parts.empty()) {
        return std::nullopt;
    }
    return parts;
}

struct Reader {
    std::ifstream in;
    std::uint64_t size = 0;
};

std::size_t read_at(void* opaque, mz_uint64 offset, void* buffer, std::size_t n) {
    auto* r = static_cast<Reader*>(opaque);
    if (offset > r->size || n > r->size - offset) {
        n = offset > r->size ? 0 : static_cast<std::size_t>(r->size - offset);
    }
    r->in.clear();
    r->in.seekg(static_cast<std::streamoff>(offset));
    r->in.read(static_cast<char*>(buffer), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(r->in.gcount());
}

struct Writer {
    std::ofstream out;
    std::uint64_t expected = 0;
    std::uint64_t written = 0;
    bool too_big = false;
};

std::size_t write_at(void* opaque, mz_uint64 offset, const void* buffer, std::size_t n) {
    auto* w = static_cast<Writer*>(opaque);
    if (offset != w->written || n > w->expected - w->written) {
        w->too_big = true;
        return 0;
    }
    w->out.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(n));
    if (!w->out) {
        return 0;
    }
    w->written += n;
    return n;
}

struct Archive {
    mz_zip_archive zip{};
    bool open = false;
    Archive() = default;
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    Archive(Archive&&) = delete;
    Archive& operator=(Archive&&) = delete;
    ~Archive() {
        if (open) {
            mz_zip_reader_end(&zip);
        }
    }
};

std::string zip_error(mz_zip_archive& zip) {
    return mz_zip_get_error_string(mz_zip_get_last_error(&zip));
}

struct Entry {
    mz_uint index = 0;
    std::vector<std::string> parts;
    bool folder = false;
    std::uint64_t size = 0;
};

// Opens `archive` for reading: false (with `error`) when it can't be.
bool open_archive(const fs::path& archive, Reader& reader, Archive& a, std::string& error) {
    std::error_code ec;
    reader.in.open(archive, std::ios::binary);
    reader.size = fs::file_size(archive, ec);
    if (!reader.in || ec) {
        error = "can't read " + path_to_utf8(archive);
        return false;
    }
    a.zip.m_pRead = read_at;
    a.zip.m_pIO_opaque = &reader;
    if (mz_zip_reader_init(&a.zip, reader.size, 0) == MZ_FALSE) {
        error = "the archive can't be opened: " + zip_error(a.zip);
        return false;
    }
    a.open = true;
    return true;
}

// The index of the plain file named exactly `name` at the archive's top.
std::optional<mz_uint> top_file(Archive& a, std::string_view name, std::uint64_t& size, std::string& error) {
    const mz_uint count = mz_zip_reader_get_num_files(&a.zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        if (mz_zip_reader_file_stat(&a.zip, i, &st) == MZ_FALSE) {
            error = "the archive's file list can't be read: " + zip_error(a.zip);
            return std::nullopt;
        }
        if (std::string_view(st.m_filename) != name) {
            continue;
        }
        if (st.m_is_directory != 0 || st.m_is_encrypted != MZ_FALSE || st.m_is_supported == MZ_FALSE ||
            ((st.m_version_made_by >> 8U) == kUnixHost &&
             ((st.m_external_attr >> 16U) & kUnixTypeMask) == kUnixSymlink)) {
            error = "the archive's " + std::string(name) + " isn't a plain file";
            return std::nullopt;
        }
        size = st.m_uncomp_size;
        return i;
    }
    return std::nullopt;
}

std::size_t append_to_string(void* opaque, mz_uint64 offset, const void* buffer, std::size_t n) {
    auto* out = static_cast<std::pair<std::string*, std::size_t>*>(opaque);
    if (offset != out->first->size() || n > out->second - out->first->size()) {
        return 0;
    }
    out->first->append(static_cast<const char*>(buffer), n);
    return n;
}

}  // namespace

fs::path safe_entry_path(std::string_view name, bool unicode_names) {
    const auto parts = split_name(name, unicode_names);
    if (!parts) {
        return {};
    }
    fs::path out;
    for (const auto& p : *parts) {
        out /= path_from_utf8(p);
    }
    return out;
}

std::optional<std::string> read_zip_entry(const fs::path& archive, std::string_view name, std::size_t max_bytes,
                                          std::string& error) {
    error.clear();
    Reader reader;
    Archive a;
    if (!open_archive(archive, reader, a, error)) {
        return std::nullopt;
    }
    std::uint64_t size = 0;
    const auto index = top_file(a, name, size, error);
    if (!index) {
        return std::nullopt;
    }
    if (size > max_bytes) {
        error = "the archive's " + std::string(name) + " is larger than FloServer reads";
        return std::nullopt;
    }
    std::string out;
    out.reserve(static_cast<std::size_t>(size));
    std::pair<std::string*, std::size_t> sink{&out, static_cast<std::size_t>(size)};
    if (mz_zip_reader_extract_to_callback(&a.zip, *index, append_to_string, &sink, 0) == MZ_FALSE ||
        out.size() != size) {
        error = "the archive's " + std::string(name) + " can't be read: " + zip_error(a.zip);
        return std::nullopt;
    }
    return out;
}

bool extract_zip_entry(const fs::path& archive, std::string_view name, const fs::path& file, std::string& error) {
    error.clear();
    Reader reader;
    Archive a;
    if (!open_archive(archive, reader, a, error)) {
        return false;
    }
    std::uint64_t size = 0;
    const auto index = top_file(a, name, size, error);
    if (!index) {
        if (error.empty()) {
            error = "the archive has no " + std::string(name);
        }
        return false;
    }
    Writer w;
    w.expected = size;
    w.out.open(file, std::ios::binary | std::ios::trunc);
    if (!w.out) {
        error = "can't write " + path_to_utf8(file);
        return false;
    }
    const bool extracted = mz_zip_reader_extract_to_callback(&a.zip, *index, write_at, &w, 0) != 0;
    w.out.close();
    if (!extracted || w.too_big || w.out.fail() || w.written != size) {
        error = "can't unpack " + std::string(name) + ": " +
                (extracted ? std::string("write failed (is the disk full?)") : zip_error(a.zip));
        return false;
    }
    return true;
}

namespace {

struct Sink {
    const std::function<bool(std::string_view)>* sink = nullptr;
    std::uint64_t written = 0;
    std::uint64_t expected = 0;
    bool stopped = false;
};

std::size_t to_sink(void* opaque, mz_uint64 offset, const void* buffer, std::size_t n) {
    auto* s = static_cast<Sink*>(opaque);
    if (offset != s->written || n > s->expected - s->written) {
        return 0;
    }
    if (!(*s->sink)(std::string_view(static_cast<const char*>(buffer), n))) {
        s->stopped = true;
        return 0;
    }
    s->written += n;
    return n;
}

}  // namespace

bool stream_zip_entry(const fs::path& archive, std::string_view name,
                      const std::function<bool(std::string_view piece)>& sink, std::string& error,
                      std::uint64_t* size) {
    error.clear();
    Reader reader;
    Archive a;
    if (!open_archive(archive, reader, a, error)) {
        return false;
    }
    std::uint64_t bytes = 0;
    const auto index = top_file(a, name, bytes, error);
    if (!index) {
        if (error.empty()) {
            error = "the archive has no " + std::string(name);
        }
        return false;
    }
    if (size != nullptr) {
        *size = bytes;
    }
    Sink s{&sink, 0, bytes, false};
    const bool ok = mz_zip_reader_extract_to_callback(&a.zip, *index, to_sink, &s, 0) != 0;
    if (!ok || s.written != bytes) {
        if (!s.stopped || error.empty()) {
            error = s.stopped ? "stopped" : "can't unpack " + std::string(name) + ": " + zip_error(a.zip);
        }
        return false;
    }
    return true;
}

std::optional<std::uint64_t> zip_entry_size(const fs::path& archive, std::string_view name, std::string& error) {
    error.clear();
    Reader reader;
    Archive a;
    if (!open_archive(archive, reader, a, error)) {
        return std::nullopt;
    }
    std::uint64_t bytes = 0;
    if (!top_file(a, name, bytes, error)) {
        return std::nullopt;
    }
    return bytes;
}

ZipResult extract_zip(const fs::path& archive, const fs::path& target, const ZipLimits& limits,
                      const ZipOptions& options) {
    ZipResult result;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        result.message = path_to_utf8(target) + " already exists";
        return result;
    }

    Reader reader;
    Archive a;
    if (!open_archive(archive, reader, a, result.message)) {
        return result;
    }

    const mz_uint count = mz_zip_reader_get_num_files(&a.zip);
    if (count > limits.max_entries) {
        result.message = "the archive has too many files";
        return result;
    }

    // First pass: check every entry before writing anything.
    std::vector<Entry> entries;
    entries.reserve(count);
    std::uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        if (mz_zip_reader_file_stat(&a.zip, i, &st) == MZ_FALSE) {
            result.message = "the archive's file list can't be read: " + zip_error(a.zip);
            return result;
        }
        const std::string_view name(st.m_filename);
        if (st.m_is_encrypted != MZ_FALSE || st.m_is_supported == MZ_FALSE) {
            result.message = "the archive has an encrypted or unsupported entry: " + std::string(name);
            return result;
        }
        if ((st.m_version_made_by >> 8U) == kUnixHost &&
            ((st.m_external_attr >> 16U) & kUnixTypeMask) == kUnixSymlink) {
            result.message = "the archive has a symbolic link, which isn't allowed: " + std::string(name);
            return result;
        }
        auto parts = split_name(name, options.unicode_names);
        if (!parts) {
            result.message = "the archive has an unsafe file name: " + std::string(name);
            return result;
        }
        const bool folder = st.m_is_directory != 0;
        if (!folder) {
            if (st.m_uncomp_size > limits.max_total_bytes - total) {
                result.message = "the archive unpacks to more than FloServer allows";
                return result;
            }
            total += st.m_uncomp_size;
        }
        entries.push_back({i, std::move(*parts), folder, st.m_uncomp_size});
    }

    // Drop a single top folder shared by every entry.
    if (options.drop_top_folder && !entries.empty()) {
        const std::string& top = entries.front().parts.front();
        const bool shared = std::all_of(entries.begin(), entries.end(), [&](const Entry& e) {
            return e.parts.front() == top && (e.folder || e.parts.size() >= 2);
        });
        if (shared) {
            for (auto& e : entries) {
                e.parts.erase(e.parts.begin());
            }
        }
    }
    // The archive's own files stay in it.
    const auto joined = [](const Entry& e) {
        std::string out;
        for (const std::string& p : e.parts) {
            out += (out.empty() ? "" : "/") + p;
        }
        return out;
    };
    std::erase_if(entries, [&](const Entry& e) {
        return !e.folder && std::find(options.skip.begin(), options.skip.end(), joined(e)) != options.skip.end();
    });
    // The same name twice (in any case: Windows takes them as one) is refused
    // before anything is written.
    std::set<std::string> names;
    std::uint64_t files_total = 0;
    std::uint64_t bytes_total = 0;
    for (const Entry& e : entries) {
        if (e.folder) {
            continue;
        }
        std::string key = joined(e);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
        if (!names.insert(key).second) {
            result.message = "the archive has the same file twice: " + joined(e);
            return result;
        }
        ++files_total;
        bytes_total += e.size;
    }
    // Room for it all, and a tenth more.
    if (options.free_space) {
        const auto free = options.free_space(target.parent_path());
        const std::uint64_t needed = bytes_total + bytes_total / 10;
        if (free && *free < needed) {
            result.no_space = true;
            result.message = "there isn't enough free space on the drive: it needs " +
                             std::to_string((needed + (1U << 20U) - 1) >> 20U) + " MB and has " +
                             std::to_string(*free >> 20U) + " MB";
            return result;
        }
    }

    fs::create_directories(target, ec);
    if (ec) {
        result.message = "can't create " + path_to_utf8(target) + ": " + error_text(ec);
        return result;
    }
    for (const Entry& e : entries) {
        if (e.parts.empty()) {
            continue;  // the dropped top folder itself
        }
        fs::path out = target;
        for (const auto& p : e.parts) {
            out /= path_from_utf8(p);
        }
        if (e.folder) {
            fs::create_directories(out, ec);
            if (ec) {
                result.message = "can't create " + path_to_utf8(out) + ": " + error_text(ec);
                return result;
            }
            continue;
        }
        fs::create_directories(out.parent_path(), ec);
        if (ec) {
            result.message = "can't create " + path_to_utf8(out.parent_path()) + ": " + error_text(ec);
            return result;
        }
        if (fs::exists(fs::symlink_status(out, ec))) {
            result.message = "the archive names the same file twice: " + path_to_utf8(out);
            return result;
        }
        Writer w;
        w.expected = e.size;
        w.out.open(out, std::ios::binary | std::ios::trunc);
        if (!w.out) {
            result.message = "can't write " + path_to_utf8(out);
            return result;
        }
        const bool extracted = mz_zip_reader_extract_to_callback(&a.zip, e.index, write_at, &w, 0) != 0;
        w.out.close();
        if (w.too_big) {
            result.message = "an entry unpacks to more than its header says: " + path_to_utf8(out);
            return result;
        }
        if (!extracted || w.out.fail() || w.written != e.size) {
            result.message = "can't unpack " + path_to_utf8(out) + ": " +
                             (extracted ? std::string("write failed (is the disk full?)") : zip_error(a.zip));
            return result;
        }
        ++result.files;
        result.bytes += w.written;
        if (options.progress && !options.progress(result.files, files_total)) {
            result.cancelled = true;
            result.message = "cancelled";
            return result;
        }
    }
    if (result.files == 0) {
        result.message = "the archive has no files";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace floserver::core

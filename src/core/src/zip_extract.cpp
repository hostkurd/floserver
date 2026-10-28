#include "floserver/core/zip_extract.h"

#include "floserver/core/paths.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <system_error>
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
    return base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '0' &&
           base[3] <= '9';
}

bool is_safe_component(std::string_view c) {
    if (c.empty() || c.size() > kMaxComponentLength || c == "." || c == "..") {
        return false;
    }
    if (c.back() == '.' || c.back() == ' ') {
        return false;  // Windows drops them, so two names could reach one file
    }
    for (const char ch : c) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u > 0x7e) {
            return false;
        }
        if (std::string_view("<>:\"|?*").find(ch) != std::string_view::npos) {
            return false;
        }
    }
    return !is_reserved_device(c);
}

// The entry's path components, or nothing when the name is refused.
// A trailing slash (a folder entry) is allowed and dropped.
std::optional<std::vector<std::string>> split_name(std::string_view name) {
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
        if (!is_safe_component(part)) {
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

}  // namespace

fs::path safe_entry_path(std::string_view name) {
    const auto parts = split_name(name);
    if (!parts) {
        return {};
    }
    fs::path out;
    for (const auto& p : *parts) {
        out /= path_from_utf8(p);
    }
    return out;
}

ZipResult extract_zip(const fs::path& archive, const fs::path& target, const ZipLimits& limits) {
    ZipResult result;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        result.message = path_to_utf8(target) + " already exists";
        return result;
    }

    Reader reader;
    reader.in.open(archive, std::ios::binary);
    reader.size = fs::file_size(archive, ec);
    if (!reader.in || ec) {
        result.message = "can't read " + path_to_utf8(archive);
        return result;
    }
    Archive a;
    a.zip.m_pRead = read_at;
    a.zip.m_pIO_opaque = &reader;
    if (mz_zip_reader_init(&a.zip, reader.size, 0) == MZ_FALSE) {
        result.message = "the archive can't be opened: " + zip_error(a.zip);
        return result;
    }
    a.open = true;

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
        auto parts = split_name(name);
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
    if (!entries.empty()) {
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
    }
    if (result.files == 0) {
        result.message = "the archive has no files";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace floserver::core

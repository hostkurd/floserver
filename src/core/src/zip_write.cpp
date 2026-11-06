#include "floserver/core/zip_write.h"

#include "floserver/core/paths.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;

namespace {

// Level 5: most of level 9's size at a good deal of its speed.
constexpr mz_uint kLevel = 5;

std::size_t write_at(void* opaque, mz_uint64 offset, const void* buffer, std::size_t n);

}  // namespace

struct ZipWriter::State {
    mz_zip_archive zip{};
    std::ofstream out;
    std::uint64_t end = 0;  // the furthest byte written
    bool started = false;
    bool finished = false;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
    ~State() {
        if (started) {
            mz_zip_writer_end(&zip);
        }
    }
};

namespace {

std::size_t write_at(void* opaque, mz_uint64 offset, const void* buffer, std::size_t n) {
    auto* s = static_cast<ZipWriter::State*>(opaque);
    s->out.seekp(static_cast<std::streamoff>(offset));
    s->out.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(n));
    if (!s->out) {
        return 0;
    }
    s->end = std::max<std::uint64_t>(s->end, offset + n);
    return n;
}

struct Source {
    std::ifstream in;
    std::uint64_t size = 0;
};

std::size_t read_at(void* opaque, mz_uint64 offset, void* buffer, std::size_t n) {
    auto* s = static_cast<Source*>(opaque);
    if (offset >= s->size) {
        return 0;
    }
    n = static_cast<std::size_t>(std::min<std::uint64_t>(n, s->size - offset));
    s->in.clear();
    s->in.seekg(static_cast<std::streamoff>(offset));
    s->in.read(static_cast<char*>(buffer), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(s->in.gcount());
}

std::string zip_error(mz_zip_archive& zip) { return mz_zip_get_error_string(mz_zip_get_last_error(&zip)); }

}  // namespace

bool already_packed(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || name.find('/', dot) != std::string_view::npos) {
        return false;
    }
    std::string ext(name.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    static constexpr std::array<std::string_view, 24> kPacked = {
        "jpg", "jpeg", "png", "gif", "webp", "avif", "heic", "mp3", "mp4", "m4a", "mov", "webm",
        "ogg", "zip", "gz",  "tgz", "bz2", "xz",   "7z",   "rar", "woff", "woff2", "pdf", "phar"};
    return std::find(kPacked.begin(), kPacked.end(), ext) != kPacked.end();
}

ZipWriter::ZipWriter() : state_(std::make_unique<State>()) {}

ZipWriter::~ZipWriter() = default;

bool ZipWriter::open(const fs::path& file, std::string& error) {
    state_ = std::make_unique<State>();
    state_->out.open(file, std::ios::binary | std::ios::trunc);
    if (!state_->out) {
        error = "can't write " + path_to_utf8(file);
        return false;
    }
    state_->zip.m_pWrite = write_at;
    state_->zip.m_pIO_opaque = state_.get();
    if (mz_zip_writer_init_v2(&state_->zip, 0, 0) == MZ_FALSE) {
        error = "can't start the archive: " + zip_error(state_->zip);
        return false;
    }
    state_->started = true;
    return true;
}

bool ZipWriter::add_file(const std::string& name, const fs::path& source, std::string& error) {
    if (!state_->started || state_->finished) {
        error = "the archive isn't open";
        return false;
    }
    Source s;
    std::error_code ec;
    s.size = fs::file_size(source, ec);
    s.in.open(source, std::ios::binary);
    if (ec || !s.in) {
        error = "can't read " + path_to_utf8(source) + (ec ? ": " + error_text(ec) : std::string());
        return false;
    }
    const mz_uint level = already_packed(name) ? 0 : kLevel;
    if (mz_zip_writer_add_read_buf_callback(&state_->zip, name.c_str(), read_at, &s, s.size, nullptr, nullptr, 0,
                                            level, nullptr, 0, nullptr, 0) == MZ_FALSE) {
        error = "can't add " + name + ": " + zip_error(state_->zip);
        return false;
    }
    return true;
}

bool ZipWriter::add_text(const std::string& name, std::string_view text, std::string& error) {
    if (!state_->started || state_->finished) {
        error = "the archive isn't open";
        return false;
    }
    if (mz_zip_writer_add_mem_ex(&state_->zip, name.c_str(), text.data(), text.size(), nullptr, 0, kLevel, 0, 0) ==
        MZ_FALSE) {
        error = "can't add " + name + ": " + zip_error(state_->zip);
        return false;
    }
    return true;
}

bool ZipWriter::finish(std::string& error) {
    if (!state_->started || state_->finished) {
        error = "the archive isn't open";
        return false;
    }
    if (mz_zip_writer_finalize_archive(&state_->zip) == MZ_FALSE) {
        error = "can't finish the archive: " + zip_error(state_->zip);
        return false;
    }
    state_->finished = true;
    state_->out.flush();
    state_->out.close();
    if (state_->out.fail()) {
        error = "the archive couldn't be written (is the disk full?)";
        return false;
    }
    return true;
}

std::uint64_t ZipWriter::size() const { return state_->end; }

}  // namespace floserver::core

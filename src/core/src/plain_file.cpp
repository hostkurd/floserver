#include "floserver/core/plain_file.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <utility>

namespace floserver::core {

namespace {

// Reads up to `limit` bytes from `read_some` (which returns how many it got, 0 at the end, -1 on an error).
template <typename ReadSome>
std::optional<std::string> read_up_to(std::uint64_t limit, ReadSome read_some) {
    std::string out;
    std::array<char, 16384> chunk{};
    while (out.size() < limit) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), limit - out.size()));
        const long long got = read_some(chunk.data(), want);
        if (got < 0) {
            return std::nullopt;
        }
        if (got == 0) {
            break;
        }
        out.append(chunk.data(), static_cast<std::size_t>(got));
    }
    return out;
}

}  // namespace

std::optional<PlainFileText> read_plain_file(const std::filesystem::path& file, std::uint64_t max_bytes,
                                             std::uint64_t tail_bytes, PlainFileRefusal& why) {
    why = PlainFileRefusal::None;
    PlainFileText out;
#ifdef _WIN32
    const auto open = [&file](DWORD flags) {
        return CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, flags, nullptr);
    };
    struct Close {
        HANDLE h;
        explicit Close(HANDLE handle) : h(handle) {}
        Close(const Close&) = delete;
        Close& operator=(const Close&) = delete;
        Close(Close&&) = delete;
        Close& operator=(Close&&) = delete;
        ~Close() {
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
    };
    const Close first(open(FILE_FLAG_OPEN_REPARSE_POINT));
    if (first.h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        why = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? PlainFileRefusal::Missing
                                                                             : PlainFileRefusal::Unreadable;
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (GetFileInformationByHandle(first.h, &info) == FALSE ||
        GetFileInformationByHandleEx(first.h, FileAttributeTagInfo, &tag, sizeof tag) == FALSE) {
        why = PlainFileRefusal::Unreadable;
        return std::nullopt;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        why = PlainFileRefusal::NotRegular;
        return std::nullopt;
    }
    const bool reparse = (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (reparse && IsReparseTagNameSurrogate(tag.ReparseTag)) {
        why = PlainFileRefusal::Link;  // a symbolic link, a junction...
        return std::nullopt;
    }
    if (info.nNumberOfLinks != 1) {
        why = PlainFileRefusal::HardLink;
        return std::nullopt;
    }
    Close data(INVALID_HANDLE_VALUE);
    HANDLE h = first.h;
    if (reparse) {
        // OneDrive, compressed or deduplicated: the data comes through its filter.
        data.h = open(0);
        BY_HANDLE_FILE_INFORMATION again{};
        if (data.h == INVALID_HANDLE_VALUE || GetFileInformationByHandle(data.h, &again) == FALSE ||
            again.dwVolumeSerialNumber != info.dwVolumeSerialNumber || again.nFileIndexHigh != info.nFileIndexHigh ||
            again.nFileIndexLow != info.nFileIndexLow) {
            why = PlainFileRefusal::Unreadable;  // not the file just checked
            return std::nullopt;
        }
        h = data.h;
    }
    if (GetFileType(h) != FILE_TYPE_DISK) {
        why = PlainFileRefusal::NotRegular;
        return std::nullopt;
    }
    const std::uint64_t size = (std::uint64_t{info.nFileSizeHigh} << 32U) | info.nFileSizeLow;
    std::uint64_t start = 0;
    if (tail_bytes > 0) {
        start = size > tail_bytes + 1 ? size - tail_bytes - 1 : 0;
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(start);
        if (SetFilePointerEx(h, at, nullptr, FILE_BEGIN) == FALSE) {
            why = PlainFileRefusal::Unreadable;
            return std::nullopt;
        }
    } else if (size > max_bytes) {
        why = PlainFileRefusal::TooBig;
        return std::nullopt;
    }
    const std::uint64_t limit = tail_bytes > 0 ? tail_bytes + 1 : max_bytes + 1;
    auto text = read_up_to(limit, [h](char* to, std::size_t n) -> long long {
        DWORD got = 0;
        return ReadFile(h, to, static_cast<DWORD>(n), &got, nullptr) == FALSE ? -1 : static_cast<long long>(got);
    });
#else
    const int fd = ::open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (fd < 0) {
        // ELOOP: a link; FreeBSD says EMLINK for O_NOFOLLOW on one.
        why = errno == ENOENT || errno == ENOTDIR    ? PlainFileRefusal::Missing
              : errno == ELOOP || errno == EMLINK ? PlainFileRefusal::Link
                                                     : PlainFileRefusal::Unreadable;
        return std::nullopt;
    }
    struct Close {
        int fd;
        explicit Close(int file) : fd(file) {}
        Close(const Close&) = delete;
        Close& operator=(const Close&) = delete;
        Close(Close&&) = delete;
        Close& operator=(Close&&) = delete;
        ~Close() { (void)::close(fd); }
    } const closer{fd};
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        why = PlainFileRefusal::Unreadable;
        return std::nullopt;
    }
    if (!S_ISREG(st.st_mode)) {
        why = PlainFileRefusal::NotRegular;
        return std::nullopt;
    }
    if (st.st_nlink != 1) {
        why = PlainFileRefusal::HardLink;
        return std::nullopt;
    }
    const auto size = static_cast<std::uint64_t>(st.st_size);
    std::uint64_t start = 0;
    if (tail_bytes > 0) {
        start = size > tail_bytes + 1 ? size - tail_bytes - 1 : 0;
        if (::lseek(fd, static_cast<off_t>(start), SEEK_SET) < 0) {
            why = PlainFileRefusal::Unreadable;
            return std::nullopt;
        }
    } else if (size > max_bytes) {
        why = PlainFileRefusal::TooBig;
        return std::nullopt;
    }
    const std::uint64_t limit = tail_bytes > 0 ? tail_bytes + 1 : max_bytes + 1;
    auto text = read_up_to(limit, [fd](char* to, std::size_t n) -> long long {
        const ssize_t got = ::read(fd, to, n);
        return got < 0 ? -1 : static_cast<long long>(got);
    });
#endif
    if (!text) {
        why = PlainFileRefusal::Unreadable;
        return std::nullopt;
    }
    if (tail_bytes == 0 && text->size() > max_bytes) {
        why = PlainFileRefusal::TooBig;  // it grew while it was read
        return std::nullopt;
    }
    out.text = std::move(*text);
    out.from_start = start == 0;
    return out;
}

std::string plain_file_refusal_text(PlainFileRefusal why, std::uint64_t max_bytes) {
    switch (why) {
        case PlainFileRefusal::None:
            return {};
        case PlainFileRefusal::Missing:
            return "isn't there";
        case PlainFileRefusal::Link:
            return "is a link; FloServer reads only a plain file (copy the file into the project instead)";
        case PlainFileRefusal::HardLink:
            return "has another name (a hard link); FloServer reads only a plain file";
        case PlainFileRefusal::NotRegular:
            return "isn't a plain file";
        case PlainFileRefusal::TooBig:
            return "is larger than " + std::to_string(max_bytes / 1024) + " KB";
        case PlainFileRefusal::Unreadable:
            break;
    }
    return "can't be read";
}

}  // namespace floserver::core

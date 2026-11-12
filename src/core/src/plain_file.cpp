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
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        why = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? PlainFileRefusal::Missing
                                                                             : PlainFileRefusal::Unreadable;
        return std::nullopt;
    }
    struct Close {
        HANDLE h;
        explicit Close(HANDLE handle) : h(handle) {}
        Close(const Close&) = delete;
        Close& operator=(const Close&) = delete;
        Close(Close&&) = delete;
        Close& operator=(Close&&) = delete;
        ~Close() { CloseHandle(h); }
    } const closer{h};
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h, &info) == FALSE) {
        why = PlainFileRefusal::Unreadable;
        return std::nullopt;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || info.nNumberOfLinks != 1) {
        why = PlainFileRefusal::Link;
        return std::nullopt;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || GetFileType(h) != FILE_TYPE_DISK) {
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
        why = errno == ENOENT || errno == ENOTDIR ? PlainFileRefusal::Missing
              : errno == ELOOP                    ? PlainFileRefusal::Link
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
        why = PlainFileRefusal::Link;
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
            return "it isn't there";
        case PlainFileRefusal::Link:
            return "it's a link (FloServer reads only a plain file)";
        case PlainFileRefusal::NotRegular:
            return "it isn't a plain file";
        case PlainFileRefusal::TooBig:
            return "it's larger than " + std::to_string(max_bytes / 1024) + " KB";
        case PlainFileRefusal::Unreadable:
            break;
    }
    return "it can't be read";
}

}  // namespace floserver::core

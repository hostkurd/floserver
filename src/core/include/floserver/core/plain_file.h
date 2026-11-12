#pragma once

// Reading a file only as itself (D32 review): a regular file that is no
// symbolic link or other reparse point and has one name only (no hard link
// to it). The checks are made on the open handle, not the path, so the file
// can't be swapped for a link between the check and the read.
//
//   POSIX    open(O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC), then fstat:
//            S_ISREG and st_nlink == 1
//   Windows  CreateFileW(FILE_FLAG_OPEN_REPARSE_POINT), then
//            GetFileInformationByHandle: no FILE_ATTRIBUTE_REPARSE_POINT or
//            _DIRECTORY, nNumberOfLinks == 1
//
// Used for each project's floserver.json (the service and `flo mcp`) and the
// logs `flo mcp` reads.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace floserver::core {

enum class PlainFileRefusal : std::uint8_t { None, Missing, Link, NotRegular, TooBig, Unreadable };

struct PlainFileText {
    std::string text;
    bool from_start = true;  // false: a tail that began after the file's start
};

// The whole of `file` (at most `max_bytes`: a larger one is TooBig), or with
// `tail_bytes` > 0 only its end: the last `tail_bytes` bytes and the one
// before them. Nothing, with `why`, when it isn't a plain file.
[[nodiscard]] std::optional<PlainFileText> read_plain_file(const std::filesystem::path& file, std::uint64_t max_bytes,
                                                           std::uint64_t tail_bytes, PlainFileRefusal& why);

// Why, in plain English: "it's a link", "it's larger than 64 KB"...
[[nodiscard]] std::string plain_file_refusal_text(PlainFileRefusal why, std::uint64_t max_bytes);

}  // namespace floserver::core

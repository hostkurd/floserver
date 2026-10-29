#pragma once

// Unpacks a runtime's .tar.gz (D5), with the same rules as a .zip
// (zip_extract.h): the archive already matched the signed list's SHA-256,
// and even so it can only write plain files and folders inside the target.
//   - every name is checked, and the sizes added up, before anything is
//     written (the archive is read twice);
//   - names follow safe_entry_path(): no "..", no absolute or drive paths,
//     no ':' or device names, printable ASCII only. Long names from the
//     pax (`path=`) and GNU (`././@LongLink`) extensions count;
//   - symbolic and hard links, devices and FIFOs are refused;
//   - the gzip CRC-32 and length are checked, and only one gzip member;
//   - every header's checksum is checked.
// A single top folder shared by every entry (python/...) is dropped. On
// POSIX a file the archive marks executable is made executable.

#include "floserver/core/zip_extract.h"

#include <filesystem>

namespace floserver::core {

[[nodiscard]] ZipResult extract_tar_gz(const std::filesystem::path& archive, const std::filesystem::path& target,
                                       const ZipLimits& limits = {});

}  // namespace floserver::core

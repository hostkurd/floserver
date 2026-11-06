#pragma once

// Writes a .zip (D22: a project's backup) with miniz, through FloServer's
// own file I/O so paths in any language work on Windows. Names are UTF-8
// (the zip's language flag is set), files are read as they're added, and an
// archive over 4 GB or 65,535 files switches to zip64 by itself. Already
// packed files (images, archives, fonts) are stored rather than packed again.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace floserver::core {

class ZipWriter {
public:
    ZipWriter();
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
    ZipWriter(ZipWriter&&) = delete;
    ZipWriter& operator=(ZipWriter&&) = delete;

    // Starts a new archive at `file` (replacing one that's there).
    [[nodiscard]] bool open(const std::filesystem::path& file, std::string& error);
    // `name` ("app/index.php", '/' between folders) holds what `source`
    // holds now: a file that grows meanwhile is cut at the size it had when
    // it was added, one that shrinks ends where it ends.
    [[nodiscard]] bool add_file(const std::string& name, const std::filesystem::path& source, std::string& error);
    [[nodiscard]] bool add_text(const std::string& name, std::string_view text, std::string& error);
    // Writes the file list; the archive is complete only after this.
    [[nodiscard]] bool finish(std::string& error);
    // Bytes written so far.
    [[nodiscard]] std::uint64_t size() const;

    struct State;  // miniz's callbacks reach it

private:
    std::unique_ptr<State> state_;
};

// Whether a file of this name is packed already (.jpg, .zip, .woff2...).
[[nodiscard]] bool already_packed(std::string_view name);

}  // namespace floserver::core

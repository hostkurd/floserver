#include "floserver/core/data_folder.h"

#include "floserver/core/paths.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// The deadline is looked at once per this many entries.
constexpr unsigned kCheckEvery = 64;

// A symbolic link, or on Windows any reparse point (a junction, a mount
// point): never followed, never read through.
bool is_link(const fs::path& p, const fs::file_status& st) {
    if (fs::is_symlink(st)) {
        return true;
    }
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)p;
    return false;
#endif
}

// Removes one file, empty folder or link (a link to a folder goes as the
// link: RemoveDirectory on a junction leaves what it points to alone).
bool remove_one(const fs::path& p, bool folder) {
#ifdef _WIN32
    const auto drop = [&] { return folder ? RemoveDirectoryW(p.c_str()) != FALSE : DeleteFileW(p.c_str()) != FALSE; };
    if (drop()) {
        return true;
    }
    // A read-only file (some archives keep the flag) needs it cleared first.
    const DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0 &&
        SetFileAttributesW(p.c_str(), attrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)) != FALSE) {
        return drop();
    }
    return false;
#else
    (void)folder;
    std::error_code ec;
    return fs::remove(p, ec) && !ec;
#endif
}

// Deletes `root` and what's in it, never through a link. Adds the bytes of
// the files deleted to `freed`; false when something stayed (in use).
bool remove_tree(const fs::path& root, std::uintmax_t& freed) {
    struct Entry {
        fs::path path;
        bool folder = false;
        std::uintmax_t bytes = 0;
    };
    // Each folder before what's in it; deleted the other way round.
    std::vector<Entry> order;
    std::vector<fs::path> todo{root};
    bool all = true;
    while (!todo.empty()) {
        const fs::path p = std::move(todo.back());
        todo.pop_back();
        std::error_code ec;
        const fs::file_status st = fs::symlink_status(p, ec);
        if (ec || !fs::exists(st)) {
            all = false;
            continue;
        }
        if (is_link(p, st)) {
            // The link itself goes; a folder link (a junction) as a folder,
            // which leaves what it points to alone.
#ifdef _WIN32
            const DWORD attrs = GetFileAttributesW(p.c_str());
            order.push_back({p, attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0, 0});
#else
            order.push_back({p, false, 0});
#endif
            continue;
        }
        if (fs::is_directory(st)) {
            order.push_back({p, true, 0});
            for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
                todo.push_back(it->path());
            }
            all = all && !ec;
            continue;
        }
        const std::uintmax_t size = fs::file_size(p, ec);
        order.push_back({p, false, ec ? 0 : size});
    }
    for (const Entry& entry : std::ranges::reverse_view(order)) {
        if (remove_one(entry.path, entry.folder)) {
            freed += entry.bytes;
        } else {
            all = false;
        }
    }
    return all;
}

bool starts_with(const fs::path& name, std::string_view prefix) {
    return path_to_utf8(name).starts_with(prefix);
}

std::vector<fs::path> log_folders(const fs::path& data) {
    return {data / "web" / "logs", data / "web" / "logs" / "sites", data / "web" / "apps", data / "cli" / "php"};
}

// A regular file (not a link) and its size.
bool regular_file(const fs::path& p, std::uintmax_t& size) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || !fs::is_regular_file(st) || is_link(p, st)) {
        return false;
    }
    size = fs::file_size(p, ec);
    return !ec;
}

}  // namespace

DiskUsage disk_usage(const fs::path& dir, Clock::time_point deadline) {
    DiskUsage usage;
    std::vector<fs::path> folders{dir};
    unsigned seen = 0;
    while (!folders.empty()) {
        const fs::path folder = std::move(folders.back());
        folders.pop_back();
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (++seen % kCheckEvery == 0 && Clock::now() >= deadline) {
                usage.complete = false;
                return usage;
            }
            std::error_code entry_ec;
            const fs::file_status st = it->symlink_status(entry_ec);
            if (entry_ec || fs::is_symlink(st)) {
                continue;
            }
            if (fs::is_directory(st)) {
                if (!is_link(it->path(), st)) {
                    folders.push_back(it->path());
                }
                continue;
            }
            if (fs::is_regular_file(st)) {
                const std::uintmax_t size = it->file_size(entry_ec);
                if (!entry_ec) {
                    usage.bytes += size;
                    ++usage.files;
                }
            }
        }
    }
    return usage;
}

bool is_log_file(const fs::path& data, const fs::path& path) {
    const fs::path p = path.lexically_normal();
    if (p.extension() != ".log" || starts_with(p.filename(), ".")) {
        return false;
    }
    const fs::path parent = p.parent_path();
    for (const fs::path& folder : log_folders(data)) {
        if (parent == folder.lexically_normal()) {
            return true;
        }
    }
    return false;
}

bool is_leftover(const fs::path& data, const fs::path& path) {
    const fs::path p = path.lexically_normal();
    const fs::path parent = p.parent_path();
    const fs::path name = p.filename();
    if (parent == (data / "downloads").lexically_normal()) {
        return (p.extension() == ".part" || p.extension() == ".download") && !starts_with(name, ".");
    }
    if (parent.parent_path() == runtimes_dir(data).lexically_normal()) {
        return starts_with(name, ".staging-") || starts_with(name, ".removing-");
    }
    if (parent == (data / "templates").lexically_normal()) {
        return starts_with(name, "job-");
    }
    return false;
}

std::vector<CleanItem> log_files(const fs::path& data) {
    std::vector<CleanItem> out;
    for (const fs::path& folder : log_folders(data)) {
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            std::uintmax_t size = 0;
            if (is_log_file(data, it->path()) && regular_file(it->path(), size) && size > 0) {
                out.push_back({it->path(), size, false});
            }
        }
    }
    return out;
}

std::vector<CleanItem> leftovers(const fs::path& data, bool runtimes_busy, bool templates_busy,
                                 Clock::time_point deadline) {
    std::vector<CleanItem> out;
    std::error_code ec;
    const auto add_folder = [&](const fs::path& p) {
        std::error_code st_ec;
        const fs::file_status st = fs::symlink_status(p, st_ec);
        if (st_ec || !is_leftover(data, p)) {
            return;
        }
        if (fs::is_directory(st) && !is_link(p, st)) {
            out.push_back({p, disk_usage(p, deadline).bytes, true});
        } else if (fs::exists(st)) {
            std::uintmax_t size = 0;
            out.push_back({p, regular_file(p, size) ? size : 0, fs::is_directory(st)});
        }
    };
    if (!runtimes_busy) {
        for (fs::directory_iterator it(downloads_dir(data), ec), end; !ec && it != end; it.increment(ec)) {
            std::uintmax_t size = 0;
            if (is_leftover(data, it->path()) && regular_file(it->path(), size)) {
                out.push_back({it->path(), size, false});
            }
        }
        ec.clear();
        for (fs::directory_iterator names(runtimes_dir(data), ec), end; !ec && names != end; names.increment(ec)) {
            std::error_code inner;
            const fs::file_status st = names->symlink_status(inner);
            if (inner || !fs::is_directory(st) || is_link(names->path(), st)) {
                continue;
            }
            for (fs::directory_iterator v(names->path(), inner); !inner && v != end; v.increment(inner)) {
                add_folder(v->path());
            }
        }
    }
    if (!templates_busy) {
        ec.clear();
        for (fs::directory_iterator it(data / "templates", ec), end; !ec && it != end; it.increment(ec)) {
            add_folder(it->path());
        }
    }
    return out;
}

std::uintmax_t total_bytes(const std::vector<CleanItem>& items) {
    std::uintmax_t total = 0;
    for (const CleanItem& item : items) {
        total += item.bytes;
    }
    return total;
}

CleanResult empty_logs(const fs::path& data, const std::vector<CleanItem>& logs) {
    CleanResult result;
    for (const CleanItem& item : logs) {
        std::uintmax_t size = 0;
        if (!is_log_file(data, item.path) || !regular_file(item.path, size)) {
            ++result.skipped;
            continue;
        }
        // Emptied, not deleted: nginx, PHP and the app servers append, so
        // their open handle goes on at the new end. A server that holds its
        // log without sharing it makes this fail: that log is skipped.
        std::error_code ec;
        fs::resize_file(item.path, 0, ec);
        if (ec) {
            ++result.skipped;
            continue;
        }
        result.freed += size;
        ++result.done;
    }
    return result;
}

CleanResult remove_leftovers(const fs::path& data, const std::vector<CleanItem>& items) {
    CleanResult result;
    for (const CleanItem& item : items) {
        if (!is_leftover(data, item.path)) {
            ++result.skipped;
            continue;
        }
        if (remove_tree(item.path, result.freed)) {
            ++result.done;
        } else {
            ++result.skipped;  // a file in use stays; what could go went
        }
    }
    return result;
}

}  // namespace floserver::core

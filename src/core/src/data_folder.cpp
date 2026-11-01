#include "floserver/core/data_folder.h"

#include "floserver/core/paths.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using FileOp = std::function<void(const fs::path&, std::error_code&)>;

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

void real_remove(const fs::path& p, std::error_code& ec) {
    ec.clear();
    if (!remove_one(p, false)) {
        ec = std::make_error_code(std::errc::permission_denied);
    }
}

void real_empty(const fs::path& p, std::error_code& ec) { fs::resize_file(p, 0, ec); }

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

bool starts_with(const fs::path& name, std::string_view prefix) {
    return path_to_utf8(name).starts_with(prefix);
}

// The places Clear old logs looks in (their own files only).
std::vector<fs::path> log_folders(const fs::path& data) {
    return {data / "web" / "logs", data / "web" / "apps", data / "cli" / "php"};
}

// The folder's text as compared: '/' separators, and on Windows any case.
std::string folded(const fs::path& p) {
    const std::u8string u = p.lexically_normal().generic_u8string();
    std::string s(u.begin(), u.end());
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
#ifdef _WIN32
    s = lower_ascii(s);  // Windows' file names ignore case
#endif
    return s;
}

// `inner` is `outer` or inside it.
bool within(const fs::path& inner, const fs::path& outer) {
    const std::string i = folded(inner);
    const std::string o = folded(outer);
    return !o.empty() && (i == o || (i.size() > o.size() && i.starts_with(o) && (o.back() == '/' || i[o.size()] == '/')));
}

// `p` is, holds or lies inside one of the protected folders.
bool touches_protected(const fs::path& p, const CleanOptions& options) {
    return std::ranges::any_of(options.protected_dirs, [&](const fs::path& dir) {
        return !dir.empty() && (within(p, dir) || within(dir, p));
    });
}

std::string relative_text(const fs::path& p, const fs::path& data) {
    const fs::path rel = p.lexically_normal().lexically_relative(data.lexically_normal());
    const std::u8string u = (rel.empty() ? p : rel).generic_u8string();
    return {u.begin(), u.end()};
}

void skip(std::vector<CleanSkip>* out, const fs::path& data, const fs::path& p, std::string_view why) {
    if (out == nullptr) {
        return;
    }
    std::string reason;
    if (why == "in_use") {
        reason = "in use by another program";
    } else if (why == "link") {
        reason = "it's a link; FloServer doesn't follow links";
    } else if (why == "protected") {
        reason = "it's a project's folder, the projects folder or the www folder";
    } else if (why == "unreadable") {
        reason = "it couldn't be read";
    } else {
        reason = "it isn't something the clean-up removes";
    }
    const std::string path = relative_text(p, data);
    // One line per path: a place met twice (listed, then cleaned) is said once.
    if (std::ranges::any_of(*out, [&](const CleanSkip& s) { return s.path == path; })) {
        return;
    }
    out->push_back({path, std::string(why), std::move(reason)});
}

// Whether the place `dir` can be looked in: every part a plain folder (a link
// is reported as such) and nothing protected about it (reported too).
bool usable_place(const fs::path& data, const fs::path& dir, const CleanOptions& options,
                  std::vector<CleanSkip>* skipped) {
    std::error_code ec;
    fs::path linked;
    if (!plain_path(data, dir, &linked)) {
        if (!linked.empty()) {
            skip(skipped, data, linked, "link");
        }
        return false;
    }
    if (!fs::is_directory(fs::symlink_status(dir, ec)) || ec) {
        return false;  // not there: nothing to clean
    }
    // A place inside a protected folder isn't gone into; one that holds one
    // is, and each item is checked (touches_protected) on its own.
    if (std::ranges::any_of(options.protected_dirs,
                            [&](const fs::path& p) { return !p.empty() && within(dir, p); })) {
        skip(skipped, data, dir, "protected");
        return false;
    }
    return true;
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

// Deletes `root` and what's in it, never through a link (links go as links).
// Adds the bytes of the files deleted to `freed`; false when something
// stayed (in use).
bool remove_tree(const fs::path& root, std::uintmax_t& freed, const FileOp& remove_file) {
    struct Entry {
        fs::path path;
        bool folder = false;
        std::uintmax_t bytes = 0;
        bool link = false;
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
            order.push_back({p, attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0, 0, true});
#else
            order.push_back({p, false, 0, true});
#endif
            continue;
        }
        if (fs::is_directory(st)) {
            order.push_back({p, true, 0, false});
            for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
                todo.push_back(it->path());
            }
            all = all && !ec;
            continue;
        }
        const std::uintmax_t size = fs::file_size(p, ec);
        order.push_back({p, false, ec ? 0 : size, false});
    }
    for (const Entry& entry : std::ranges::reverse_view(order)) {
        bool gone = false;
        if (entry.folder || entry.link) {
            gone = remove_one(entry.path, entry.folder);
        } else {
            std::error_code ec;
            remove_file(entry.path, ec);
            gone = !ec;
        }
        if (gone) {
            freed += entry.bytes;
        } else {
            all = false;
        }
    }
    return all;
}

// The item, if it's a log Clear old logs may touch now; why not, otherwise.
// `why` stays empty for one that simply isn't due (a current log written to
// within the day, an empty one).
std::optional<CleanItem> log_item(const fs::path& data, const fs::path& p, const CleanOptions& options,
                                  std::string& why) {
    why.clear();
    if (!is_log_file(data, p)) {
        why = "not_allowed";
        return std::nullopt;
    }
    if (touches_protected(p, options)) {
        why = "protected";
        return std::nullopt;
    }
    fs::path linked;
    if (!plain_path(data, p, &linked)) {
        why = linked.empty() ? "not_allowed" : "link";
        return std::nullopt;
    }
    std::uintmax_t size = 0;
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || !fs::exists(st)) {
        return std::nullopt;  // gone already
    }
    if (!regular_file(p, size)) {
        why = "not_allowed";
        return std::nullopt;
    }
    if (size == 0) {
        return std::nullopt;
    }
    const bool rotated = is_rotated_log_name(path_to_utf8(p.filename()));
    if (!rotated) {
        const auto written = fs::last_write_time(p, ec);
        if (ec || options.now - written < kLogQuietFor) {
            return std::nullopt;  // written to within the day: something may still be using it
        }
    }
    return CleanItem{p, size, false, rotated};
}

}  // namespace

bool plain_path(const fs::path& data, const fs::path& path, fs::path* linked) {
    const fs::path rel = path.lexically_normal().lexically_relative(data.lexically_normal());
    if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") {
        return false;
    }
    fs::path at = data;
    for (const fs::path& part : rel) {
        if (part == ".") {
            continue;
        }
        at /= part;
        std::error_code ec;
        const fs::file_status st = fs::symlink_status(at, ec);
        if (ec || !fs::exists(st)) {
            return true;  // missing from here on: nothing below it to follow
        }
        if (is_link(at, st)) {
            if (linked != nullptr) {
                *linked = at;
            }
            return false;
        }
    }
    return true;
}

bool is_rotated_log_name(std::string_view name) {
    const std::string n = lower_ascii(name);
    const std::size_t at = n.find(".log.");
    if (at == std::string::npos || at == 0) {
        return false;
    }
    std::string rest = n.substr(at + 5);
    for (const std::string_view packed : {".gz", ".zip", ".bz2", ".xz"}) {
        if (rest.ends_with(packed)) {
            rest.resize(rest.size() - packed.size());
            break;
        }
    }
    if (rest == "gz" || rest == "zip" || rest == "bz2" || rest == "xz") {
        return true;  // "x.log.gz"
    }
    if (rest == "old" || rest == "bak") {
        return true;
    }
    // "x.log.1", "x.log.20261101", "x.log.2026-11-01"
    return !rest.empty() && rest.size() <= 10 && std::isdigit(static_cast<unsigned char>(rest.front())) != 0 &&
           std::ranges::all_of(rest, [](char c) { return (c >= '0' && c <= '9') || c == '-'; });
}

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

// See the rule in data_folder.h: older copies in the log places are deleted,
// a current *.log emptied once quiet for a day; never a project's request log
// (web/logs/sites/ isn't a place), a pool's *.access.log, MariaDB's files
// (not in a place), anything in a sub-folder.
bool is_log_file(const fs::path& data, const fs::path& path) {
    const fs::path p = path.lexically_normal();
    const std::string name = lower_ascii(path_to_utf8(p.filename()));
    if (name.starts_with(".") || name.find(".access.log") != std::string::npos) {
        return false;
    }
    if (!name.ends_with(".log") && !is_rotated_log_name(name)) {
        return false;
    }
    const fs::path parent = p.parent_path();
    return std::ranges::any_of(log_folders(data),
                               [&](const fs::path& folder) { return parent == folder.lexically_normal(); });
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

std::vector<CleanItem> log_files(const fs::path& data, const CleanOptions& options, std::vector<CleanSkip>* skipped) {
    std::vector<CleanItem> out;
    for (const fs::path& folder : log_folders(data)) {
        if (!usable_place(data, folder, options, skipped)) {
            continue;
        }
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (!is_log_file(data, it->path())) {
                continue;  // not a log here: not looked at
            }
            std::string why;
            if (auto item = log_item(data, it->path(), options, why)) {
                out.push_back(std::move(*item));
            } else if (why == "link" || why == "protected") {
                skip(skipped, data, it->path(), why);
            }
        }
    }
    return out;
}

std::vector<CleanItem> leftovers(const fs::path& data, bool runtimes_busy, bool templates_busy,
                                 Clock::time_point deadline, const CleanOptions& options,
                                 std::vector<CleanSkip>* skipped) {
    std::vector<CleanItem> out;
    std::error_code ec;
    const auto add_folder = [&](const fs::path& p) {
        std::error_code st_ec;
        const fs::file_status st = fs::symlink_status(p, st_ec);
        if (st_ec || !is_leftover(data, p)) {
            return;
        }
        if (touches_protected(p, options)) {
            skip(skipped, data, p, "protected");
            return;
        }
        if (fs::is_directory(st) && !is_link(p, st)) {
            out.push_back({p, disk_usage(p, deadline).bytes, true, true});
        } else if (fs::exists(st)) {
            // A file, or a link (removed as the link, what it points to left alone).
            std::uintmax_t size = 0;
            out.push_back({p, regular_file(p, size) ? size : 0, fs::is_directory(st), true});
        }
    };
    if (!runtimes_busy) {
        const fs::path downloads = downloads_dir(data);
        if (usable_place(data, downloads, options, skipped)) {
            for (fs::directory_iterator it(downloads, ec), end; !ec && it != end; it.increment(ec)) {
                std::uintmax_t size = 0;
                if (!is_leftover(data, it->path())) {
                    continue;
                }
                if (touches_protected(it->path(), options)) {
                    skip(skipped, data, it->path(), "protected");
                } else if (regular_file(it->path(), size)) {
                    out.push_back({it->path(), size, false, true});
                }
            }
        }
        ec.clear();
        const fs::path runtimes = runtimes_dir(data);
        if (usable_place(data, runtimes, options, skipped)) {
            for (fs::directory_iterator names(runtimes, ec), end; !ec && names != end; names.increment(ec)) {
                std::error_code inner;
                const fs::file_status st = names->symlink_status(inner);
                if (inner) {
                    continue;
                }
                if (is_link(names->path(), st)) {
                    skip(skipped, data, names->path(), "link");
                    continue;
                }
                if (!fs::is_directory(st)) {
                    continue;
                }
                for (fs::directory_iterator v(names->path(), inner); !inner && v != end; v.increment(inner)) {
                    add_folder(v->path());
                }
            }
        }
    }
    if (!templates_busy) {
        ec.clear();
        const fs::path templates = data / "templates";
        if (usable_place(data, templates, options, skipped)) {
            for (fs::directory_iterator it(templates, ec), end; !ec && it != end; it.increment(ec)) {
                add_folder(it->path());
            }
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

CleanResult empty_logs(const fs::path& data, const std::vector<CleanItem>& logs, const CleanOptions& options) {
    const FileOp remove_file = options.remove_file ? options.remove_file : FileOp(real_remove);
    const FileOp empty_file = options.empty_file ? options.empty_file : FileOp(real_empty);
    CleanResult result;
    for (const CleanItem& item : logs) {
        // Checked again whatever the list says, and deleted or emptied by its
        // name now, not by the list's word.
        std::string why;
        const auto now = log_item(data, item.path, options, why);
        if (!now) {
            if (!why.empty()) {
                skip(&result.skipped_items, data, item.path, why);
            }
            continue;
        }
        // An older copy is deleted. A current log is emptied, not deleted:
        // nginx, PHP and the app servers append, so an open handle goes on
        // at the new end. A server that holds its log without sharing it
        // makes either fail: that log is skipped and named.
        std::error_code ec;
        (now->remove ? remove_file : empty_file)(item.path, ec);
        if (ec) {
            skip(&result.skipped_items, data, item.path, "in_use");
            continue;
        }
        result.freed += now->bytes;
        ++result.done;
    }
    result.skipped = result.skipped_items.size();
    return result;
}

CleanResult remove_leftovers(const fs::path& data, const std::vector<CleanItem>& items, const CleanOptions& options) {
    const FileOp remove_file = options.remove_file ? options.remove_file : FileOp(real_remove);
    CleanResult result;
    for (const CleanItem& item : items) {
        if (!is_leftover(data, item.path)) {
            skip(&result.skipped_items, data, item.path, "not_allowed");
            continue;
        }
        if (touches_protected(item.path, options)) {
            skip(&result.skipped_items, data, item.path, "protected");
            continue;
        }
        // The folders above it are plain ones: never through a linked
        // downloads, runtimes, runtimes/<name> or templates. The leftover
        // itself may be a link: remove_tree takes it away as the link.
        fs::path linked;
        if (!plain_path(data, item.path.parent_path(), &linked)) {
            skip(&result.skipped_items, data, linked.empty() ? item.path : linked, linked.empty() ? "not_allowed" : "link");
            continue;
        }
        if (remove_tree(item.path, result.freed, remove_file)) {
            ++result.done;
        } else {
            skip(&result.skipped_items, data, item.path, "in_use");  // a file in use stays; what could go went
        }
    }
    result.skipped = result.skipped_items.size();
    return result;
}

}  // namespace floserver::core

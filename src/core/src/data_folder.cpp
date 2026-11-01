#include "floserver/core/data_folder.h"

#include "floserver/core/paths.h"
#include "floserver/core/project_registry.h"  // real_folder

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <algorithm>
#include <cctype>
#include <optional>
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

class CleanCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "floserver.clean"; }
    [[nodiscard]] std::string message(int code) const override {
        switch (code) {
            case kCleanIsLink:
                return "it's a link";
            case kCleanHardLinked:
                return "it has more than one hard link";
            case kCleanWritten:
                return "it was written to within the day";
            case kCleanGone:
                return "it isn't there any more";
            default:
                return "clean-up error";
        }
    }
};

std::error_code clean_error(int code) { return {code, clean_category()}; }

// The real file operations: the file is opened without following a link,
// checked again on that handle, and changed through it (see data_folder.h).
// `quiet_before` (a current log): refused when written to after it.
#ifdef _WIN32
std::error_code last_error() { return {static_cast<int>(GetLastError()), std::system_category()}; }

// FILETIME (100 ns since 1601) as file_clock's time, as std::filesystem
// reads it on Windows (the same epoch and unit there).
fs::file_time_type file_time(const FILETIME& t) {
    const auto ticks = static_cast<std::int64_t>((std::uint64_t{t.dwHighDateTime} << 32U) | t.dwLowDateTime);
    return fs::file_time_type(fs::file_time_type::duration(ticks));
}

void handle_op(const fs::path& p, bool remove, const std::optional<fs::file_time_type>& quiet_before,
               std::error_code& ec) {
    ec.clear();
    const DWORD access = remove ? (DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES)
                                : (GENERIC_WRITE | FILE_READ_ATTRIBUTES);
    HANDLE h = CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        ec = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? clean_error(kCleanGone) : last_error();
        return;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h, &info) == FALSE) {
        ec = last_error();
    } else if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        ec = clean_error(kCleanIsLink);
    } else if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        ec = std::make_error_code(std::errc::is_a_directory);
    } else if (!remove && info.nNumberOfLinks > 1) {
        ec = clean_error(kCleanHardLinked);
    } else if (quiet_before && file_time(info.ftLastWriteTime) > *quiet_before) {
        ec = clean_error(kCleanWritten);
    } else if (remove) {
        // A read-only file (some archives keep the flag) needs it cleared first.
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
            FILE_BASIC_INFO basic{};
            basic.FileAttributes = (info.dwFileAttributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)) |
                                   FILE_ATTRIBUTE_NORMAL;
            (void)SetFileInformationByHandle(h, FileBasicInfo, &basic, sizeof basic);
        }
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        if (SetFileInformationByHandle(h, FileDispositionInfo, &disposition, sizeof disposition) == FALSE) {
            ec = last_error();
        }
    } else {
        FILE_END_OF_FILE_INFO end{};
        end.EndOfFile.QuadPart = 0;
        if (SetFileInformationByHandle(h, FileEndOfFileInfo, &end, sizeof end) == FALSE) {
            ec = last_error();
        }
    }
    CloseHandle(h);
}
#else
void handle_op(const fs::path& p, bool remove, const std::optional<fs::file_time_type>& quiet_before,
               std::error_code& ec) {
    ec.clear();
    const int fd = ::open(p.c_str(), (remove ? O_RDONLY : O_WRONLY) | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        ec = errno == ENOENT ? clean_error(kCleanGone)
             : errno == ELOOP ? clean_error(kCleanIsLink)
                              : std::error_code(errno, std::generic_category());
        return;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ec = {errno, std::generic_category()};
    } else if (!S_ISREG(st.st_mode)) {
        ec = clean_error(kCleanIsLink);
    } else if (!remove && st.st_nlink > 1) {
        ec = clean_error(kCleanHardLinked);
    } else if (quiet_before) {
        // fstat's mtime as file_clock's time, through the system clock.
        const auto since = std::chrono::seconds(st.st_mtim.tv_sec) + std::chrono::nanoseconds(st.st_mtim.tv_nsec);
        const std::chrono::system_clock::time_point written(
            std::chrono::duration_cast<std::chrono::system_clock::duration>(since));
        if (std::chrono::clock_cast<std::chrono::file_clock>(written) > *quiet_before) {
            ec = clean_error(kCleanWritten);
        }
    }
    if (!ec && remove) {
        // Deleted by name: still the file just opened (same device and
        // inode, by an lstat that doesn't follow a link).
        struct stat again {};
        if (::lstat(p.c_str(), &again) != 0 || again.st_dev != st.st_dev || again.st_ino != st.st_ino) {
            ec = clean_error(kCleanIsLink);
        } else if (::unlink(p.c_str()) != 0) {
            ec = {errno, std::generic_category()};
        }
    } else if (!ec && ::ftruncate(fd, 0) != 0) {
        ec = {errno, std::generic_category()};
    }
    ::close(fd);
}
#endif

void real_remove(const fs::path& p, std::error_code& ec) { handle_op(p, true, std::nullopt, ec); }

void real_empty(const fs::path& p, std::error_code& ec) { handle_op(p, false, std::nullopt, ec); }

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

// `dir` as given and as the file system names it (long names, real case),
// when it's there: an 8.3 short data folder path still matches a project's.
std::vector<fs::path> spellings(const fs::path& dir) {
    std::vector<fs::path> out{dir};
    std::string ignored;
    if (const auto real = real_folder(dir, ignored); real && folded(*real) != folded(dir)) {
        out.push_back(*real);
    }
    return out;
}

// The spelling of `p` that compares with the protected folders' real names:
// the data folder's part of it as the file system names it.
fs::path real_spelling(const fs::path& p, const fs::path& data, const fs::path& real_data) {
    if (real_data.empty() || !within(p, data)) {
        return p;
    }
    const fs::path rel = p.lexically_normal().lexically_relative(data.lexically_normal());
    return rel.empty() || rel == "." ? real_data : real_data / rel;
}

struct Protection {
    bool hit = false;
    std::string inside;  // the kind of the folder `p` lies inside, when named
};

// `p` is, holds or lies inside one of the protected folders (with `holds`
// false: only is or lies inside). Compared as given and in real names.
Protection protection(const fs::path& p, const fs::path& data, const CleanOptions& options, bool holds = true) {
    std::string ignored;
    const auto real_data = real_folder(data, ignored);
    const fs::path real_p = real_spelling(p, data, real_data ? *real_data : fs::path());
    for (std::size_t i = 0; i < options.protected_dirs.size(); ++i) {
        const fs::path& dir = options.protected_dirs[i];
        if (dir.empty()) {
            continue;
        }
        for (const fs::path& d : spellings(dir)) {
            for (const fs::path& q : {p, real_p}) {
                if (within(q, d)) {
                    return {true, i < options.protected_kinds.size() ? options.protected_kinds[i] : std::string()};
                }
                if (holds && within(d, q)) {
                    return {true, {}};
                }
            }
        }
    }
    return {};
}

// The number of hard links `p` has (1 for a plain file); 0 when it can't tell.
std::uintmax_t link_count(const fs::path& p) {
#ifdef _WIN32
    HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info) != FALSE;
    CloseHandle(h);
    return ok ? info.nNumberOfLinks : 0;
#else
    struct stat st {};
    return ::lstat(p.c_str(), &st) == 0 ? static_cast<std::uintmax_t>(st.st_nlink) : 0;
#endif
}

std::string relative_text(const fs::path& p, const fs::path& data) {
    const fs::path rel = p.lexically_normal().lexically_relative(data.lexically_normal());
    const std::u8string u = (rel.empty() ? p : rel).generic_u8string();
    return {u.begin(), u.end()};
}

void skip(std::vector<CleanSkip>* out, const fs::path& data, const fs::path& p, std::string_view why,
          const std::string& inside = {}) {
    if (out == nullptr) {
        return;
    }
    std::string reason;
    if (why == "in_use") {
        reason = "in use by another program";
    } else if (why == "link") {
        reason = "it's a link; FloServer doesn't follow links";
    } else if (why == "hard_link") {
        reason = "it has another name (a hard link), so it's left as it is";
    } else if (why == "protected" && inside.starts_with("project:")) {
        reason = "it's inside project " + inside.substr(8);
    } else if (why == "protected" && inside == "projects") {
        reason = "it's inside the projects folder";
    } else if (why == "protected" && inside == "www") {
        reason = "it's inside the www folder";
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
    out->push_back({path, std::string(why), std::move(reason), why == "protected" ? inside : std::string()});
}

// Why a file operation's error left a file: the skip's code (empty: not said,
// as for one that's gone already).
std::string why_of(const std::error_code& ec) {
    if (ec.category() == clean_category()) {
        switch (ec.value()) {
            case kCleanIsLink:
                return "link";
            case kCleanHardLinked:
                return "hard_link";
            case kCleanWritten:
            case kCleanGone:
                return {};
            default:
                break;
        }
    }
    if (ec == std::errc::no_such_file_or_directory) {
        return {};
    }
    return "in_use";
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
    // A place inside a protected folder isn't gone into (and says which);
    // one that holds one is, and each item is checked on its own.
    if (const Protection p = protection(dir, data, options, false); p.hit) {
        skip(skipped, data, dir, "protected", p.inside);
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

enum class TreeResult : std::uint8_t { Removed, Gone, Stayed };

// Deletes `root` and what's in it, never through a link (links go as links).
// Adds the bytes of the files deleted to `freed`. Gone: it wasn't there any
// more (removed meanwhile, nothing to say); Stayed: something is still there
// (in use).
TreeResult remove_tree(const fs::path& root, std::uintmax_t& freed, const FileOp& remove_file) {
    {
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(root, ec))) {
            return TreeResult::Gone;
        }
    }
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
        if (!fs::exists(st)) {
            continue;  // gone meanwhile: nothing to remove
        }
        if (ec) {
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
            if (ec && why_of(ec).empty()) {
                continue;  // gone meanwhile: neither freed nor in use
            }
        }
        if (gone) {
            freed += entry.bytes;
        } else {
            std::error_code ec;
            all = all && !fs::exists(fs::symlink_status(entry.path, ec));  // gone meanwhile is fine
        }
    }
    if (all) {
        return TreeResult::Removed;
    }
    std::error_code ec;
    return fs::exists(fs::symlink_status(root, ec)) ? TreeResult::Stayed : TreeResult::Removed;
}

// The item, if it's a log Clear old logs may touch now; why not, otherwise.
// `why` stays empty for one that simply isn't due (a current log written to
// within the day, an empty one).
std::optional<CleanItem> log_item(const fs::path& data, const fs::path& p, const CleanOptions& options,
                                  std::string& why, std::string* inside = nullptr) {
    why.clear();
    if (!is_log_file(data, p)) {
        why = "not_allowed";
        return std::nullopt;
    }
    if (const Protection guard = protection(p, data, options); guard.hit) {
        why = "protected";
        if (inside != nullptr) {
            *inside = guard.inside;
        }
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
        // The same file under another name (a hard link, maybe outside the
        // data folder): emptying it here would empty that too.
        if (link_count(p) != 1) {
            why = "hard_link";
            return std::nullopt;
        }
    }
    return CleanItem{p, size, false, rotated};
}

}  // namespace

const std::error_category& clean_category() {
    static const CleanCategory category;
    return category;
}

// Each part looked at in turn without following it. A part swapped for a
// link after it's looked at (by a program of the same user) isn't caught
// here; the file itself is checked again on its handle (handle_op).
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
            std::string inside;
            if (auto item = log_item(data, it->path(), options, why, &inside)) {
                out.push_back(std::move(*item));
            } else if (why == "link" || why == "protected" || why == "hard_link") {
                skip(skipped, data, it->path(), why, inside);
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
        if (const Protection guard = protection(p, data, options); guard.hit) {
            skip(skipped, data, p, "protected", guard.inside);
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
                if (const Protection guard = protection(it->path(), data, options); guard.hit) {
                    skip(skipped, data, it->path(), "protected", guard.inside);
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
        std::string inside;
        const auto now = log_item(data, item.path, options, why, &inside);
        if (!now) {
            if (!why.empty()) {
                skip(&result.skipped_items, data, item.path, why, inside);
            }
            continue;
        }
        // An older copy is deleted. A current log is emptied, not deleted:
        // nginx, PHP and the app servers append, so an open handle goes on
        // at the new end. A server that holds its log without sharing it
        // makes either fail: that log is skipped and named. The real
        // operations check again on the open file (a link, hard links, and
        // for a current log its last write, read from the handle).
        std::error_code ec;
        if (now->remove) {
            remove_file(item.path, ec);
        } else if (options.empty_file) {
            options.empty_file(item.path, ec);
        } else {
            handle_op(item.path, false, options.now - kLogQuietFor, ec);
        }
        if (ec) {
            if (const std::string left = why_of(ec); !left.empty()) {
                skip(&result.skipped_items, data, item.path, left);
            }
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
        if (const Protection guard = protection(item.path, data, options); guard.hit) {
            skip(&result.skipped_items, data, item.path, "protected", guard.inside);
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
        switch (remove_tree(item.path, result.freed, remove_file)) {
            case TreeResult::Removed:
                ++result.done;
                break;
            case TreeResult::Gone:
                break;  // removed meanwhile (an install finishing, another clean-up): nothing to say
            case TreeResult::Stayed:
                skip(&result.skipped_items, data, item.path, "in_use");  // a file in use stays; what could go went
                break;
        }
    }
    result.skipped = result.skipped_items.size();
    return result;
}

}  // namespace floserver::core

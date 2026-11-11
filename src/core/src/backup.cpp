#include "floserver/core/backup.h"

#include "floserver/core/build_info.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/project_config.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/zip_extract.h"
#include "floserver/core/zip_write.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <random>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

bool is_real_folder(const fs::path& p) {
    std::error_code ec;
    return fs::symlink_status(p, ec).type() == fs::file_type::directory && !ec;
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(fs::symlink_status(p, ec)) && !ec;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r' || c == '\n'; }

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

// The same name as Windows compares them (any case), so "Vendor" is left out too there.
bool same_name(std::string_view a, std::string_view b) {
#ifdef _WIN32
    return lower(a) == lower(b);
#else
    return a == b;
#endif
}

std::string two(int n) { return (n < 10 ? "0" : "") + std::to_string(n); }

std::string day_text(const std::tm& t) {
    return std::to_string(t.tm_year + 1900) + "-" + two(t.tm_mon + 1) + "-" + two(t.tm_mday);
}

}  // namespace

std::vector<std::string> leavable_folders(const fs::path& dir) {
    std::vector<std::string> out;
    for (const LeavableFolder& f : kLeavableFolders) {
        if (is_real_folder(dir / path_from_utf8(f.folder))) {
            out.emplace_back(f.folder);
        }
    }
    return out;
}

json backup_info_json(const BackupInfo& info) {
    json out = {{"format", info.format},
                {"name", info.name},
                {"date", info.date},
                {"made_by", "FloServer " + info.made_by},
                {"computer", info.computer},
                {"versions", info.versions.is_object() ? info.versions : json::object()},
                {"left_out", info.left_out},
                {"database", info.database}};
    if (info.database) {
        out["database_file"] = info.database_file;
    }
    return out;
}

std::optional<BackupInfo> parse_backup_info(std::string_view text, std::string& error) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        error = "its floserver-backup.json can't be read";
        return std::nullopt;
    }
    BackupInfo info;
    if (!j.contains("format") || !j.at("format").is_number_integer()) {
        error = "its floserver-backup.json has no format number";
        return std::nullopt;
    }
    info.format = j.at("format").get<int>();
    if (info.format < 1 || info.format > kBackupFormat) {
        error = "it was made by a newer FloServer (backup format " + std::to_string(info.format) +
                "); update FloServer to open it";
        return std::nullopt;
    }
    const auto text_of = [&j](const char* key) {
        return j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string();
    };
    info.name = text_of("name");
    info.date = text_of("date");
    info.made_by = text_of("made_by");
    info.computer = text_of("computer");
    if (j.contains("versions") && j.at("versions").is_object()) {
        for (const auto& [key, value] : j.at("versions").items()) {
            if (value.is_string()) {
                info.versions[shown_text(key)] = shown_text(value.get<std::string>());
            }
        }
    }
    if (j.contains("left_out") && j.at("left_out").is_array()) {
        for (const json& f : j.at("left_out")) {
            if (f.is_string() && std::any_of(kLeavableFolders.begin(), kLeavableFolders.end(),
                                             [&](const LeavableFolder& l) { return l.folder == f.get<std::string>(); })) {
                info.left_out.push_back(f.get<std::string>());
            }
        }
    }
    info.database = j.contains("database") && j.at("database").is_boolean() && j.at("database").get<bool>();
    if (info.database) {
        // Only the name FloServer writes: never any file of the project's.
        const std::string file = text_of("database_file");
        if (!file.empty() && file != kBackupDatabaseFile) {
            error = "its floserver-backup.json names a database file FloServer doesn't write";
            return std::nullopt;
        }
    }
    info.date = shown_text(info.date);
    info.made_by = shown_text(info.made_by);
    if (info.name.empty() || project_name_problem(info.name)) {
        error = "its floserver-backup.json has no project name FloServer can use";
        return std::nullopt;
    }
    return info;
}

std::optional<BackupInfo> read_backup_info(const fs::path& zip, std::string& error) {
    const auto text = read_zip_entry(zip, kBackupInfoFile, kBackupInfoMaxBytes, error);
    if (!text) {
        if (error.empty()) {
            error = path_to_utf8(zip.filename()) + " isn't a FloServer backup (it has no floserver-backup.json)";
        }
        return std::nullopt;
    }
    return parse_backup_info(*text, error);
}

namespace {

fs::path computer_id_file(const fs::path& data) { return data / "state" / "computer-id"; }

bool is_id(std::string_view s) {
    return s.size() == 32 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

}  // namespace

std::optional<std::string> read_computer_id(const fs::path& data) {
    auto text = read_small_file(computer_id_file(data), 128);
    if (!text) {
        return std::nullopt;
    }
    while (!text->empty() && is_space(text->back())) {
        text->pop_back();
    }
    return is_id(*text) ? text : std::nullopt;
}

std::optional<std::string> computer_id(const fs::path& data, std::string& error) {
    if (auto id = read_computer_id(data)) {
        return id;
    }
    std::random_device rd;
    std::string id;
    static constexpr std::string_view kHex = "0123456789abcdef";
    while (id.size() < 32) {
        const unsigned v = rd();
        for (unsigned k = 0; k < 8 && id.size() < 32; ++k) {
            id += kHex.at((v >> (4 * k)) & 0xFU);
        }
    }
    std::error_code ec;
    fs::create_directories(computer_id_file(data).parent_path(), ec);
    if (!write_file_atomically(computer_id_file(data), id + "\n", error)) {
        return std::nullopt;
    }
    return id;
}

bool made_here(const BackupInfo& info, const fs::path& data) {
    const auto id = read_computer_id(data);
    return id && !info.computer.empty() && *id == info.computer;
}

std::string shown_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        if (b < 0x80) {
            if (b == '\t' || b == '\n' || b == '\r') {
                out += ' ';
            } else if (b >= 0x20 && b != 0x7F) {
                out += static_cast<char>(b);
            }
            ++i;
            continue;
        }
        const std::size_t len = (b & 0xE0U) == 0xC0U ? 2 : (b & 0xF0U) == 0xE0U ? 3 : (b & 0xF8U) == 0xF0U ? 4 : 1;
        if (len == 1 || i + len > text.size()) {
            ++i;  // a broken byte: left out
            continue;
        }
        char32_t cp = b & (len == 2 ? 0x1FU : len == 3 ? 0x0FU : 0x07U);
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            const auto c = static_cast<unsigned char>(text[i + k]);
            ok = ok && (c & 0xC0U) == 0x80U;
            cp = (cp << 6U) | (c & 0x3FU);
        }
        const bool hidden = cp < 0xA0 || cp == 0x061C || cp == 0xFEFF || (cp >= 0x200B && cp <= 0x200F) ||
                            (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
        if (ok && !hidden) {
            out.append(text.substr(i, len));
        }
        i += ok ? len : 1;
    }
    return out;
}

json backup_versions(const fs::path& dir) {
    json out = json::object();
    const auto text = read_small_file(dir / path_from_utf8(kProjectConfigFileName), std::size_t{1024} * 1024);
    if (!text) {
        return out;
    }
    const json j = json::parse(*text, nullptr, false);
    if (!j.is_object()) {
        return out;
    }
    for (const char* key : {"php", "node", "bun", "deno", "python", "database", "web"}) {
        if (j.contains(key) && j.at(key).is_string()) {
            out[key] = j.at(key);
        } else if (std::string_view(key) == "php" && j.contains(key) && j.at(key).is_boolean() &&
                   !j.at(key).get<bool>()) {
            out[key] = std::string(kPhpNone);
        }
    }
    return out;
}

std::tm local_time_now() {
    const std::time_t now = std::time(nullptr);
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &now);
#else
    localtime_r(&now, &out);
#endif
    return out;
}

std::string backup_file_name(const std::string& project, const std::tm& when) {
    return project + " " + backup_stamp(when) + ".zip";
}

std::string backup_date(const std::tm& when) { return day_text(when) + " " + two(when.tm_hour) + ":" + two(when.tm_min); }

std::string backup_stamp(const std::tm& when) { return day_text(when) + " " + two(when.tm_hour) + two(when.tm_min); }

std::optional<fs::path> default_backups_dir() {
#ifdef _WIN32
    PWSTR documents = nullptr;
    std::optional<fs::path> out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents != nullptr) {
        out = fs::path(documents) / L"FloServer Backups";
    }
    CoTaskMemFree(documents);
    if (out) {
        return out;
    }
    const auto home = get_env("USERPROFILE");
#else
    const auto home = get_env("HOME");
#endif
    if (!home || home->empty() || !path_from_utf8(*home).is_absolute()) {
        return std::nullopt;
    }
    return path_from_utf8(*home) / "Documents" / "FloServer Backups";
}

std::string copy_name(const std::string& name, const std::function<bool(const std::string&)>& taken) {
    for (int n = 1; n < 1000; ++n) {
        const std::string end = n == 1 ? "-copy" : "-copy-" + std::to_string(n);
        std::string base = name.substr(0, kMaxProjectNameLength - end.size());
        while (!base.empty() && base.back() == '-') {
            base.pop_back();
        }
        const std::string candidate = (base.empty() ? std::string("project") : base) + end;
        if (!taken || !taken(candidate)) {
            return candidate;
        }
    }
    return name + "-copy";
}

std::optional<fs::path> keep_file(const fs::path& file, const std::vector<fs::path>& folders, const std::string& base,
                                  const std::string& ext, std::string& error) {
    std::string why;
    for (const fs::path& folder : folders) {
        std::error_code ec;
        fs::create_directories(folder, ec);
        for (int n = 1; n < 100; ++n) {
            std::string name = base;
            if (n > 1) {
                name += " " + std::to_string(n);
            }
            name += "." + ext;
            fs::path to = folder / path_from_utf8(name);
            if (fs::symlink_status(to, ec).type() != fs::file_type::not_found) {
                continue;
            }
            // Moved where it can be (no copy); copied across drives.
            fs::rename(file, to, ec);
            if (!ec) {
                return to;
            }
            fs::copy_file(file, to, ec);
            if (!ec) {
                fs::remove(file, ec);
                return to;
            }
            fs::remove(to, ec);
            why = path_to_utf8(folder) + ": " + error_text(ec);
            break;
        }
    }
    error = "it couldn't be kept anywhere (" + why + ")";
    return std::nullopt;
}

std::optional<BackupContents> backup_contents(const fs::path& dir, const std::vector<std::string>& left_out,
                                              std::string& error) {
    if (!is_real_folder(dir)) {
        error = path_to_utf8(dir) + " isn't a folder";
        return std::nullopt;
    }
    BackupContents out;
    std::error_code ec;
    auto it = fs::recursive_directory_iterator(dir, fs::directory_options::none, ec);
    if (ec) {
        error = "can't read " + path_to_utf8(dir) + ": " + error_text(ec);
        return std::nullopt;
    }
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            error = "can't read " + path_to_utf8(it->path()) + ": " + error_text(ec);
            return std::nullopt;
        }
        const fs::path rel = it->path().lexically_relative(dir);
        const auto status = it->symlink_status(ec);
        if (ec) {
            error = "can't read " + path_to_utf8(it->path()) + ": " + error_text(ec);
            return std::nullopt;
        }
        const bool top = rel.has_filename() && rel.parent_path().empty();
        const std::string top_name = top ? path_to_utf8(rel) : std::string();
        // A link or junction is never followed (it could lead anywhere) nor kept.
        if (fs::is_symlink(status) || it->is_symlink(ec)) {
            ++out.links;
            if (status.type() == fs::file_type::directory || it->is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
#ifdef _WIN32
        // A junction isn't a symlink to std::filesystem on every library: Windows says.
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            ++out.links;
            it.disable_recursion_pending();
            continue;
        }
#endif
        if (status.type() == fs::file_type::directory) {
            if (top && std::any_of(left_out.begin(), left_out.end(),
                                   [&](const std::string& f) { return same_name(f, top_name); })) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (status.type() != fs::file_type::regular) {
            continue;  // a device, a pipe: nothing to back up
        }
        if (top && same_name(top_name, kBackupInfoFile)) {
            continue;  // it would take the backup's own place
        }
        const auto size = it->file_size(ec);
        if (ec) {
            error = "can't read " + path_to_utf8(it->path()) + ": " + error_text(ec);
            return std::nullopt;
        }
        const std::u8string generic = rel.generic_u8string();
        std::string name(generic.begin(), generic.end());
        if (same_name(name, kBackupDatabaseFile)) {
            continue;  // the backup's own dump goes there
        }
        out.files.push_back({std::move(name), it->path(), size});
        out.bytes += size;
    }
    // In a steady order, so two backups of the same files list them alike.
    std::sort(out.files.begin(), out.files.end(),
              [](const BackupFile& a, const BackupFile& b) { return a.name < b.name; });
    return out;
}

bool write_backup_zip(const fs::path& out, const BackupContents& contents, const BackupInfo& info,
                      const std::optional<fs::path>& database_sql, const std::function<bool(std::uint64_t done)>& progress,
                      bool& cancelled, std::string& error) {
    cancelled = false;
    ZipWriter zip;
    if (!zip.open(out, error)) {
        return false;
    }
    std::uint64_t done = 0;
    for (const BackupFile& f : contents.files) {
        if (!zip.add_file(f.name, f.path, error)) {
            return false;
        }
        ++done;
        if (progress && !progress(done)) {
            cancelled = true;
            error = "cancelled";
            return false;
        }
    }
    if (database_sql && !zip.add_file(info.database_file, *database_sql, error)) {
        return false;
    }
    if (!zip.add_text(std::string(kBackupInfoFile), backup_info_json(info).dump(2) + "\n", error)) {
        return false;
    }
    return zip.finish(error);
}

std::string strip_definer(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    std::size_t i = 0;
    while (i < line.size()) {
        const std::size_t at = line.find("DEFINER=", i);
        if (at == std::string_view::npos) {
            out.append(line.substr(i));
            break;
        }
        out.append(line.substr(i, at - i));
        // `user`@`host`, 'user'@'host' or user@host, then one space.
        std::size_t j = at + 8;
        for (int part = 0; part < 2 && j < line.size(); ++part) {
            const char quote = line[j];
            if (quote == '`' || quote == '\'' || quote == '"') {
                const std::size_t end = line.find(quote, j + 1);
                j = end == std::string_view::npos ? line.size() : end + 1;
            } else {
                while (j < line.size() && line[j] != '@' && line[j] != ' ' && line[j] != '*') {
                    ++j;
                }
            }
            if (part == 0 && j < line.size() && line[j] == '@') {
                ++j;
            } else {
                break;
            }
        }
        if (j < line.size() && line[j] == ' ') {
            ++j;
        }
        i = j;
    }
    return out;
}

namespace {


// The mariadb program's own commands in their long forms: at a
// statement's start they're its, not the server's.
bool is_client_command(std::string_view word) {
    static constexpr std::array<std::string_view, 22> kCommands = {
        "?",      "charset", "clear", "connect", "edit",  "ego",     "exit",   "go",
        "help",   "nopager", "notee", "nowarning", "pager", "print", "prompt", "quit",
        "rehash", "sandbox", "source", "status",  "system", "tee"};
    return std::find(kCommands.begin(), kCommands.end(), word) != kCommands.end() || word == "warnings";
}

}  // namespace

bool SqlLoadFilter::line(std::string& line, std::string& error) {
    if (in_ == In::Code) {
        std::size_t start = 0;
        while (start < line.size() && is_space(line[start])) {
            ++start;
        }
        const std::string head = lower(std::string_view(line).substr(start, 40));
        // Where mariadb-dump's routines and events begin.
        if (head.starts_with("-- dumping events for database") || head.starts_with("-- dumping routines for database")) {
            extras_ = true;
        }
        if (at_start_) {
            // Into whichever database it's loaded into.
            if (head.starts_with("use ") || head.starts_with("use`") || head.starts_with("use\t") ||
                head == "use" || head.starts_with("create database") || head.starts_with("/*!40000 create database") ||
                head.starts_with("create schema")) {
                line.clear();
                return true;
            }
            if (head.starts_with("delimiter")) {
                std::string d = line.substr(start + 9);
                while (!d.empty() && is_space(d.front())) {
                    d.erase(d.begin());
                }
                while (!d.empty() && is_space(d.back())) {
                    d.pop_back();
                }
                if (d.empty() || d.find('\\') != std::string::npos) {
                    error = "its database.sql changes the statement end in a way FloServer doesn't run";
                    return false;
                }
                delimiter_ = d;
                return true;
            }
            std::size_t end = 0;
            while (end < head.size() && !is_space(head[end]) && head[end] != ';') {
                ++end;
            }
            if (is_client_command(std::string_view(head).substr(0, end))) {
                error = "its database.sql holds a command for the mariadb program itself (" + head.substr(0, end) +
                        "), which FloServer doesn't run";
                return false;
            }
        }
        if (head.starts_with("/*!") || head.starts_with("create")) {
            line = strip_definer(line);
        }
    }
    bool code_seen = false;  // something besides comments on this line
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        const char next = i + 1 < line.size() ? line[i + 1] : '\0';
        switch (in_) {
            case In::Code:
                if (c == '\'') {
                    in_ = In::Single;
                } else if (c == '"') {
                    in_ = In::Double;
                } else if (c == '`') {
                    in_ = In::Backtick;
                } else if (c == '#' || (c == '-' && next == '-' && (i + 2 >= line.size() || is_space(line[i + 2])))) {
                    i = line.size();  // the rest is a comment
                    continue;
                } else if (c == '/' && next == '*') {
                    // /*! and /*M! are run as SQL; only a plain /* is a comment.
                    const bool runs = i + 2 < line.size() &&
                                      (line[i + 2] == '!' || (line[i + 2] == 'M' && i + 3 < line.size() && line[i + 3] == '!'));
                    if (!runs) {
                        in_ = In::Comment;
                    }
                    ++i;
                    continue;
                } else if (c == '\\') {
                    if (next != '-') {
                        error = "its database.sql holds a command for the mariadb program itself (\\" +
                                std::string(1, next) + "), which FloServer doesn't run";
                        return false;
                    }
                    ++i;
                }
                if (!is_space(c)) {
                    code_seen = true;
                }
                break;
            case In::Single:
            case In::Double:
                code_seen = true;
                if (c == '\\') {
                    ++i;  // the next character is escaped
                } else if ((in_ == In::Single && c == '\'') || (in_ == In::Double && c == '"')) {
                    in_ = In::Code;
                }
                break;
            case In::Backtick:
                code_seen = true;
                if (c == '`') {
                    in_ = In::Code;
                }
                break;
            case In::Comment:
                if (c == '*' && next == '/') {
                    in_ = In::Code;
                    ++i;
                }
                break;
        }
    }
    // The next line starts a statement when this one ended one (outside a string).
    if (in_ == In::Code && code_seen) {
        std::string trimmed = line;
        while (!trimmed.empty() && is_space(trimmed.back())) {
            trimmed.pop_back();
        }
        at_start_ = trimmed.ends_with(delimiter_);
    }
    return true;
}

bool SqlLoadWriter::open(const fs::path& main, const fs::path& extras, const std::string& database,
                         std::string& error) {
    main_path_ = main;
    extras_path_ = extras;
    main_.open(main, std::ios::binary | std::ios::trunc);
    extras_.open(extras, std::ios::binary | std::ios::trunc);
    if (!main_ || !extras_) {
        error = "can't write " + path_to_utf8(main.parent_path());
        return false;
    }
    // FloServer's own first lines: the client reads commands of its own
    // only on its first line, so it's never one from the backup.
    const std::string db = sql_identifier(database);
    main_ << "DROP DATABASE IF EXISTS " << db << ";\n"
          << "CREATE DATABASE " << db << " CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;\n"
          << "USE " << db << ";\n";
    extras_ << "USE " << db << ";\n";
    return true;
}

bool SqlLoadWriter::flush_line(std::string& error) {
    if (!filter_.line(pending_, error)) {
        return false;
    }
    if (filter_.extras()) {
        has_extras_ = true;
        extras_ << pending_ << '\n';
    } else {
        main_ << pending_ << '\n';
    }
    pending_.clear();
    return true;
}

bool SqlLoadWriter::feed(std::string_view piece, std::string& error) {
    std::size_t from = 0;
    while (from < piece.size()) {
        const std::size_t nl = piece.find('\n', from);
        if (nl == std::string_view::npos) {
            pending_.append(piece.substr(from));
            break;
        }
        pending_.append(piece.substr(from, nl - from));
        if (!flush_line(error)) {
            return false;
        }
        from = nl + 1;
    }
    return true;
}

bool SqlLoadWriter::finish(std::string& error) {
    if (!pending_.empty() && !flush_line(error)) {
        return false;
    }
    main_.close();
    extras_.close();
    if (main_.fail() || extras_.fail()) {
        error = "can't write " + path_to_utf8(main_path_) + " (is the disk full?)";
        return false;
    }
    return true;
}

bool prepare_sql_for_load(const fs::path& in, const fs::path& main, const fs::path& extras,
                          const std::string& database, bool& has_extras, std::string& error) {
    std::ifstream from(in, std::ios::binary);
    if (!from) {
        error = "can't read " + path_to_utf8(in);
        return false;
    }
    SqlLoadWriter writer;
    if (!writer.open(main, extras, database, error)) {
        return false;
    }
    std::array<char, 65536> buffer{};
    while (from.read(buffer.data(), buffer.size()) || from.gcount() > 0) {
        if (!writer.feed(std::string_view(buffer.data(), static_cast<std::size_t>(from.gcount())), error)) {
            return false;
        }
    }
    if (from.bad()) {
        error = "can't read " + path_to_utf8(in);
        return false;
    }
    if (!writer.finish(error)) {
        return false;
    }
    has_extras = writer.has_extras();
    return true;
}

std::vector<ReinstallStep> reinstall_steps(const fs::path& dir, const std::vector<std::string>& left_out) {
    std::vector<ReinstallStep> out;
    const auto wanted = [&](std::string_view folder, std::string_view list) {
        return std::find(left_out.begin(), left_out.end(), folder) != left_out.end() &&
               !is_real_folder(dir / path_from_utf8(folder)) && is_file(dir / path_from_utf8(list));
    };
    if (wanted("vendor", "composer.json")) {
        out.push_back({"composer", {"install", "--no-interaction"}, "composer install"});
    }
    if (wanted("node_modules", "package.json")) {
        out.push_back({"npm", {"install"}, "npm install"});
    }
    if (wanted(".venv", "requirements.txt")) {
        out.push_back({"python", {"-m", "venv", ".venv"}, "python -m venv .venv"});
        out.push_back({"pip", {"install", "--disable-pip-version-check", "-r", "requirements.txt"},
                       "pip install -r requirements.txt"});
    }
    return out;
}

std::optional<std::string> update_wp_config_text(const std::string& text, const DatabaseLogin& login) {
    const std::array<std::pair<std::string_view, std::string>, 4> values = {{
        {"DB_NAME", login.database},
        {"DB_USER", login.user},
        {"DB_PASSWORD", login.password},
        {"DB_HOST", "127.0.0.1:" + std::to_string(login.port)},
    }};
    // Where each one's value is: the characters between its single quotes.
    std::array<std::pair<std::size_t, std::size_t>, 4> found{};
    std::array<bool, 4> seen{};
    const std::size_t n = text.size();
    const auto skip_space = [&](std::size_t i) {
        while (i < n && is_space(text[i])) {
            ++i;
        }
        return i;
    };
    // define( 'KEY', 'value' ) at `i` (just after "define"): the key and the
    // value's span, or nothing.
    const auto define_at = [&](std::size_t i, std::string& key, std::pair<std::size_t, std::size_t>& value,
                               std::size_t& end) {
        i = skip_space(i);
        if (i >= n || text[i] != '(') {
            return false;
        }
        i = skip_space(i + 1);
        if (i >= n || (text[i] != '\'' && text[i] != '"')) {
            return false;
        }
        const char q = text[i];
        const std::size_t key_end = text.find(q, i + 1);
        if (key_end == std::string::npos) {
            return false;
        }
        key = text.substr(i + 1, key_end - i - 1);
        i = skip_space(key_end + 1);
        if (i >= n || text[i] != ',') {
            return false;
        }
        i = skip_space(i + 1);
        if (i >= n || text[i] != '\'') {
            return false;
        }
        const std::size_t from = i + 1;
        std::size_t j = from;
        while (j < n && text[j] != '\'') {
            j += text[j] == '\\' ? std::size_t{2} : std::size_t{1};
        }
        if (j >= n) {
            return false;
        }
        i = skip_space(j + 1);
        if (i >= n || text[i] != ')') {
            return false;
        }
        value = {from, j};
        end = i + 1;
        return true;
    };
    for (std::size_t i = 0; i < n;) {
        const char c = text[i];
        const char next = i + 1 < n ? text[i + 1] : '\0';
        if ((c == '/' && next == '/') || c == '#') {
            const std::size_t nl = text.find('\n', i);
            i = nl == std::string::npos ? n : nl + 1;
        } else if (c == '/' && next == '*') {
            const std::size_t close = text.find("*/", i + 2);
            i = close == std::string::npos ? n : close + 2;
        } else if (c == '\'' || c == '"') {
            std::size_t j = i + 1;
            while (j < n && text[j] != c) {
                j += text[j] == '\\' ? std::size_t{2} : std::size_t{1};
            }
            i = j + 1;
        } else if ((c == 'd' || c == 'D') && lower(std::string_view(text).substr(i, 6)) == "define" &&
                   (i == 0 || (std::isalnum(static_cast<unsigned char>(text[i - 1])) == 0 && text[i - 1] != '_'))) {
            std::string key;
            std::pair<std::size_t, std::size_t> value;
            std::size_t end = 0;
            if (define_at(i + 6, key, value, end)) {
                for (std::size_t k = 0; k < values.size(); ++k) {
                    if (key == values.at(k).first && !seen.at(k)) {
                        found.at(k) = value;
                        seen.at(k) = true;
                    }
                }
                i = end;
            } else {
                i += 6;
            }
        } else {
            ++i;
        }
    }
    if (std::any_of(seen.begin(), seen.end(), [](bool f) { return !f; })) {
        return std::nullopt;
    }
    // Replaced from the end, so the earlier places stay where they are.
    std::array<std::size_t, 4> order = {0, 1, 2, 3};
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return found.at(a).first > found.at(b).first; });
    std::string out = text;
    for (const std::size_t k : order) {
        // The value as a PHP single-quoted string: \ and ' escaped.
        std::string quoted;
        for (const char ch : values.at(k).second) {
            if (ch == '\\' || ch == '\'') {
                quoted += '\\';
            }
            quoted += ch;
        }
        const auto& [from, to] = found.at(k);
        out.replace(from, to - from, quoted);
    }
    return out;
}

}  // namespace floserver::core

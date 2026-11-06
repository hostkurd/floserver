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
#include <regex>
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
    if (j.contains("versions") && j.at("versions").is_object()) {
        for (const auto& [key, value] : j.at("versions").items()) {
            if (value.is_string()) {
                info.versions[key] = value;
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
        // Only one of the two names FloServer writes: never any file of the zip.
        info.database_file = text_of("database_file");
        if (info.database_file.empty()) {
            info.database_file = std::string(kBackupDatabaseFile);
        }
        if (info.database_file != kBackupDatabaseFile && info.database_file != kBackupDatabaseFileAlt) {
            error = "its floserver-backup.json names a database file FloServer doesn't write";
            return std::nullopt;
        }
    }
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
    for (const char* key : {"php", "node", "python", "database", "web"}) {
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

std::optional<BackupContents> backup_contents(const fs::path& dir, const std::vector<std::string>& left_out,
                                              std::string& error) {
    if (!is_real_folder(dir)) {
        error = path_to_utf8(dir) + " isn't a folder";
        return std::nullopt;
    }
    BackupContents out;
    out.database_file = is_file(dir / path_from_utf8(kBackupDatabaseFile)) ? std::string(kBackupDatabaseFileAlt)
                                                                           : std::string(kBackupDatabaseFile);
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
        out.files.push_back({std::string(generic.begin(), generic.end()), it->path(), size});
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

bool SqlLoadFilter::line(std::string& line, std::string& error) {
    if (in_ == In::Code) {
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        const std::string head = lower(std::string_view(line).substr(start, 32));
        // Into whichever database it's loaded into.
        if (head.starts_with("use ") || head.starts_with("use`") || head.starts_with("create database") ||
            head.starts_with("/*!40000 create database") || head.starts_with("create schema")) {
            line.clear();
            return true;
        }
        if (head.starts_with("/*!") || head.starts_with("create")) {
            line = strip_definer(line);
        }
    }
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
                } else if (c == '#' || (c == '-' && next == '-' &&
                                        (i + 2 >= line.size() || line[i + 2] == ' ' || line[i + 2] == '\t'))) {
                    i = line.size();  // the rest is a comment
                } else if (c == '/' && next == '*') {
                    // /*! and /*M! are run as SQL; only a plain /* is a comment.
                    const bool runs = i + 2 < line.size() && (line[i + 2] == '!' || line[i + 2] == 'M');
                    if (!runs) {
                        in_ = In::Comment;
                    }
                    ++i;
                } else if (c == '\\') {
                    if (next != '-') {
                        error = "its database.sql holds a command for the mariadb program itself (\\" +
                                std::string(1, next) + "), which FloServer doesn't run";
                        return false;
                    }
                    ++i;
                }
                break;
            case In::Single:
            case In::Double:
                if (c == '\\') {
                    ++i;  // the next character is escaped
                } else if ((in_ == In::Single && c == '\'') || (in_ == In::Double && c == '"')) {
                    in_ = In::Code;
                }
                break;
            case In::Backtick:
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
    return true;
}

bool prepare_sql_for_load(const fs::path& in, const fs::path& out, const std::string& database, std::string& error) {
    std::ifstream from(in, std::ios::binary);
    if (!from) {
        error = "can't read " + path_to_utf8(in);
        return false;
    }
    std::ofstream to(out, std::ios::binary | std::ios::trunc);
    if (!to) {
        error = "can't write " + path_to_utf8(out);
        return false;
    }
    // FloServer's own first line: the client reads commands of its own only
    // on its first line, so it's never one from the backup.
    const std::string db = sql_identifier(database);
    to << "DROP DATABASE IF EXISTS " << db << ";\n"
       << "CREATE DATABASE " << db << " CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;\n"
       << "USE " << db << ";\n";
    SqlLoadFilter filter;
    std::string line;
    while (std::getline(from, line)) {
        if (!filter.line(line, error)) {
            return false;
        }
        to << line << '\n';
    }
    if (from.bad()) {
        error = "can't read " + path_to_utf8(in);
        return false;
    }
    to.close();
    if (to.fail()) {
        error = "can't write " + path_to_utf8(out) + " (is the disk full?)";
        return false;
    }
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
    std::string out = text;
    const std::pair<const char*, std::string> values[] = {
        {"DB_NAME", login.database},
        {"DB_USER", login.user},
        {"DB_PASSWORD", login.password},
        {"DB_HOST", "127.0.0.1:" + std::to_string(login.port)},
    };
    for (const auto& [key, value] : values) {
        const std::regex line(std::string(R"re((define\s*\(\s*['"]))re") + key + R"re((['"]\s*,\s*)'[^'\\]*'(\s*\)))re");
        std::smatch m;
        if (!std::regex_search(out, m, line)) {
            return std::nullopt;
        }
        // The value as a PHP single-quoted string: \ and ' escaped.
        std::string quoted;
        for (const char c : value) {
            if (c == '\\' || c == '\'') {
                quoted += '\\';
            }
            quoted += c;
        }
        out = m.prefix().str() + m[1].str() + key + m[2].str() + "'" + quoted + "'" + m[3].str() + m.suffix().str();
    }
    return out;
}

}  // namespace floserver::core

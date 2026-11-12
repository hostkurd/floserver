#pragma once

// A project as a .zip (D22, approved 5 Nov): backing it up, restoring it,
// and making a new project from a backup. The rules, with no service or
// window in them, so they're tested on every platform; backup_service.h
// runs them as jobs.
//
// A backup holds:
//   - the project folder's files, as they are inside it. Links and
//     junctions are neither followed nor kept; the folders that can be
//     installed again (vendor, node_modules, .venv) may be left out, at the
//     folder's top only;
//   - .floserver/database.sql: mariadb-dump of the project's own database,
//     with its own login (when it has one and it was asked for), its
//     routines and events too;
//   - floserver-backup.json: what the backup is (backup_info_json()), and
//     which computer made it (computer_id()).
// The project's own floserver-backup.json and .floserver/database.sql, if
// it has them, aren't backed up: they'd take the backup's own places.
// Unpacking one uses zip_extract.h's rules (no "..", absolute paths,
// devices, links, the same name twice...), with the names in any language.

#include "floserver/core/mariadb.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

inline constexpr std::string_view kBackupInfoFile = "floserver-backup.json";
// In a folder of its own in the zip, so no file of the project's can be it.
inline constexpr std::string_view kBackupDatabaseFile = ".floserver/database.sql";
inline constexpr int kBackupFormat = 1;
// floserver-backup.json is a few hundred bytes; anything this big isn't ours.
inline constexpr std::size_t kBackupInfoMaxBytes = std::size_t{64} * 1024;

// A folder a backup can leave out, what brings it back, and the file that
// says what goes in it.
struct LeavableFolder {
    std::string_view folder;  // "vendor"
    std::string_view tool;    // "composer"
    std::string_view list;    // "composer.json"
};
inline constexpr std::array<LeavableFolder, 3> kLeavableFolders = {{
    {"vendor", "composer", "composer.json"},
    {"node_modules", "npm", "package.json"},
    {".venv", "pip", "requirements.txt"},
}};

// Which of kLeavableFolders the project has: real folders at its top (a
// link or junction there isn't backed up anyway).
[[nodiscard]] std::vector<std::string> leavable_folders(const std::filesystem::path& dir);

// This computer's own id (<data>/state/computer-id, 32 random hex
// characters), made the first time. Backups say which computer made
// them: the packages a backup left out are installed again (their own
// install scripts run as the user) only for a backup made here, unless
// the user asks.
[[nodiscard]] std::optional<std::string> computer_id(const std::filesystem::path& data, std::string& error);
// The same, only read: nothing when there's none yet.
[[nodiscard]] std::optional<std::string> read_computer_id(const std::filesystem::path& data);

// floserver-backup.json.
struct BackupInfo {
    int format = kBackupFormat;
    std::string name;      // the project's
    std::string date;      // when it was made, local time: "2026-11-05 14:30"
    std::string made_by;   // FloServer's version
    std::string computer;  // computer_id() of the one that made it
    // From its floserver.json: "php", "node", "bun", "deno", "python", "database", "web"
    // as written there ("8.5", "mariadb-11.4"; "php": "none" for false).
    nlohmann::json versions = nlohmann::json::object();
    std::vector<std::string> left_out;  // "vendor", "node_modules", ".venv"
    bool database = false;              // the zip has the database
    std::string database_file = std::string(kBackupDatabaseFile);
};
[[nodiscard]] nlohmann::json backup_info_json(const BackupInfo& info);
// Nothing (with `error`) when the text isn't a backup's description FloServer
// can use. Its texts are made safe to show (shown_text()).
[[nodiscard]] std::optional<BackupInfo> parse_backup_info(std::string_view text, std::string& error);
// The description in a .zip; nothing with "isn't a FloServer backup" when it has none.
[[nodiscard]] std::optional<BackupInfo> read_backup_info(const std::filesystem::path& zip, std::string& error);
// The backup was made on this computer (its id matches <data>'s).
[[nodiscard]] bool made_here(const BackupInfo& info, const std::filesystem::path& data);

// Text from a backup or a program's output, as it may be shown: control
// characters (the terminal's escape sequences among them), direction
// marks and overrides taken out, line breaks and tabs made spaces.
[[nodiscard]] std::string shown_text(std::string_view text);

// The versions floserver-backup.json records, from the folder's floserver.json.
[[nodiscard]] nlohmann::json backup_versions(const std::filesystem::path& dir);

// The time now, on this computer's clock.
[[nodiscard]] std::tm local_time_now();
// "shop 2026-11-05 1430.zip": the name a new backup gets.
[[nodiscard]] std::string backup_file_name(const std::string& project, const std::tm& when);
// "2026-11-05 14:30", for floserver-backup.json.
[[nodiscard]] std::string backup_date(const std::tm& when);
// "2026-11-05 1430", for what a restore keeps ("shop before restore 2026-11-05 1430.sql").
[[nodiscard]] std::string backup_stamp(const std::tm& when);
// Documents\FloServer Backups (Documents/FloServer Backups elsewhere);
// nothing when there's no home folder.
[[nodiscard]] std::optional<std::filesystem::path> default_backups_dir();

// A new project's name from a backup's: "shop-copy", or "shop-copy-2" and
// on while `taken` says the name is in use; cut so it stays a project name
// (63 characters at most).
[[nodiscard]] std::string copy_name(const std::string& name, const std::function<bool(const std::string&)>& taken);

// Moves `file` into the first of `folders` it can be put in, as
// "<base>.<ext>" (or "<base> 2.<ext>"... when that's taken). Where it is
// now, or nothing (with `error`) when none of them would take it.
[[nodiscard]] std::optional<std::filesystem::path> keep_file(const std::filesystem::path& file,
                                                             const std::vector<std::filesystem::path>& folders,
                                                             const std::string& base, const std::string& ext,
                                                             std::string& error);

struct BackupFile {
    std::string name;  // in the zip: "app/index.php"
    std::filesystem::path path;
    std::uint64_t size = 0;
};
struct BackupContents {
    std::vector<BackupFile> files;
    std::uint64_t bytes = 0;
    std::uint64_t links = 0;  // links and junctions passed over
};
// What the zip holds of the project folder, `left_out` folders left out at
// its top. Nothing (with `error`) when the folder can't be read.
[[nodiscard]] std::optional<BackupContents> backup_contents(const std::filesystem::path& dir,
                                                            const std::vector<std::string>& left_out,
                                                            std::string& error);

// Writes the backup to `out`: the files, then the dump (when `database_sql`
// is given) and floserver-backup.json. `progress` is told how many files
// are done after each; it returns false to stop (`cancelled`), and `out`
// is then left for the caller to remove.
[[nodiscard]] bool write_backup_zip(const std::filesystem::path& out, const BackupContents& contents,
                                    const BackupInfo& info, const std::optional<std::filesystem::path>& database_sql,
                                    const std::function<bool(std::uint64_t done)>& progress, bool& cancelled,
                                    std::string& error);

// A backup's SQL made ready for the mariadb program, line by line: it goes
// into whichever database it's loaded into (a "USE" or "CREATE DATABASE" of
// its own is dropped), triggers, views and routines lose their DEFINER
// (another user's would be refused), and a line holding one of the mariadb
// program's own commands is refused: "\!" (runs a program), "\." and the
// long forms ("source", "system", "tee", "connect"...) at a statement's
// start. Only the "\-" a new mariadb-dump writes (its sandbox mode) is let
// through. From the dump's "Dumping events" or "Dumping routines" line on,
// lines are extras(): loaded on their own, where a refusal for lack of
// rights is only a warning.
class SqlLoadFilter {
public:
    // Changes `line` (without its line ending) in place; false (with
    // `error`) when it must not be run.
    bool line(std::string& line, std::string& error);
    // The lines from here on are routines and events.
    [[nodiscard]] bool extras() const { return extras_; }

private:
    enum class In : std::uint8_t { Code, Single, Double, Backtick, Comment };
    In in_ = In::Code;
    bool at_start_ = true;        // the next line starts a statement
    std::string delimiter_ = ";"; // DELIMITER changes it (triggers, routines)
    bool extras_ = false;
};
// DEFINER=`user`@`host` taken out of a line.
[[nodiscard]] std::string strip_definer(std::string_view line);

// Writes the SQL that empties `database` and loads a backup's dump into
// it, fed piece by piece as the dump is unpacked (no copy of the dump is
// kept): `main` the tables and their rows, `extras` (when there are any)
// the routines and events.
class SqlLoadWriter {
public:
    [[nodiscard]] bool open(const std::filesystem::path& main, const std::filesystem::path& extras,
                            const std::string& database, std::string& error);
    [[nodiscard]] bool feed(std::string_view piece, std::string& error);
    [[nodiscard]] bool finish(std::string& error);
    [[nodiscard]] bool has_extras() const { return has_extras_; }

private:
    bool flush_line(std::string& error);
    std::ofstream main_;
    std::ofstream extras_;
    std::filesystem::path main_path_;
    std::filesystem::path extras_path_;
    std::string pending_;
    SqlLoadFilter filter_;
    bool has_extras_ = false;
};
// The same from a file.
[[nodiscard]] bool prepare_sql_for_load(const std::filesystem::path& in, const std::filesystem::path& main,
                                        const std::filesystem::path& extras, const std::string& database,
                                        bool& has_extras, std::string& error);

// The commands that bring back what a backup left out, in the project's
// folder with its own runtimes (the terminal shims): composer install,
// npm install, and a .venv with pip install -r requirements.txt. Only for
// folders that were left out, aren't there now, and whose list file is.
struct ReinstallStep {
    std::string tool;               // a shim's name: "composer", "npm", "python", "pip"
    std::vector<std::string> args;  // after it
    std::string shown;              // "composer install"
};
[[nodiscard]] std::vector<ReinstallStep> reinstall_steps(const std::filesystem::path& dir,
                                                         const std::vector<std::string>& left_out);

// wp-config.php with its database lines set to `login`, or nothing when it
// doesn't have the lines WordPress writes. A define() in a comment or a
// string isn't one of them.
[[nodiscard]] std::optional<std::string> update_wp_config_text(const std::string& text, const DatabaseLogin& login);

}  // namespace floserver::core

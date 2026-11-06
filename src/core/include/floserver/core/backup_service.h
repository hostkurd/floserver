#pragma once

// Backups as jobs in the service (D22, approved 5 Nov), one thread each,
// followed with backup.get and stopped with backup.cancel the way runtime
// installs are (jobs.get, jobs.cancel). Three kinds:
//
//   backup   the project keeps running: its database is dumped first
//            (--single-transaction), then its files go into "<to>.part",
//            renamed to <to> once the zip is whole;
//   restore  the project stops; today's database is saved next to the
//            backup ("<name> before restore <date>.sql") and the files are
//            unpacked beside the folder, and only then is anything replaced:
//            the database loaded from the backup, the folder swapped in
//            (one the window didn't move to the Recycle Bin is renamed
//            "<folder> before restore <date>", never deleted), .env updated,
//            the left-out packages installed. Then it starts again;
//   create   a new project from a backup: unpacked into the projects
//            folder, its packages installed, added and turned on, its own
//            database made from the backup and .env (or wp-config.php)
//            pointed at it.
// What the service needs from its other parts comes in as hooks, so the
// rules are tested without the database.

#include "floserver/core/backup.h"
#include "floserver/core/mariadb.h"
#include "floserver/core/runtime_service.h"  // ServiceRefusal

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace floserver::core {

// A registered project, as a backup job needs it.
struct BackupProject {
    std::string name;
    std::filesystem::path dir;
    bool on = false;
    bool database = false;  // its floserver.json asks for one
};

struct BackupHooks {
    std::function<std::optional<BackupProject>(const std::string& name, std::string& error)> project;
    // mariadb-dump of its database into <work>/database.sql.
    std::function<bool(const std::string& project, const std::filesystem::path& work, const std::atomic<bool>& cancel,
                       std::string& error)>
        dump_database;
    // Runs prepared SQL (prepare_sql_for_load) with the project's own login.
    std::function<bool(const std::string& project, const std::filesystem::path& sql, const std::atomic<bool>& cancel,
                       std::string& error)>
        load_database;
    // The project's database login (made the first time).
    std::function<std::optional<DatabaseLogin>(const std::string& project, std::string& error)> database_login;
    // Registers the folder as a project, turned on; its name, or nothing.
    std::function<std::optional<std::string>(const std::filesystem::path& dir, const std::string& name,
                                             std::string& error)>
        add_project;
    // Turns a project on or off (a restore stops it meanwhile).
    std::function<bool(const std::string& project, bool on, std::string& error)> set_on;
};

struct BackupRequest {
    std::string project;
    std::filesystem::path to;  // the .zip to write (absolute)
    bool database = true;      // when it has one
    // The folders to leave out; nothing: every one it has (leavable_folders()).
    std::optional<std::vector<std::string>> leave_out;
};

struct RestoreRequest {
    std::string project;
    std::filesystem::path from;
    bool files = true;
    bool database = true;
    bool install = true;
};

struct FromBackupRequest {
    std::filesystem::path from;
    std::string name;              // the new project's
    std::filesystem::path parent;  // where its folder is made (absolute)
    bool install = true;
};

class BackupService {
public:
    BackupService(std::filesystem::path data_dir, BackupHooks hooks);
    ~BackupService();
    BackupService(const BackupService&) = delete;
    BackupService& operator=(const BackupService&) = delete;
    BackupService(BackupService&&) = delete;
    BackupService& operator=(BackupService&&) = delete;

    // Each starts a job; its id, or nothing (with `refusal`) when the request
    // is refused before anything is done.
    [[nodiscard]] std::optional<std::int64_t> backup(const BackupRequest& request, ServiceRefusal& refusal);
    [[nodiscard]] std::optional<std::int64_t> restore(const RestoreRequest& request, ServiceRefusal& refusal);
    [[nodiscard]] std::optional<std::int64_t> create(const FromBackupRequest& request, ServiceRefusal& refusal);

    // {"id", "kind": "backup" | "restore" | "create", "state": "running" |
    //  "done" | "failed" | "cancelled", "step", "project", "file",
    //  "files", "total_files", "database": "none" | "saving" | "saved" |
    //  "loading" | "loaded", "log": [...], "size"? (the zip, when done),
    //  "folder"?, "saved_database"? (restore: today's, kept),
    //  "set_aside"? (restore: the folder it replaced), "error"?,
    //  "cancellable": false once a restore has begun replacing things}
    [[nodiscard]] std::optional<nlohmann::json> job(std::int64_t id) const;
    bool cancel(std::int64_t id);
    [[nodiscard]] bool busy() const;
    // Cancels every job and waits for them.
    void stop();

    static constexpr std::size_t kLogLines = 400;

private:
    struct Job;
    std::optional<std::int64_t> start(const std::string& kind, const std::string& project, const std::string& file,
                                      const std::function<void(const std::shared_ptr<Job>&)>& work,
                                      ServiceRefusal& refusal);
    void run_backup(const std::shared_ptr<Job>& job, const BackupRequest& request);
    void run_restore(const std::shared_ptr<Job>& job, const RestoreRequest& request);
    void run_create(const std::shared_ptr<Job>& job, const FromBackupRequest& request);
    // The commands that bring back what was left out; false (with `error`) when one fails.
    bool reinstall(const std::shared_ptr<Job>& job, const std::filesystem::path& dir,
                   const std::vector<std::string>& left_out, std::string& error);
    // The backup's database.sql loaded into the project's database.
    bool load_from_backup(const std::shared_ptr<Job>& job, const std::filesystem::path& zip, const BackupInfo& info,
                          const std::string& project, const std::filesystem::path& work, std::string& error);
    // .env (or wp-config.php) pointed at the project's database.
    void point_at_database(const std::shared_ptr<Job>& job, const std::filesystem::path& dir,
                           const std::string& project);
    [[nodiscard]] std::filesystem::path work_dir(std::int64_t id) const;

    std::filesystem::path data_dir_;
    BackupHooks hooks_;
    mutable std::mutex mutex_;
    std::map<std::int64_t, std::shared_ptr<Job>> jobs_;
    std::int64_t next_id_ = 1;
    bool stopping_ = false;
};

}  // namespace floserver::core

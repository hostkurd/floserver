#include "floserver/core/backup_service.h"

#include "floserver/core/build_info.h"
#include "floserver/core/child_process.h"
#include "floserver/core/paths.h"
#include "floserver/core/phpmyadmin.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/secret_store.h"
#include "floserver/core/shim.h"
#include "floserver/core/zip_extract.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kMaxRunningJobs = 2;
constexpr std::size_t kKeptFinishedJobs = 8;
constexpr auto kStepTimeLimit = 20min;
// A backup unpacks to at most this much (a project, its database included).
constexpr std::uint64_t kMaxRestoreBytes = std::uint64_t{64} * 1024 * 1024 * 1024;
constexpr std::uint64_t kMaxRestoreEntries = 2'000'000;

// The same folder, by Windows' rules on Windows (case doesn't count).
std::string folder_key(const fs::path& p) {
    const std::u8string generic = p.lexically_normal().generic_u8string();
    std::string text(generic.begin(), generic.end());
#ifdef _WIN32
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
#endif
    return text;
}

// A new project's folder must not be there, or be a real, empty folder.
bool folder_is_free(const fs::path& dir) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(dir, ec);
    if (status.type() == fs::file_type::not_found) {
        return true;
    }
    return !ec && status.type() == fs::file_type::directory && fs::is_empty(dir, ec) && !ec;
}

// `base`, or "<base> 2", "<base> 3"... whichever isn't there.
fs::path free_name(const fs::path& folder, const std::string& base, const std::string& extension) {
    std::error_code ec;
    for (int n = 1; n < 100; ++n) {
        const std::string name = base + (n == 1 ? std::string() : " " + std::to_string(n)) + extension;
        const fs::path p = folder / path_from_utf8(name);
        if (fs::symlink_status(p, ec).type() == fs::file_type::not_found) {
            return p;
        }
    }
    return folder / path_from_utf8(base + " " + std::to_string(std::time(nullptr)) + extension);
}

std::string shown_arg(const std::string& a) { return a.find(' ') == std::string::npos ? a : "\"" + a + "\""; }

// The work folder goes however the job ends.
class RemoveOnExit {
public:
    explicit RemoveOnExit(fs::path p) : path_(std::move(p)) {}
    RemoveOnExit(const RemoveOnExit&) = delete;
    RemoveOnExit& operator=(const RemoveOnExit&) = delete;
    RemoveOnExit(RemoveOnExit&&) = delete;
    RemoveOnExit& operator=(RemoveOnExit&&) = delete;
    ~RemoveOnExit() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

private:
    fs::path path_;
};

}  // namespace

struct BackupService::Job {
    std::int64_t id = 0;
    std::string kind;
    std::thread thread;
    std::atomic<bool> cancel{false};
    mutable std::mutex mutex;
    std::string state = "running";
    std::string step;
    std::deque<std::string> log;
    std::string project;
    std::string file;
    std::string folder;
    std::uint64_t files = 0;
    std::uint64_t total_files = 0;
    std::string database = "none";
    std::optional<std::uint64_t> size;
    std::string saved_database;
    std::string set_aside;
    std::string error;
    bool cancellable = true;

    void add_line(const std::string& line) {
        const std::lock_guard lock(mutex);
        log.push_back(line);
        while (log.size() > kLogLines) {
            log.pop_front();
        }
    }
    void set_step(const std::string& s) {
        const std::lock_guard lock(mutex);
        step = s;
        log.push_back(s);
        while (log.size() > kLogLines) {
            log.pop_front();
        }
    }
    void set(const std::function<void(Job&)>& change) {
        const std::lock_guard lock(mutex);
        change(*this);
    }
    void finish(const std::string& final_state, const std::string& message) {
        const std::lock_guard lock(mutex);
        state = final_state;
        error = message;
    }
    // Asked for, and still possible: the job ends as cancelled.
    bool cancelled() {
        const std::lock_guard lock(mutex);
        if (cancel && cancellable) {
            state = "cancelled";
            error = "cancelled";
            return true;
        }
        return false;
    }
};

BackupService::BackupService(fs::path data_dir, BackupHooks hooks)
    : data_dir_(std::move(data_dir)), hooks_(std::move(hooks)) {
    // Work folders a stopped service left (a dump may hold a database's data).
    std::error_code ignored;
    fs::remove_all(data_dir_ / "backups", ignored);
}

BackupService::~BackupService() { stop(); }

fs::path BackupService::work_dir(std::int64_t id) const {
    return data_dir_ / "backups" / ("job-" + std::to_string(id));
}

std::optional<std::int64_t> BackupService::start(const std::string& kind, const std::string& project,
                                                 const std::string& file,
                                                 const std::function<void(const std::shared_ptr<Job>&)>& work,
                                                 ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    if (stopping_) {
        refusal = {"stopping", "FloServer is stopping"};
        return std::nullopt;
    }
    std::size_t running = 0;
    std::vector<std::int64_t> finished;
    for (const auto& [id, j] : jobs_) {
        const std::lock_guard job_lock(j->mutex);
        if (j->state == "running") {
            ++running;
            if (j->project == project) {
                refusal = {"busy", project + " is being backed up or restored; try again when that's done"};
                return std::nullopt;
            }
        } else {
            finished.push_back(id);
        }
    }
    if (running >= kMaxRunningJobs) {
        refusal = {"busy", "two backups are already running; try again when one is done"};
        return std::nullopt;
    }
    while (finished.size() > kKeptFinishedJobs) {
        const auto it = jobs_.find(finished.front());
        if (it->second->thread.joinable()) {
            it->second->thread.join();
        }
        jobs_.erase(it);
        finished.erase(finished.begin());
    }
    auto job = std::make_shared<Job>();
    job->id = next_id_++;
    job->kind = kind;
    job->project = project;
    job->file = file;
    try {
        job->thread = std::thread([job, work] {
            try {
                work(job);
            } catch (const std::exception& e) {
                job->finish("failed", std::string("something went wrong: ") + e.what());
            } catch (...) {
                job->finish("failed", "something went wrong");
            }
        });
    } catch (const std::system_error&) {
        refusal = {"busy", "FloServer couldn't start the job; try again"};
        return std::nullopt;
    }
    jobs_[job->id] = job;
    return job->id;
}

namespace {

// A .zip to write or read: a full path with that extension.
bool zip_path_ok(const fs::path& p) {
    std::string ext = path_to_utf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    return p.is_absolute() && ext == ".zip" && p.has_filename();
}

bool is_regular(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && !ec;
}

}  // namespace

std::optional<std::int64_t> BackupService::backup(const BackupRequest& request, ServiceRefusal& refusal) {
    if (!zip_path_ok(request.to)) {
        refusal = {"bad_params", "the backup is a .zip file, named with its full path"};
        return std::nullopt;
    }
    std::string error;
    const auto project = hooks_.project ? hooks_.project(request.project, error) : std::nullopt;
    if (!project) {
        refusal = {"unknown_project", error.empty() ? "there's no project called " + request.project : error};
        return std::nullopt;
    }
    // Never inside the project itself: the zip would take itself in.
    const std::string to = folder_key(request.to);
    const std::string inside = folder_key(project->dir) + "/";
    if (to.starts_with(inside)) {
        refusal = {"bad_params", "the backup can't be saved inside the project's own folder"};
        return std::nullopt;
    }
    if (request.leave_out) {
        for (const std::string& f : *request.leave_out) {
            if (std::none_of(kLeavableFolders.begin(), kLeavableFolders.end(),
                             [&](const LeavableFolder& l) { return l.folder == f; })) {
                refusal = {"bad_params", "only vendor, node_modules and .venv can be left out"};
                return std::nullopt;
            }
        }
    }
    return start("backup", project->name, path_to_utf8(request.to),
                 [this, request](const std::shared_ptr<Job>& job) { run_backup(job, request); }, refusal);
}

std::optional<std::int64_t> BackupService::restore(const RestoreRequest& request, ServiceRefusal& refusal) {
    if (!zip_path_ok(request.from) || !is_regular(request.from)) {
        refusal = {"bad_params", "the backup must be an existing .zip file, named with its full path"};
        return std::nullopt;
    }
    if (!request.files && !request.database) {
        refusal = {"bad_params", "there's nothing to restore: neither its files nor its database was asked for"};
        return std::nullopt;
    }
    std::string error;
    const auto project = hooks_.project ? hooks_.project(request.project, error) : std::nullopt;
    if (!project) {
        refusal = {"unknown_project", error.empty() ? "there's no project called " + request.project : error};
        return std::nullopt;
    }
    const auto info = read_backup_info(request.from, error);
    if (!info) {
        refusal = {"not_a_backup", error};
        return std::nullopt;
    }
    if (request.database && !request.files && !info->database) {
        refusal = {"no_database", "this backup has no database to restore"};
        return std::nullopt;
    }
    return start("restore", project->name, path_to_utf8(request.from),
                 [this, request](const std::shared_ptr<Job>& job) { run_restore(job, request); }, refusal);
}

std::optional<std::int64_t> BackupService::create(const FromBackupRequest& request, ServiceRefusal& refusal) {
    if (const auto problem = project_name_problem(request.name)) {
        refusal = {"bad_name", "the name " + request.name + " can't be used: " + *problem};
        return std::nullopt;
    }
    if (reserved_project_name(request.name)) {
        refusal = {"bad_name", request.name + ".localhost is kept for phpMyAdmin; choose another name"};
        return std::nullopt;
    }
    if (!zip_path_ok(request.from) || !is_regular(request.from)) {
        refusal = {"bad_params", "the backup must be an existing .zip file, named with its full path"};
        return std::nullopt;
    }
    std::error_code ec;
    const std::string parent_text = path_to_utf8(request.parent);
    if (!request.parent.is_absolute() || parent_text.starts_with("\\\\") || parent_text.starts_with("//")) {
        refusal = {"bad_folder", "the folder to make it in must be a full path on this computer"};
        return std::nullopt;
    }
    fs::create_directories(request.parent, ec);
    if (!fs::is_directory(request.parent, ec)) {
        refusal = {"bad_folder", "can't make " + parent_text};
        return std::nullopt;
    }
    const fs::path dir = request.parent / path_from_utf8(request.name);
    if (!folder_is_free(dir)) {
        refusal = {"folder_exists", path_to_utf8(dir) + " already exists and isn't an empty folder"};
        return std::nullopt;
    }
    std::string error;
    if (hooks_.project && hooks_.project(request.name, error)) {
        refusal = {"name_taken", "there's already a project called " + request.name};
        return std::nullopt;
    }
    if (!read_backup_info(request.from, error)) {
        refusal = {"not_a_backup", error};
        return std::nullopt;
    }
    return start("create", request.name, path_to_utf8(request.from),
                 [this, request](const std::shared_ptr<Job>& job) { run_create(job, request); }, refusal);
}

std::optional<json> BackupService::job(std::int64_t id) const {
    std::shared_ptr<Job> j;
    {
        const std::lock_guard lock(mutex_);
        const auto it = jobs_.find(id);
        if (it == jobs_.end()) {
            return std::nullopt;
        }
        j = it->second;
    }
    const std::lock_guard lock(j->mutex);
    json out = {{"id", j->id},
                {"kind", j->kind},
                {"state", j->state},
                {"step", j->step},
                {"project", j->project},
                {"file", j->file},
                {"files", j->files},
                {"total_files", j->total_files},
                {"database", j->database},
                {"log", j->log},
                {"cancellable", j->cancellable && j->state == "running"}};
    if (j->size) {
        out["size"] = *j->size;
    }
    if (!j->folder.empty()) {
        out["folder"] = j->folder;
    }
    if (!j->saved_database.empty()) {
        out["saved_database"] = j->saved_database;
    }
    if (!j->set_aside.empty()) {
        out["set_aside"] = j->set_aside;
    }
    if (!j->error.empty()) {
        out["error"] = j->error;
    }
    return out;
}

bool BackupService::cancel(std::int64_t id) {
    const std::lock_guard lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return false;
    }
    const std::lock_guard job_lock(it->second->mutex);
    if (it->second->state != "running" || !it->second->cancellable) {
        return false;
    }
    it->second->cancel = true;
    return true;
}

bool BackupService::busy() const {
    const std::lock_guard lock(mutex_);
    return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& entry) {
        const std::lock_guard job_lock(entry.second->mutex);
        return entry.second->state == "running";
    });
}

void BackupService::stop() {
    std::vector<std::shared_ptr<Job>> jobs;
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& [id, j] : jobs_) {
            j->cancel = true;
            jobs.push_back(j);
        }
    }
    for (const auto& j : jobs) {
        if (j->thread.joinable()) {
            j->thread.join();
        }
    }
}

// ------------------------------------------------------------------ backup

void BackupService::run_backup(const std::shared_ptr<Job>& job, const BackupRequest& request) {
    std::string error;
    const auto project = hooks_.project ? hooks_.project(request.project, error) : std::nullopt;
    if (!project) {
        job->finish("failed", error.empty() ? "there's no project called " + request.project : error);
        return;
    }
    const std::vector<std::string> left_out = request.leave_out.value_or(leavable_folders(project->dir));
    job->set_step("Listing the files…");
    const auto contents = backup_contents(project->dir, left_out, error);
    if (!contents) {
        job->finish("failed", error);
        return;
    }
    job->set([&](Job& j) { j.total_files = contents->files.size(); });
    if (job->cancelled()) {
        return;
    }
    const fs::path work = work_dir(job->id);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const RemoveOnExit remove_work(work);

    // 1. The database, while the project keeps running.
    std::optional<fs::path> dump;
    const bool with_database = request.database && project->database;
    if (with_database) {
        job->set([](Job& j) { j.database = "saving"; });
        job->set_step("Saving the database…");
        if (!hooks_.dump_database || !hooks_.dump_database(project->name, work, job->cancel, error)) {
            if (!job->cancelled()) {
                job->finish("failed", "the database couldn't be saved: " + error);
            }
            return;
        }
        dump = work / "database.sql";
        job->set([](Job& j) { j.database = "saved"; });
    }

    // 2. The zip, as "<to>.part" until it's whole.
    const std::tm now = local_time_now();
    BackupInfo info;
    info.name = project->name;
    info.date = backup_date(now);
    info.made_by = kVersion;
    info.versions = backup_versions(project->dir);
    info.left_out = left_out;
    info.database = with_database;
    info.database_file = contents->database_file;
    fs::create_directories(request.to.parent_path(), ec);
    const fs::path part = request.to.parent_path() / path_from_utf8(path_to_utf8(request.to.filename()) + ".part");
    job->set_step("Packing the files…");
    bool cancelled = false;
    const bool written = write_backup_zip(
        part, *contents, info, dump,
        [&job](std::uint64_t done) {
            job->set([done](Job& j) { j.files = done; });
            return !job->cancel;
        },
        cancelled, error);
    if (!written) {
        fs::remove(part, ec);
        if (cancelled || job->cancelled()) {
            job->finish("cancelled", "cancelled");
        } else {
            job->finish("failed", error);
        }
        return;
    }
    // The finished zip takes the name (replacing one there: the user said so in the save dialog).
    fs::remove(request.to, ec);
    fs::rename(part, request.to, ec);
    if (ec) {
        job->finish("failed", "the backup was made as " + path_to_utf8(part) + " but couldn't be renamed: " +
                                  error_text(ec));
        return;
    }
    const auto size = fs::file_size(request.to, ec);
    job->set([&](Job& j) {
        j.size = ec ? 0 : size;
        j.files = j.total_files;
    });
    if (contents->links > 0) {
        job->add_line("Left out " + std::to_string(contents->links) + " links (they aren't followed)");
    }
    job->set_step("Done");
    job->finish("done", {});
}

// ------------------------------------------------------------------ shared steps

bool BackupService::reinstall(const std::shared_ptr<Job>& job, const fs::path& dir,
                              const std::vector<std::string>& left_out, std::string& error) {
    const fs::path work = work_dir(job->id);
    std::error_code ec;
    fs::create_directories(work, ec);
    const std::vector<ReinstallStep> steps = reinstall_steps(dir, left_out);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const ReinstallStep& step = steps[i];
        if (job->cancel) {
            error = "cancelled";
            return false;
        }
        const auto target = resolve_shim(step.tool, dir, data_dir_, error);
        if (!target) {
            error = step.tool + ": " + error;
            return false;
        }
        std::string shown = "$ " + step.tool;
        for (const std::string& a : step.args) {
            shown += " " + shown_arg(a);
        }
        job->set_step(shown);
        ChildOptions options;
        options.program = target->program;
        options.args = target->args;
        options.args.insert(options.args.end(), step.args.begin(), step.args.end());
        options.working_dir = dir;
        options.env = target->env;
        options.env.emplace_back("COMPOSER_NO_INTERACTION", "1");
        options.env.emplace_back("NO_COLOR", "1");
        options.env.emplace_back("npm_config_color", "false");
        options.output_log = work / ("step-" + std::to_string(i + 1) + ".log");
        auto child = ChildProcess::start(options, error);
        if (!child) {
            error = step.tool + " didn't start: " + error;
            return false;
        }
        const auto started = std::chrono::steady_clock::now();
        std::optional<int> code;
        while (!(code = child->wait(250ms))) {
            if (job->cancel) {
                child->kill();
                error = "cancelled";
                return false;
            }
            if (std::chrono::steady_clock::now() - started > kStepTimeLimit) {
                child->kill();
                error = step.shown + " ran for 20 minutes without finishing and was stopped";
                return false;
            }
        }
        // Its last lines, for the log.
        const auto text = read_small_file(options.output_log, std::size_t{4} * 1024 * 1024).value_or("");
        std::vector<std::string> lines;
        std::size_t from = 0;
        while (from < text.size()) {
            std::size_t nl = text.find('\n', from);
            if (nl == std::string::npos) {
                nl = text.size();
            }
            std::string line = text.substr(from, std::min<std::size_t>(nl - from, 1000));
            if (const std::size_t cr = line.rfind('\r'); cr != std::string::npos) {
                line.erase(0, cr + 1 == line.size() ? 0 : cr + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
            }
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
            from = nl + 1;
        }
        for (std::size_t k = lines.size() > 20 ? lines.size() - 20 : 0; k < lines.size(); ++k) {
            job->add_line(lines[k]);
        }
        if (*code != 0) {
            error = step.shown + " ended with exit code " + std::to_string(*code);
            return false;
        }
    }
    return true;
}

bool BackupService::load_from_backup(const std::shared_ptr<Job>& job, const fs::path& zip, const BackupInfo& info,
                                     const std::string& project, const fs::path& work, std::string& error) {
    job->set([](Job& j) { j.database = "loading"; });
    job->set_step("Loading the database from the backup…");
    const fs::path raw = work / "backup.sql";
    const fs::path ready = work / "load.sql";
    if (!extract_zip_entry(zip, info.database_file, raw, error)) {
        return false;
    }
    if (!prepare_sql_for_load(raw, ready, project_database_name(project), error)) {
        return false;
    }
    std::error_code ec;
    fs::remove(raw, ec);
    // Never cancelled halfway: an empty database is worse than waiting.
    static const std::atomic<bool> never{false};
    if (!hooks_.load_database || !hooks_.load_database(project, ready, never, error)) {
        return false;
    }
    job->set([](Job& j) { j.database = "loaded"; });
    return true;
}

void BackupService::point_at_database(const std::shared_ptr<Job>& job, const fs::path& dir,
                                      const std::string& project) {
    std::string error;
    const bool has_env = is_regular(dir / ".env");
    const bool has_wp = is_regular(dir / "wp-config.php");
    if (!has_env && !has_wp) {
        return;
    }
    const auto login = hooks_.database_login ? hooks_.database_login(project, error) : std::nullopt;
    if (!login) {
        job->add_line("The database settings weren't updated: " + error);
        return;
    }
    if (has_env) {
        const auto text = read_small_file(dir / ".env", std::size_t{1024} * 1024);
        if (text && write_private_file_atomically(dir / ".env", update_env_text(*text, env_values(env_style(dir), *login)),
                                                  error)) {
            job->add_line("Wrote the database lines into .env");
        } else {
            job->add_line(".env couldn't be updated" + (error.empty() ? std::string() : ": " + error));
        }
    }
    if (has_wp) {
        const auto text = read_small_file(dir / "wp-config.php", std::size_t{1024} * 1024);
        const auto updated = text ? update_wp_config_text(*text, *login) : std::nullopt;
        if (updated && write_private_file_atomically(dir / "wp-config.php", *updated, error)) {
            job->add_line("Wrote the database lines into wp-config.php");
        } else {
            job->add_line("wp-config.php couldn't be updated" + (error.empty() ? std::string() : ": " + error));
        }
    }
}

namespace {

// Unpacks a backup's files (not its own two) beside `dir`; the folder it
// made, or nothing. `progress` gets each file done.
std::optional<fs::path> unpack_beside(const fs::path& zip, const BackupInfo& info, const fs::path& dir,
                                      std::int64_t id, const std::function<bool(std::uint64_t, std::uint64_t)>& progress,
                                      bool& cancelled, std::string& error) {
    const fs::path temp = dir.parent_path() / path_from_utf8("." + path_to_utf8(dir.filename()) + ".restoring-" +
                                                             std::to_string(id));
    std::error_code ec;
    fs::remove_all(temp, ec);
    ZipLimits limits;
    limits.max_entries = kMaxRestoreEntries;
    limits.max_total_bytes = kMaxRestoreBytes;
    ZipOptions options;
    options.drop_top_folder = false;
    options.unicode_names = true;
    options.skip = {std::string(kBackupInfoFile)};
    if (info.database) {
        options.skip.push_back(info.database_file);
    }
    options.progress = progress;
    const ZipResult r = extract_zip(zip, temp, limits, options);
    if (!r.ok) {
        fs::remove_all(temp, ec);
        cancelled = r.cancelled;
        // A backup of an empty folder has nothing but its own files.
        if (!r.cancelled && r.message == "the archive has no files") {
            fs::create_directories(temp, ec);
            return temp;
        }
        error = "the backup can't be unpacked: " + r.message;
        return std::nullopt;
    }
    return temp;
}

}  // namespace

// ------------------------------------------------------------------ restore

void BackupService::run_restore(const std::shared_ptr<Job>& job, const RestoreRequest& request) {
    std::string error;
    const auto project = hooks_.project ? hooks_.project(request.project, error) : std::nullopt;
    if (!project) {
        job->finish("failed", error.empty() ? "there's no project called " + request.project : error);
        return;
    }
    const auto info = read_backup_info(request.from, error);
    if (!info) {
        job->finish("failed", error);
        return;
    }
    const bool database = request.database && info->database && project->database;
    const fs::path work = work_dir(job->id);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const RemoveOnExit remove_work(work);
    const std::tm now = local_time_now();

    // It stops meanwhile; it starts again however the restore ends.
    const bool was_on = project->on;
    if (was_on && hooks_.set_on) {
        job->set_step("Stopping " + project->name + "…");
        if (!hooks_.set_on(project->name, false, error)) {
            job->finish("failed", project->name + " couldn't be stopped: " + error);
            return;
        }
    }
    struct StartAgain {
        const BackupHooks& hooks;
        std::string name;
        bool on;
        StartAgain(const BackupHooks& h, std::string n, bool o) : hooks(h), name(std::move(n)), on(o) {}
        StartAgain(const StartAgain&) = delete;
        StartAgain& operator=(const StartAgain&) = delete;
        StartAgain(StartAgain&&) = delete;
        StartAgain& operator=(StartAgain&&) = delete;
        ~StartAgain() {
            if (on && hooks.set_on) {
                std::string ignored;
                (void)hooks.set_on(name, true, ignored);
            }
        }
    } const start_again(hooks_, project->name, was_on);

    // 1. Today's database, kept next to the backup.
    if (database) {
        job->set([](Job& j) { j.database = "saving"; });
        job->set_step("Saving today's database…");
        if (!hooks_.dump_database || !hooks_.dump_database(project->name, work, job->cancel, error)) {
            if (!job->cancelled()) {
                job->finish("failed", "today's database couldn't be saved, so nothing was changed: " + error);
            }
            return;
        }
        const fs::path kept = free_name(request.from.parent_path(), project->name + " before restore " + backup_stamp(now), ".sql");
        fs::copy_file(work / "database.sql", kept, ec);
        if (ec) {
            job->finish("failed", "today's database couldn't be kept next to the backup (" + path_to_utf8(kept) +
                                      "), so nothing was changed: " + error_text(ec));
            return;
        }
        fs::remove(work / "database.sql", ec);
        job->set([&](Job& j) {
            j.saved_database = path_to_utf8(kept);
            j.database = "saved";
        });
        job->add_line("Today's database is kept in " + path_to_utf8(kept));
    }

    // 2. The files, unpacked beside the folder.
    std::optional<fs::path> unpacked;
    if (request.files) {
        job->set_step("Unpacking the files…");
        bool cancelled = false;
        unpacked = unpack_beside(
            request.from, *info, project->dir, job->id,
            [&job](std::uint64_t done, std::uint64_t total) {
                job->set([&](Job& j) {
                    j.files = done;
                    j.total_files = total;
                });
                return !job->cancel;
            },
            cancelled, error);
        if (!unpacked) {
            if (!job->cancelled()) {
                job->finish("failed", error + "; nothing was changed");
            }
            return;
        }
    }
    if (job->cancelled()) {
        if (unpacked) {
            fs::remove_all(*unpacked, ec);
        }
        return;
    }

    // From here on nothing is cancelled: it's replaced.
    job->set([](Job& j) { j.cancellable = false; });
    if (database) {
        if (!load_from_backup(job, request.from, *info, project->name, work, error)) {
            if (unpacked) {
                fs::remove_all(*unpacked, ec);
            }
            const std::string kept = [&] {
                const std::lock_guard lock(job->mutex);
                return job->saved_database;
            }();
            job->finish("failed", "the database couldn't be loaded from the backup: " + error +
                                      ". Today's database is in " + kept);
            return;
        }
    }
    if (unpacked) {
        job->set_step("Putting the files in place…");
        if (!folder_is_free(project->dir)) {
            // The window moves it to the Recycle Bin first; a folder still
            // there (flo restore) is renamed, never deleted.
            const fs::path aside =
                free_name(project->dir.parent_path(), path_to_utf8(project->dir.filename()) + " before restore " +
                                                          backup_stamp(now),
                          "");
            fs::rename(project->dir, aside, ec);
            if (ec) {
                fs::remove_all(*unpacked, ec);
                job->finish("failed", "the project's folder couldn't be moved aside (is a file in it open?): " +
                                          error_text(ec));
                return;
            }
            job->set([&](Job& j) { j.set_aside = path_to_utf8(aside); });
            job->add_line("Today's files are in " + path_to_utf8(aside));
        } else {
            fs::remove(project->dir, ec);  // an empty folder, or none
        }
        fs::rename(*unpacked, project->dir, ec);
        if (ec) {
            job->finish("failed", "the restored files are in " + path_to_utf8(*unpacked) +
                                      " but couldn't be renamed to the project's folder: " + error_text(ec));
            return;
        }
        job->set([&](Job& j) { j.folder = path_to_utf8(project->dir); });
    }
    if (database) {
        point_at_database(job, project->dir, project->name);
    }
    if (request.install && request.files) {
        if (!reinstall(job, project->dir, info->left_out, error)) {
            job->finish("failed", "the project is restored, but " + error);
            return;
        }
    }
    job->set_step("Done");
    job->finish("done", {});
}

// ------------------------------------------------------------------ new project

void BackupService::run_create(const std::shared_ptr<Job>& job, const FromBackupRequest& request) {
    std::string error;
    const auto info = read_backup_info(request.from, error);
    if (!info) {
        job->finish("failed", error);
        return;
    }
    const fs::path dir = request.parent / path_from_utf8(request.name);
    const std::string dir_text = path_to_utf8(dir);
    job->set([&](Job& j) { j.folder = dir_text; });
    const fs::path work = work_dir(job->id);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const RemoveOnExit remove_work(work);

    job->set_step("Unpacking the files…");
    bool cancelled = false;
    const auto unpacked = unpack_beside(
        request.from, *info, dir, job->id,
        [&job](std::uint64_t done, std::uint64_t total) {
            job->set([&](Job& j) {
                j.files = done;
                j.total_files = total;
            });
            return !job->cancel;
        },
        cancelled, error);
    if (!unpacked) {
        if (!job->cancelled()) {
            job->finish("failed", error);
        }
        return;
    }
    if (!folder_is_free(dir)) {
        fs::remove_all(*unpacked, ec);
        job->finish("failed", dir_text + " already exists and isn't an empty folder");
        return;
    }
    fs::remove(dir, ec);
    fs::rename(*unpacked, dir, ec);
    if (ec) {
        fs::remove_all(*unpacked, ec);
        job->finish("failed", "couldn't make " + dir_text + ": " + error_text(ec));
        return;
    }
    const std::string left_note = " The folder " + dir_text + " is left as it is.";
    // The packages before it's turned on (a Node project's own start would
    // run npm install too).
    if (request.install && !reinstall(job, dir, info->left_out, error)) {
        if (job->cancel) {
            job->finish("cancelled", "cancelled." + left_note);
        } else {
            job->finish("failed", error + "." + left_note);
        }
        return;
    }
    if (job->cancelled()) {
        return;
    }
    job->set([](Job& j) { j.cancellable = false; });
    job->set_step("Adding the project…");
    const auto name = hooks_.add_project ? hooks_.add_project(dir, request.name, error) : std::nullopt;
    if (!name) {
        job->finish("failed", "couldn't add the project: " + error + "." + left_note);
        return;
    }
    job->set([&](Job& j) { j.project = *name; });
    if (info->database) {
        if (!load_from_backup(job, request.from, *info, *name, work, error)) {
            job->finish("failed", "the project was made, but its database couldn't be loaded: " + error);
            return;
        }
        point_at_database(job, dir, *name);
    }
    job->set_step("Done");
    job->finish("done", {});
}

}  // namespace floserver::core

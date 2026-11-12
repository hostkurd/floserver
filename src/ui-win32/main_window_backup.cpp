// Backups in the window (D22, mockups approved 5 Nov): Back up as zip… and
// Restore from backup… in a project row's menu, From a backup… on New
// project's first page, and a .zip opened with FloServer. The dialogs are
// backup_dialogs.cpp's; the service does all the work as a job (a restore
// checks the backup before it touches today's files, and moves them aside
// itself), which a dialog of its own follows. As with removing a project, the dialogs run their own
// message loop: a quit or a new language waits for them (asking_).

#include "main_window.h"

#include "model/project_menu.h"
#include "model/view_model.h"

#include "floserver/core/backup.h"
#include "floserver/core/paths.h"

#include <algorithm>
#include <string_view>

namespace floserver::ui {

namespace {

constexpr std::string_view kPlanPurpose = "backup-plan\n";

}  // namespace

bool MainWindow::backup_busy() const {
    return backup_job_ || asking_ || quitting_ || !backup_planning_.empty() || !remove_checking_.empty();
}

void MainWindow::open_backup(std::string file) {
    open_backup_ = std::move(file);
    if (worker_ && snap_.link == Link::Connected) {
        PostMessageW(hwnd_, kBackupMessage, 0, 0);
    }
}

void MainWindow::start_backup(const std::string& project) {
    if (!worker_ || snap_.link != Link::Connected || leaving(project) || backup_busy()) {
        return;
    }
    // The folder's size is counted by the service, off this thread.
    backup_planning_ = project;
    dismiss_error();
    worker_->database_call("backup.plan", {{"name", project}}, std::string(kPlanPurpose) + project);
    SetCursor(LoadCursorW(nullptr, IDC_APPSTARTING));
}

void MainWindow::on_backup_answers(std::vector<Snapshot::DbAnswer>& answers) {
    for (auto it = answers.begin(); it != answers.end();) {
        if (it->method != "backup.plan" || !it->purpose.starts_with(kPlanPurpose)) {
            ++it;
            continue;
        }
        const std::string project = it->purpose.substr(kPlanPurpose.size());
        if (project == backup_planning_) {
            backup_planning_.clear();
            const auto plan = it->reply && it->error.empty() ? parse_backup_plan(*it->reply) : std::nullopt;
            if (plan) {
                // Not from inside the snapshot's handling: the dialog runs its own loop.
                backup_plan_ = plan;
                PostMessageW(hwnd_, kBackupMessage, 0, 0);
            } else {
                show_notice(fill(lang_, Str::BackupFailed, {project, it->error.empty() ? "no answer" : it->error}), true);
            }
        }
        it = answers.erase(it);
    }
}

void MainWindow::run_backup_steps() {
    if (asking_ || backup_job_ || quitting_) {
        return;  // tried again when they're done (run_backup_steps after each)
    }
    if (backup_plan_) {
        const BackupPlan plan = *backup_plan_;
        backup_plan_.reset();
        asking_ = true;
        backup_job_ = true;
        hover_ = {};
        pressed_ = {};
        InvalidateRect(hwnd_, nullptr, FALSE);
        const auto choice = ask_backup(hwnd_, lang_, plan);
        JobResult result;
        if (choice && worker_) {
            result = run_backup_job(hwnd_, lang_, "backup", plan.name, "backup.start",
                                    backup_start_params(plan, choice->file, choice->database, choice->leave_out));
        }
        asking_ = false;
        modal_closed();
        backup_job_ = false;
        if (choice) {
            show_backup_outcome("backup", plan.name, result);
        }
    } else if (!open_backup_.empty() && worker_ && snap_.link == Link::Connected) {
        const std::string file = std::exchange(open_backup_, {});
        start_from_backup(file);
    } else {
        return;
    }
    first_run_after_ask();
    if (quit_after_ask_) {
        quit_after_ask_ = false;
        quit();
        return;
    }
    // Whatever else waits.
    if (backup_plan_ || !open_backup_.empty()) {
        PostMessageW(hwnd_, kBackupMessage, 0, 0);
    }
}

void MainWindow::start_restore(const std::string& project) {
    if (!worker_ || snap_.link != Link::Connected || leaving(project) || backup_busy()) {
        return;
    }
    const auto found = std::find_if(snap_.rows.begin(), snap_.rows.end(),
                                    [&](const ProjectRow& r) { return r.name == project; });
    if (found == snap_.rows.end()) {
        return;
    }
    const ProjectRow row = *found;
    asking_ = true;
    hover_ = {};
    pressed_ = {};
    InvalidateRect(hwnd_, nullptr, FALSE);
    const auto choice = ask_restore(hwnd_, lang_, project, row_has_database(row) ? row_server_database(row) : std::string(),
                                    backups_folder());
    asking_ = false;
    modal_closed();
    first_run_after_ask();
    if (quit_after_ask_) {
        quit_after_ask_ = false;
        quit();
        return;
    }
    if (!choice || !worker_ || snap_.link != Link::Connected || leaving(project)) {
        return;
    }
    run_restore(project, *choice);
}

void MainWindow::run_restore(const std::string& project, const RestoreChoice& choice) {
    if (!worker_ || snap_.link != Link::Connected) {
        show_notice(fill(lang_, Str::RestoreFailed, {project, "FloServer isn't running"}), true);
        return;
    }
    asking_ = true;
    backup_job_ = true;
    const JobResult result = run_backup_job(hwnd_, lang_, "restore", project, "backup.restore",
                                            {{"name", project},
                                             {"file", choice.file},
                                             {"files", choice.files},
                                             {"database", choice.database},
                                             {"install", choice.install}});
    asking_ = false;
    modal_closed();
    backup_job_ = false;
    show_backup_outcome("restore", project, result);
}

void MainWindow::start_from_backup(const std::string& file) {
    if (!worker_ || snap_.link != Link::Connected || backup_busy()) {
        return;
    }
    FromBackupSetup setup;
    setup.file = file;
    setup.projects_dir = !snap_.projects_dir.empty() ? snap_.projects_dir : default_projects_parent();
    // Taken: a project of that name, or a folder of it where it would go.
    const std::vector<ProjectRow> rows = snap_.rows;
    const std::string dir = setup.projects_dir;
    setup.name_taken = [rows, dir](const std::string& name) {
        return std::any_of(rows.begin(), rows.end(), [&](const ProjectRow& r) { return r.name == name; }) ||
               GetFileAttributesW(win::widen(dir + "\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    asking_ = true;
    backup_job_ = true;
    hover_ = {};
    pressed_ = {};
    InvalidateRect(hwnd_, nullptr, FALSE);
    const auto choice = ask_from_backup(hwnd_, lang_, setup);
    JobResult result;
    if (choice && worker_) {
        result = run_backup_job(hwnd_, lang_, "create", choice->name, "projects.from_backup",
                                {{"file", choice->file}, {"name", choice->name}, {"install", choice->install}});
    }
    asking_ = false;
    modal_closed();
    backup_job_ = false;
    if (choice) {
        show_backup_outcome("create", choice->name, result);
        // Its panel opens once its row is listed.
        if (result.job.is_object() && result.job.value("state", std::string()) == "done") {
            pending_added_ = choice->name;
        }
    }
}

void MainWindow::show_backup_outcome(const std::string& kind, const std::string& project, const JobResult& result) {
    const BackupOutcome said = backup_outcome(kind, project, result.job, result.error, lang_);
    if (!said.show_file.empty()) {
        show_notice_with_file(said.text, said.show_file);
    } else {
        show_notice(said.text, said.error);
    }
    if (worker_) {
        worker_->refresh();
    }
}

void MainWindow::show_notice_with_file(std::string message, const std::string& file) {
    toast_file_ = file;
    toast_ = toast_with(toast_, std::move(message), ToastKind::Info, std::string(text(lang_, Str::BackupShowInFolder)));
    access_.notify(toast_.text, false);
    sync_toast_timer();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace floserver::ui

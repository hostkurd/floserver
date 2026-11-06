#include "model/backup_dialogs.h"

#include "model/folders_page.h"

#include "floserver/core/mariadb.h"

#include <algorithm>

namespace floserver::ui {

using nlohmann::json;

namespace {

std::string text_of(const json& j, const char* key) {
    return j.is_object() && j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string();
}

std::uint64_t number_of(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_number_integer()) {
        return 0;
    }
    const json& v = j.at(key);
    return v.is_number_unsigned() ? v.get<std::uint64_t>() : static_cast<std::uint64_t>(std::max<std::int64_t>(0, v.get<std::int64_t>()));
}

}  // namespace

std::optional<BackupPlan> parse_backup_plan(const json& reply) {
    if (!reply.is_object() || text_of(reply, "name").empty()) {
        return std::nullopt;
    }
    BackupPlan plan;
    plan.name = text_of(reply, "name");
    plan.path = text_of(reply, "path");
    plan.bytes = number_of(reply, "bytes");
    plan.complete = !reply.contains("complete") || !reply.at("complete").is_boolean() || reply.at("complete").get<bool>();
    plan.default_file = text_of(reply, "default_file");
    if (reply.contains("folders") && reply.at("folders").is_array()) {
        for (const json& f : reply.at("folders")) {
            if (!text_of(f, "name").empty()) {
                plan.folders.push_back({text_of(f, "name"), text_of(f, "tool"), number_of(f, "bytes")});
            }
        }
    }
    if (reply.contains("database") && reply.at("database").is_object() && !text_of(reply.at("database"), "name").empty()) {
        plan.database = text_of(reply.at("database"), "name");
        plan.database_bytes = number_of(reply.at("database"), "bytes");
    }
    return plan;
}

BackupForm backup_form(const BackupPlan& plan, Lang lang) {
    BackupForm f;
    f.title = fill(lang, Str::BackupTitle, {plan.name});
    f.save_as = std::string(text(lang, Str::BackupSaveAs));
    f.change = std::string(text(lang, Str::BackupChange));
    f.include = std::string(text(lang, Str::BackupInclude));
    if (plan.database) {
        f.database = fill(lang, Str::BackupDatabase, {*plan.database});
        f.database_detail = fill(lang, Str::BackupDatabaseDetail, {disk_size_text(plan.database_bytes)});
    }
    if (!plan.folders.empty()) {
        f.leave_out = std::string(text(lang, Str::BackupLeaveOut));
    }
    for (const BackupPlan::Folder& folder : plan.folders) {
        const Str detail = folder.name == "vendor"         ? Str::BackupVendor
                           : folder.name == "node_modules" ? Str::BackupNodeModules
                                                           : Str::BackupVenv;
        f.folders.emplace_back(is_rtl(lang) ? isolate_ltr(folder.name) : folder.name,
                               fill(lang, detail, {disk_size_text(folder.bytes)}));
    }
    f.back_up = std::string(text(lang, Str::BackupConfirm));
    f.cancel = std::string(text(lang, Str::WizardCancel));
    return f;
}

std::uint64_t backup_bytes(const BackupPlan& plan, bool database, const std::vector<bool>& leave_out) {
    std::uint64_t bytes = plan.bytes;
    for (std::size_t i = 0; i < plan.folders.size() && i < leave_out.size(); ++i) {
        if (leave_out[i]) {
            bytes -= std::min(bytes, plan.folders[i].bytes);
        }
    }
    if (database && plan.database) {
        bytes += plan.database_bytes;
    }
    return bytes;
}

std::string backup_total_text(const BackupPlan& plan, bool database, const std::vector<bool>& leave_out, Lang lang) {
    return fill(lang, plan.complete ? Str::BackupTotal : Str::BackupTotalAtLeast,
                {disk_size_text(backup_bytes(plan, database, leave_out))});
}

json backup_start_params(const BackupPlan& plan, const std::string& file, bool database,
                         const std::vector<bool>& leave_out) {
    json out = {{"name", plan.name}, {"to", file}, {"database", database && plan.database.has_value()}};
    json folders = json::array();
    for (std::size_t i = 0; i < plan.folders.size() && i < leave_out.size(); ++i) {
        if (leave_out[i]) {
            folders.push_back(plan.folders[i].name);
        }
    }
    out["leave_out"] = std::move(folders);
    return out;
}

std::string with_zip_extension(const std::string& file) {
    std::string end = file.size() >= 4 ? file.substr(file.size() - 4) : std::string();
    std::transform(end.begin(), end.end(), end.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
    return end == ".zip" ? file : file + ".zip";
}

std::string backup_versions_text(const json& versions) {
    std::string out;
    const auto add = [&out](const std::string& part) {
        out += (out.empty() ? "" : " · ") + part;
    };
    const std::string php = text_of(versions, "php");
    if (!php.empty() && php != "none" && php != "auto") {
        add(php == "newest" ? std::string("PHP") : "PHP " + php);
    }
    const std::string database = text_of(versions, "database");
    if (database.starts_with("mariadb-")) {
        add("MariaDB " + database.substr(8));
    } else if (database == "mariadb") {
        add("MariaDB");
    }
    if (const std::string node = text_of(versions, "node"); !node.empty()) {
        add("Node.js " + node);
    }
    if (const std::string python = text_of(versions, "python"); !python.empty()) {
        add("Python " + python);
    }
    return out;
}

std::string describe_backup(const BackupSummary& backup, Lang lang) {
    std::string out = fill(lang, Str::BackupDescribed, {backup.name, backup.date});
    const std::string versions = backup_versions_text(backup.versions);
    if (!versions.empty()) {
        out += " · " + (is_rtl(lang) ? isolate_ltr(versions) : versions);
    }
    if (backup.database) {
        out += " · " + std::string(text(lang, Str::BackupWithDatabase));
    }
    return out;
}

FromBackupForm from_backup_form(Lang lang) {
    FromBackupForm f;
    f.title = std::string(text(lang, Str::FromBackupTitle));
    f.backup = std::string(text(lang, Str::FromBackupBackup));
    f.choose = std::string(text(lang, Str::FromBackupChoose));
    f.name = std::string(text(lang, Str::FromBackupName));
    f.database_note = std::string(text(lang, Str::FromBackupDatabaseNote));
    f.install = std::string(text(lang, Str::BackupInstall));
    f.foreign = std::string(text(lang, Str::BackupForeignNote));
    f.create = std::string(text(lang, Str::FromBackupCreate));
    f.cancel = std::string(text(lang, Str::WizardCancel));
    return f;
}

std::string from_backup_where(const std::string& projects_dir, const std::string& name, Lang lang) {
    std::string folder = projects_dir;
    if (!folder.empty() && folder.back() != '\\' && folder.back() != '/') {
        folder += folder.find('\\') != std::string::npos ? '\\' : '/';
    }
    return fill(lang, Str::FromBackupWhere, {folder + name, name + ".localhost"});
}

RestoreForm restore_form(const std::string& project, const std::string& database, Lang lang) {
    RestoreForm f;
    f.title = fill(lang, Str::RestoreTitle, {project});
    f.backup = std::string(text(lang, Str::FromBackupBackup));
    f.choose = std::string(text(lang, Str::FromBackupChoose));
    f.files = std::string(text(lang, Str::RestoreFiles));
    f.files_note = std::string(text(lang, Str::RestoreFilesNote));
    if (!database.empty()) {
        f.database = fill(lang, Str::RestoreDatabase, {database});
        f.database_note = std::string(text(lang, Str::RestoreDatabaseNote));
    }
    f.install = std::string(text(lang, Str::BackupInstall));
    f.foreign = std::string(text(lang, Str::BackupForeignNote));
    f.note = std::string(text(lang, Str::RestoreNote));
    f.restore = std::string(text(lang, Str::RestoreConfirm));
    f.cancel = std::string(text(lang, Str::WizardCancel));
    return f;
}

std::string restore_backed_up(const BackupSummary& backup, Lang lang) {
    std::string out = fill(lang, Str::RestoreBackedUp, {backup.date});
    if (backup.database) {
        out += " · " + std::string(text(lang, Str::BackupWithDatabase));
    }
    return out;
}

std::string backup_job_title(const std::string& kind, const std::string& project, Lang lang) {
    const Str key = kind == "restore"  ? Str::RestoreProgressTitle
                    : kind == "create" ? Str::FromBackupProgressTitle
                                       : Str::BackupProgressTitle;
    return fill(lang, key, {project});
}

std::string backup_progress_text(const json& job, Lang lang) {
    const std::string kind = text_of(job, "kind");
    std::vector<std::string> parts;
    const std::uint64_t total = number_of(job, "total_files");
    if (total > 0) {
        parts.push_back(fill_pattern(text(lang, Str::BackupProgressFiles), false,
                                     {std::to_string(number_of(job, "files")), std::to_string(total)}));
    }
    const std::string database = text_of(job, "database");
    if (database == "saving") {
        parts.emplace_back(text(lang, Str::BackupDbSaving));
    } else if (database == "saved" && kind == "backup") {
        parts.emplace_back(text(lang, Str::BackupDbSaved));
    } else if (database == "loading") {
        parts.emplace_back(text(lang, Str::BackupDbLoading));
    }
    if (text_of(job, "step").starts_with("$ ")) {
        parts.emplace_back(text(lang, Str::BackupInstalling));
    }
    if (kind == "backup") {
        parts.emplace_back(text(lang, Str::BackupKeepsRunning));
    } else if (kind == "restore") {
        parts.emplace_back(text(lang, Str::RestoreStopped));
    }
    std::string out;
    for (const std::string& p : parts) {
        out += (out.empty() ? "" : " · ") + p;
    }
    return out;
}

std::optional<int> backup_progress_percent(const json& job) {
    const std::uint64_t total = number_of(job, "total_files");
    if (total == 0 || text_of(job, "step").starts_with("$ ")) {
        return std::nullopt;
    }
    return static_cast<int>(std::min<std::uint64_t>(100, number_of(job, "files") * 100 / total));
}

BackupOutcome backup_outcome(const std::string& kind, const std::string& project, const json& job, const std::string& error,
                       Lang lang) {
    BackupOutcome out;
    const std::string state = text_of(job, "state");
    const Str failed = kind == "restore" ? Str::RestoreFailed : kind == "create" ? Str::FromBackupFailed : Str::BackupFailed;
    if (!job.is_object() || (state != "done" && state != "cancelled")) {
        const std::string why = !error.empty() ? error : !text_of(job, "error").empty() ? text_of(job, "error") : "no answer";
        out.text = fill(lang, failed, {project, why});
        out.error = true;
        return out;
    }
    if (state == "cancelled") {
        const Str key = kind == "restore" ? Str::RestoreCancelled : kind == "create" ? Str::FromBackupFailed : Str::BackupCancelled;
        out.text = kind == "create" ? fill(lang, key, {project, text_of(job, "error")}) : fill(lang, key, {project});
        return out;
    }
    if (kind == "backup") {
        // The file's own name and its size, under the headline.
        std::string file = text_of(job, "file");
        const std::size_t slash = file.find_last_of("\\/");
        out.show_file = file;
        file = slash == std::string::npos ? file : file.substr(slash + 1);
        out.text = fill(lang, Str::BackupDone, {project}) + "\n" + (is_rtl(lang) ? isolate_ltr(file) : file) + " · " +
                   disk_size_text(number_of(job, "size"));
        return out;
    }
    if (kind == "restore") {
        const std::string kept = text_of(job, "saved_database");
        out.text = kept.empty() ? fill(lang, Str::RestoreDone, {project}) : fill(lang, Str::RestoreDoneKept, {project, kept});
        return out;
    }
    out.text = fill(lang, Str::FromBackupDone, {project});
    return out;
}

}  // namespace floserver::ui

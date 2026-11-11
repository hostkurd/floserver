#include "model/project_menu.h"

#include "floserver/core/mariadb.h"
#include "floserver/core/project_registry.h"

#include <algorithm>

namespace floserver::ui {

namespace {

// A database FloServer makes for the project: MariaDB's, or MySQL's (D28).
bool is_mariadb(const std::string& database) { return database.starts_with("mariadb") || database.starts_with("mysql"); }

}  // namespace

std::vector<MenuItem> project_menu(const ProjectRow& row, const ProjectMenuState& state, std::string_view editor) {
    // Only a served project's own local address is ever opened or copied.
    const bool address = is_openable_address(row.address) && row.state != RowState::Problem &&
                         row.state != RowState::NeedsDownload;
    const bool folder = !row.path.empty();
    const bool change = state.connected && !state.removing;
    return {
        {MenuCommand::OpenBrowser, Str::OpenInBrowser, address, false},
        {MenuCommand::OpenFolder, Str::SharedOpenFolder, folder, false},
        {MenuCommand::OpenTerminal, Str::MenuOpenTerminal, folder, false},
        // Choosing an editor needs no folder.
        {MenuCommand::OpenEditor, editor.empty() ? Str::ChooseEditor : Str::OpenInEditor, folder || editor.empty(),
         false},
        {MenuCommand::CopyAddress, Str::MenuCopyAddress, address, false},
        {},
        {MenuCommand::Toggle, state.on ? Str::MenuTurnOff : Str::MenuTurnOn, change, false},
        {MenuCommand::KeepRunning, Str::KeepRunning, change, state.keep},
        {},
        // No Project settings…: a click on the row opens its panel (approved 4 Nov).
        {MenuCommand::Rename, Str::RenameProject, change, false},
        // Backups (D22): its folder is what's backed up.
        {MenuCommand::BackUp, Str::MenuBackUp, change && folder, false},
        {MenuCommand::Restore, Str::MenuRestore, change && folder, false},
        {},
        {MenuCommand::Remove, Str::RemoveProject, change, false},
    };
}

std::vector<MenuItem> shared_menu(const SharedSites& shared) {
    const bool address = is_shared_address(shared.address) && shared.state != "needs_download" &&
                         shared.state != "problem";
    return {
        {MenuCommand::OpenBrowser, Str::OpenInBrowser, address, false},
        {MenuCommand::OpenFolder, Str::SharedOpenFolder, !shared.dir.empty(), false},
        {},
        {MenuCommand::Settings, Str::MenuSettings, true, false},
    };
}

std::optional<Glyph> menu_glyph(MenuCommand command) {
    switch (command) {
        case MenuCommand::OpenBrowser:
            return Glyph::Globe;
        case MenuCommand::OpenFolder:
            return Glyph::Folder;
        case MenuCommand::OpenTerminal:
            return Glyph::Terminal;
        case MenuCommand::OpenEditor:
            return Glyph::Code;
        case MenuCommand::CopyAddress:
            return Glyph::Copy;
        case MenuCommand::Toggle:
            return Glyph::Power;
        case MenuCommand::Remove:
            return Glyph::Trash;
        case MenuCommand::None:
        case MenuCommand::KeepRunning:  // a ticked item: its tick is its mark
        case MenuCommand::Settings:
        case MenuCommand::Rename:
        case MenuCommand::BackUp:  // no glyph of the panel's for them (yet)
        case MenuCommand::Restore:
            break;
    }
    return std::nullopt;
}

std::string menu_item_text(const MenuItem& item, Lang lang, std::string_view editor) {
    if (item.label == Str::OpenInEditor) {
        return fill(lang, item.label, {editor});
    }
    return std::string(text(lang, item.label));
}

bool row_has_database(const ProjectRow& row) { return is_mariadb(row.database) || is_mariadb(row.wants.database); }

std::string row_database_name(const ProjectRow& row) {
    return row.database_name.empty() ? core::project_database_name(row.name) : row.database_name;
}

namespace {

// What's wrong with `name` as the project's new name, or nothing. The
// service's own rules (project_registry.h); it checks them again.
std::optional<std::string> rename_problem(const ProjectRow& row, std::string_view name,
                                          const std::vector<ProjectRow>& rows, Lang lang) {
    if (name.empty()) {
        return std::string(text(lang, Str::RenameEmpty));
    }
    if (name.size() > core::kMaxProjectNameLength) {
        return std::string(text(lang, Str::RenameTooLong));
    }
    if (const auto problem = core::project_name_problem(name)) {
        return std::string(text(lang, name.starts_with("xn--") ? Str::RenameXn : Str::RenameRule));
    }
    if (name == "phpmyadmin") {
        return fill(lang, Str::RenameReserved, {name});
    }
    const bool taken = std::any_of(rows.begin(), rows.end(),
                                   [&](const ProjectRow& r) { return r.name == name && r.name != row.name; });
    if (taken) {
        return fill(lang, Str::RenameTaken, {name});
    }
    return std::nullopt;
}

}  // namespace

RenameQuestion rename_question(const ProjectRow& row, std::string_view typed, const std::vector<ProjectRow>& rows,
                               const FooterText& footer, Lang lang) {
    RenameQuestion q;
    q.window_title = "FloServer";
    q.title = fill(lang, Str::RenameTitle, {row.name});
    q.label = std::string(text(lang, Str::RenameNewName));
    q.rename = std::string(text(lang, Str::RenameConfirm));
    q.cancel = std::string(text(lang, Str::WizardCancel));
    const std::string name(typed);
    const auto problem = rename_problem(row, name, rows, lang);
    q.problem = problem.has_value();
    q.help = problem ? *problem : fill(lang, Str::RenameHelp, {new_project_address(name, footer)});
    q.can_rename = !problem && name != row.name;
    if (!row.path.empty()) {
        q.folder = std::string(text(lang, Str::RenameFolder));
        q.folder_note = fill(lang, Str::RenameFolderNote, {row.path, problem ? std::string("…") : name});
    }
    if (row_has_database(row)) {
        // Kept, unless it's one of MariaDB's own names (never made): the
        // service's rule (rename_project()).
        const std::string database = row_database_name(row);
        q.database_note =
            core::database_follows_rename(database)
                ? fill(lang, Str::RenameDatabaseFollows,
                       {database, problem ? std::string("…") : core::project_database_name(name)})
                : fill(lang, Str::RenameDatabaseNote, {database});
    }
    return q;
}

std::string rename_outcome(const std::string& old_name, const std::string& new_name, bool folder, Lang lang) {
    return fill(lang, folder ? Str::RenameDoneFolder : Str::RenameDone, {old_name, new_name});
}

RemoveQuestion remove_question(const ProjectRow& row, Lang lang, FolderOffer offer) {
    RemoveQuestion q;
    q.window_title = "FloServer";
    q.title = fill(lang, Str::RemoveTitle, {row.name});
    q.text = std::string(text(lang, Str::RemoveText));
    q.remove = std::string(text(lang, Str::RemoveConfirm));
    q.cancel = std::string(text(lang, Str::WizardCancel));
    if (row_has_database(row)) {
        // The name the service gives it; the service works it out again itself.
        q.drop = fill(lang, Str::RemoveDropDatabase, {row_database_name(row)});
    }
    if (offer == FolderOffer::Offer && !row.path.empty()) {
        q.trash = std::string(text(lang, Str::RemoveTrashFolder));
        q.folder = fill(lang, Str::RemoveFolderPath, {row.path});
    } else if (offer == FolderOffer::NoRecycleBin) {
        q.trash_note = std::string(text(lang, Str::RemoveNoRecycleBin));
    }
    return q;
}

RemoveOutcome remove_outcome(const std::string& project, bool database_dropped, FolderOutcome folder,
                             const std::string& reason, Lang lang) {
    switch (folder) {
        case FolderOutcome::Recycled:
            return {fill(lang, database_dropped ? Str::RemoveDoneTrashDatabase : Str::RemoveDoneTrash, {project}), false};
        case FolderOutcome::Deleted:
            return {fill(lang, Str::RemoveDoneDeleted, {project}), false};
        case FolderOutcome::Gone:
            return {fill(lang, Str::RemoveDoneGone, {project}), false};
        case FolderOutcome::Stayed:
            // The project is removed; the folder is where it was, and why is said.
            return {fill(lang, Str::RemoveFolderStayed, {project, reason}), true};
        case FolderOutcome::Cancelled:
            return {fill(lang, Str::RemoveFolderCancelled, {project}), false};
        case FolderOutcome::NotAsked:
            break;
    }
    return {fill(lang, database_dropped ? Str::RemoveDoneDatabase : Str::RemoveDone, {project}), false};
}

}  // namespace floserver::ui

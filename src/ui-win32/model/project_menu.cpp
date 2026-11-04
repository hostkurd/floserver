#include "model/project_menu.h"

#include "floserver/core/mariadb.h"

namespace floserver::ui {

namespace {

bool is_mariadb(const std::string& database) { return database.starts_with("mariadb"); }

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
        {MenuCommand::Settings, Str::MenuProjectSettings, true, false},
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

std::string menu_item_text(const MenuItem& item, Lang lang, std::string_view editor) {
    if (item.label == Str::OpenInEditor) {
        return fill(lang, item.label, {editor});
    }
    return std::string(text(lang, item.label));
}

bool row_has_database(const ProjectRow& row) { return is_mariadb(row.database) || is_mariadb(row.wants.database); }

RemoveQuestion remove_question(const ProjectRow& row, Lang lang, FolderOffer offer) {
    RemoveQuestion q;
    q.window_title = "FloServer";
    q.title = fill(lang, Str::RemoveTitle, {row.name});
    q.text = std::string(text(lang, Str::RemoveText));
    q.remove = std::string(text(lang, Str::RemoveConfirm));
    q.cancel = std::string(text(lang, Str::WizardCancel));
    if (row_has_database(row)) {
        // The name the service gives it; the service works it out again itself.
        q.drop = fill(lang, Str::RemoveDropDatabase, {core::project_database_name(row.name)});
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

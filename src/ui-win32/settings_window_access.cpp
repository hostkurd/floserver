// The Settings window for screen readers (approved 5 Nov): what it draws,
// as a tree of elements (model/access_tree.h) that access_provider.cpp hands
// to UI Automation. The sidebar is a list of tabs, one per section; the
// section's page is a scrolling pane of cards, texts and controls. Each
// control is a focus stop of the keyboard model (focus_order()), so what a
// screen reader can do is what the keys can do; its role and state are
// said here, and its words are the ones the page draws inside it. The words
// come from the page's own text pass, run into a list instead of a bitmap
// (Canvas's record), so they're always what's on the screen. What a screen
// reader asks an element to do runs what a click on it runs, after the call
// has returned.

#include "settings_window.h"

#include "main_window_layout.h"
#include "model/main_access.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

namespace floserver::ui {

namespace {

// What a toast's close mark and the page are in AccessTarget::part (they aren't focus stops).
constexpr int kToastClosePart = 1000;
constexpr int kPagePart = 1001;
constexpr int kLineDip = 48;  // a scroll line: a runtime's row, as the mouse wheel's step
constexpr std::string_view kPageId = "page";

AccessRect ar(const RECT& r) { return {r.left, r.top, r.right, r.bottom}; }
bool is_empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

}  // namespace

bool SettingsWindow::ctrl_only() {
    // AltGr is Ctrl+Alt: AltGr+Tab isn't Ctrl+Tab.
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
}

void SettingsWindow::step_section(bool back) {
    const auto at = static_cast<std::size_t>(section_);
    const std::size_t next = back ? (at + kSections - 1) % kSections : (at + 1) % kSections;
    open_section(static_cast<Section>(next));
    if (hwnd_ == nullptr || static_cast<std::size_t>(section_) != next) {
        return;  // a Save question was answered with Cancel
    }
    focus_ = static_cast<int>(next);  // its entry in the sidebar
    show_focus_ = true;
    if (GetFocus() != hwnd_) {
        SetFocus(hwnd_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

UINT SettingsWindow::track_menu(HMENU menu, UINT flags, POINT at) {
    ++modal_;
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu, flags, at.x, at.y, hwnd_, nullptr));
    --modal_;
    modal_closed();
    return command;
}

// --- the focus across layouts ------------------------------------------------------

std::vector<std::string> SettingsWindow::focus_ids() const {
    std::vector<std::string> ids;
    for (const Hit& hit : focus_order()) {
        ids.push_back(access_id(hit));
    }
    return ids;
}

void SettingsWindow::keep_focus() {
    std::vector<std::string> now = focus_ids();
    if (section_ == focus_section_) {
        focus_ = keep_focus_index(focus_ids_, focus_, now, lost_focus_);
    } else {
        lost_focus_ = {};  // another section: what opening it chose stands
        focus_ = std::min(focus_, static_cast<int>(now.size()) - 1);
    }
    focus_section_ = section_;
    focus_ids_ = std::move(now);
}

void SettingsWindow::modal_closed() {
    // Requests that came while it was open run now, not at some later message.
    if (modal_ == 0 && !asking_ && !access_running_ && hwnd_ != nullptr && !access_queue_.empty()) {
        access_deferred_ = false;
        PostMessageW(hwnd_, kAccessMessage, 0, 0);
    }
}

// --- ids ---------------------------------------------------------------------------

std::string SettingsWindow::access_id(const Hit& hit) const {
    // By Part, in its order.
    static constexpr std::array<std::string_view, static_cast<std::size_t>(Part::GeneralRemove) + 1> kIds = {
        "none", "nav", "runtimes/check", "runtimes/picker", "runtimes/button",
        "php/version", "php/runtimes", "php/find", "php/ext", "php/more", "php/edit-ini", "php/final-ini",
        "php/field", "php/reset", "php/discard", "php/save",
        "ports/choice", "ports/http", "ports/https", "ports/use", "ports/switch", "ports/trust", "ports/remove",
        "ports/open-cert",
        "db/version", "db/runtimes", "db/start", "db/stop", "db/restart", "db/port", "db/keep", "db/final-ini",
        "db/edit-ini", "db/field", "db/reset", "db/root-show", "db/root-copy", "db/new-password", "db/set-password",
        "db/discard", "db/save", "db/phpmyadmin", "db/engine",
        "mail/install", "mail/inbox-link", "mail/copy-inbox", "mail/open-inbox", "mail/copy-smtp", "mail/switch",
        "mail/max", "mail/test", "mail/delete", "mail/discard", "mail/save",
        "apache/version", "apache/runtimes", "apache/find", "apache/module", "apache/more", "apache/final-conf",
        "apache/edit-conf", "apache/discard", "apache/save",
        "folders/projects/change", "folders/projects/open", "folders/projects/default", "folders/www/change",
        "folders/www/open", "folders/data/open", "folders/clear-logs", "folders/leftovers",
        "shared/switch", "shared/change", "shared/open", "shared/field", "shared/copy-env",
        "general/lang", "general/theme", "general/start", "general/start-mode", "general/close", "general/editor",
        "general/updates", "general/update-notes", "general/update-now", "general/licences", "general/remove"};
    static_assert(kIds[static_cast<std::size_t>(Part::PhpVersion)] == "php/version" &&
                      kIds[static_cast<std::size_t>(Part::PortsChoice)] == "ports/choice" &&
                      kIds[static_cast<std::size_t>(Part::DbVersion)] == "db/version" &&
                      kIds[static_cast<std::size_t>(Part::DbEngine)] == "db/engine" &&
                      kIds[static_cast<std::size_t>(Part::MailInstall)] == "mail/install" &&
                      kIds[static_cast<std::size_t>(Part::ApacheVersion)] == "apache/version" &&
                      kIds[static_cast<std::size_t>(Part::FoldersProjectsChange)] == "folders/projects/change" &&
                      kIds[static_cast<std::size_t>(Part::SharedSwitch)] == "shared/switch" &&
                      kIds[static_cast<std::size_t>(Part::GeneralLang)] == "general/lang",
                  "an id for each Part, in its order");
    const auto row = static_cast<std::size_t>(std::max(0, hit.row));
    const std::string part(kIds.at(static_cast<std::size_t>(hit.part)));
    switch (hit.part) {
        case Part::Nav:
            return "nav/" + std::to_string(hit.group);
        case Part::Picker:
            // By the runtime, not the card's place: the cards change with the list.
            if (hit.group >= 0 && std::cmp_less(hit.group, page_.groups.size())) {
                const RuntimeGroup& group = page_.groups[static_cast<std::size_t>(hit.group)];
                return "runtimes/" + (group.runtime.empty() ? group.title : group.runtime) + "/default";
            }
            return {};
        case Part::Button:
            if (const RuntimeRow* r = row_of(hit); r != nullptr) {
                return "runtime/" + r->name + "/" + r->version + "/button";
            }
            return {};
        case Part::PhpExt:
            return row < geometry_.php.ext_names.size() ? part + "/" + geometry_.php.ext_names[row] : std::string();
        case Part::ApacheModule:
            return row < geometry_.apache.names.size() ? part + "/" + geometry_.apache.names[row] : std::string();
        case Part::PhpField:
        case Part::PhpReset:
            return row < settings_php_ini_fields().size() ? part + "/" + settings_php_ini_fields()[row].key
                                                          : std::string();
        case Part::DbField:
        case Part::DbReset:
            return row < settings_db_ini_fields().size() ? part + "/" + settings_db_ini_fields()[row].key
                                                         : std::string();
        case Part::DbEngine:
            // By the engine: "db/engine/mysql".
            return row < db_engines().size() ? part + "/" + std::string(db_engines()[row].family) : std::string();
        default:
            break;
    }
    return hit.row >= 0 ? part + "/" + std::to_string(hit.row) : part;
}

std::string SettingsWindow::access_focus_id() const {
    if (hwnd_ == nullptr || GetFocus() != hwnd_ || focus_ < 0) {
        return {};  // a box has the keys: it says so itself
    }
    const std::vector<Hit> order = focus_order();
    if (std::cmp_greater_equal(focus_, order.size())) {
        return {};
    }
    const Hit& hit = order[static_cast<std::size_t>(focus_)];
    return edit_of(hit) != nullptr ? std::string() : access_id(hit);
}

// --- the focus stops ---------------------------------------------------------------------

AccessNode SettingsWindow::access_node(const Hit& hit) const {
    const auto row = static_cast<std::size_t>(std::max(0, hit.row));
    const auto button = [&](bool enabled) {
        AccessNode n = button_node(access_id(hit), {}, enabled);  // its words are the ones drawn on it
        n.focusable = true;
        return n;
    };
    switch (hit.part) {
        // Settings > Runtimes.
        case Part::Check:
            return button(!checking_ && connected_);
        case Part::Picker: {
            const RuntimeGroup& group = page_.groups[static_cast<std::size_t>(hit.group)];
            std::string value;
            if (group.picker) {
                const std::size_t shown = picker_value(group);
                if (shown < group.picker->choices.size()) {
                    value = group.picker->choices[shown].label;
                }
            }
            return combo_node(access_id(hit), access_join(lang_, {group.title, std::string(text(lang_, Str::SettingsDefaultFor))}),
                              value, connected_);
        }
        case Part::Button: {
            const RowGeometry& rg =
                geometry_.groups[static_cast<std::size_t>(hit.group)].rows[static_cast<std::size_t>(hit.row)];
            AccessNode n = button(rg.button_kind != Button::RemoveDisabled);
            if (const RuntimeRow* r = row_of(hit); r != nullptr && rg.button_kind == Button::RemoveDisabled) {
                n.help = access_text(fill_pattern(text(lang_, Str::SettingsUsedBy), false, {r->used_by}));
            }
            return n;
        }

        // Settings > PHP.
        case Part::PhpVersion: {
            std::string label = "PHP " + php_series_;
            for (const auto& v : php_versions()) {
                if (v.first == php_series_) {
                    label = "PHP " + v.second;
                }
            }
            return combo_node(access_id(hit), access_join(lang_, {"PHP", std::string(text(lang_, Str::PhpVersion))}),
                              label, !php_saving_);
        }
        case Part::PhpRuntimes:
        case Part::DbRuntimes:
        case Part::ApacheRuntimes:
            return button(connected_);
        case Part::PhpExt: {
            if (!php_ || row >= geometry_.php.ext_names.size()) {
                break;
            }
            const std::string& name = geometry_.php.ext_names[row];
            const auto it = std::find_if(php_->extensions.begin(), php_->extensions.end(),
                                         [&](const PhpExtension& e) { return e.name == name; });
            const bool on = extension_on(*php_, php_changes_, name);
            // One this install doesn't have can only be turned off.
            const bool can = !php_saving_ && !php_->extensions_fixed &&
                             (it == php_->extensions.end() || !it->missing || on);
            return check_node(access_id(hit), {}, on, can);
        }
        case Part::PhpMore:
        case Part::ApacheMore:
            return page_link_node(access_id(hit), {});
        case Part::PhpEditIni:
        case Part::PhpFinalIni:
        case Part::DbEditIni:
        case Part::DbFinalIni:
        case Part::ApacheEditConf:
        case Part::ApacheFinalConf:
            return button(connected_);
        case Part::PhpField: {
            // A picker (display_errors); the boxes are real controls.
            if (!php_ || row >= settings_php_ini_fields().size()) {
                break;
            }
            const std::string& key = settings_php_ini_fields()[row].key;
            const std::string value = ini_text(*php_, php_changes_, key);
            return combo_node(access_id(hit), key, value.empty() ? text(lang_, Str::PhpDefaultCue) : std::string_view(value),
                              !php_saving_);
        }
        case Part::PhpReset:
            if (row < settings_php_ini_fields().size()) {
                return page_link_node(access_id(hit), reset_name(settings_php_ini_fields()[row].key, lang_), !php_saving_);
            }
            break;
        case Part::PhpDiscard:
            return button(php_ && !php_changes_.empty() && !php_saving_);
        case Part::PhpSave:
            return button(php_ && !php_changes_.empty() && !php_saving_ && connected_);

        // Settings > Ports & HTTPS.
        case Part::PortsChoice:
            return radio_node(access_id(hit), {}, row == shown_port_choice());
        case Part::PortsUse:
            return button(connected_ && ports_waiting_ == 0);
        case Part::PortsSwitch: {
            AccessNode n = switch_node(access_id(hit), {}, shown_switch(hit.row), connected_, lang_);
            n.focusable = true;
            return n;
        }
        case Part::PortsTrust:
        case Part::PortsRemove:
            return button(connected_ && ports_waiting_ == 0);
        case Part::PortsOpenCert:
            return button(true);

        // Settings > Database.
        case Part::DbEngine: {
            // A tab per engine (D28); its help names the group.
            if (row >= db_engines().size()) {
                break;
            }
            AccessNode n = tab_node(access_id(hit), db_engines()[row].title, db_engines()[row].family == db_family_,
                                    static_cast<int>(row) + 1, static_cast<int>(db_engines().size()));
            n.help = access_text(text(lang_, Str::DbEngines));
            n.enabled = !db_saving_;
            return n;
        }
        case Part::DbVersion: {
            std::string label = db_title();
            for (const auto& v : db_versions()) {
                if (v.first == db_series_) {
                    label = db_server_title(db_family_, v.second);
                }
            }
            return combo_node(access_id(hit),
                              access_join(lang_, {db_engine_title(db_family_), std::string(text(lang_, Str::PhpVersion))}),
                              label, !db_saving_);
        }
        case Part::DbStart:
            return button(db_actions(db_server_).start && connected_);
        case Part::DbStop:
            return button(db_actions(db_server_).stop && connected_);
        case Part::DbRestart:
            return button(db_actions(db_server_).restart && connected_);
        case Part::DbKeep:
            return check_node(access_id(hit), {}, db_ && db_keep_running(*db_, db_changes_), !db_saving_);
        case Part::DbReset:
            if (row < settings_db_ini_fields().size()) {
                return page_link_node(access_id(hit), reset_name(settings_db_ini_fields()[row].label, lang_), !db_saving_);
            }
            break;
        case Part::DbRootShow:
            return button(connected_ || !root_shown_.empty());
        case Part::DbRootCopy:
        case Part::DbSetPassword:
            return button(connected_ && !root_busy_);
        case Part::DbPhpMyAdmin:
            return button(connected_ && !pma_busy_ && phpmyadmin_installed(installed_));
        case Part::DbDiscard:
            return button(!db_changes_.empty() && !db_saving_);
        case Part::DbSave:
            return button(!db_changes_.empty() && !db_saving_ && connected_);

        // Settings > Mail.
        case Part::MailInstall:
            return button(connected_ && !mailpit_installing());
        case Part::MailInboxLink: {
            AccessNode n = page_link_node(access_id(hit), {});
            n.focusable = true;
            return n;
        }
        case Part::MailCopyInbox:
        case Part::MailCopySmtp: {
            // Two Copy buttons: each says what it copies.
            AccessNode n = button_node(access_id(hit),
                                       access_join(lang_, {std::string(text(lang_, Str::DbCopy)),
                                                           std::string(text(lang_, hit.part == Part::MailCopyInbox
                                                                                       ? Str::MailInbox
                                                                                       : Str::MailSmtpServer))}));
            n.focusable = true;
            return n;
        }
        case Part::MailOpenInbox:
            return button(true);
        case Part::MailSwitch: {
            // Its words are the label and the note drawn in its row.
            const bool on = !mail_ ? false
                            : row == 0 ? mail_php(*mail_, mail_changes_)
                            : row == 1 ? mail_notices(*mail_, mail_changes_)
                                       : mail_keep_running(*mail_, mail_changes_);
            AccessNode n = switch_node(access_id(hit), {}, on, connected_ && !mail_saving_, lang_);
            n.focusable = true;
            return n;
        }
        case Part::MailTest:
        case Part::MailDelete:
            return button(connected_ && !mail_busy_);
        case Part::MailDiscard:
            return button(!mail_changes_.empty() && !mail_saving_);
        case Part::MailSave:
            return button(!mail_changes_.empty() && !mail_saving_ && connected_ && !mail_max_bad(mail_changes_));

        // Settings > Apache.
        case Part::ApacheVersion:
            return combo_node(access_id(hit),
                              access_join(lang_, {"Apache", std::string(text(lang_, Str::ApacheVersion))}),
                              apache_ ? apache_choice_label(*apache_, apache_shown_choice(*apache_, apache_changes_), lang_)
                                      : std::string(),
                              !apache_saving_);
        case Part::ApacheModule: {
            if (!apache_ || row >= geometry_.apache.names.size()) {
                break;
            }
            const std::string& name = geometry_.apache.names[row];
            const auto it = std::find_if(apache_->modules.begin(), apache_->modules.end(),
                                         [&](const ApacheModule& m) { return m.name == name; });
            const bool locked = it == apache_->modules.end() || apache_module_locked(*apache_, apache_changes_, *it);
            return check_node(access_id(hit), {}, apache_module_on(*apache_, apache_changes_, name),
                              !locked && !apache_saving_);
        }
        case Part::ApacheDiscard:
            return button(!apache_changes_.empty() && !apache_saving_);
        case Part::ApacheSave:
            return button(!apache_changes_.empty() && !apache_saving_ && connected_);

        // Settings > Folders.
        case Part::FoldersProjectsChange:
        case Part::FoldersProjectsDefault:
            return button(connected_ && !picking_folder_ && !folders_setting_);
        case Part::FoldersWwwChange:
            return button(connected_ && !picking_folder_);
        case Part::FoldersProjectsOpen:
        case Part::FoldersWwwOpen:
        case Part::FoldersDataOpen:
            return button(true);
        case Part::FoldersClearLogs:
            return button(connected_ && cleaning_.empty() && folders_ && folders_->logs_bytes > 0);
        case Part::FoldersLeftovers:
            return button(connected_ && cleaning_.empty() && folders_ && folders_->leftovers_bytes > 0);

        // Settings > Shared sites.
        case Part::SharedSwitch: {
            AccessNode n = switch_node(access_id(hit), text(lang_, Str::SharedSites), shown_shared_on(), connected_, lang_);
            n.focusable = true;
            return n;
        }
        case Part::SharedChange:
            return button(connected_ && !picking_folder_);
        case Part::SharedOpen:
            return button(true);
        case Part::SharedField:
            if (row < shared_page_.fields.size()) {
                const VersionField& f = shared_page_.fields[row];
                const std::size_t shown = shared_field_value(f);
                return combo_node(access_id(hit), f.label, shown < f.choices.size() ? f.choices[shown].label : std::string(),
                                  connected_);
            }
            break;
        case Part::SharedCopyEnv:
            return button(connected_ && !copying_env_);

        // Settings > General.
        case Part::GeneralLang:
            return radio_node(access_id(hit), {}, row < kLangChoices.size() && kLangChoices.at(row) == prefs_.lang);
        case Part::GeneralTheme:
            return radio_node(access_id(hit), {}, row < kThemeChoices.size() && kThemeChoices.at(row) == prefs_.theme);
        case Part::GeneralStart: {
            AccessNode n = switch_node(access_id(hit), {}, run_entry_.has_value(), true, lang_);
            n.focusable = true;
            return n;
        }
        case Part::GeneralStartMode:
            return radio_node(access_id(hit), {}, (row == 1) == shown_start_tray());
        case Part::GeneralClose:
            return radio_node(access_id(hit), {}, (row == 1) == (prefs_.close == CloseAction::Quit));
        case Part::GeneralEditor: {
            const std::vector<EditorRow> rows = editor_rows();
            if (row >= rows.size()) {
                break;
            }
            // Choose program… is on once a program is chosen.
            const bool on = rows[row].program ? prefs_.code_editor.kind == EditorChoice::Kind::Program
                                              : rows[row].choice == prefs_.code_editor;
            return radio_node(access_id(hit), {}, on);
        }
        case Part::GeneralUpdates:
            return button(can_check_updates());
        case Part::GeneralUpdateNotes:
        case Part::GeneralUpdateNow:
        case Part::GeneralLicences:
        case Part::GeneralRemove:
            return button(true);
        default:
            break;
    }
    return button(true);
}

// --- the parts that hold others ------------------------------------------------------------

void SettingsWindow::access_parts(std::vector<PageItem>& items) const {
    const auto card = [&](const std::string& id, const RECT& r, std::string_view name = {}) {
        if (is_empty(r)) {
            return;
        }
        PageItem item;
        item.node.id = id;
        item.node.role = AccessRole::Group;
        item.node.name = access_text(name);  // empty: its title's words
        item.node.bounds = ar(r);
        item.container = true;
        items.push_back(std::move(item));
    };
    const Geometry& g = geometry_;
    switch (section_) {
        case Section::General:
            card("general/card/language", g.general.lang_card);
            card("general/card/theme", g.general.theme_card);
            card("general/card/start", g.general.start_card);
            card("general/card/close", g.general.close_card);
            card("general/card/editor", g.general.editor_card);
            card("general/card/updates", g.general.updates_card);
            card("general/card/about", g.general.about_card);
            card("general/card/remove", g.general.remove_card);
            return;
        case Section::Php:
            if (php_) {
                card("php/card/extensions", g.php.ext_card);
                card("php/card/ini", g.php.ini_card);
            }
            return;
        case Section::Ports:
            if (ports_) {
                card("ports/card/web", g.ports.web_card);
                card("ports/card/https", g.ports.https_card);
                card("ports/card/ca", g.ports.ca_card);
            }
            return;
        case Section::Database:
            if (db_) {
                card("db/card/server", g.db.server_card);
                card("db/card/ini", g.db.ini_card);
                card("db/card/root", g.db.root_card);
            }
            return;
        case Section::Mail:
            if (mail_ && mail_->installed) {
                card("mail/card/addresses", g.mail.addr_card);
                card("mail/card/options", g.mail.opt_card);
                card("mail/card/emails", g.mail.emails_card);
            }
            return;
        case Section::Apache:
            if (apache_ && !apache_->versions.empty()) {
                card("apache/card/modules", g.apache.modules_card);
                card("apache/card/conf", g.apache.conf_card);
            }
            return;
        case Section::Folders:
            if (folders_) {
                card("folders/card/projects", g.folders.projects_card);
                card("folders/card/www", g.folders.www_card);
                card("folders/card/data", g.folders.data_card);
                card("folders/card/clean", g.folders.clean_card);
            }
            return;
        case Section::Shared:
            if (!is_empty(g.shared.toggle)) {
                card("shared/card", g.shared.card);
                card("shared/card/login", g.shared.db_box, shared_page_.login_title);
            }
            return;
        case Section::Runtimes:
            break;
    }
    // Runtimes: a card per group, a row per version with its name and state,
    // and a running download's progress bar.
    for (std::size_t i = 0; i < g.groups.size() && i < page_.groups.size(); ++i) {
        const GroupGeometry& gg = g.groups[i];
        const RuntimeGroup& group = page_.groups[i];
        const std::string id = "runtimes/" + (group.runtime.empty() ? group.title : group.runtime);
        card(id, gg.card, group.title);
        for (std::size_t r = 0; r < gg.rows.size() && r < group.rows.size(); ++r) {
            const RowGeometry& rg = gg.rows[r];
            const RuntimeRow& row = group.rows[r];
            const std::string row_id = "runtime/" + row.name + "/" + row.version;
            PageItem item;
            item.node.id = row_id;
            item.node.role = AccessRole::Group;
            // While it downloads, the moving words ("19 MB of 31 MB") are its help, not its
            // name: a name change would be said at each step.
            const bool moving = rg.button_kind == Button::Cancel;
            item.node.name = runtime_row_name(group.title, row, moving ? std::string() : rg.line, lang_);
            if (moving) {
                item.node.help = access_text(rg.line);
            }
            item.node.position = static_cast<int>(r) + 1;
            item.node.set_size = static_cast<int>(group.rows.size());
            item.node.bounds = ar(rg.row);
            item.container = true;
            item.absorbs = true;  // the version, the chip and the line are in its name
            items.push_back(std::move(item));
            if (rg.fraction >= 0 && !is_empty(rg.bar)) {
                PageItem bar;
                bar.node = progress_node(row_id + "/progress", runtime_row_label(group.title, row), rg.fraction);
                bar.node.help = access_text(rg.line);
                bar.node.bounds = ar(rg.bar);
                items.push_back(std::move(bar));
            }
        }
    }
}

// --- the words ---------------------------------------------------------------------------

std::vector<PageText> SettingsWindow::access_texts() const {
    std::vector<DrawnText> drawn;
    {
        Canvas record(drawn, width_, rtl_);
        record.begin_text();
        paint_text(record);
    }
    // The sidebar's labels are its tabs' names; the page starts at its column.
    const int column = main_left() - px(4);
    std::vector<PageText> out;
    out.reserve(drawn.size());
    for (const DrawnText& d : drawn) {
        if (d.rect.right <= column) {
            continue;
        }
        PageText t;
        t.rect = ar(d.rect);
        t.text = win::narrow(d.text);
        // Root's password, once shown: a fixed id, so its words never become one.
        if (section_ == Section::Database && d.rect.left == geometry_.db.root_value.left &&
            d.rect.top == geometry_.db.root_value.top && !is_empty(geometry_.db.root_value)) {
            t.id = "db/root-value";
        }
        t.style = d.font == fonts_.title.get()       ? TextStyle::Title
                  : d.font == fonts_.body_bold.get() ? TextStyle::Heading
                                                     : TextStyle::Body;
        out.push_back(std::move(t));
    }
    return out;
}

// --- the tree ----------------------------------------------------------------------------

AccessTree SettingsWindow::build_access_tree() {
    AccessTree t;
    if (hwnd_ == nullptr) {
        access_rects_.clear();
        t.finish();
        return t;
    }

    // The sidebar: one tab per section, the shown one selected.
    const int nav = t.add(-1, sections_node(lang_));
    for (std::size_t i = 0; i < kSections; ++i) {
        AccessNode n = section_node(static_cast<int>(i), text(lang_, kSettingsSidebar.at(i)),
                                    static_cast<std::size_t>(section_) == i, static_cast<int>(kSections));
        n.bounds = ar(geometry_.navs.at(i));
        t.add(nav, std::move(n));
    }

    // The section's page, scrolling as one.
    const RECT column{main_left() - px(4), 0, width_, height_};
    AccessNode page;
    page.id = std::string(kPageId);
    page.role = AccessRole::Pane;
    page.name = std::string(text(lang_, kSettingsSidebar.at(static_cast<std::size_t>(section_))));
    page.bounds = ar(column);
    page.scrollable = true;
    page.scroll_percent = scroll_percent(scroll_, geometry_.content_h, height_);
    page.scroll_view = scroll_view_size(geometry_.content_h, height_);
    page.target.part = kPagePart;
    const int page_index = t.add(-1, std::move(page));

    std::vector<PageItem> items;
    access_parts(items);
    for (const Hit& hit : focus_order()) {
        const RECT* r = rect_of(hit);
        // The boxes are real controls and say what they are themselves.
        if (hit.part == Part::Nav || r == nullptr || is_empty(*r) || edit_of(hit) != nullptr) {
            continue;
        }
        PageItem item;
        item.node = access_node(hit);
        if (item.node.id.empty()) {
            continue;
        }
        item.node.bounds = ar(*r);
        item.node.target.part = static_cast<int>(hit.part);
        item.absorbs = true;
        items.push_back(std::move(item));
    }
    access_rects_.clear();
    add_page(t, page_index, "page/", std::move(items), access_texts(), ar(column), lang_, &access_rects_);

    // A toast, where its popup is.
    if (!toast_.text.empty() && !is_empty(toast_geometry_.card)) {
        ToastNodes nodes = toast_nodes(toast_.text, lang_);
        nodes.group.bounds = ar(toast_geometry_.card);
        nodes.text.bounds = ar(toast_geometry_.text);
        nodes.close.bounds = ar(toast_geometry_.close);
        nodes.close.keys = toast_.kind == ToastKind::Error ? "Esc" : "";
        nodes.close.target.part = kToastClosePart;
        const int group = t.add(-1, std::move(nodes.group));
        t.add(group, std::move(nodes.text));
        t.add(group, std::move(nodes.close));
    }

    t.focus = access_focus_id();
    t.finish();
    return t;
}

// --- AccessSource --------------------------------------------------------------------------

const AccessTree& SettingsWindow::access_tree() {
    if (access_dirty_) {
        access_now_ = build_access_tree();
        access_dirty_ = false;
    }
    return access_now_;
}

void SettingsWindow::access_request(const std::string& id, AccessAction action) {
    const bool first = access_queue_.empty();
    access_queue_.push_back({id, action, -1});
    // While one runs (a menu or a question it opened is still open), the next waits for it.
    if (first && !access_running_ && hwnd_ != nullptr) {
        PostMessageW(hwnd_, kAccessMessage, 0, 0);
    }
}

void SettingsWindow::access_scroll_to(const std::string& id, double percent) {
    const bool first = access_queue_.empty();
    access_queue_.push_back({id, AccessAction::ScrollDown, std::clamp(percent, 0.0, 100.0)});
    if (first && !access_running_ && hwnd_ != nullptr) {
        PostMessageW(hwnd_, kAccessMessage, 0, 0);
    }
}

void SettingsWindow::access_after_paint() {
    // Only once a client has asked this window (not any client anywhere), and while one listens.
    if (!access_asked_ || !AccessRoot::listening()) {
        access_said_.reset();  // nobody to tell: nothing kept
        return;
    }
    access_now_ = build_access_tree();
    access_dirty_ = false;
    if (access_said_) {
        access_.raise(access_changes(*access_said_, access_now_));
    }
    access_said_ = access_now_;
}

void SettingsWindow::run_access_requests() {
    // Never inside another: a picker's menu and the Save question run
    // messages, and a second one must not open over them. Nor inside a menu
    // or question opened by the mouse or the keys: they wait for it to close.
    if (access_running_) {
        return;
    }
    if (modal_ > 0 || asking_) {
        access_deferred_ = true;
        return;
    }
    access_running_ = true;
    while (!access_queue_.empty()) {
        const AccessJob job = access_queue_.front();
        access_queue_.erase(access_queue_.begin());
        if (hwnd_ == nullptr) {
            access_queue_.clear();
            break;
        }
        // Found again in the window as it is now: it may have gone meanwhile.
        access_dirty_ = true;
        const AccessTree& tree = access_tree();
        const AccessNode* n = tree.node(tree.find(job.id));
        const bool moves = job.action == AccessAction::Focus || job.action == AccessAction::ScrollIntoView ||
                           job.action == AccessAction::ScrollUp || job.action == AccessAction::ScrollDown ||
                           job.action == AccessAction::PageUp || job.action == AccessAction::PageDown;
        if (n == nullptr || (!n->enabled && !moves)) {
            continue;
        }
        // A copy: a menu or question it opens runs messages, and the tree is made again under it.
        const AccessNode node = *n;
        run_access(node, job.action, job.percent);
    }
    access_running_ = false;
}

void SettingsWindow::run_access(const AccessNode& n, AccessAction action, double percent) {
    if (n.target.part == kToastClosePart) {
        toast_event(ToastEvent::Close);
        return;
    }
    if (n.target.part == kPagePart) {
        const int line = px(kLineDip);
        const int page = std::max(line, height_ - line);
        if (percent >= 0) {
            const int most = std::max(0, geometry_.content_h - height_);
            scroll_to(static_cast<int>(std::lround(percent * static_cast<double>(most) / 100.0)));
        } else if (action == AccessAction::ScrollUp || action == AccessAction::ScrollDown) {
            scroll_to(scroll_ + (action == AccessAction::ScrollUp ? -line : line));
        } else if (action == AccessAction::PageUp || action == AccessAction::PageDown) {
            scroll_to(scroll_ + (action == AccessAction::PageUp ? -page : page));
        }
        return;
    }
    // The focus stop it is, as the keys know it now.
    const std::vector<Hit> order = focus_order();
    const auto it = std::find_if(order.begin(), order.end(), [&](const Hit& h) { return access_id(h) == n.id; });
    if (action == AccessAction::ScrollIntoView) {
        if (it != order.end() && it->part != Part::Nav) {
            ensure_visible(*it);
        } else if (const auto r = access_rects_.find(n.id); r != access_rects_.end() && it == order.end()) {
            // A text or a card: its top in view (its whole when it fits).
            const AccessRect& want = r->second;
            if (want.top < px(8)) {
                scroll_to(scroll_ + want.top - px(8));
            } else if (want.bottom > height_ - px(8)) {
                scroll_to(scroll_ + std::min(want.bottom - height_ + px(8), want.top - px(8)));
            }
        }
        return;
    }
    if (it == order.end()) {
        return;
    }
    const Hit hit = *it;
    focus_ = static_cast<int>(it - order.begin());
    if (action == AccessAction::Focus) {
        show_focus_ = true;
        ensure_visible(hit);
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    // A Toggle on a checkbox or a switch, a Select on a radio button or a
    // section, an Expand on a picker and an Invoke on a button all do what
    // a click (and Enter) does.
    ensure_visible(hit);
    on_click(hit);
}

}  // namespace floserver::ui

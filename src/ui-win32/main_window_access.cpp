// The main window for screen readers (approved 5 Nov): what it draws, as a
// tree of elements (model/access_tree.h) that access_provider.cpp hands to
// UI Automation. The tree is made from the same geometry and view-model
// data the last paint used, and the window's keyboard focus model says which
// element has the focus. What a screen reader asks an element to do runs
// what a click or a key on it runs, after the call has returned.

#include "main_window.h"

#include "main_window_layout.h"
#include "model/main_access.h"
#include "model/pages_access.h"
#include "model/runtimes_page.h"
#include "model/view_model.h"

#include "floserver/core/mariadb.h"
#include "floserver/core/project_config.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <tuple>
#include <utility>

namespace floserver::ui {

using namespace layout;
namespace core = floserver::core;

namespace {

constexpr int kLogLineH = 17;  // as main_window_panel.cpp draws the log
// What a toast's close mark is in AccessTarget::part (it isn't one of the window's Parts).
constexpr int kToastClosePart = 1000;
constexpr int kToastActionPart = 1001;  // its button ("Show in folder")

AccessRect ar(const RECT& r) { return {r.left, r.top, r.right, r.bottom}; }
bool shown(const RECT& r) { return r.right > r.left && r.bottom > r.top; }

std::string_view tab_word(Lang lang, PanelTab tab) {
    switch (tab) {
        case PanelTab::Php:
            return text(lang, Str::FieldPhp);
        case PanelTab::Database:
            return text(lang, Str::FieldDatabase);
        case PanelTab::Log:
            return text(lang, Str::Log);
        case PanelTab::General:
            break;
    }
    return text(lang, Str::PanelTabGeneral);
}

}  // namespace

// --- ids ---------------------------------------------------------------------------

std::string MainWindow::access_id(const Hit& hit) const {
    const std::string panel = "panel/" + hit.project + "/";
    const auto index = static_cast<std::size_t>(std::max(0, hit.row));
    switch (hit.part) {
        case Part::Row:
            return row_id(hit.project);
        case Part::Switch:
            return row_part_id(hit.project, "switch");
        case Part::Address:
            return row_part_id(hit.project, "address");
        case Part::Download:
            return row_part_id(hit.project, "download");
        case Part::NewProject:
            return "top/new";
        case Part::Settings:
            return "top/settings";
        case Part::Banner:
            return "status";
        case Part::MailLink:
            return "mail/link";
        case Part::UpdateNotes:
            return "update/notes";
        case Part::UpdateCancel:
            return "update/cancel";
        case Part::UpdateLater:
            return "update/later";
        case Part::UpdateNow:
            return "update/now";
        case Part::PanelClose:
            return panel + "close";
        case Part::PanelAddress:
            return panel + "address";
        case Part::PanelTab:
            // By the tab, not its place: the bar changes with the project.
            return index < panel_geometry_.tabs.size()
                       ? panel + "tab/" + std::to_string(static_cast<int>(panel_geometry_.tabs[index].second))
                       : std::string();
        case Part::PanelButton:
            return index < panel_geometry_.buttons.size()
                       ? panel + "button/" + std::to_string(static_cast<int>(panel_geometry_.buttons[index].second))
                       : std::string();
        case Part::PanelField:
            return index < fields_.size() ? panel + "field/" + fields_[index].key : std::string();
        case Part::PanelCopyEnv:
            return panel + "copy-env";
        case Part::PanelPhpMyAdmin:
            return panel + "phpmyadmin";
        case Part::PanelKeep:
            return panel + "keep";
        case Part::PanelRemove:
            return panel + "remove";
        case Part::PanelPhpSettings:
            return panel + "php/settings";
        case Part::PanelPhpChange:
            return panel + "php/change";
        case Part::PanelPhpField:
            return panel + "php/field/" + std::to_string(index);
        case Part::PanelPhpReset:
            return panel + "php/reset";
        case Part::PanelPhpDiscard:
            return panel + "php/discard";
        case Part::PanelPhpSave:
            return panel + "php/save";
        case Part::SharedRow:
            return "shared";
        case Part::SharedSwitch:
            return "shared/switch";
        case Part::SharedEnd:
            return "shared/end";
        case Part::SharedOpen:
            return "shared/open";
        case Part::SharedGear:
            return "shared/gear";
        // New project.
        case Part::WizCard:
            return index < snap_.templates.size() ? "wizard/card/" + snap_.templates[index].id : std::string();
        case Part::WizField:
            return index < wizard_.fields.size() ? "wizard/field/" + wizard_.fields[index].key : std::string();
        case Part::WizBrowse:
            return "wizard/browse";
        case Part::WizAddFolder:
            return "wizard/add-folder";
        case Part::WizFromBackup:
            return "wizard/from-backup";
        case Part::WizCancel:
            return "wizard/cancel";
        case Part::WizBack:
            return "wizard/back";
        case Part::WizNext:
            return "wizard/next";
        // The first-run page.
        case Part::FrLang:
            return "first-run/lang/" + std::to_string(index);
        case Part::FrTick:
            return index < first_run_.offers.size() ? "first-run/offer/" + first_run_.offers[index].name : std::string();
        case Part::FrCancel:
            // The rows are first_run_.wanted's, in its order.
            return index < first_run_.wanted.size() ? "first-run/download/" + first_run_.wanted[index].name + "/cancel"
                                                     : std::string();
        case Part::FrRetry:
            return "first-run/retry";
        case Part::FrChange:
            return "first-run/change";
        case Part::FrCreate:
            return "first-run/create";
        case Part::FrAdd:
            return "first-run/add";
        case Part::FrBack:
            return "first-run/back";
        case Part::FrNext:
            return "first-run/next";
        case Part::FrSkipSetup:
            return "first-run/skip";
        case Part::FrWithout:
            return "first-run/without";
        default:
            break;
    }
    return {};  // the name box is a real EDIT: Windows says when it has the focus
}

std::string MainWindow::access_focus_id() const {
    if (hwnd_ == nullptr || GetFocus() != hwnd_) {
        return {};
    }
    if (first_run_open()) {
        return first_run_focus_index() >= 0 ? access_id(first_run_.focus) : std::string();
    }
    if (wizard_open()) {
        return access_id(wizard_focus());
    }
    if (mail_keys()) {
        return "mail/link";
    }
    if (update_focus_ >= 0) {
        const std::vector<Part> order = update_focus_order();
        return std::cmp_less(update_focus_, order.size())
                   ? access_id({order[static_cast<std::size_t>(update_focus_)], -1, {}})
                   : std::string();
    }
    if (panel_open() && panel_focus_ >= 0) {
        const std::vector<Hit> order = panel_focus_order();
        if (std::cmp_less(panel_focus_, order.size())) {
            return access_id(order[static_cast<std::size_t>(panel_focus_)]);
        }
        return {};
    }
    if (shared_focused_ && width_of(shared_geometry_.row) > 0) {
        const std::vector<Part> order = shared_focus_order();
        const int at = std::clamp(shared_part_, 0, static_cast<int>(order.size()) - 1);
        return access_id({order[static_cast<std::size_t>(at)], -1, {}});
    }
    if (focus_ >= 0 && std::cmp_less(focus_, visible_.size())) {
        return row_id(snap_.rows[visible_[static_cast<std::size_t>(focus_)]].name);
    }
    return {};
}

// --- the tree ----------------------------------------------------------------------

AccessTree MainWindow::build_access_tree() const {
    AccessTree t;
    const auto target = [](Part part, int index, const std::string& project) {
        return AccessTarget{static_cast<int>(part), index, project};
    };
    const BannerText status = status_line();

    if (first_run_open()) {
        // The service's state at the page's top, then the page.
        if (!status.text.empty() && shown(banner_rect())) {
            AccessNode n = status_node(status.text, status.clickable);
            n.bounds = ar(banner_rect());
            n.keys = status.clickable ? "F5" : "";
            n.target = target(Part::Banner, -1, {});
            t.add(-1, std::move(n));
        }
        add_first_run_access(t);
    } else {
        // The top bar (the search box is a real EDIT and says what it is itself).
        const RECT search = search_box();
        AccessNode title = text_node("top/title", std::string(text(lang_, Str::Projects)));
        title.bounds = ar(RECT{px(kPad), 0, search.left - px(kGap), px(kTopBarH)});
        t.add(-1, std::move(title));
        AccessNode new_project = button_node("top/new", std::string(text(lang_, Str::AccessNewProject)),
                                     snap_.link == Link::Connected && !wizard_running());
        new_project.bounds = ar(new_project_button());
        new_project.keys = "Ctrl+N";
        new_project.target = target(Part::NewProject, -1, {});
        t.add(-1, std::move(new_project));
        AccessNode settings = button_node("top/settings", std::string(text(lang_, Str::Settings)));
        settings.bounds = ar(settings_button());
        settings.keys = "Ctrl+,";
        settings.target = target(Part::Settings, -1, {});
        t.add(-1, std::move(settings));

        if (wizard_open()) {
            add_wizard_access(t);
        } else {
            add_update_access(t);
            add_list_access(t);
            add_shared_access(t);
            add_panel_access(t);
        }

        // The status line: the service's state (Stopped starts it), else the counts.
        if (!status.text.empty()) {
            const RECT f = footer_rect();
            AccessNode n = status_node(status.text, status.clickable);
            n.bounds = ar(RECT{px(kPad), f.top, f.right - px(kPad), f.bottom});
            n.keys = status.clickable ? "F5" : "";
            n.target = target(Part::Banner, -1, {});
            t.add(-1, std::move(n));
        } else if (shows_rows() && snap_.link == Link::Connected) {
            AccessNode n = footer_node(snap_.footer, lang_);
            n.bounds = ar(footer_rect());
            t.add(-1, std::move(n));
            // Mail · N new (D27): a button on the status line, once Mailpit is installed.
            if (mail_link_visible()) {
                AccessNode mail = mail_link_node(mail_link_text(*snap_.mail, lang_), lang_);
                mail.bounds = ar(mail_link_rect());
                mail.target = target(Part::MailLink, -1, {});
                t.add(-1, std::move(mail));
            }
        }
    }

    // A toast, where its popup is.
    if (!toast_.text.empty() && shown(toast_geometry_.card)) {
        ToastNodes nodes = toast_nodes(toast_.mail ? access_join(lang_, {toast_.text, toast_.detail}) : toast_.text,
                                       lang_, toast_.action);
        nodes.group.bounds = ar(toast_geometry_.card);
        nodes.text.bounds = ar(toast_geometry_.text);
        nodes.close.bounds = ar(toast_geometry_.close);
        nodes.close.keys = toast_.kind == ToastKind::Error || !toast_.action.empty() ? "Esc" : "";
        nodes.close.target = AccessTarget{kToastClosePart, -1, {}};
        const int group = t.add(-1, std::move(nodes.group));
        t.add(group, std::move(nodes.text));
        const bool action = !nodes.action.name.empty() && shown(toast_geometry_.action);
        if (action) {
            nodes.action.bounds = ar(toast_geometry_.action);
            nodes.action.target = AccessTarget{kToastActionPart, -1, {}};
        }
        t.add(group, std::move(nodes.close));
        if (action) {
            t.add(group, std::move(nodes.action));
        }
    }

    t.focus = access_focus_id();
    t.finish();
    return t;
}

void MainWindow::add_list_access(AccessTree& t) const {
    const RECT list_r = list_rect();
    AccessNode list;
    list.id = "list";
    list.role = AccessRole::List;
    list.name = std::string(text(lang_, Str::Projects));
    list.bounds = ar(list_r);
    const int list_index = t.add(-1, std::move(list));
    if (geometry_.empty()) {
        if (shows_rows()) {
            // Why it's empty, as the card says.
            const std::string message =
                snap_.rows.empty() ? fill_pattern(text(lang_, Str::NoProjects), false, {text(lang_, Str::NewProject)})
                                   : fill_pattern(text(lang_, Str::NoMatches), false, {isolate_auto(query_)});
            AccessNode n = text_node("list/empty", message);
            n.bounds = ar(list_r);
            t.add(list_index, std::move(n));
        }
        return;
    }
    const AccessRect clip = ar(list_r);
    const int count = static_cast<int>(geometry_.size());
    for (std::size_t i = 0; i < geometry_.size(); ++i) {
        const RowGeometry& g = geometry_[i];
        const auto found = std::find_if(snap_.rows.begin(), snap_.rows.end(),
                                        [&](const ProjectRow& r) { return r.name == g.project; });
        if (found == snap_.rows.end()) {
            continue;
        }
        const ProjectRow& row = *found;
        const int index = static_cast<int>(i);
        RowAccessState s;
        s.on = switch_value(row);
        // One at most: the panel's row, else a right-click's highlight.
        s.selected = panel_open() ? row.name == selected_ : row.name == highlighted_;
        s.can_change = snap_.link == Link::Connected && !leaving(row.name);
        s.end = g.end == RowGeometry::End::Download   ? RowEndKind::Download
                : g.end == RowGeometry::End::Progress ? RowEndKind::Progress
                                                      : RowEndKind::State;
        s.end_text = g.end_text;
        s.position = index + 1;
        s.count = count;
        // Scrolled out of the card: there, but off screen.
        const auto place = [&](AccessNode& n, const RECT& r) {
            const AccessRect cut = clip_rect(ar(r), clip);
            n.offscreen = cut.empty();
            n.bounds = n.offscreen ? ar(r) : cut;
        };
        AccessNode item = project_row_node(row, s, lang_);
        place(item, g.row);
        item.target = {static_cast<int>(Part::Row), index, row.name};
        const int item_index = t.add(list_index, std::move(item));

        AccessNode sw = switch_node(row_part_id(row.name, "switch"), row.name, s.on, s.can_change, lang_);
        place(sw, g.toggle);
        sw.keys = "Space";
        sw.target = {static_cast<int>(Part::Switch), index, row.name};
        t.add(item_index, std::move(sw));
        if (shown(g.address)) {
            AccessNode link = link_node(row_part_id(row.name, "address"), row.address);
            place(link, g.address);
            link.target = {static_cast<int>(Part::Address), index, row.name};
            t.add(item_index, std::move(link));
        }
        if (g.end == RowGeometry::End::Download) {
            AccessNode download = button_node(row_part_id(row.name, "download"), g.end_text, s.can_change);
            place(download, g.state);
            download.target = {static_cast<int>(Part::Download), index, row.name};
            t.add(item_index, std::move(download));
        }
    }
}

void MainWindow::add_shared_access(AccessTree& t) const {
    const SharedGeometry& g = shared_geometry_;
    if (!shown(g.row)) {
        return;
    }
    const SharedRow row = shared_row(snap_.shared, snap_.installed, lang_);
    const bool on = shared_switch_value();
    const bool connected = snap_.link == Link::Connected;
    AccessNode group;
    group.id = "shared";
    group.role = AccessRole::Group;
    group.name = shared_row_name(row, on, g.end_text, lang_);
    group.bounds = ar(g.row);
    const int index = t.add(-1, std::move(group));

    const auto part = [&](AccessNode n, const RECT& r, Part p) {
        n.bounds = ar(r);
        n.focusable = true;
        n.target = {static_cast<int>(p), -1, {}};
        t.add(index, std::move(n));
    };
    part(switch_node("shared/switch", std::string(text(lang_, Str::SharedSites)), on, connected, lang_), g.toggle,
         Part::SharedSwitch);
    if (g.end == SharedGeometry::End::Address) {
        part(link_node("shared/end", g.end_text), g.end_rect, Part::SharedEnd);
    } else if (g.end == SharedGeometry::End::Download) {
        part(button_node("shared/end", g.end_text, connected), g.end_rect, Part::SharedEnd);
    } else if (g.end == SharedGeometry::End::Progress) {
        AccessNode progress = text_node("shared/progress", g.end_text);
        progress.bounds = ar(g.end_rect);
        t.add(index, std::move(progress));
    }
    part(button_node("shared/open", std::string(text(lang_, Str::SharedOpenFolder))), g.open, Part::SharedOpen);
    part(button_node("shared/gear", std::string(text(lang_, Str::AccessSharedSettings))), g.gear, Part::SharedGear);
}

void MainWindow::add_panel_access(AccessTree& t) const {
    const PanelGeometry& g = panel_geometry_;
    const ProjectRow* row = selected_row();
    if (row == nullptr || g.project != row->name || !shown(g.panel)) {
        return;
    }
    const std::string& p = row->name;
    const std::string base = "panel/" + p + "/";
    const bool connected = snap_.link == Link::Connected;
    const auto target = [&](Part part, int index) { return AccessTarget{static_cast<int>(part), index, p}; };

    AccessNode pane;
    pane.id = "panel/" + p;
    pane.role = AccessRole::Pane;
    pane.name = access_text(p);
    pane.bounds = ar(g.panel);
    const int panel = t.add(-1, std::move(pane));

    // The header: the name, the address (or the folder) and Close.
    AccessNode title = text_node(base + "title", p);
    title.bounds = ar(g.title);
    t.add(panel, std::move(title));
    if (shown(g.address)) {
        AccessNode link = link_node(access_id({Part::PanelAddress, -1, p}), row->address);
        link.bounds = ar(g.address);
        link.target = target(Part::PanelAddress, -1);
        t.add(panel, std::move(link));
    } else {
        AccessNode folder = text_node(base + "path", row->path);
        folder.bounds = ar(g.line2);
        t.add(panel, std::move(folder));
    }
    AccessNode close_button = button_node(access_id({Part::PanelClose, -1, p}), std::string(text(lang_, Str::ClosePanel)));
    close_button.bounds = ar(g.close);
    close_button.focusable = true;
    close_button.keys = "Esc";
    close_button.target = target(Part::PanelClose, -1);
    t.add(panel, std::move(close_button));

    // The tabs: one is always shown.
    AccessNode bar;
    bar.id = base + "tabs";
    bar.role = AccessRole::TabList;
    bar.bounds = ar(g.tab_bar);
    bar.selection_required = true;
    const int tabs = t.add(panel, std::move(bar));
    const PanelTab shown_now = shown_tab();
    for (std::size_t i = 0; i < g.tabs.size(); ++i) {
        const int index = static_cast<int>(i);
        AccessNode tab = tab_node(access_id({Part::PanelTab, index, p}), std::string(tab_word(lang_, g.tabs[i].second)),
                                  g.tabs[i].second == shown_now, index + 1, static_cast<int>(g.tabs.size()));
        tab.bounds = ar(g.tabs[i].first);
        tab.keys = "Ctrl+Tab";
        tab.target = target(Part::PanelTab, index);
        t.add(tabs, std::move(tab));
    }

    // The shown tab's page; what's scrolled out of it is off screen.
    AccessNode page;
    page.id = base + "page";
    page.role = AccessRole::Pane;
    page.name = std::string(tab_word(lang_, shown_now));
    page.bounds = ar(g.content);
    const int content = t.add(panel, std::move(page));
    const AccessRect clip = ar(g.content);
    const auto add = [&](AccessNode n, const RECT& r) {
        const AccessRect cut = clip_rect(ar(r), clip);
        n.offscreen = cut.empty();
        n.bounds = n.offscreen ? ar(r) : cut;
        return t.add(content, std::move(n));
    };
    const auto words = [&](const std::string& id, const std::string& said, const RECT& r) {
        if (shown(r) && !said.empty()) {
            add(text_node(base + id, said), r);
        }
    };

    if (shown_now == PanelTab::Php) {
        add_php_access(t, content, clip);
        return;
    }
    for (std::size_t i = 0; i < g.buttons.size(); ++i) {
        const auto& [r, action] = g.buttons[i];
        const int index = static_cast<int>(i);
        const std::string name = is_icon_action(action) ? panel_button_tip(action) : panel_button_text(action);
        AccessNode b = button_node(access_id({Part::PanelButton, index, p}), name, panel_action_enabled(action));
        b.focusable = b.enabled;
        b.target = target(Part::PanelButton, index);
        add(std::move(b), r);
    }
    if (shown(g.folder)) {
        words("folder-label", std::string(text(lang_, Str::PanelFolder)), g.folder_label);
        words("folder", row->path, g.folder);
    }
    words("versions", std::string(text(lang_, Str::Versions)), g.versions);
    for (std::size_t i = 0; i < g.fields.size() && i < fields_.size(); ++i) {
        const int index = static_cast<int>(i);
        AccessNode picker = picker_node(access_id({Part::PanelField, index, p}), fields_[i], field_value(fields_[i]));
        picker.enabled = connected;
        picker.target = target(Part::PanelField, index);
        add(std::move(picker), g.fields[i].second);
    }
    words("php-note", field_note(*row, lang_), g.php_note);
    words("saved-in", fill(lang_, Str::SavedIn, {core::kProjectConfigFileName}), g.saved_in);
    if (shown(g.keep_switch)) {
        const bool keep = keep_value(*row);
        AccessNode sw = switch_node(access_id({Part::PanelKeep, -1, p}), std::string(text(lang_, Str::KeepRunning)),
                                    keep, connected && !leaving(p), lang_);
        sw.help = access_text(keep_running_detail(keep, snap_.footer.idle_minutes, lang_));
        sw.focusable = true;
        sw.target = target(Part::PanelKeep, -1);
        add(std::move(sw), g.keep_switch);
    }
    if (shown(g.remove)) {
        const bool busy = removing_.contains(p);
        AccessNode remove_button = button_node(access_id({Part::PanelRemove, -1, p}),
                                        std::string(text(lang_, busy ? Str::Removing : Str::RemoveProject)),
                                        connected && !busy);
        remove_button.focusable = true;
        remove_button.target = target(Part::PanelRemove, -1);
        add(std::move(remove_button), g.remove);
    }

    // Database.
    if (shown(g.db_box)) {
        const std::string database = !row->database.empty() ? row->database : row->wants.database;
        const std::string name = row_database_name(*row);  // kept through a rename (D22)
        const std::uint16_t port = database_port(snap_.databases, database);
        const std::string host = port != 0 ? "127.0.0.1:" + std::to_string(port) : std::string("127.0.0.1");
        words("db-title", fill(lang_, Str::DatabaseTitle, {name}), g.db_title);
        words("db-detail", fill(lang_, Str::DatabaseDetail, {host, name}), g.db_detail);
        AccessNode copy_env = button_node(access_id({Part::PanelCopyEnv, -1, p}), std::string(text(lang_, Str::CopyEnv)),
                                      connected);
        copy_env.focusable = true;
        copy_env.target = target(Part::PanelCopyEnv, -1);
        add(std::move(copy_env), g.copy_env);
    }
    if (shown(g.pma)) {
        const DatabaseServerState server =
            database_server(snap_.databases, !row->database.empty() ? row->database : row->wants.database);
        const std::string label = pma_busy_ ? phpmyadmin_busy_text(server.series, server.state, lang_)
                                            : std::string(text(lang_, Str::PmaOpenProject));
        AccessNode pma = button_node(access_id({Part::PanelPhpMyAdmin, -1, p}), label,
                                     phpmyadmin_installed(snap_.installed) && !pma_busy_ && connected);
        pma.focusable = true;
        pma.target = target(Part::PanelPhpMyAdmin, -1);
        add(std::move(pma), g.pma);
        words("pma-hint", std::string(text(lang_, Str::PmaNotInstalled)), g.pma_hint);
    }

    // The log: the lines shown, one element each.
    if (height_of(g.log_box) > px(kLogLineH)) {
        AccessNode log_group;
        log_group.id = base + "log";
        log_group.role = AccessRole::Group;
        log_group.name = std::string(text(lang_, Str::Log));
        const int log_index = add(std::move(log_group), g.log_box);
        RECT inner = g.log_box;
        InflateRect(&inner, -px(10), -px(8));
        const bool ours = snap_.panel.project == p && snap_.panel.has_log;
        const std::vector<LogLine> none;
        const int fit = std::max(1, height_of(inner) / px(kLogLineH));
        const std::vector<std::string> lines =
            log_access_lines(ours ? snap_.panel.log : none, static_cast<std::size_t>(fit), ours, lang_);
        int y = inner.top;
        // Known by their words (and which of the same words), not their place: a new
        // request moves every line up, and only the new one is new.
        std::map<std::string, int> seen;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const int copy = seen[lines[i]]++;
            AccessNode line = text_node(base + "log/" + lines[i] + "#" + std::to_string(copy), lines[i]);
            line.bounds = clip_rect(ar(RECT{inner.left, y, inner.right, y + px(kLogLineH)}), clip);
            t.add(log_index, std::move(line));
            y += px(kLogLineH);
        }
    }
}

void MainWindow::add_php_access(AccessTree& t, int content, const AccessRect& clip) const {
    const PhpPanelGeometry& g = php_geometry_;
    const ProjectRow* row = selected_row();
    if (!php_open_ || row == nullptr || g.project != row->name) {
        return;
    }
    const std::string& p = row->name;
    const std::string base = "panel/" + p + "/php/";
    const auto add = [&](AccessNode n, const RECT& r) {
        const AccessRect cut = clip_rect(ar(r), clip);
        n.offscreen = cut.empty();
        n.bounds = n.offscreen ? ar(r) : cut;
        t.add(content, std::move(n));
    };
    const auto words = [&](const std::string& id, const std::string& said, const RECT& r) {
        if (shown(r) && !said.empty()) {
            add(text_node(base + id, said), r);
        }
    };
    const auto button = [&](Part part, const std::string& name, bool enabled, const RECT& r) {
        if (!shown(r)) {
            return;
        }
        AccessNode b = button_node(access_id({part, -1, p}), name, enabled);
        b.focusable = true;
        b.target = {static_cast<int>(part), -1, p};
        add(std::move(b), r);
    };
    const bool dirty = php_dirty();
    const bool can_save = dirty && !php_saving_ && snap_.link == Link::Connected;
    const bool can_reset = !php_after_.extensions.empty() || !php_after_.ini.empty() || php_another_.find_first_not_of(" \t") != std::string::npos;
    const bool can_change = php_info_ && !php_info_->extensions_fixed;

    // As paint_php_panel() words it.
    std::string line2(text(lang_, Str::PanelPhpSettings));
    if (php_info_ && !php_info_->version.empty()) {
        line2 += " · PHP " + php_info_->version;
    } else if (php_series_.empty()) {
        line2 = std::string(text(lang_, Str::ProjectPhpNoSeries));
    } else if (php_loading_) {
        line2 += " · " + std::string(text(lang_, Str::SettingsLoading));
    }
    words("line", line2, g.line2);
    button(Part::PanelPhpSettings, fill(lang_, Str::ProjectPhpOpenSettings, {php_series_}), true, g.open_settings);
    words("ext-title", std::string(text(lang_, Str::PhpExtensions)), g.ext_title);
    words("ext-hint", fill(lang_, Str::ProjectPhpExtHint, {php_series_}), g.ext_hint);
    const std::vector<std::pair<std::string, bool>> chips = php_chips();
    for (std::size_t i = 0; i < g.chips.size() && i < chips.size(); ++i) {
        const auto& [name, on] = chips[i];
        // Crossed out on the screen: off for this project.
        words("chip/" + name, on ? "+ " + name : access_join(lang_, {name, std::string(text(lang_, Str::AccessSwitchOff))}),
              g.chips[i]);
    }
    words("no-changes", std::string(text(lang_, Str::ProjectPhpNoChanges)), g.no_changes);
    button(Part::PanelPhpChange, std::string(text(lang_, Str::ProjectPhpChange)), can_change && !php_saving_, g.change);
    words("ini-title", "php.ini", g.ini_title);
    words("ini-hint", fill(lang_, Str::ProjectPhpIniHint, {php_series_}), g.ini_hint);
    for (std::size_t i = 0; i < g.boxes.size() && i < php_keys_.size(); ++i) {
        const std::string& key = php_keys_[i];
        const std::string label = key.empty() ? std::string(text(lang_, Str::ProjectPhpAnother)) : key;
        if (php_edit_of(i) != nullptr) {
            continue;  // a real box: it says its name (make_project_php_edits) and value itself
        }
        // A picker (display_errors): the project's value, or the series'.
        AccessNode picker;
        picker.id = access_id({Part::PanelPhpField, static_cast<int>(i), p});
        picker.role = AccessRole::ComboBox;
        picker.name = access_text(label);
        if (const auto own = php_after_.ini.find(key); own != php_after_.ini.end()) {
            picker.value = access_text(own->second);
        } else {
            std::string series_value = "?";
            if (php_info_) {
                if (const auto it = php_info_->ini.find(key); it != php_info_->ini.end()) {
                    series_value = it->second;
                }
            }
            picker.value = access_text(fill(lang_, Str::ProjectPhpDefaultChoice, {series_value}));
        }
        picker.focusable = true;
        picker.expandable = true;
        picker.enabled = !php_saving_;
        picker.target = {static_cast<int>(Part::PanelPhpField), static_cast<int>(i), p};
        add(std::move(picker), g.boxes[i]);
    }
    for (std::size_t i = 0; i < g.warnings.size(); ++i) {
        words("warning/" + std::to_string(i), g.warnings[i].second, g.warnings[i].first);
    }
    words("info", fill(lang_, Str::ProjectPhpSavedIn, {core::kProjectConfigFileName}), g.info);
    words("message", php_message_, g.message);
    button(Part::PanelPhpReset, std::string(text(lang_, Str::ProjectPhpReset)), can_reset, g.reset);
    button(Part::PanelPhpDiscard, std::string(text(lang_, Str::PhpDiscard)), dirty, g.discard);
    button(Part::PanelPhpSave, std::string(text(lang_, php_saving_ ? Str::PhpSaving : Str::PhpSave)), can_save, g.save);
}

// --- New project ---------------------------------------------------------------------

void MainWindow::add_wizard_access(AccessTree& t) const {
    const WizardGeometry& g = wizard_geometry_;
    if (!shown(g.area)) {
        return;
    }
    const auto target = [](Part part, int index) { return AccessTarget{static_cast<int>(part), index, {}}; };
    const bool connected = snap_.link == Link::Connected;
    AccessNode page;
    page.id = "wizard";
    page.role = AccessRole::Pane;
    page.name = std::string(text(lang_, Str::AccessNewProject));
    page.bounds = ar(g.area);
    const int pane = t.add(-1, std::move(page));
    const auto add = [&](int parent, AccessNode n, const RECT& r) {
        n.bounds = ar(r);
        return t.add(parent, std::move(n));
    };

    // The steps, and the title.
    const bool versions = wizard_.step == 0 || wizard_has_versions();
    const std::vector<std::string> steps = wizard_step_names(versions, lang_);
    const int current = wizard_strip_step(wizard_.step, versions);
    const int strip = add(pane, step_strip_node("wizard/steps", lang_), g.steps);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const int index = static_cast<int>(i);
        add(strip, step_node("wizard/step/" + std::to_string(i), steps[i], index, current, static_cast<int>(steps.size()), lang_),
            i < g.step_rects.size() ? g.step_rects[i] : RECT{});
    }
    add(pane, heading_node("wizard/title", wizard_title(wizard_.step, snap_.create.state, wizard_.name, lang_)), g.title);

    switch (wizard_.step) {
        case 0: {
            if (snap_.templates.empty()) {
                const std::string message = !snap_.templates_loaded ? std::string(text(lang_, Str::WizardLoading))
                                            : fill_pattern(text(lang_, Str::WizardNoTemplates), false,
                                                           {snap_.templates_error.empty() ? std::string("-")
                                                                                          : snap_.templates_error});
                add(pane, text_node("wizard/message", message), g.message);
                break;
            }
            // The cards that fit: one list, the chosen one selected.
            AccessNode list;
            list.id = "wizard/cards";
            list.role = AccessRole::List;
            list.name = std::string(text(lang_, Str::AccessTemplates));
            RECT all{};
            for (const RECT& r : g.cards) {
                UnionRect(&all, &all, &r);
            }
            const int cards = add(pane, std::move(list), all);
            const int count = static_cast<int>(std::min(g.cards.size(), snap_.templates.size()));
            for (int i = 0; i < count; ++i) {
                const auto at = static_cast<std::size_t>(i);
                AccessNode card = template_card_node(snap_.templates[at], at == wizard_.card, i + 1, count, lang_);
                card.keys = "Enter";
                card.target = target(Part::WizCard, i);
                add(cards, std::move(card), g.cards[at]);
            }
            break;
        }
        case 1: {
            // The name box is a real EDIT: its name and help are set on it (make_name_edit).
            AccessNode folder;
            folder.id = "wizard/folder";
            folder.role = AccessRole::Edit;
            folder.name = access_text(text(lang_, Str::WizardFolder));
            folder.value = access_text(wizard_.parent);
            folder.help = access_text(wizard_made_in(wizard_.parent, wizard_name(), lang_));
            add(pane, std::move(folder), g.folder_box);
            break;
        }
        case 2:
            for (std::size_t i = 0; i < g.fields.size() && i < wizard_.fields.size(); ++i) {
                const VersionField& f = wizard_.fields[i];
                const std::size_t pick = wizard_.picks.contains(f.key) ? wizard_.picks.at(f.key) : f.current;
                AccessNode picker = picker_node(access_id({Part::WizField, static_cast<int>(i), {}}), f, pick);
                picker.keys = "F4";
                picker.target = target(Part::WizField, static_cast<int>(i));
                add(pane, std::move(picker), g.fields[i].second);
            }
            add(pane, text_node("wizard/downloads", std::string(text(lang_, Str::WizardDownloads))), g.message);
            break;
        default: {
            // The step it's on, or why it failed; then the commands as they run.
            const bool ended = snap_.create.state == "failed" || snap_.create.state == "cancelled";
            const std::string& line = ended ? snap_.create.error : snap_.create.step;
            if (!line.empty()) {
                add(pane, text_node("wizard/line", line), g.message);
            }
            if (height_of(g.log_box) > px(kLogLineH)) {
                AccessNode log_group;
                log_group.id = "wizard/log";
                log_group.role = AccessRole::Group;
                log_group.name = std::string(text(lang_, Str::Log));
                const int log_index = add(pane, std::move(log_group), g.log_box);
                RECT inner = g.log_box;
                InflateRect(&inner, -px(10), -px(8));
                const auto fit = static_cast<std::size_t>(std::max(1, height_of(inner) / px(kLogLineH)));
                const std::vector<std::string>& log = snap_.create.log;
                const std::size_t first = log.size() > fit ? log.size() - fit : 0;
                int y = inner.top;
                // Known by their words and which of the same words, as the panel's log.
                std::map<std::string, int> seen;
                for (std::size_t i = first; i < log.size(); ++i) {
                    const std::string said = access_text(log[i]);
                    const int copy = seen[said]++;
                    add(log_index, text_node("wizard/log/" + said + "#" + std::to_string(copy), said),
                        RECT{inner.left, y, inner.right, y + px(kLogLineH)});
                    y += px(kLogLineH);
                }
            }
            break;
        }
    }

    // Browse, then the footer's buttons in reading order.
    const auto button = [&](Part part, Str label, bool enabled, const RECT& r, std::string keys) {
        if (!shown(r)) {
            return;
        }
        AccessNode b = button_node(access_id({part, -1, {}}), text(lang_, label), enabled);
        b.focusable = true;
        b.keys = std::move(keys);
        b.target = target(part, -1);
        add(pane, std::move(b), r);
    };
    button(Part::WizBrowse, Str::WizardBrowse, true, g.browse, {});
    button(Part::WizAddFolder, Str::AddExistingFolder, connected, g.add_folder, {});
    button(Part::WizFromBackup, Str::BackupFromBackup, connected, g.from_backup, {});
    button(Part::WizCancel, Str::WizardCancel, true, g.cancel, "Esc");
    button(Part::WizBack, Str::WizardBack, true, g.back, {});
    button(Part::WizNext, g.next_label, true, g.next, "Enter");

    WizardNewsState news;
    news.step = wizard_.step;
    news.versions = versions;
    news.state = snap_.create.state;
    news.name = wizard_.name;
    news.error = snap_.create.error;
    news.templates_error = snap_.templates_loaded ? snap_.templates_error : std::string();
    t.news = wizard_news(news, lang_);
}

// --- the first-run page --------------------------------------------------------------

void MainWindow::add_first_run_access(AccessTree& t) const {
    const FirstRunGeometry& g = first_run_geometry_;
    if (!shown(g.area)) {
        return;
    }
    const auto target = [](Part part, int index) { return AccessTarget{static_cast<int>(part), index, {}}; };
    const std::vector<FirstRunRow> rows = first_run_.started ? first_run_rows_now() : std::vector<FirstRunRow>{};

    Str title = Str::FirstRunWelcome;
    std::string intro;
    switch (first_run_.step) {
        case FirstRunStep::Language:
            intro = std::string(text(lang_, Str::FirstRunIntro));
            break;
        case FirstRunStep::Runtimes:
            if (first_run_.started) {
                const FirstRunProgress progress = first_run_progress(rows);
                title = progress.running() ? Str::FirstRunDownloadingTitle : Str::FirstRunDownloadedTitle;
                intro = fill(lang_, Str::FirstRunDownloadingIntro, {std::to_string(progress.ready), std::to_string(progress.total)});
            } else {
                title = Str::FirstRunRuntimesTitle;
                intro = std::string(text(lang_, Str::FirstRunRuntimesIntro));
            }
            break;
        case FirstRunStep::Ready:
            title = Str::FirstRunReadyTitle;
            break;
    }

    AccessNode page;
    page.id = "first-run";
    page.role = AccessRole::Pane;
    page.name = access_text(text(lang_, title));
    page.bounds = ar(g.area);
    const int pane = t.add(-1, std::move(page));
    const auto add = [&](int parent, AccessNode n, const RECT& r) {
        n.bounds = ar(r);
        return t.add(parent, std::move(n));
    };
    // In the part that scrolls: what's scrolled out of it is off screen.
    const AccessRect clip = ar(g.content);
    const auto add_in = [&](int parent, AccessNode n, const RECT& r) {
        const AccessRect cut = clip_rect(ar(r), clip);
        n.offscreen = cut.empty();
        n.bounds = n.offscreen ? ar(r) : cut;
        return t.add(parent, std::move(n));
    };
    const auto rect_at = [](const std::vector<RECT>& v, std::size_t i) { return i < v.size() ? v[i] : RECT{}; };

    // The steps.
    const std::vector<std::string> steps = first_run_step_names(lang_);
    const int current = static_cast<int>(first_run_.step);
    const int strip = add(pane, step_strip_node("first-run/steps", lang_), g.steps);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        AccessNode step = step_node("first-run/step/" + std::to_string(i), steps[i], static_cast<int>(i), current,
                                    static_cast<int>(steps.size()), lang_);
        step.offscreen = i >= g.step_rects.size();  // its words didn't fit in the strip
        add(strip, std::move(step), rect_at(g.step_rects, i));
    }
    add_in(pane, heading_node("first-run/title", text(lang_, title)), g.title);
    if (!intro.empty()) {
        add_in(pane, text_node("first-run/intro", intro), g.intro);
    }

    switch (first_run_.step) {
        case FirstRunStep::Language: {
            // The languages: radio buttons in a group called Language.
            AccessNode group;
            group.id = "first-run/langs";
            group.role = AccessRole::Group;
            group.name = access_text(text(lang_, Str::GeneralLanguage));
            const int langs = add_in(pane, std::move(group), g.card);
            const int count = static_cast<int>(std::min(g.rows.size(), kLangChoices.size()));
            for (int i = 0; i < count; ++i) {
                const auto at = static_cast<std::size_t>(i);
                AccessNode radio = language_radio_node(access_id({Part::FrLang, i, {}}),
                                                       first_run_language_row(kLangChoices.at(at), first_run_.system_lang, lang_),
                                                       kLangChoices.at(at) == prefs_.lang, i + 1, count);
                radio.enabled = !asking_;
                radio.target = target(Part::FrLang, i);
                add_in(langs, std::move(radio), g.rows[at]);
            }
            add_in(pane, text_node("first-run/lang-note", text(lang_, Str::FirstRunLanguageNote)), g.note);
            break;
        }
        case FirstRunStep::Runtimes:
            if (first_run_.started) {
                // The downloads: a group per row, its progress bar and Cancel.
                AccessNode group;
                group.id = "first-run/downloads";
                group.role = AccessRole::Group;
                group.name = access_text(text(lang_, Str::FirstRunStepRuntimes));
                const int downloads = add_in(pane, std::move(group), g.card);
                for (std::size_t i = 0; i < rows.size() && i < g.rows.size(); ++i) {
                    const FirstRunRow& r = rows[i];
                    const std::string base = "first-run/download/" + r.name;
                    const int row = add_in(downloads, download_row_node(base, r, lang_), g.rows[i]);
                    if (r.state == FirstRunRowState::Running) {
                        add_in(row, download_progress_node(base + "/progress", r), g.rows[i]);
                    }
                    if (const RECT cancel = rect_at(g.cancels, i); shown(cancel)) {
                        AccessNode b = button_node(access_id({Part::FrCancel, static_cast<int>(i), {}}),
                                                   access_join(lang_, {std::string(text(lang_, Str::WizardCancel)), r.label}));
                        b.focusable = true;
                        b.target = target(Part::FrCancel, static_cast<int>(i));
                        add_in(row, std::move(b), cancel);
                    }
                }
                if (shown(g.info)) {
                    add_in(pane, text_node("first-run/trust", first_run_https_line(first_run_.trust, first_run_.trust_error, lang_)),
                           g.info);
                }
                if (shown(g.info_note)) {
                    add_in(pane, text_node("first-run/trust-note", text(lang_, Str::FirstRunTrustAskingNote)), g.info_note);
                }
                break;
            }
            switch (first_run_.list) {
                case FirstRunList::Ready: {
                    // The runtimes offered: check boxes; nginx is always included.
                    AccessNode group;
                    group.id = "first-run/offers";
                    group.role = AccessRole::Group;
                    group.name = access_text(text(lang_, Str::FirstRunStepRuntimes));
                    const int offers = add_in(pane, std::move(group), g.card);
                    const int count = static_cast<int>(std::min(g.rows.size(), first_run_.offers.size()));
                    for (int i = 0; i < count; ++i) {
                        const auto at = static_cast<std::size_t>(i);
                        const bool ticked = at < first_run_.ticks.size() && first_run_.ticks[at];
                        AccessNode box = offer_node(access_id({Part::FrTick, i, {}}), first_run_.offers[at], ticked, i + 1,
                                                    count, lang_);
                        box.keys = box.enabled ? "Space" : "";
                        box.target = target(Part::FrTick, i);
                        add_in(offers, std::move(box), g.rows[at]);
                    }
                    if (shown(g.info)) {
                        add_in(pane, text_node("first-run/cert", text(lang_, Str::FirstRunCertNote)), g.info);
                    }
                    break;
                }
                case FirstRunList::Failed: {
                    add_in(pane, text_node("first-run/message", first_run_list_error(first_run_.list_error, lang_)), g.message);
                    AccessNode retry = button_node(access_id({Part::FrRetry, -1, {}}), text(lang_, Str::FirstRunTryAgain),
                                                   snap_.link == Link::Connected);
                    retry.focusable = true;
                    retry.target = target(Part::FrRetry, -1);
                    add_in(pane, std::move(retry), g.retry);
                    break;
                }
                case FirstRunList::Idle:
                case FirstRunList::Loading:
                    add_in(pane, text_node("first-run/message", text(lang_, Str::FirstRunLoading)), g.message);
                    break;
            }
            break;
        case FirstRunStep::Ready: {
            // The summary: what's ready, https, where projects go (and Change…).
            const std::string runtimes = first_run_runtimes_line(rows, lang_, first_run_.without);
            const std::string https = first_run_https_line(first_run_.trust, first_run_.trust_error, lang_);
            const std::string folder = first_run_folder();
            const std::pair<Str, const std::string*> summary[] = {
                {Str::SettingsRuntimes, &runtimes}, {Str::FirstRunHttps, &https}, {Str::FirstRunFolderTitle, &folder}};
            const char* ids[] = {"runtimes", "https", "folder"};
            for (std::size_t i = 0; i < std::size(summary) && i < g.rows.size(); ++i) {
                add_in(pane,
                       text_node(std::string("first-run/summary/") + ids[i],
                                 access_join(lang_, {std::string(text(lang_, summary[i].first)), *summary[i].second})),
                       g.rows[i]);
            }
            if (shown(g.change)) {
                AccessNode change = button_node(
                    access_id({Part::FrChange, -1, {}}),
                    access_join(lang_, {std::string(text(lang_, Str::FirstRunChange)), std::string(text(lang_, Str::FirstRunFolderTitle))}),
                    first_run_.folder_ask.empty() && snap_.link == Link::Connected);
                change.focusable = true;
                change.target = target(Part::FrChange, -1);
                add_in(pane, std::move(change), g.change);
            }
            // The two big buttons: their heading is the name, the words under it the help.
            for (const auto& [part, head, note, r] : {std::tuple{Part::FrCreate, Str::FirstRunCreate, Str::FirstRunCreateNote, g.create},
                                                      std::tuple{Part::FrAdd, Str::FirstRunAddFolder, Str::FirstRunAddFolderNote, g.add}}) {
                if (!shown(r)) {
                    continue;
                }
                AccessNode b = button_node(access_id({part, -1, {}}), text(lang_, head));
                b.help = access_text(text(lang_, note));
                b.focusable = true;
                b.target = target(part, -1);
                add_in(pane, std::move(b), r);
            }
            break;
        }
    }

    // The footer: its words, then Skip setup, Back, Continue without downloading and the step's button.
    if (shown(g.footer_text)) {
        add(pane, text_node("first-run/footer-text", first_run_footer_words()), g.footer_text);
    }
    const auto button = [&](Part part, Str label, const RECT& r) {
        if (!shown(r)) {
            return;
        }
        AccessNode b = button_node(access_id({part, -1, {}}), text(lang_, label));
        b.focusable = true;
        b.target = target(part, -1);
        add(pane, std::move(b), r);
    };
    button(Part::FrSkipSetup, Str::FirstRunSkipSetup, g.skip);
    button(Part::FrBack, Str::WizardBack, g.back);
    button(Part::FrWithout, Str::FirstRunWithout, g.without);
    button(Part::FrNext, g.next_label, g.next);

    FirstRunNewsState news;
    news.step = first_run_.step;
    news.list = first_run_.list;
    news.list_error = first_run_.list_error;
    news.started = first_run_.started;
    news.rows = rows;
    news.trust = first_run_.trust;
    news.trust_error = first_run_.trust_error;
    news.folder = first_run_.folder;
    t.news = first_run_news(news, lang_);
}

// --- AccessSource ------------------------------------------------------------------

const AccessTree& MainWindow::access_tree() {
    if (access_dirty_) {
        access_now_ = build_access_tree();
        access_dirty_ = false;
    }
    return access_now_;
}

void MainWindow::access_request(const std::string& id, AccessAction action) {
    const bool first = access_queue_.empty();
    access_queue_.emplace_back(id, action);
    // While one runs (a menu it opened is still open), the next waits for it.
    if (access_gate_.post_queued(first) && hwnd_ != nullptr) {
        PostMessageW(hwnd_, kAccessMessage, 0, 0);
    }
}

void MainWindow::modal_closed() {
    // Requests that came while it was open run now, not at some later message.
    if (hwnd_ != nullptr && access_gate_.replay(modal_open(), access_queue_.empty())) {
        PostMessageW(hwnd_, kAccessMessage, 0, 0);
    }
}

UINT MainWindow::track_menu(HMENU menu, UINT flags, POINT at) {
    const ModalScope modal(*this);
    return static_cast<UINT>(TrackPopupMenuEx(menu, flags, at.x, at.y, hwnd_, nullptr));
}

void MainWindow::access_after_paint() {
    const bool opened = access_page_opened_;
    access_page_opened_ = false;
    if (!AccessRoot::listening()) {
        access_said_.reset();  // nobody to tell: nothing kept
        return;
    }
    access_now_ = build_access_tree();
    access_dirty_ = false;
    if (!access_said_ && opened) {
        // The first tree since the screen reader came (the window just made with
        // the first-run page open): nothing to compare with, but the page's step is said.
        for (const AccessNews& news : access_now_.news) {
            if (news.key == "step") {
                access_.notify(news.text, news.important, news.key);
            }
        }
    }
    if (access_said_) {
        access_.raise(access_changes(*access_said_, access_now_));
        // What's new on a page (its step, a download that ended, the project made): said once.
        for (const AccessNews& news : access_news(*access_said_, access_now_)) {
            access_.notify(news.text, news.important, news.key);
        }
    }
    access_said_ = access_now_;
}

void MainWindow::run_access_requests() {
    // Never inside another: a picker's menu runs messages, and a second menu
    // or Remove's question must not open over it. Nor inside a menu or
    // question opened by the mouse or the keys: they wait for it to close.
    // Quitting: nothing more is done (its last message box may be open).
    if (quitting_) {
        access_queue_.clear();
        return;
    }
    if (!access_gate_.may_run(modal_open())) {
        return;
    }
    access_gate_.begin();
    while (!access_queue_.empty()) {
        const auto [id, action] = access_queue_.front();
        access_queue_.erase(access_queue_.begin());
        if (hwnd_ == nullptr || quitting_) {
            access_queue_.clear();
            break;
        }
        // Found again in the window as it is now: it may have gone meanwhile.
        access_dirty_ = true;
        const AccessTree& tree = access_tree();
        const AccessNode* n = tree.node(tree.find(id));
        if (n == nullptr || (!n->enabled && action != AccessAction::Focus)) {
            continue;
        }
        // A copy: a menu or question it opens runs messages, and the tree is made again under it.
        const AccessNode node = *n;
        run_access(node, action);
    }
    access_gate_.end();
}

void MainWindow::run_access(const AccessNode& n, AccessAction action) {
    if (n.target.part == kToastClosePart) {
        toast_event(ToastEvent::Close);
        return;
    }
    if (n.target.part == kToastActionPart) {
        PostMessageW(hwnd_, kToastMessage, kToastActionClicked, 0);
        return;
    }
    const auto part = static_cast<Part>(n.target.part);
    const Hit hit{part, n.target.index, n.target.project};
    if (action == AccessAction::Focus) {
        access_focus(hit);
        return;
    }
    switch (part) {
        case Part::WizCard:
            // Select chooses it, as a click does; Invoke goes on with it, as Enter does.
            on_click(hit);
            if (action == AccessAction::Invoke && wizard_open() && wizard_.step == 0) {
                wizard_next();
            }
            return;
        case Part::Row:
            // Opening its panel is what Enter and a click on it do.
            if (const int row = visible_index(hit.project); row >= 0) {
                focus_ = row;
                shared_focused_ = false;
                panel_focus_ = -1;
                ensure_visible(row);
                select(hit.project);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        case Part::PanelKeep:
            toggle_keep_running_for(hit.project);
            return;
        case Part::SharedSwitch:
        case Part::SharedEnd:
        case Part::SharedOpen:
        case Part::SharedGear:
            run_shared_part(part);
            return;
        case Part::None:
            return;
        default:
            break;
    }
    // A Toggle on a switch, a Select on a tab, an Expand on a picker and an
    // Invoke on a button all do what a click does.
    on_click(hit);
}

void MainWindow::access_focus(const Hit& hit) {
    if (is_update_part(hit.part)) {
        const std::vector<Part> order = update_focus_order();
        if (const auto it = std::find(order.begin(), order.end(), hit.part); it != order.end()) {
            update_focus_ = static_cast<int>(it - order.begin());
            focus_ = -1;
            shared_focused_ = false;
            panel_focus_ = -1;
        }
    } else if (is_panel_part(hit.part)) {
        const std::vector<Hit> order = panel_focus_order();
        if (const auto it = std::find(order.begin(), order.end(), hit); it != order.end()) {
            panel_focus_ = static_cast<int>(it - order.begin());
            ensure_panel_visible(hit);
        }
    } else if (hit.part == Part::SharedSwitch || hit.part == Part::SharedEnd || hit.part == Part::SharedOpen ||
               hit.part == Part::SharedGear) {
        const std::vector<Part> order = shared_focus_order();
        if (const auto it = std::find(order.begin(), order.end(), hit.part); it != order.end()) {
            focus_ = -1;
            panel_focus_ = -1;
            shared_focused_ = true;
            shared_part_ = static_cast<int>(it - order.begin());
        }
    } else if (hit.part == Part::MailLink) {
        focus_ = -1;
        panel_focus_ = -1;
        update_focus_ = -1;
        shared_focused_ = false;
        mail_focused_ = true;
    } else if (hit.part == Part::Row) {
        if (const int row = visible_index(hit.project); row >= 0) {
            focus_ = row;
            shared_focused_ = false;
            panel_focus_ = -1;
            ensure_visible(row);
        }
    } else if (wizard_open() && (hit.part == Part::WizCard || hit.part == Part::WizField || hit.part == Part::WizBrowse ||
                                 hit.part == Part::WizAddFolder || hit.part == Part::WizFromBackup ||
                                 hit.part == Part::WizCancel ||
                                 hit.part == Part::WizBack || hit.part == Part::WizNext)) {
        const std::vector<Hit> order = wizard_focus_order();
        if (hit.part == Part::WizCard || std::find(order.begin(), order.end(), hit) != order.end()) {
            wizard_focus_to(hit, true);  // it takes the window's focus itself
        }
        return;
    } else if (first_run_open()) {
        const std::vector<Hit> order = first_run_focus_order();
        if (const auto it = std::find(order.begin(), order.end(), hit); it != order.end()) {
            first_run_focus_at(static_cast<int>(it - order.begin()));
        }
    } else {
        return;
    }
    show_focus_ = true;
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace floserver::ui

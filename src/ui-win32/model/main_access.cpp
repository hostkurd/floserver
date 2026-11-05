#include "model/main_access.h"

#include <utility>

namespace floserver::ui {

namespace {

std::string switch_word(bool on, Lang lang) {
    return std::string(text(lang, on ? Str::AccessSwitchOn : Str::AccessSwitchOff));
}

// The end's words only repeat the switch when the state is On with it on, or Off with it off.
bool repeats_switch(RowEndKind end, const std::string& end_text, bool on, Lang lang) {
    return end == RowEndKind::State && end_text == text(lang, on ? Str::StateOn : Str::StateOff);
}

}  // namespace

std::string row_id(const std::string& project) { return "row/" + project; }

std::string row_part_id(const std::string& project, std::string_view part) {
    std::string id = row_id(project);
    id += '/';
    id += part;
    return id;
}

std::string project_row_name(const ProjectRow& row, const RowAccessState& state, Lang lang) {
    std::vector<std::string> parts{row.name, switch_word(state.on, lang)};
    if (!repeats_switch(state.end, state.end_text, state.on, lang)) {
        parts.push_back(state.end_text);
    }
    parts.insert(parts.end(), row.chips.begin(), row.chips.end());
    parts.push_back(!row.address.empty() ? row.address : row.detail);
    return access_join(lang, parts);
}

AccessNode project_row_node(const ProjectRow& row, const RowAccessState& state, Lang lang) {
    AccessNode n;
    n.id = row_id(row.name);
    n.role = AccessRole::ListItem;
    n.name = project_row_name(row, state, lang);
    // The address is in the name; the detail (what's missing, the problem) is said again after it.
    if (!row.address.empty() && !row.detail.empty()) {
        n.help = access_text(row.detail);
    }
    n.focusable = true;
    n.invokable = true;  // opens its panel
    n.selected = state.selected;
    n.position = state.position;
    n.set_size = state.count;
    return n;
}

AccessNode switch_node(std::string id, std::string_view name, bool on, bool enabled, Lang lang) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::Switch;
    n.name = access_text(name);
    n.kind = std::string(text(lang, Str::AccessToggleSwitch));
    n.toggled = on;
    n.enabled = enabled;
    return n;
}

AccessNode button_node(std::string id, std::string_view name, bool enabled) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::Button;
    n.name = access_text(name);
    n.enabled = enabled;
    n.invokable = true;
    return n;
}

AccessNode text_node(std::string id, std::string_view said) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::Text;
    n.name = access_text(said);
    return n;
}

AccessNode link_node(std::string id, const std::string& address) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::Link;
    n.name = access_text(address);
    n.invokable = true;
    return n;
}

std::string shared_row_name(const SharedRow& row, bool on, const std::string& end_text, Lang lang) {
    std::vector<std::string> parts{std::string(text(lang, Str::SharedSites)), switch_word(on, lang)};
    const std::string state(text(lang, state_label(row.state)));
    if (!repeats_switch(RowEndKind::State, state, on, lang)) {
        parts.push_back(state);
    }
    parts.insert(parts.end(), row.chips.begin(), row.chips.end());
    // The address, a Download button's words or the progress; then the line under the title.
    if (!end_text.empty()) {
        parts.push_back(end_text);
    }
    parts.push_back(row.detail);
    return access_join(lang, parts);
}

AccessNode picker_node(std::string id, const VersionField& field, std::size_t shown) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::ComboBox;
    n.name = access_text(field.label);
    if (shown < field.choices.size()) {
        n.value = access_text(field.choices[shown].label);
    }
    n.focusable = true;
    n.expandable = true;
    return n;
}

AccessNode tab_node(std::string id, std::string_view label, bool shown, int position, int count) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::TabItem;
    n.name = access_text(label);
    n.selected = shown;
    n.focusable = true;
    n.position = position;
    n.set_size = count;
    return n;
}

std::string keep_running_detail(bool keep, std::uint64_t idle_minutes, Lang lang) {
    return keep ? std::string(text(lang, Str::KeepRunningOn))
                : fill(lang, Str::KeepRunningOff, {std::to_string(idle_minutes != 0 ? idle_minutes : 10)});
}

AccessNode status_node(const std::string& text, bool clickable) {
    return clickable ? button_node("status", text) : text_node("status", text);
}

AccessNode footer_node(const FooterText& footer, Lang lang) {
    return text_node("footer", access_join(lang, {footer.counts, footer.nginx, footer.memory}));
}

ToastNodes toast_nodes(const std::string& message, Lang lang) {
    ToastNodes t;
    t.group.id = "toast";
    t.group.role = AccessRole::Group;
    t.group.name = std::string(text(lang, Str::AccessMessage));
    t.text = text_node("toast/text", message);
    t.close = button_node("toast/close", std::string(text(lang, Str::AccessCloseMessage)));
    return t;
}

std::vector<std::string> log_access_lines(const std::vector<LogLine>& lines, std::size_t shown, bool known,
                                          Lang lang) {
    std::vector<std::string> out;
    if (lines.empty()) {
        if (known) {
            out.emplace_back(text(lang, Str::LogEmpty));
        }
        return out;
    }
    const std::size_t first = lines.size() > shown ? lines.size() - shown : 0;
    for (std::size_t i = first; i < lines.size(); ++i) {
        out.push_back(log_text(lines[i]));
    }
    return out;
}

}  // namespace floserver::ui

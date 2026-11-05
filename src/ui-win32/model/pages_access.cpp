#include "model/pages_access.h"

#include "model/main_access.h"
#include "model/runtimes_page.h"

#include <algorithm>
#include <utility>

namespace floserver::ui {

namespace {

// "1 Template" as it's drawn: the number goes, the strip's place says it.
std::string without_number(std::string_view label) {
    std::size_t i = 0;
    while (i < label.size() && label[i] >= '0' && label[i] <= '9') {
        ++i;
    }
    if (i > 0 && i < label.size() && label[i] == ' ') {
        return std::string(label.substr(i + 1));
    }
    return std::string(label);
}

std::string join_labels(const std::vector<std::string>& labels) {
    std::string out;
    for (const std::string& l : labels) {
        if (!out.empty()) {
            out += ", ";
        }
        out += l;
    }
    return out;
}

}  // namespace

// --- both pages ----------------------------------------------------------------

AccessNode heading_node(std::string id, std::string_view title) {
    AccessNode n = text_node(std::move(id), title);
    n.heading = 1;
    return n;
}

AccessNode step_strip_node(std::string id, Lang lang) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::List;
    n.name = std::string(text(lang, Str::AccessSteps));
    return n;
}

AccessNode step_node(std::string id, std::string_view name, int index, int current, int count, Lang lang) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::ListItem;
    n.name = index < current ? access_join(lang, {std::string(name), std::string(text(lang, Str::AccessStepDone))})
                             : access_text(name);
    n.selected = index == current;
    n.position = index + 1;
    n.set_size = count;
    return n;
}

std::string step_said(int current, int count, std::string_view name, Lang lang) {
    return access_text(fill(lang, Str::AccessStepOf, {std::to_string(current + 1), std::to_string(count), name}));
}

// --- New project ---------------------------------------------------------------

std::vector<std::string> wizard_step_names(bool versions, Lang lang) {
    std::vector<std::string> out{without_number(text(lang, Str::WizardStepTemplate)),
                                 without_number(text(lang, Str::WizardStepName))};
    if (versions) {
        out.push_back(without_number(text(lang, Str::WizardStepVersions)));
    }
    return out;
}

int wizard_strip_step(int step, bool versions) { return std::min(step, versions ? 2 : 1); }

std::string wizard_title(int step, const std::string& state, const std::string& name, Lang lang) {
    switch (step) {
        case 0:
            return std::string(text(lang, Str::WizardWhat));
        case 1:
            return std::string(text(lang, Str::WizardNameTitle));
        case 2:
            return std::string(text(lang, Str::WizardVersionsTitle));
        default:
            break;
    }
    if (state == "done") {
        return fill(lang, Str::WizardDone, {name});
    }
    if (state == "failed" || state == "cancelled") {
        return std::string(text(lang, Str::WizardNotMade));
    }
    return fill(lang, Str::WizardMaking, {name});
}

std::string wizard_name_hint(const std::string& name, const FooterText& footer, Lang lang) {
    return is_project_name(name) ? fill(lang, Str::WizardAddress, {new_project_address(name, footer)})
                                 : std::string(text(lang, Str::WizardNameRule));
}

std::string wizard_made_in(const std::string& parent, const std::string& name, Lang lang) {
    std::string full = parent;
    if (!full.empty() && full.back() != '\\' && full.back() != '/') {
        full += '\\';
    }
    full += is_project_name(name) ? name : std::string("…");
    return fill(lang, Str::WizardMadeIn, {full});
}

AccessNode template_card_node(const TemplateCard& card, bool chosen, int position, int count, Lang lang) {
    AccessNode n;
    n.id = "wizard/card/" + card.id;
    n.role = AccessRole::ListItem;
    std::vector<std::string> parts{card.title, card.description};
    parts.insert(parts.end(), card.chips.begin(), card.chips.end());
    n.name = access_join(lang, parts);
    n.selected = chosen;
    n.focusable = true;
    n.invokable = true;  // chosen, and on to the next step (Enter on a card)
    n.position = position;
    n.set_size = count;
    return n;
}

std::vector<AccessNews> wizard_news(const WizardNewsState& s, Lang lang) {
    std::vector<AccessNews> out;
    const std::vector<std::string> steps = wizard_step_names(s.versions, lang);
    const int at = wizard_strip_step(s.step, s.versions);
    out.push_back({"step", step_said(at, static_cast<int>(steps.size()), steps[static_cast<std::size_t>(at)], lang),
                   false});
    if (s.step == 0 && !s.templates_error.empty()) {
        out.push_back({"templates",
                       access_text(fill_pattern(text(lang, Str::WizardNoTemplates), false, {s.templates_error})), true});
    }
    if (s.step == 3) {
        if (s.state == "failed") {
            out.push_back({"made",
                           s.error.empty() ? std::string(text(lang, Str::WizardNotMade))
                                           : access_text(fill(lang, Str::WizardFailed, {s.error})),
                           true});
        } else {
            out.push_back({"made", access_text(wizard_title(s.step, s.state, s.name, lang)), false});
        }
    }
    return out;
}

// --- the first-run page -----------------------------------------------------------

std::vector<std::string> first_run_step_names(Lang lang) {
    return {std::string(text(lang, Str::FirstRunStepLanguage)), std::string(text(lang, Str::FirstRunStepRuntimes)),
            std::string(text(lang, Str::FirstRunStepReady))};
}

LanguageRowText first_run_language_row(LangChoice choice, Lang system_lang, Lang lang) {
    LanguageRowText row;
    switch (choice) {
        case LangChoice::System:
            row.label = std::string(text(lang, Str::GeneralSameAsWindows));
            row.note = fill_pattern(text(lang, Str::FirstRunLanguageNow), false, {language_name(system_lang)});
            break;
        case LangChoice::En:
            row.label = std::string(language_name(Lang::En));
            break;
        case LangChoice::Ar:
            row.label = std::string(language_name(Lang::Ar));
            break;
        case LangChoice::Ku:
            row.label = std::string(language_name(Lang::Ku));
            row.note = std::string(text(lang, Str::FirstRunSorani));
            break;
    }
    return row;
}

AccessNode language_radio_node(std::string id, const LanguageRowText& row, bool chosen, int position, int count) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::Radio;
    n.name = access_text(row.label);
    n.help = access_text(row.note);
    n.selected = chosen;
    n.focusable = true;
    n.position = position;
    n.set_size = count;
    return n;
}

AccessNode offer_node(std::string id, const FirstRunOffer& offer, bool ticked, int position, int count, Lang lang) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::CheckBox;
    n.name = access_join(lang, {offer.label, std::string(text(lang, offer.what)),
                                offer.size > 0 ? size_text(offer.size) : std::string()});
    n.toggled = offer.always || ticked;
    n.enabled = !offer.always;
    n.focusable = !offer.always;
    n.position = position;
    n.set_size = count;
    return n;
}

AccessNode download_row_node(std::string id, const FirstRunRow& row, Lang lang) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::Group;
    // While the size is known the line counts up ("19 MB of 31 MB"): said as
    // Downloading, the numbers in its help and its progress bar.
    const std::string state =
        row.state == FirstRunRowState::Running && row.fraction ? std::string(text(lang, Str::SettingsDownloadingDots)) : row.line;
    n.name = access_join(lang, {row.label, state});
    if (access_text(state) != access_text(row.line)) {
        n.help = access_text(row.line);
    }
    return n;
}

AccessNode download_progress_node(std::string id, const FirstRunRow& row) {
    AccessNode n;
    n.id = std::move(id);
    n.role = AccessRole::Progress;
    n.name = access_text(row.label);
    n.progress = row.fraction;
    return n;
}

std::vector<AccessNews> first_run_news(const FirstRunNewsState& s, Lang lang) {
    std::vector<AccessNews> out;
    const std::vector<std::string> steps = first_run_step_names(lang);
    const int at = static_cast<int>(s.step);
    out.push_back({"step", step_said(at, static_cast<int>(steps.size()), steps[static_cast<std::size_t>(at)], lang),
                   false});
    if (s.step == FirstRunStep::Runtimes && !s.started && s.list == FirstRunList::Failed) {
        out.push_back({"list", access_text(first_run_list_error(s.list_error, lang)), true});
    }
    if (s.started) {
        std::vector<std::string> failed;
        for (const FirstRunRow& r : s.rows) {
            if (r.state == FirstRunRowState::Failed || r.state == FirstRunRowState::Cancelled) {
                out.push_back({"row/" + r.name, access_join(lang, {r.label, r.line}),
                               r.state == FirstRunRowState::Failed});
                failed.push_back(r.label);
            }
        }
        const FirstRunProgress progress = first_run_progress(s.rows);
        if (progress.total > 0 && !progress.running()) {
            std::vector<std::string> parts{std::string(text(lang, Str::FirstRunDownloadedTitle))};
            if (!failed.empty()) {
                parts.push_back(fill(lang, Str::FirstRunDidntDownload, {join_labels(failed)}));
            }
            out.push_back({"downloads", access_join(lang, parts), false});
        }
    }
    // Windows' question is said before its window opens; then its answer.
    if (s.trust != FirstRunTrust::NotAsked && s.trust != FirstRunTrust::Checking) {
        out.push_back({"trust", access_text(first_run_https_line(s.trust, s.trust_error, lang)),
                       s.trust == FirstRunTrust::Asking || s.trust == FirstRunTrust::Failed});
    }
    if (!s.folder.empty()) {
        out.push_back({"folder", access_join(lang, {std::string(text(lang, Str::FirstRunFolderTitle)), s.folder}), false});
    }
    return out;
}

}  // namespace floserver::ui

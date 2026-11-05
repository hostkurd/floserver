#pragma once

// The main window's two pages for screen readers (model/access_tree.h): New
// project and the first-run page. What their step strip, template cards,
// check boxes, radio buttons and download rows are called and what state
// they say, and what's worth saying once when it happens (the step, a
// download that failed, "my-shop is ready"), from the same data the window
// paints. The window adds where each one is and what it does.

#include "model/access_tree.h"
#include "model/first_run.h"
#include "model/general_page.h"
#include "model/ui_strings.h"
#include "model/view_model.h"

#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// --- both pages ----------------------------------------------------------------

// A page's title: text that's a heading.
[[nodiscard]] AccessNode heading_node(std::string id, std::string_view title);

// The step strip: a list called Steps, one item per step, the current one
// selected, each with its place ("2 of 3"); a step done says so ("Language, done").
[[nodiscard]] AccessNode step_strip_node(std::string id, Lang lang);
[[nodiscard]] AccessNode step_node(std::string id, std::string_view name, int index, int current, int count, Lang lang);

// "Step 2 of 3, Runtimes": said when a page shows a step. `current` is 0-based.
[[nodiscard]] std::string step_said(int current, int count, std::string_view name, Lang lang);

// --- New project ---------------------------------------------------------------

// The strip's steps: Template, Name and folder, and Versions when the
// template has something to choose (`versions`); without the drawn numbers.
[[nodiscard]] std::vector<std::string> wizard_step_names(bool versions, Lang lang);
// The strip's current step for the page's `step` (3, making it, stays on the last).
[[nodiscard]] int wizard_strip_step(int step, bool versions);

// The page's title: "What are you building?", "Name and folder",
// "Versions", then "Making my-shop…", "my-shop is ready" or "The project wasn't made".
// `state` is projects.create's job state ("running", "done", "failed", "cancelled").
[[nodiscard]] std::string wizard_title(int step, const std::string& state, const std::string& name, Lang lang);

// Under the name box: the address it'll have, or the name's rule.
[[nodiscard]] std::string wizard_name_hint(const std::string& name, const FooterText& footer, Lang lang);
// Under the folder: "The project folder: C:\Sites\my-shop" (… while the name isn't one).
[[nodiscard]] std::string wizard_made_in(const std::string& parent, const std::string& name, Lang lang);

// A template card: a list item, its title, description and chips, selected
// when it's the one chosen. Its id is "wizard/card/<template id>".
[[nodiscard]] AccessNode template_card_node(const TemplateCard& card, bool chosen, int position, int count, Lang lang);

// What New project says once: its step; then making it, made, or why not.
struct WizardNewsState {
    int step = 0;              // 0 template, 1 name and folder, 2 versions, 3 making it
    bool versions = true;      // the template has a Versions step
    std::string state;         // projects.create's job state, at step 3
    std::string name;          // the project's
    std::string error;         // why it failed
    std::string templates_error;  // the template list couldn't be had
};
[[nodiscard]] std::vector<AccessNews> wizard_news(const WizardNewsState& s, Lang lang);

// --- the first-run page -----------------------------------------------------------

[[nodiscard]] std::vector<std::string> first_run_step_names(Lang lang);

// A language row's words: "Same as Windows" with "English now", "کوردی" with "Sorani".
struct LanguageRowText {
    std::string label;
    std::string note;
};
[[nodiscard]] LanguageRowText first_run_language_row(LangChoice choice, Lang system_lang, Lang lang);

// A language: a radio button, its note said after it.
[[nodiscard]] AccessNode language_radio_node(std::string id, const LanguageRowText& row, bool chosen, int position,
                                             int count);

// A runtime offered: a check box, "PHP 8.5, for PHP projects, 31 MB". nginx
// (always included) is ticked and can't be changed.
[[nodiscard]] AccessNode offer_node(std::string id, const FirstRunOffer& offer, bool ticked, int position, int count,
                                    Lang lang);

// A download's row: a group "PHP 8.5.12, Downloading…" (the sizes as it
// goes are its help, so they're not said on every tick), and while it runs
// a progress bar with how far it got.
[[nodiscard]] AccessNode download_row_node(std::string id, const FirstRunRow& row, Lang lang);
[[nodiscard]] AccessNode download_progress_node(std::string id, const FirstRunRow& row);

// What the first-run page says once: its step, the list not had, a
// download that failed, the downloads finished, Windows' certificate
// question and its answer, a new projects folder.
struct FirstRunNewsState {
    FirstRunStep step = FirstRunStep::Language;
    FirstRunList list = FirstRunList::Idle;
    std::string list_error;
    bool started = false;
    std::vector<FirstRunRow> rows;
    FirstRunTrust trust = FirstRunTrust::NotAsked;
    std::string trust_error;
    std::string folder;  // chosen with Change… (empty: not changed)
};
[[nodiscard]] std::vector<AccessNews> first_run_news(const FirstRunNewsState& s, Lang lang);

}  // namespace floserver::ui

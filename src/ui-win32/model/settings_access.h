#pragma once

// The Settings window's elements for screen readers (model/access_tree.h),
// approved 5 Nov. Settings draws a sidebar of sections and, beside it, one
// scrolling page of cards, texts and controls. The window says which
// controls there are (each focus stop: its role, state and place) and which
// parts hold others (a card, a runtime's row); the words come from what the
// page draws. add_page() puts the two together: a control takes the words
// drawn inside it as its name (a button's label, a radio button's label and
// the note under it), a picker takes them as its value, and the rest become
// text elements (headings for the titles) inside the card they're drawn in.
// The names and states here are tested in English, Arabic and Kurdish.

#include "model/access_tree.h"
#include "model/runtimes_page.h"
#include "model/ui_strings.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// How a piece of text is drawn: a page's title, a card's title, or anything else.
enum class TextStyle : std::uint8_t { Title, Heading, Body };

// Words as the page draws them, in the window's logical client coordinates.
struct PageText {
    AccessRect rect;
    std::string text;
    TextStyle style = TextStyle::Body;
    std::string id;  // its element's id under the page's; empty: made from its words (never for a secret)
};

// One element of the page: a control (a focus stop), or a part that holds
// others. `node.bounds` is where it's laid out, scrolled or not.
struct PageItem {
    AccessNode node;
    bool container = false;  // what's inside its bounds is its children (a card, a runtime's row)
    bool absorbs = false;    // the words drawn inside it are its own, not elements of their own
};

// Adds the page's elements under `parent` (a scrolling Pane): the items and
// the texts not taken by one, each inside the smallest container around it,
// in reading order (top to bottom, then from the reading start). Words drawn
// inside an absorbing item become its name when it has none, a picker's
// value when it has none, else its help (when they don't repeat its name).
// A container with no name takes its first heading's (a card's title).
// Controls with the same name ("Open", three times in Folders) get their
// container's name as help. What's outside `view` (scrolled away) is
// offscreen; every element can be scrolled into view. Text ids are made
// under `base` from their words, so a text that changes is a new element.
// `whole`, when given, gets each element's whole rectangle (its bounds are
// cut at the view's edge), for scrolling one into view.
void add_page(AccessTree& t, int parent, const std::string& base, std::vector<PageItem> items,
              const std::vector<PageText>& texts, const AccessRect& view, Lang lang,
              std::map<std::string, AccessRect, std::less<>>* whole = nullptr);

// What a drawn text is said as: without direction marks, and a masked
// password ("●●●●●●●●") as "Hidden".
[[nodiscard]] std::string page_said(std::string_view text, Lang lang);

// A section in the sidebar ("nav/<index>"), `index` of `count`, selected while it's shown.
[[nodiscard]] AccessNode section_node(int index, std::string_view label, bool shown, int count);

// The sidebar: a tab list called "Sections"; one section is always shown.
[[nodiscard]] AccessNode sections_node(Lang lang);

// One of a few choices (the language, the ports), selected or not; its name
// is the label drawn in its row when `name` is empty.
[[nodiscard]] AccessNode radio_node(std::string id, std::string_view name, bool selected, bool enabled = true);

// A checkbox (an extension, a module, Keep running), on or off.
[[nodiscard]] AccessNode check_node(std::string id, std::string_view name, bool on, bool enabled = true);

// A picker: its name ("PHP version"), its value the choice shown; Expand opens its menu.
[[nodiscard]] AccessNode combo_node(std::string id, std::string_view name, std::string_view value, bool enabled = true);

// A link drawn in the page's words ("Show all", "Reset"); `name` empty takes its words.
[[nodiscard]] AccessNode page_link_node(std::string id, std::string_view name, bool enabled = true);

// A field's Reset link: "Reset memory_limit".
[[nodiscard]] std::string reset_name(std::string_view field, Lang lang);

// A runtime's row in Settings > Runtimes: "PHP 8.5.1, Installed, Default,
// 3 projects · Shared sites", "PHP 8.3.29, Not installed, 30 MB", "nginx
// 1.30.6, Installed, Always used". A version alone gets its group's name
// first. `line` is what the row shows after its chip (the usage, the
// download's size, "Downloading… 19 MB of 31 MB", "Removing…").
[[nodiscard]] std::string runtime_row_name(std::string_view group, const RuntimeRow& row, std::string_view line,
                                           Lang lang);
// What the row is, alone: "PHP 8.5.1", "nginx 1.30.6".
[[nodiscard]] std::string runtime_row_label(std::string_view group, const RuntimeRow& row);

// A download's progress: a progress bar named after its row ("PHP 8.3.29"), `fraction` 0 to 1.
[[nodiscard]] AccessNode progress_node(std::string id, std::string_view said, double fraction);

// The keys across a layout. `before` are the focus stops' ids as the last
// layout left them (every key and click since indexes them), `focus` the
// index the keys are at, `after` the stops' ids now. The keys stay on the
// same id. When it went (a Download that became "Starting…"), they go to the
// stop before it still there (else the one after) and `lost` remembers both;
// when it's back (Cancel, the same id) and they haven't been moved, they go
// back to it. The index the keys are at now; -1 when there are no stops.
struct LostFocus {
    std::string id;         // the stop that went
    std::string neighbour;  // where the keys went meanwhile
};
[[nodiscard]] int keep_focus_index(const std::vector<std::string>& before, int focus,
                                   const std::vector<std::string>& after, LostFocus& lost);

// The page's scroll position as UI Automation says it: 0 to 100, or below 0
// when the page fits (`content` no taller than `view`).
[[nodiscard]] double scroll_percent(int scroll, int content, int view);
[[nodiscard]] double scroll_view_size(int content, int view);

}  // namespace floserver::ui

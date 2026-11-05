// What screen readers are told about the Settings window (src/ui-win32/model/settings_access.h).

#include "model/access_tree.h"
#include "model/main_access.h"
#include "model/settings_access.h"

#include <doctest/doctest.h>

using namespace floserver::ui;

namespace {

PageItem card(std::string id, AccessRect r) {
    PageItem item;
    item.node.id = std::move(id);
    item.node.role = AccessRole::Group;
    item.node.bounds = r;
    item.container = true;
    return item;
}

PageItem control(AccessNode n, AccessRect r) {
    PageItem item;
    item.node = std::move(n);
    item.node.bounds = r;
    item.absorbs = true;
    return item;
}

PageText words(std::string s, AccessRect r, TextStyle style = TextStyle::Body) { return {r, std::move(s), style, {}}; }

AccessNode by_id(const AccessTree& t, const std::string& id) {
    const AccessNode* n = t.node(t.find(id));
    REQUIRE(n != nullptr);
    return *n;
}

// Settings > General's Theme card, as the window lays it out: the title, three choices.
AccessTree theme_page(Lang lang, int scroll) {
    AccessTree t;
    AccessNode pane;
    pane.id = "page";
    pane.role = AccessRole::Pane;
    const int page = t.add(-1, pane);
    std::vector<PageItem> items;
    items.push_back(card("card/theme", {200, 100 - scroll, 800, 260 - scroll}));
    items.push_back(control(radio_node("general/theme/0", {}, true), {216, 140 - scroll, 784, 170 - scroll}));
    items.push_back(control(radio_node("general/theme/1", {}, false), {216, 172 - scroll, 784, 202 - scroll}));
    items.push_back(control(radio_node("general/theme/2", {}, false), {216, 204 - scroll, 784, 234 - scroll}));
    const std::vector<PageText> texts{
        words(std::string(text(lang, Str::SettingsGeneral)), {200, 10 - scroll, 800, 38 - scroll}, TextStyle::Title),
        words(std::string(text(lang, Str::GeneralTheme)), {216, 114 - scroll, 784, 138 - scroll}, TextStyle::Heading),
        words(std::string(text(lang, Str::GeneralSameAsWindows)), {242, 144 - scroll, 784, 164 - scroll}),
        words(std::string(text(lang, Str::GeneralLight)), {242, 176 - scroll, 784, 196 - scroll}),
        words(std::string(text(lang, Str::GeneralDark)), {242, 208 - scroll, 784, 228 - scroll}),
    };
    add_page(t, page, "general/", std::move(items), texts, {196, 0, 820, 600}, lang);
    t.finish();
    return t;
}

}  // namespace

TEST_CASE("settings access: a card's choices take the words drawn in them") {
    const AccessTree t = theme_page(Lang::En, 0);
    // The page's title first, then the card with its heading and its choices.
    CHECK(t.child_ids(t.find("page")) ==
          std::vector<std::string>{"general/text/General#0", "card/theme"});
    const AccessNode title = by_id(t, "general/text/General#0");
    CHECK(title.role == AccessRole::Text);
    CHECK(title.heading == 1);
    const AccessNode group = by_id(t, "card/theme");
    CHECK(group.name == "Theme");  // its heading's words
    CHECK(t.child_ids(t.find("card/theme")) == std::vector<std::string>{"general/text/Theme#0", "general/theme/0",
                                                                          "general/theme/1", "general/theme/2"});
    CHECK(by_id(t, "general/text/Theme#0").heading == 2);
    const AccessNode first = by_id(t, "general/theme/0");
    CHECK(first.role == AccessRole::Radio);
    CHECK(first.name == "Same as Windows");
    CHECK(first.selected == true);
    CHECK(by_id(t, "general/theme/2").name == "Dark");
    CHECK(by_id(t, "general/theme/2").selected == false);
    CHECK(first.scroll_item);
    CHECK_FALSE(first.offscreen);
}

TEST_CASE("settings access: the same card in Arabic and Kurdish") {
    const AccessTree ar = theme_page(Lang::Ar, 0);
    CHECK(by_id(ar, "card/theme").name == std::string(text(Lang::Ar, Str::GeneralTheme)));
    CHECK(by_id(ar, "general/theme/1").name == std::string(text(Lang::Ar, Str::GeneralLight)));
    const AccessTree ku = theme_page(Lang::Ku, 0);
    CHECK(by_id(ku, "card/theme").name == std::string(text(Lang::Ku, Str::GeneralTheme)));
    CHECK(by_id(ku, "general/theme/2").name == std::string(text(Lang::Ku, Str::GeneralDark)));
}

TEST_CASE("settings access: scrolled away is offscreen, and cut at the page's edge") {
    const AccessTree t = theme_page(Lang::En, 200);
    const AccessNode title = by_id(t, "general/text/General#0");
    CHECK(title.offscreen);
    CHECK(title.bounds == AccessRect{200, -190, 800, -162});  // where it is, out of view
    const AccessNode group = by_id(t, "card/theme");
    CHECK_FALSE(group.offscreen);
    CHECK(group.bounds == AccessRect{200, 0, 800, 60});
    CHECK(by_id(t, "general/theme/0").offscreen);
    CHECK_FALSE(by_id(t, "general/theme/2").offscreen);
}

TEST_CASE("settings access: notes, values and the same name twice") {
    AccessTree t;
    std::vector<PageItem> items;
    items.push_back(card("card/projects", {0, 0, 400, 100}));
    items.push_back(card("card/data", {0, 110, 400, 210}));
    const AccessNode open1 = button_node("folders/projects/open", {});
    items.push_back(control(open1, {300, 50, 380, 80}));
    const AccessNode open2 = button_node("folders/data/open", {});
    items.push_back(control(open2, {300, 160, 380, 190}));
    // A switch's row: its label and the note under it.
    items.push_back(control(switch_node("ports/switch/0", {}, true, true, Lang::En), {10, 220, 390, 270}));
    // A picker: the words in its box are its value.
    items.push_back(control(combo_node("php/field/display_errors", "display_errors", {}), {10, 280, 200, 310}));
    const std::vector<PageText> texts{
        words("Projects folder", {16, 10, 380, 34}, TextStyle::Heading),
        words("Open", {300, 50, 380, 80}),
        words("FloServer's data", {16, 120, 380, 144}, TextStyle::Heading),
        words("Open", {300, 160, 380, 190}),
        words("HTTPS for projects", {60, 224, 390, 244}),
        words("Each project at https://<name>.localhost", {60, 246, 390, 266}),
        words("On", {18, 284, 180, 306}),
    };
    add_page(t, -1, "p/", std::move(items), texts, {0, 0, 400, 400}, Lang::En);
    t.finish();
    CHECK(by_id(t, "folders/projects/open").name == "Open");
    CHECK(by_id(t, "folders/projects/open").help == "Projects folder");
    CHECK(by_id(t, "folders/data/open").help == "FloServer's data");
    const AccessNode sw = by_id(t, "ports/switch/0");
    CHECK(sw.name == "HTTPS for projects");
    CHECK(sw.help == "Each project at https://<name>.localhost");
    CHECK(sw.toggled == true);
    const AccessNode picker = by_id(t, "php/field/display_errors");
    CHECK(picker.name == "display_errors");
    CHECK(picker.value == "On");
    CHECK(picker.expandable);
    // Nothing taken by a control is left as a text of its own.
    CHECK(t.find("p/text/Open#0") < 0);
    CHECK(t.find("p/text/On#0") < 0);
}

TEST_CASE("settings access: a runtime's row, in each language") {
    RuntimeRow row;
    row.name = "php";
    row.version = "8.5.1";
    row.label = "8.5.1";
    row.installed = true;
    row.is_default = true;
    CHECK(runtime_row_name("PHP", row, "3 projects · Shared sites", Lang::En) ==
          "PHP 8.5.1, Installed, Default, 3 projects · Shared sites");
    CHECK(runtime_row_name("PHP", row, "", Lang::Ar) == "PHP 8.5.1، مثبّت، افتراضي");
    CHECK(runtime_row_name("PHP", row, "", Lang::Ku) == "PHP 8.5.1، دامەزراوە، بنەڕەت");

    RuntimeRow offer;
    offer.label = "8.3.29";
    CHECK(runtime_row_name("PHP", offer, "30 MB", Lang::En) == "PHP 8.3.29, Not installed, 30 MB");
    CHECK(runtime_row_name("PHP", offer, "30 MB", Lang::Ar) == "PHP 8.3.29، غير مثبّت، 30 MB");
    CHECK(runtime_row_name("PHP", offer, "30 MB", Lang::Ku) == "PHP 8.3.29، دانەمەزراوە، 30 MB");

    // A web server says what it is; the newest nginx is always used.
    RuntimeRow nginx;
    nginx.label = "nginx 1.30.6";
    nginx.installed = true;
    nginx.always_used = true;
    CHECK(runtime_row_name("Web servers", nginx, "", Lang::En) == "nginx 1.30.6, Installed, Always used");

    // The download's progress: named after its row, so nothing is said at each step.
    CHECK(runtime_row_label("PHP", offer) == "PHP 8.3.29");
    CHECK(runtime_row_label("Web servers", nginx) == "nginx 1.30.6");
    const AccessNode bar = progress_node("runtime/php/8.3.29/progress", runtime_row_label("PHP", offer), 19.0 / 31.0);
    CHECK(bar.role == AccessRole::Progress);
    CHECK(bar.name == "PHP 8.3.29");
    REQUIRE(bar.progress.has_value());
    CHECK(*bar.progress == doctest::Approx(19.0 / 31.0));
    CHECK(progress_node("x", "", 1.5).progress == 1.0);
}

TEST_CASE("settings access: the sidebar, links and pickers") {
    const AccessNode list = sections_node(Lang::En);
    CHECK(list.role == AccessRole::TabList);
    CHECK(list.name == "Sections");
    CHECK(list.selection_required);
    CHECK(sections_node(Lang::Ar).name == "الأقسام");
    CHECK(sections_node(Lang::Ku).name == "بەشەکان");
    const AccessNode php = section_node(2, text(Lang::En, Str::SettingsPhp), true, 8);
    CHECK(php.id == "nav/2");
    CHECK(php.role == AccessRole::TabItem);
    CHECK(php.name == "PHP");
    CHECK(php.selected == true);
    CHECK(php.position == 3);
    CHECK(php.set_size == 8);
    CHECK(php.keys == "Ctrl+Tab");

    CHECK(reset_name("memory_limit", Lang::En) == "Reset memory_limit");
    CHECK(reset_name("memory_limit", Lang::Ar) == "إعادة تعيين memory_limit");
    CHECK(reset_name("memory_limit", Lang::Ku) == "گەڕاندنەوەی memory_limit");
    const AccessNode more = page_link_node("php/more", {}, true);
    CHECK(more.role == AccessRole::Link);
    CHECK(more.invokable);
    CHECK(more.focusable);

    const AccessNode ext = check_node("php/ext/curl", "curl", false, false);
    CHECK(ext.role == AccessRole::CheckBox);
    CHECK(ext.toggled == false);
    CHECK_FALSE(ext.enabled);
}

TEST_CASE("settings access: a secret's words never make its id") {
    AccessTree t;
    const PageText shown{{0, 0, 100, 20}, "s3cret-Pa55", TextStyle::Body, "db/root-value"};
    add_page(t, -1, "page/", {}, {shown}, {0, 0, 400, 400}, Lang::En);
    t.finish();
    REQUIRE(t.find("page/db/root-value") >= 0);
    CHECK(by_id(t, "page/db/root-value").name == "s3cret-Pa55");
    for (const AccessNode& n : t.nodes()) {
        CHECK(n.id.find("s3cret") == std::string::npos);
    }
}

TEST_CASE("settings access: a masked password is said as hidden") {
    CHECK(page_said("\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F", Lang::En) == "Hidden");
    CHECK(page_said("\xE2\x97\x8F\xE2\x97\x8F", Lang::Ar) == "مخفية");
    CHECK(page_said("\xE2\x97\x8F", Lang::Ku) == "شاراوەیە");
    CHECK(page_said("s3cret", Lang::En) == "s3cret");
    CHECK(page_said(isolate_ltr("PHP 8.5"), Lang::Ar) == "PHP 8.5");
}

TEST_CASE("settings access: the keys stay on their control across a layout") {
    // Tab to PHP 8.3.29's Download (index 3), Enter: the row says "Starting…" and has no button.
    const std::vector<std::string> before{"nav/1", "runtimes/check", "runtime/php/8.5.1/button",
                                          "runtime/php/8.3.29/button", "runtime/php/8.2.30/button"};
    const std::vector<std::string> starting{"nav/1", "runtimes/check", "runtime/php/8.5.1/button",
                                            "runtime/php/8.2.30/button"};
    LostFocus lost;
    int focus = keep_focus_index(before, 3, starting, lost);
    CHECK(focus == 2);  // the stop before it, not the next row's button
    CHECK(lost.id == "runtime/php/8.3.29/button");
    // Still starting: the keys stay where they went.
    focus = keep_focus_index(starting, focus, starting, lost);
    CHECK(focus == 2);
    // Cancel comes (the same id): back to it.
    focus = keep_focus_index(starting, focus, before, lost);
    CHECK(focus == 3);
    CHECK(lost.id.empty());

    // Moved meanwhile (Tab): nothing goes back.
    LostFocus moved;
    focus = keep_focus_index(before, 3, starting, moved);
    focus = keep_focus_index(starting, focus + 1, before, moved);
    CHECK(focus == 4);  // runtime/php/8.2.30/button, where Tab took them
    CHECK(moved.id.empty());

    // PHP: Tab to "show all", Enter: every extension comes before it.
    const std::vector<std::string> few{"php/version", "php/ext/curl", "php/ext/gd", "php/more", "php/save"};
    const std::vector<std::string> all{"php/version", "php/ext/curl", "php/ext/gd", "php/ext/intl",
                                       "php/ext/mbstring", "php/more", "php/save"};
    LostFocus none;
    CHECK(keep_focus_index(few, 3, all, none) == 5);
    CHECK(keep_focus_index(all, 5, few, none) == 3);
    CHECK(none.id.empty());
    // Nothing left: no stop.
    CHECK(keep_focus_index(few, 1, {}, none) == -1);
}

TEST_CASE("settings access: the page's scroll position") {
    CHECK(scroll_percent(0, 500, 600) < 0);  // it fits
    CHECK(scroll_percent(0, 1000, 600) == doctest::Approx(0.0));
    CHECK(scroll_percent(200, 1000, 600) == doctest::Approx(50.0));
    CHECK(scroll_percent(900, 1000, 600) == doctest::Approx(100.0));
    CHECK(scroll_view_size(1000, 600) == doctest::Approx(60.0));
    CHECK(scroll_view_size(500, 600) == doctest::Approx(100.0));
}

TEST_CASE("settings access: a progress bar's value isn't a change (it's read when asked)") {
    const auto make = [](double done) {
        AccessTree t;
        AccessNode n = progress_node("bar", "Downloading…", 0);
        n.progress = done;
        t.add(-1, n);
        t.finish();
        return t;
    };
    CHECK(access_changes(make(0.1), make(0.2)).empty());
}

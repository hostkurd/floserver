// What screen readers are told about the main window (src/ui-win32/model/access_tree.h, main_access.h).

#include "model/access_tree.h"
#include "model/main_access.h"

#include <doctest/doctest.h>

using namespace floserver::ui;

namespace {

ProjectRow shop() {
    ProjectRow row;
    row.name = "my-shop";
    row.on = true;
    row.state = RowState::Asleep;
    row.address = "https://my-shop.localhost";
    row.chips = {"PHP 8.2", "MariaDB 11.4"};
    return row;
}

RowAccessState state_of(const ProjectRow& row, Lang lang) {
    RowAccessState s;
    s.on = row.on;
    s.end_text = std::string(text(lang, state_label(row.state)));
    s.position = 2;
    s.count = 5;
    return s;
}

AccessNode named(std::string id, std::string name = {}) {
    AccessNode n;
    n.id = std::move(id);
    n.name = std::move(name);
    return n;
}

}  // namespace

TEST_CASE("access: the tree links children in the order they were added") {
    AccessTree t;
    const int list = t.add(-1, named("list"));
    const int a = t.add(list, named("row/a"));
    const int status = t.add(-1, named("status"));
    const int b = t.add(list, named("row/b"));
    const int sw = t.add(a, named("row/a/switch"));
    t.finish();
    CHECK(t.first_top() == list);
    CHECK(t.last_top() == status);
    CHECK(t.node(list)->next == status);
    CHECK(t.node(status)->prev == list);
    CHECK(t.node(list)->first_child == a);
    CHECK(t.node(list)->last_child == b);
    CHECK(t.node(a)->next == b);
    CHECK(t.node(b)->prev == a);
    CHECK(t.node(sw)->parent == a);
    CHECK(t.node(b)->first_child == -1);
    CHECK(t.find("row/b") == b);
    CHECK(t.find("row/c") == -1);
    CHECK(t.node(-1) == nullptr);
    CHECK(t.child_ids(-1) == std::vector<std::string>{"list", "status"});
    CHECK(t.child_ids(list) == std::vector<std::string>{"row/a", "row/b"});
}

TEST_CASE("access: the element at a point is the deepest one shown there") {
    AccessTree t;
    AccessNode list = named("list");
    list.bounds = {0, 0, 100, 100};
    const int l = t.add(-1, list);
    AccessNode row = named("row/a");
    row.bounds = {0, 0, 100, 50};
    const int r = t.add(l, row);
    AccessNode sw = named("row/a/switch");
    sw.bounds = {10, 10, 30, 20};
    const int s = t.add(r, sw);
    AccessNode hidden = named("row/b");
    hidden.bounds = {0, 0, 100, 50};
    hidden.offscreen = true;
    t.add(l, hidden);
    t.finish();
    CHECK(t.at(15, 15) == s);
    CHECK(t.at(50, 40) == r);
    CHECK(t.at(50, 80) == l);
    CHECK(t.at(200, 200) == -1);
}

TEST_CASE("access: clip_rect keeps what's inside") {
    CHECK(clip_rect({0, 0, 50, 50}, {10, 10, 100, 100}) == AccessRect{10, 10, 50, 50});
    CHECK(clip_rect({0, 0, 5, 5}, {10, 10, 100, 100}).empty());
}

TEST_CASE("access: what changed between two trees") {
    const auto make = [](bool on, const std::string& state, bool with_b, const std::string& focus) {
        AccessTree t;
        const int list = t.add(-1, named("list"));
        t.add(list, named("row/a", "a, " + state));
        AccessNode sw = named("row/a/switch", "a");
        sw.toggled = on;
        t.add(t.find("row/a"), sw);
        if (with_b) {
            t.add(list, named("row/b", "b"));
        }
        t.focus = focus;
        t.finish();
        return t;
    };
    const AccessTree before = make(false, "Off", true, "row/a");
    CHECK(access_changes(before, before).empty());
    const auto changes = access_changes(before, make(true, "On", false, "row/a/switch"));
    REQUIRE(changes.size() == 4);
    CHECK(changes[0] == AccessChange{AccessChangeKind::Children, "list"});
    CHECK(changes[1] == AccessChange{AccessChangeKind::Name, "row/a"});
    CHECK(changes[2] == AccessChange{AccessChangeKind::Toggle, "row/a/switch"});
    CHECK(changes[3] == AccessChange{AccessChangeKind::Focus, "row/a/switch"});
    // The window's own children.
    AccessTree empty;
    empty.finish();
    CHECK(access_changes(empty, before).front() == AccessChange{AccessChangeKind::Children, ""});
}

TEST_CASE("access: direction marks aren't said") {
    CHECK(access_text(isolate_ltr("PHP 8.2")) == "PHP 8.2");
    CHECK(access_text(fill(Lang::Ar, Str::Needs, {"php 8.2"})) == "يحتاج php 8.2");
    CHECK(access_text("\xE2\x81\xA6x\xE2\x81\xA9\xE2\x80\x8F") == "x");
    CHECK(access_text("plain") == "plain");
    CHECK(access_join(Lang::En, {"a", "", isolate_ltr("b")}) == "a, b");
    CHECK(access_join(Lang::Ar, {"a", "b"}) == "a\xD8\x8C b");
}

TEST_CASE("access: a project row's name, in each language") {
    const ProjectRow row = shop();
    CHECK(project_row_name(row, state_of(row, Lang::En), Lang::En) ==
          "my-shop, on, Sleeping, PHP 8.2, MariaDB 11.4, https://my-shop.localhost");
    CHECK(project_row_name(row, state_of(row, Lang::Ar), Lang::Ar) ==
          "my-shop، مفعّل، نائم، PHP 8.2، MariaDB 11.4، https://my-shop.localhost");
    CHECK(project_row_name(row, state_of(row, Lang::Ku), Lang::Ku) ==
          "my-shop، هەڵکراوە، خەوتووە، PHP 8.2، MariaDB 11.4، https://my-shop.localhost");

    // On and on: the state isn't said twice. Off: the detail instead of an address.
    ProjectRow on = row;
    on.state = RowState::On;
    CHECK(project_row_name(on, state_of(on, Lang::En), Lang::En) ==
          "my-shop, on, PHP 8.2, MariaDB 11.4, https://my-shop.localhost");
    ProjectRow off = row;
    off.on = false;
    off.state = RowState::Off;
    off.address.clear();
    off.detail = isolate_ltr("C:\\Sites\\my-shop");
    CHECK(project_row_name(off, state_of(off, Lang::En), Lang::En) == "my-shop, off, PHP 8.2, MariaDB 11.4, C:\\Sites\\my-shop");

    // A Download button at its end is said as it reads.
    ProjectRow needs = off;
    needs.state = RowState::NeedsDownload;
    RowAccessState s = state_of(needs, Lang::En);
    s.end = RowEndKind::Download;
    s.end_text = download_button_text({{"php", "8.2"}, {"nginx", ""}}, Lang::En);
    CHECK(project_row_name(needs, s, Lang::En) ==
          "my-shop, off, Download 2 runtimes, PHP 8.2, MariaDB 11.4, C:\\Sites\\my-shop");
}

TEST_CASE("access: a project row is a list item with its place and selection") {
    ProjectRow row = shop();
    row.detail = "never shown";
    RowAccessState s = state_of(row, Lang::En);
    s.selected = true;
    const AccessNode n = project_row_node(row, s, Lang::En);
    CHECK(n.id == "row/my-shop");
    CHECK(n.role == AccessRole::ListItem);
    CHECK(n.position == 2);
    CHECK(n.set_size == 5);
    CHECK(n.selected == std::optional<bool>(true));
    CHECK(n.invokable);
    CHECK(n.focusable);
    CHECK(n.help == "never shown");
    CHECK(row_part_id("my-shop", "switch") == "row/my-shop/switch");
}

TEST_CASE("access: a switch says what it is, in each language") {
    const AccessNode en = switch_node("row/my-shop/switch", "my-shop", true, true, Lang::En);
    CHECK(en.role == AccessRole::Switch);
    CHECK(en.name == "my-shop");
    CHECK(en.kind == "toggle switch");
    CHECK(en.toggled == std::optional<bool>(true));
    CHECK(switch_node("k", std::string(text(Lang::Ar, Str::KeepRunning)), false, false, Lang::Ar).kind == "مفتاح تبديل");
    const AccessNode ku = switch_node("k", std::string(text(Lang::Ku, Str::KeepRunning)), false, false, Lang::Ku);
    CHECK(ku.name == "با هەمیشە کار بکات");
    CHECK(ku.kind == "سویچی گۆڕین");
    CHECK_FALSE(ku.enabled);
    CHECK(ku.toggled == std::optional<bool>(false));
}

TEST_CASE("access: the Shared sites row's name") {
    SharedRow row;
    row.state = RowState::Asleep;
    row.chips = {"PHP 8.5", "Apache"};
    row.detail = isolate_ltr("C:\\www");
    CHECK(shared_row_name(row, true, "localhost:8080", Lang::En) ==
          "Shared sites, on, Sleeping, PHP 8.5, Apache, localhost:8080, C:\\www");
    row.state = RowState::Off;
    CHECK(shared_row_name(row, false, {}, Lang::Ar) == "المواقع المشتركة، غير مفعّل، PHP 8.5، Apache، C:\\www");
}

TEST_CASE("access: a version picker, a tab, Keep running") {
    const VersionField field{Field::Php, "php", "PHP", {{"None", "none"}, {"8.4", "8.4"}}, 0, false};
    const AccessNode picker = picker_node("panel/my-shop/field/php", field, 1);
    CHECK(picker.role == AccessRole::ComboBox);
    CHECK(picker.name == "PHP");
    CHECK(picker.value == "8.4");
    CHECK(picker.expandable);
    CHECK(picker_node("p", field, 7).value.empty());

    const AccessNode tab = tab_node("panel/my-shop/tab/1", std::string(text(Lang::Ku, Str::Log)), false, 3, 4);
    CHECK(tab.role == AccessRole::TabItem);
    CHECK(tab.name == "تۆمار");
    CHECK(tab.selected == std::optional<bool>(false));
    CHECK(tab.position == 3);
    CHECK(tab.set_size == 4);

    CHECK(keep_running_detail(true, 0, Lang::En) == "On: never sleeps");
    CHECK(access_text(keep_running_detail(false, 0, Lang::En)) == "Off: sleeps after 10 minutes without requests");
    CHECK(access_text(keep_running_detail(false, 30, Lang::En)).find("30") != std::string::npos);
}

TEST_CASE("access: the status line, the footer and a toast") {
    const AccessNode stopped = status_node(std::string(text(Lang::En, Str::ServiceStopped)), true);
    CHECK(stopped.role == AccessRole::Button);
    CHECK(stopped.invokable);
    CHECK(stopped.name == "FloServer isn't running. Click here to start it.");
    const AccessNode connecting = status_node(std::string(text(Lang::Ar, Str::Connecting)), false);
    CHECK(connecting.role == AccessRole::Text);
    CHECK_FALSE(connecting.invokable);
    CHECK(connecting.name == "جارٍ الاتصال بـ FloServer…");

    FooterText footer;
    footer.counts = "2 running · 1 sleeping";
    footer.memory = "Service memory: 23.4 MB";
    CHECK(footer_node(footer, Lang::En).name == "2 running · 1 sleeping, Service memory: 23.4 MB");

    const ToastNodes t = toast_nodes(fill(Lang::Ar, Str::OpenFailed, {"C:\\x"}), Lang::Ar);
    CHECK(t.group.id == "toast");
    CHECK(t.group.name == "رسالة");
    CHECK(t.text.role == AccessRole::Text);
    CHECK(t.text.name.find("C:\\x") != std::string::npos);
    CHECK(t.close.role == AccessRole::Button);
    CHECK(t.close.name == "إغلاق الرسالة");
    CHECK(toast_nodes("x", Lang::Ku).close.name == "داخستنی پەیام");
    // A backup's toast has its Show in folder button; others have none.
    CHECK(t.action.name.empty());
    const ToastNodes with = toast_nodes("Backed up my-shop", Lang::En, "Show in folder");
    CHECK(with.action.id == "toast/action");
    CHECK(with.action.role == AccessRole::Button);
    CHECK(with.action.name == "Show in folder");
}

TEST_CASE("access: the log's lines as they're shown") {
    std::vector<LogLine> lines(3);
    lines[0].time = "19:41:00";
    lines[1].time = "19:41:01";
    lines[2].time = "19:41:02";
    for (LogLine& l : lines) {
        l.method = "GET";
        l.uri = "/";
        l.status = 200;
        l.duration = "1 ms";
    }
    const auto shown = log_access_lines(lines, 2, true, Lang::En);
    REQUIRE(shown.size() == 2);
    CHECK(shown[0] == log_text(lines[1]));
    CHECK(shown[1] == log_text(lines[2]));
    CHECK(log_access_lines({}, 5, true, Lang::En) == std::vector<std::string>{"No requests yet"});
    CHECK(log_access_lines({}, 5, false, Lang::En).empty());
}

TEST_CASE("a screen reader's requests wait for a menu or question to close") {
    AccessGate gate;
    // The first request into an empty queue posts the message that runs it; more just queue.
    CHECK(gate.post_queued(true));
    CHECK_FALSE(gate.post_queued(false));

    // Nothing open: the queue runs at once.
    CHECK(gate.may_run(false));
    CHECK_FALSE(gate.waiting());

    // A menu opened with the mouse (or a question, a picker) is open: the
    // request waits, and nothing replays it while it stays open.
    CHECK_FALSE(gate.may_run(true));
    CHECK(gate.waiting());
    CHECK_FALSE(gate.replay(true, false));
    CHECK(gate.waiting());
    // Closed: posted again, once.
    CHECK(gate.replay(false, false));
    CHECK_FALSE(gate.waiting());
    CHECK_FALSE(gate.replay(false, false));
    CHECK(gate.may_run(false));
}

TEST_CASE("a request's own menu or question doesn't let the next one run inside it") {
    AccessGate gate;
    REQUIRE(gate.may_run(false));
    gate.begin();
    // The request opened a menu (or Remove's question): a request that comes
    // meanwhile isn't posted, and its message (one already posted) doesn't run it.
    CHECK(gate.running());
    CHECK_FALSE(gate.post_queued(true));
    CHECK_FALSE(gate.may_run(false));
    CHECK_FALSE(gate.may_run(true));
    CHECK_FALSE(gate.replay(false, false));
    // The one running goes on to the queued ones when its menu has closed.
    gate.end();
    CHECK_FALSE(gate.running());
    CHECK(gate.post_queued(true));
}

TEST_CASE("requests that waited and went meanwhile aren't posted again") {
    AccessGate gate;
    CHECK_FALSE(gate.may_run(true));
    // The window went (or was made again in another language): the queue was emptied.
    CHECK_FALSE(gate.replay(false, true));
    CHECK_FALSE(gate.waiting());
    gate.clear();
    CHECK_FALSE(gate.waiting());
}

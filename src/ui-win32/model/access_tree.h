#pragma once

// What a screen reader is told about a window that draws its own controls:
// a plain tree of elements (role, name, state, where it is, what it does),
// made from what the window shows each time it changes. Plain C++ so the
// words and states are tested on every platform; access_provider.h hands
// the tree to Windows' UI Automation, and any window (the main one now,
// Settings and the first-run page later) only has to say what it draws.
//
// Elements are known by an id that stays the same while the thing it names
// is there ("row/my-shop/switch"): a screen reader holding one from before
// gets the element as it is now, or is told it's gone.

#include "model/ui_strings.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// What an element is, as UI Automation's control types.
enum class AccessRole : std::uint8_t {
    Pane,       // a part of the window (the project panel, a tab's page)
    Group,      // a few elements that belong together
    Text,
    Button,
    Switch,     // a button with an on/off state (Windows' toggle switch)
    Link,       // an address that opens in the browser
    List,
    ListItem,
    TabList,
    TabItem,
    ComboBox,   // a picker: its value, a menu of choices
    Edit,       // a box to type in, drawn by the window (real EDIT controls describe themselves)
};

// What a screen reader can ask an element to do.
enum class AccessAction : std::uint8_t {
    Invoke,  // a click
    Toggle,  // the switch's click
    Select,  // a list item or tab chosen
    Expand,  // a picker's menu opened
    Focus,   // the keyboard focus moved to it
};

// In the window's client coordinates, logical (from the reading start: a
// right-to-left window is mirrored, so these are what it draws with).
struct AccessRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool operator==(const AccessRect&) const = default;
    [[nodiscard]] bool empty() const { return right <= left || bottom <= top; }
};

// `r` cut to `clip`; empty when they don't overlap.
[[nodiscard]] AccessRect clip_rect(const AccessRect& r, const AccessRect& clip);

// What the window does when it's asked to act on an element: its own
// description of the part (a button's kind, an index, the project), kept
// as plain numbers so the tree doesn't know the window's types.
struct AccessTarget {
    int part = 0;
    int index = -1;
    std::string project;
    bool operator==(const AccessTarget&) const = default;
};

struct AccessNode {
    std::string id;
    AccessRole role = AccessRole::Group;
    std::string name;
    std::string help;    // said after the name ("Turn it off to stop it")
    std::string value;   // a picker's choice
    std::string kind;    // the role in words when Windows' own isn't right ("toggle switch")
    std::string keys;    // a keyboard shortcut ("Ctrl+N")
    AccessRect bounds;
    bool offscreen = false;  // scrolled out of view
    bool enabled = true;
    bool focusable = false;  // the window's keyboard focus can be on it
    bool invokable = false;
    bool expandable = false;           // a picker's menu
    std::optional<bool> toggled;       // a switch: on or off
    std::optional<bool> selected;      // a list item or tab
    bool selection_required = false;   // a container whose items are selected: one always is (tabs)
    int position = 0;                  // 1-based place among its kind ("2 of 5"); 0: not said
    int set_size = 0;
    AccessTarget target;
    // Filled by AccessTree::finish(): indexes into the tree's nodes, -1 for none.
    int parent = -1;
    int first_child = -1;
    int last_child = -1;
    int next = -1;
    int prev = -1;
};

class AccessTree {
public:
    // Adds `node` as the last child of `parent` (-1: the window itself);
    // its index. Ids must be unique; finish() links them up.
    int add(int parent, AccessNode node);
    void finish();

    [[nodiscard]] const std::vector<AccessNode>& nodes() const { return nodes_; }
    [[nodiscard]] const AccessNode* node(int index) const;
    // -1 when there's no such element (any more).
    [[nodiscard]] int find(std::string_view id) const;
    // The window's direct children.
    [[nodiscard]] int first_top() const { return first_top_; }
    [[nodiscard]] int last_top() const { return last_top_; }
    // The deepest element on screen at (x, y), or -1.
    [[nodiscard]] int at(int x, int y) const;
    // The child ids of `parent` (-1: the window), in order.
    [[nodiscard]] std::vector<std::string> child_ids(int parent) const;

    // The element the window's keyboard focus is on; empty: the window itself.
    std::string focus;

private:
    std::vector<AccessNode> nodes_;
    std::map<std::string, int, std::less<>> index_;
    int first_top_ = -1;
    int last_top_ = -1;
};

// What changed between two trees of the same window, for the events a screen
// reader listens to.
enum class AccessChangeKind : std::uint8_t {
    Focus,     // the keyboard focus moved to `id` (empty: the window)
    Name,
    Toggle,
    Selected,  // `id` became selected or not
    Value,
    Enabled,
    Children,  // the children of `id` came, went or moved (empty: the window's)
};

struct AccessChange {
    AccessChangeKind kind = AccessChangeKind::Name;
    std::string id;
    bool operator==(const AccessChange&) const = default;
};

[[nodiscard]] std::vector<AccessChange> access_changes(const AccessTree& before, const AccessTree& after);

// Text as it's said: without the direction marks put around code words for
// drawing (fill() and isolate_ltr() add them in Arabic and Kurdish).
[[nodiscard]] std::string access_text(std::string_view text);

// The pieces with the language's list comma between them ("my-shop, on,
// Sleeping"; "، " in Arabic and Kurdish), empty ones left out, marks taken out.
[[nodiscard]] std::string access_join(Lang lang, const std::vector<std::string>& parts);

}  // namespace floserver::ui

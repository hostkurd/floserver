#pragma once

// The main window's elements for screen readers (model/access_tree.h): what
// a project row, its switch, the Shared sites row, a version picker, the
// status line and a toast are called and what state they say, made from the
// same view-model data the window paints. The window adds where each one is
// and what it does when it's used.

#include "model/access_tree.h"
#include "model/ui_strings.h"
#include "model/view_model.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// What a row shows at its end: its state, a Download button, or an install's progress.
enum class RowEndKind : std::uint8_t { State, Download, Progress };

struct RowAccessState {
    bool on = false;        // the switch as shown (the user's choice while it's on its way)
    bool selected = false;  // its panel is open
    bool can_change = true; // the service answers and it isn't being removed
    RowEndKind end = RowEndKind::State;
    std::string end_text;   // the state's label, the button's words or the progress
    int position = 0;       // 1-based, among the rows shown (the search counts)
    int count = 0;
};

// "my-shop, on, Sleeping, PHP 8.2, MariaDB 11.4, https://my-shop.localhost":
// the name, the switch, what the row's end says (left out when it only
// repeats the switch: "on, On"), the chips, then the address or the detail.
[[nodiscard]] std::string project_row_name(const ProjectRow& row, const RowAccessState& state, Lang lang);

// The row as a list item ("row/<name>"), selected while its panel is open.
[[nodiscard]] AccessNode project_row_node(const ProjectRow& row, const RowAccessState& state, Lang lang);

// A switch called `name` ("my-shop", "Keep running"): a toggle switch, on or off.
[[nodiscard]] AccessNode switch_node(std::string id, std::string_view name, bool on, bool enabled, Lang lang);

// A button ("Open folder", "Download 2 runtimes").
[[nodiscard]] AccessNode button_node(std::string id, std::string_view name, bool enabled = true);

[[nodiscard]] AccessNode text_node(std::string id, std::string_view said);

// An address that opens in the browser.
[[nodiscard]] AccessNode link_node(std::string id, const std::string& address);

// The row's parts: its switch, its address and its Download button.
[[nodiscard]] std::string row_id(const std::string& project);
[[nodiscard]] std::string row_part_id(const std::string& project, std::string_view part);

// The Shared sites row: "Shared sites, on, Sleeping, PHP 8.5, Apache, localhost:8080".
// `end_text`: what its end shows (the address, a Download button, progress); `on`: its switch.
[[nodiscard]] std::string shared_row_name(const SharedRow& row, bool on, const std::string& end_text, Lang lang);

// A version picker: "PHP", its value the choice shown ("8.4", "Newest installed").
[[nodiscard]] AccessNode picker_node(std::string id, const VersionField& field, std::size_t shown);

// A tab of the project panel, `position` of `count`.
[[nodiscard]] AccessNode tab_node(std::string id, std::string_view label, bool shown, int position, int count);

// Keep running's second line: "On: never sleeps" / "Off: sleeps after 10 minutes…".
[[nodiscard]] std::string keep_running_detail(bool keep, std::uint64_t idle_minutes, Lang lang);

// The status line: the service's state; a button that starts it when it's
// stopped (`clickable`), else text.
[[nodiscard]] AccessNode status_node(const std::string& text, bool clickable);

// The status line's Mail button (D27): "Mail · 3 new" (or "Mail"); it opens the inbox in the browser.
[[nodiscard]] AccessNode mail_link_node(const std::string& text, Lang lang);

// The footer while the service is well: "2 running · 1 sleeping, nginx on ports 80 and 443, …".
[[nodiscard]] AccessNode footer_node(const FooterText& footer, Lang lang);

// A toast: a group with its words and its close button ("toast", "toast/text", "toast/close").
// With an action ("Show in folder"), its button too ("toast/action").
struct ToastNodes {
    AccessNode group;
    AccessNode text;
    AccessNode close;
    AccessNode action;  // its name empty when there's none
};
[[nodiscard]] ToastNodes toast_nodes(const std::string& text, Lang lang, const std::string& action = {});

// The log's lines as they're shown, oldest first: a group called Log with
// one text element per line ("No requests yet" when there are none).
[[nodiscard]] std::vector<std::string> log_access_lines(const std::vector<LogLine>& lines, std::size_t shown,
                                                        bool known, Lang lang);

// When the main window runs a screen reader's requests (as Settings does):
// after the call that asked has returned (a posted message), one at a time,
// and never while a menu or a question is open. Such a window runs its own
// message loop, and a request run inside it would re-enter code that is
// still waiting for its answer, so the requests wait for it to close, then
// run in the order they came. `open`: a menu, a question or a picker is open.
class AccessGate {
public:
    // A request was queued (`first`: into an empty queue). True: post the message that runs the queue.
    [[nodiscard]] bool post_queued(bool first) const { return first && !running_; }
    // The posted message came. True: run the queue now, between begin() and end().
    // False: one is being run, or something is open (the queue then waits for replay()).
    [[nodiscard]] bool may_run(bool open);
    void begin() { running_ = true; }
    void end() { running_ = false; }
    // Any message, or a menu or question that just closed. True: post the
    // message that runs the queue, which waited while something was open.
    [[nodiscard]] bool replay(bool open, bool queue_empty);
    // The window went: nothing waits any more.
    void clear() { waiting_ = false; }
    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] bool waiting() const { return waiting_; }

private:
    bool running_ = false;  // a request is being run (it may have a menu or question open)
    bool waiting_ = false;  // requests wait for a menu or question opened otherwise to close
};

}  // namespace floserver::ui

#pragma once

// UI Automation for a window that draws its own controls: the provider side
// that Narrator, NVDA and Accessibility Insights talk to. The window
// describes what it shows as an AccessTree (model/access_tree.h); this hands
// that tree to Windows and turns what a screen reader asks for into the
// window's own actions, and the window's changes into events. Any window
// can use it: answer WM_GETOBJECT with on_get_object(), detach() on
// WM_DESTROY, and call changed() after each paint.
//
// Elements are found again by their id on every call, in the tree as it is
// then: one that's gone answers UIA_E_ELEMENTNOTAVAILABLE, and nothing here
// keeps a pointer into the window once it's detached.

#include "model/access_tree.h"

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// The window's side.
class AccessSource {
public:
    AccessSource() = default;
    virtual ~AccessSource() = default;
    AccessSource(const AccessSource&) = delete;
    AccessSource& operator=(const AccessSource&) = delete;
    AccessSource(AccessSource&&) = delete;
    AccessSource& operator=(AccessSource&&) = delete;

    // The tree as the window is now.
    [[nodiscard]] virtual const AccessTree& access_tree() = 0;
    // A screen reader asked for `action` on element `id`. Done later (a
    // posted message), never inside the call: a menu or a question must not
    // run while the screen reader waits for the answer.
    virtual void access_request(const std::string& id, AccessAction action) = 0;
    // A screen reader asked a scrolling part (AccessNode::scrollable) to
    // show `percent` (0 to 100) of the way down; done later, as a request is.
    virtual void access_scroll_to(const std::string& /*id*/, double /*percent*/) {}
};

struct AccessBridge;
class AccessRootProvider;

class AccessRoot {
public:
    AccessRoot() = default;
    ~AccessRoot();
    AccessRoot(const AccessRoot&) = delete;
    AccessRoot& operator=(const AccessRoot&) = delete;
    AccessRoot(AccessRoot&&) = delete;
    AccessRoot& operator=(AccessRoot&&) = delete;

    // WM_CREATE: the window and what describes it.
    void attach(HWND hwnd, AccessSource* source);
    // WM_DESTROY: every element goes, and Windows is told.
    void detach();
    // WM_GETOBJECT: true with `result` set when it asked for UI Automation's root.
    [[nodiscard]] bool on_get_object(WPARAM wparam, LPARAM lparam, LRESULT& result);

    // A screen reader (or any UI Automation client) is listening: only then
    // is the tree made and compared after a paint.
    [[nodiscard]] static bool listening();
    // The events for `changes` (access_changes()), from the window's tree as it is now.
    void raise(const std::vector<AccessChange>& changes);
    // Said without moving the focus (a toast): `important` for errors.
    // `activity` tells one kind of message from another ("step", "trust"):
    // a newer one of the same kind replaces an older one still waiting.
    void notify(std::string_view text, bool important, std::string_view activity = {});

private:
    std::shared_ptr<AccessBridge> bridge_;
    AccessRootProvider* root_ = nullptr;  // one reference held while attached
};

// A real child control's name for screen readers (an EDIT whose label the
// window draws): "Search projects". Windows keeps it with the control.
void set_accessible_name(HWND control, std::string_view name);
// Its help, said after its name and value (the rule under New project's name box).
void set_accessible_help(HWND control, std::string_view help);

}  // namespace floserver::ui

#pragma once

// FloServer Settings (D20): its own top-level window, not modal, one at a
// time, opened from the main window's gear. A sidebar of sections; Runtimes
// is the first (download, remove, the default for new projects).
//
// Nothing runs while it's closed: it's destroyed on close, and while open it
// adds no timer or poll. Opening asks runtimes.list {"cached": true} once (no
// network); what's installed then comes from the main window's 2 s
// snapshot, and the full list is asked again only after a change is done or
// "Check for updates".

#include "canvas.h"
#include "model/runtimes_page.h"
#include "model/ui_strings.h"
#include "service_worker.h"
#include "win_util.h"
#include "window_style.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace floserver::ui {

class SettingsWindow {
public:
    // `closed_message` is posted to `owner` once the window is gone, so the
    // owner can let go of this object outside its window procedure.
    SettingsWindow(HINSTANCE instance, Lang lang, ServiceWorker& worker, HWND owner, UINT closed_message);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;
    SettingsWindow(SettingsWindow&&) = delete;
    SettingsWindow& operator=(SettingsWindow&&) = delete;

    // Opens where it was last time (`placement`), else at the mockup's size.
    [[nodiscard]] bool create(const std::optional<WINDOWPLACEMENT>& placement);
    void bring_to_front();
    // The worker's latest snapshot, as the main window took it.
    void on_snapshot(const Snapshot& snap);
    // Where it was when it closed, for the next time it opens.
    [[nodiscard]] const std::optional<WINDOWPLACEMENT>& placement() const { return placement_; }
    [[nodiscard]] bool is_open() const { return hwnd_ != nullptr; }

private:
    enum class Part : std::uint8_t { None, Nav, Check, Picker, Button };
    enum class Button : std::uint8_t { None, Remove, RemoveDisabled, Download, Cancel };
    struct Hit {
        Part part = Part::None;
        int group = -1;
        int row = -1;
        bool operator==(const Hit&) const = default;
    };
    struct RowGeometry {
        RECT row{};
        RECT version{};
        RECT chip{};  // empty when there's no chip
        Str chip_text = Str::SettingsDefaultChip;
        RECT text{};  // usage, note or progress
        std::string line;
        bool line_code = false;
        RECT bar{};   // a download's progress; empty when there's none
        double fraction = 0;
        RECT button{};
        Button button_kind = Button::None;
    };
    struct GroupGeometry {
        RECT card{};
        RECT title{};
        RECT picker_label{};
        RECT picker{};  // empty when the group has none
        std::vector<RowGeometry> rows;
    };
    struct Geometry {
        RECT nav{};
        RECT title{};
        RECT hint{};
        RECT check{};
        RECT message{};
        RECT note{};  // "Loading…" before the first answer
        std::vector<GroupGeometry> groups;
        int content_h = 0;  // of the main column, unscrolled
    };

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT msg, WPARAM wparam, LPARAM lparam);

    void on_create();
    void on_destroy();
    void on_paint();
    void on_key(WPARAM key);
    void on_click(const Hit& hit);
    void on_mouse_move(int x, int y);

    [[nodiscard]] int px(int dip) const;
    [[nodiscard]] float pxf(float dip) const;
    void apply_theme();
    void rebuild();  // page_ from what's known, then the layout
    [[nodiscard]] Geometry layout(HDC dc) const;
    void relayout();
    [[nodiscard]] int main_left() const;
    [[nodiscard]] Hit hit_test(int x, int y) const;
    [[nodiscard]] std::vector<Hit> focus_order() const;
    [[nodiscard]] const RECT* rect_of(const Hit& hit) const;
    void scroll_to(int target);
    void ensure_visible(const Hit& hit);
    void show_message(std::string text, bool error);
    void update_tip(const Hit& hit);

    [[nodiscard]] const RuntimeRow* row_of(const Hit& hit) const;
    [[nodiscard]] std::size_t picker_value(const RuntimeGroup& group) const;
    void check_for_updates();
    void picker_menu(int group);
    void run_button(const Hit& hit);

    void paint_shapes(Canvas& c) const;
    void paint_text(Canvas& c) const;
    void paint_button(Canvas& c, const RECT& r, bool hover, bool focused, bool enabled) const;
    void paint_ring(Canvas& c, const RECT& r) const;

    HINSTANCE instance_;
    Lang lang_;
    bool rtl_;
    ServiceWorker& worker_;
    HWND owner_;
    UINT closed_message_;
    HWND hwnd_ = nullptr;
    HWND tooltip_ = nullptr;
    std::wstring tip_text_;
    UINT dpi_ = 96;
    bool dark_ = false;
    int width_ = 0;
    int height_ = 0;
    UiFonts fonts_;
    win::UniqueIcon icon_small_;
    win::UniqueIcon icon_big_;
    std::optional<WINDOWPLACEMENT> placement_;

    // What the service said.
    bool connected_ = false;
    bool has_reply_ = false;
    nlohmann::json reply_;           // runtimes.list's full answer (merged after a failed check)
    bool loading_ = true;            // the first answer hasn't come
    bool checking_ = false;          // "Check for updates" is on its way
    Installed installed_;
    bool has_installed_ = false;
    nlohmann::json databases_;
    std::vector<RuntimeJob> jobs_;
    RuntimesPage page_;

    // What the user did, shown until the worker reports it done.
    std::set<std::string> starting_;   // ServiceWorker::install_key(settings_install_key(...))
    std::set<std::string> removing_;   // ServiceWorker::remove_key(...)
    std::map<std::string, std::optional<std::string>> pending_default_;  // runtime -> series
    std::string message_;
    bool message_error_ = false;

    Geometry geometry_;
    int scroll_ = 0;
    Hit hover_;
    Hit pressed_;
    int focus_ = -1;  // index into focus_order()
    bool show_focus_ = false;
    bool tracking_mouse_ = false;
};

}  // namespace floserver::ui

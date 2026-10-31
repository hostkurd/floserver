#pragma once

// FloServer Settings (D20): its own top-level window, not modal, one at a
// time, opened from the main window's gear. A sidebar of sections; Runtimes
// is the first (download, remove, the default for new projects).
//
// PHP is the second section: extensions and php.ini values per series. It
// asks php.get once when it's opened or the version changes, and changes
// wait for Save (one php.set).
//
// Nothing runs while it's closed: it's destroyed on close, and while open it
// adds no timer or poll. Opening asks runtimes.list {"cached": true} once (no
// network); what's installed then comes from the main window's 2 s
// snapshot, and the full list is asked again only after a change is done or
// "Check for updates".

#include "canvas.h"
#include "form_parts.h"
#include "model/php_page.h"
#include "model/runtimes_page.h"
#include "model/ui_strings.h"
#include "service_worker.h"
#include "win_util.h"
#include "window_style.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <array>
#include <map>
#include <memory>
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
    // Closes it as the user would (FloServer's window is being hidden),
    // asking first about PHP changes not saved; false: the user chose to stay.
    bool close();
    // The worker's latest snapshot, as the main window took it.
    void on_snapshot(const Snapshot& snap);
    // Where it was when it closed, for the next time it opens.
    [[nodiscard]] const std::optional<WINDOWPLACEMENT>& placement() const { return placement_; }
    [[nodiscard]] bool is_open() const { return hwnd_ != nullptr; }
    // Opens the PHP section on `series` ("8.5"), from a project's panel.
    void show_php(const std::string& series);

private:
    enum class Section : std::uint8_t { Runtimes, Php };
    enum class Part : std::uint8_t {
        None,
        Nav,     // row: the section
        Check,
        Picker,
        Button,
        // Settings > PHP; `row` is the extension's or field's index.
        PhpVersion,
        PhpRuntimes,  // "Go to Runtimes" when no PHP is installed
        PhpFind,      // the find box (an edit)
        PhpExt,
        PhpMore,      // "show all" / "show fewer"
        PhpEditIni,
        PhpField,     // an edit box, or a picker
        PhpReset,
        PhpDiscard,
        PhpSave,
    };
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
        double fraction = -1;  // below 0: no bar (most rows; a download of unknown size)
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
    struct PhpGeometry {
        RECT version_label{};
        RECT version{};     // empty while there's nothing to pick
        RECT note{};        // Loading…, or no PHP installed
        RECT runtimes{};    // Go to Runtimes
        RECT problems{};
        RECT ext_card{};
        RECT ext_title{};
        RECT ext_count{};
        RECT find{};
        std::vector<RECT> ext_cells;
        std::vector<RECT> ext_extras;  // "slows PHP", "not in this PHP"; empty when none
        std::vector<std::string> ext_names;  // what each cell shows
        RECT ext_note{};
        RECT more{};
        RECT ini_card{};
        RECT ini_title{};
        RECT ini_hint{};
        RECT edit_ini{};
        std::vector<RECT> labels;   // one per settings_php_ini_fields()
        std::vector<RECT> boxes;
        std::vector<RECT> resets;   // empty when the field isn't marked
        int error_field = -1;       // the field the last refusal was about
        RECT field_error{};         // its words, under it
        RECT custom_note{};
        RECT footer_text{};
        RECT discard{};
        RECT save{};
    };
    struct Geometry {
        std::array<RECT, 2> navs{};  // Runtimes, PHP
        RECT title{};
        RECT hint{};
        RECT check{};
        RECT message{};
        RECT note{};  // "Loading…" before the first answer
        std::vector<GroupGeometry> groups;
        PhpGeometry php;
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
    [[nodiscard]] bool is_focused(const Hit& hit) const;

    // --- Settings > PHP (settings_php.cpp) ---
    // Leaves PHP's unsaved changes as the user says; false: stay (Cancel).
    [[nodiscard]] bool settle_php_changes();
    void try_close();
    // `series`: the PHP version to show; empty for the newest.
    void open_section(Section section, const std::string& series = {});
    void load_php(const std::string& series);
    void on_php_answers(const Snapshot& snap);
    void make_php_edits();
    void fill_php_edits();
    void place_php_edits();
    void on_edit_change(int id);
    void on_edit_focus(int id);
    bool on_edit_key(int id, WPARAM key);
    [[nodiscard]] Hit hit_of_edit(int id) const;
    [[nodiscard]] FieldEdit* edit_of(const Hit& hit) const;
    void layout_php(HDC dc, Geometry& g, int x0, int x1, int y) const;
    [[nodiscard]] std::vector<PhpExtension> shown_extensions(std::size_t& hidden) const;
    [[nodiscard]] const RECT* php_rect_of(const Hit& hit) const;
    void php_focus_order(std::vector<Hit>& order) const;
    void on_php_click(const Hit& hit);
    void php_version_menu();
    void php_field_menu(int index);
    void save_php();
    void discard_php();
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> php_versions() const;
    void paint_php_shapes(Canvas& c) const;
    void paint_php_text(Canvas& c) const;

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

    Section section_ = Section::Runtimes;
    // Settings > PHP: the series shown, its php.get answer, and the changes
    // not saved yet.
    std::string php_series_;
    std::optional<PhpSeries> php_;
    bool php_loading_ = false;
    bool php_saving_ = false;
    PhpChanges php_changes_;
    std::string php_query_;
    std::string php_error_key_;  // the php.ini key Save was refused for
    std::string php_error_;      // and why, in the service's words
    bool php_all_ = false;  // "show all" extensions
    // The find box, then one per text field; only while the section is shown.
    std::vector<std::unique_ptr<FieldEdit>> edits_;
    std::vector<int> field_edit_;  // field index -> index in edits_, or -1 (a picker)
    win::UniqueBrush input_brush_;
    win::UniqueBrush input_set_brush_;

    Geometry geometry_;
    int scroll_ = 0;
    Hit hover_;
    Hit pressed_;
    int focus_ = -1;  // index into focus_order()
    bool show_focus_ = false;
    bool tracking_mouse_ = false;
};

}  // namespace floserver::ui

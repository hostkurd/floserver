#pragma once

// FloServer's main window: the project list with its On/Off switches, the
// top bar and the footer (step 5a), and the project panel beside the list
// (step 5b), in a tray icon's keeping (D2). Everything it shows comes from
// floserverd through ServiceWorker.

#include "canvas.h"
#include "model/ui_strings.h"
#include "service_worker.h"
#include "win_util.h"

#include <windows.h>
#include <shellapi.h>

#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace floserver::ui {

// Posted by a second FloServer.exe to bring this one forward.
inline constexpr UINT kShowWindowMessage = WM_APP + 1;

class MainWindow {
public:
    MainWindow(HINSTANCE instance, Lang lang, std::wstring class_name);
    ~MainWindow() = default;
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    [[nodiscard]] bool create(int show);

private:
    enum class Part : std::uint8_t {
        None,
        Row,
        Switch,
        Address,
        NewProject,
        Settings,
        Banner,
        // The project panel; `row` is the item's index where there are several.
        PanelClose,
        PanelAddress,
        PanelButton,
        PanelField,
        PanelCopyEnv,
        PanelKeep,
    };
    enum class PanelAction : std::uint8_t { Browser, Folder, Terminal, Studio };
    [[nodiscard]] static constexpr bool is_panel_part(Part part) {
        return part == Part::PanelClose || part == Part::PanelAddress || part == Part::PanelButton ||
               part == Part::PanelField || part == Part::PanelCopyEnv || part == Part::PanelKeep;
    }
    struct Hit {
        Part part = Part::None;
        int row = -1;         // index into visible_ when it was hit (a panel part: its index)
        std::string project;  // the row's project: the list may change before the click ends
        bool operator==(const Hit&) const = default;
    };
    struct RowGeometry {
        std::string project;
        RECT row{};
        RECT toggle{};
        RECT name{};
        RECT line2{};    // the address, or the detail
        RECT address{};  // the address text itself; empty when there's nothing to open
        RECT state{};
        std::vector<RECT> chips;  // the first chips.size() of the row's chips that fit
    };
    // The panel's pieces, from the last paint (logical coordinates).
    struct PanelGeometry {
        std::string project;
        RECT panel{};
        RECT close{};
        RECT title{};
        RECT line2{};    // the address, or the folder
        RECT address{};  // the address text itself; empty when there's nothing to open
        std::vector<std::pair<RECT, PanelAction>> buttons;
        RECT versions{};
        std::vector<std::pair<RECT, RECT>> fields;  // label, box; one per fields_ entry
        RECT saved_in{};
        RECT db_box{};
        RECT db_title{};
        RECT db_detail{};
        RECT copy_env{};  // empty when the project has no database
        RECT keep_title{};
        RECT keep_detail{};
        RECT keep_switch{};
        RECT log_label{};
        RECT log_box{};
    };
    struct Fonts {
        win::UniqueFont title;
        win::UniqueFont panel_title;
        win::UniqueFont mono;
        win::UniqueFont body;
        win::UniqueFont body_bold;
        win::UniqueFont caption;  // not "small": the Windows headers #define small
        win::UniqueFont small_bold;
        win::UniqueFont button;
        win::UniqueFont button_bold;
    };
    struct Notice {
        std::string text;
        bool error = false;
    };
    struct BannerText {
        std::string text;
        bool error = false;
        bool clickable = false;
    };

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    // The row of `project` in visible_, or -1.
    [[nodiscard]] int visible_index(const std::string& project) const;
    // The search box's placeholder, in the theme's colour.
    void paint_search_cue(HWND edit) const;
    static LRESULT CALLBACK search_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                                        DWORD_PTR self);
    LRESULT handle(UINT msg, WPARAM wparam, LPARAM lparam);

    void on_create();
    void on_destroy();
    void on_snapshot();
    void on_paint();
    void on_mouse_move(int x, int y);
    void on_click(const Hit& hit);
    void on_key(WPARAM key);
    void on_dpi_changed(UINT dpi, const RECT& suggested);
    void on_tray(LPARAM lparam, WPARAM wparam);

    [[nodiscard]] int px(int dip) const;
    [[nodiscard]] float pxf(float dip) const;
    void make_fonts();
    void make_icons();
    void apply_theme();
    void layout();  // positions the search box and tooltips
    void update_visible();
    void scroll_to(int target);
    void ensure_visible(int row);
    [[nodiscard]] int list_height() const;
    [[nodiscard]] Hit hit_test(int x, int y) const;
    [[nodiscard]] BannerText banner() const;
    [[nodiscard]] bool shows_rows() const;
    [[nodiscard]] bool switch_value(const ProjectRow& row) const;
    void toggle(int visible_row);
    void open_address(int visible_row);
    [[nodiscard]] bool is_settled(const std::string& key) const;
    void settle(const std::vector<std::string>& keys);
    void show_notice(std::string text, bool error);
    void put_on_clipboard(std::string& text);

    // --- the project panel (main_window_panel.cpp) ---
    [[nodiscard]] const ProjectRow* selected_row() const;
    [[nodiscard]] bool panel_open() const;
    [[nodiscard]] bool panel_beside_list() const;  // else it takes the list's place
    [[nodiscard]] RECT panel_rect() const;
    [[nodiscard]] PanelGeometry panel_geometry(HDC dc) const;
    [[nodiscard]] std::vector<Hit> panel_focus_order() const;
    [[nodiscard]] Hit panel_hit_test(int x, int y) const;
    [[nodiscard]] bool keep_value(const ProjectRow& row) const;
    [[nodiscard]] std::size_t field_value(const VersionField& field) const;
    void select(const std::string& project);
    void close_panel();
    void update_fields();
    void on_panel_click(const Hit& hit);
    void on_panel_key(WPARAM key);
    void run_panel_action(PanelAction action);
    void show_field_menu(std::size_t index);
    void toggle_keep_running();
    void open_folder(const std::string& path);
    void add_existing_folder();
    void open_terminal(const std::string& path);
    void paint_panel(Canvas& c, HDC dc, bool text_pass) const;
    void paint_switch(Canvas& c, const RECT& r, bool on, bool hover) const;
    void paint_button(Canvas& c, const RECT& r, bool hover, bool focused) const;
    void show_window();
    void quit();
    void add_tray_icon();
    void update_tray_tip();
    void show_tray_menu(POINT at);

    // Geometry shared by painting and hit testing (logical coordinates).
    [[nodiscard]] RECT top_bar() const;
    [[nodiscard]] RECT settings_button() const;
    [[nodiscard]] RECT new_project_button() const;
    [[nodiscard]] RECT search_box() const;
    [[nodiscard]] RECT banner_rect() const;
    [[nodiscard]] RECT card_rect() const;
    [[nodiscard]] RECT list_rect() const;  // inside the card's border
    [[nodiscard]] RECT footer_rect() const;
    [[nodiscard]] std::vector<RowGeometry> row_geometry(HDC dc) const;

    void paint_top_bar(Canvas& c, HDC dc, bool text_pass) const;
    void paint_banner(Canvas& c, bool text_pass) const;
    void paint_list(Canvas& c, HDC dc, bool text_pass) const;
    void paint_footer(Canvas& c, HDC dc, bool text_pass) const;

    HINSTANCE instance_;
    Lang lang_;
    bool rtl_;
    std::wstring class_name_;
    HWND hwnd_ = nullptr;
    HWND search_ = nullptr;
    HWND tooltip_ = nullptr;
    UINT dpi_ = 96;
    bool dark_ = false;
    int width_ = 0;  // client size
    int height_ = 0;
    Fonts fonts_;
    int new_button_width_ = 0;
    std::wstring tip_new_project_;
    std::wstring tip_settings_;
    std::wstring search_cue_;
    win::UniqueBrush input_brush_;
    win::UniqueIcon icon_small_;
    win::UniqueIcon icon_big_;
    UINT taskbar_created_ = 0;
    bool tray_added_ = false;
    std::wstring tray_tip_;
    bool quitting_ = false;

    std::unique_ptr<ServiceWorker> worker_;
    Snapshot snap_;
    // What the user chose, shown until the service has done it.
    std::map<std::string, bool> pending_;       // On/Off, per project
    std::map<std::string, bool> pending_keep_;  // Keep running, per project
    std::map<std::string, std::optional<std::string>> pending_use_;  // ServiceWorker::use_key -> value
    std::map<std::string, int> outstanding_;    // commands not yet finished, per ServiceWorker key
    std::string change_error_;
    Notice notice_;  // a short message in the banner ("copied")

    std::string selected_;  // the project whose panel is open
    std::string pending_added_;  // a folder just added; its panel opens once its row is listed
    bool picking_folder_ = false;  // the folder picker is open
    std::vector<VersionField> fields_;
    mutable PanelGeometry panel_geometry_;  // from the last paint
    int panel_focus_ = -1;  // keyboard focus among panel_focus_order(); -1: in the list
    std::string query_;
    std::vector<std::size_t> visible_;  // indexes into snap_.rows that match query_
    mutable std::vector<RowGeometry> geometry_;  // from the last paint

    int scroll_ = 0;
    Hit hover_;
    Hit pressed_;
    int focus_ = -1;  // keyboard focus in the list
    bool show_focus_ = false;
    bool tracking_mouse_ = false;
};

}  // namespace floserver::ui

#pragma once

// FloServer's main window: the project list with its On/Off switches, the
// top bar and the footer (step 5a), and the project panel beside the list
// (step 5b), in a tray icon's keeping (D2). Everything it shows comes from
// floserverd through ServiceWorker.

#include "canvas.h"
#include "form_parts.h"
#include "model/php_page.h"
#include "model/ui_strings.h"
#include "scroll_bar.h"
#include "service_worker.h"
#include "settings_window.h"
#include "win_util.h"
#include "window_style.h"

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
        Download,  // a row's Download button
        // The project panel; `row` is the item's index where there are several.
        PanelClose,
        PanelAddress,
        PanelTab,      // `row`: the tab's index in PanelGeometry::tabs
        PanelButton,
        PanelField,
        PanelCopyEnv,
        PanelPhpMyAdmin,  // Open in phpMyAdmin (the project's own login)
        PanelKeep,
        // The panel's PHP tab (D20); `row` is the field's index.
        PanelPhpSettings,
        PanelPhpChange,
        PanelPhpField,
        PanelPhpReset,
        PanelPhpDiscard,
        PanelPhpSave,
        // The New project page; `row` is the card's or field's index.
        WizCard,
        WizField,
        WizBrowse,
        WizAddFolder,
        WizCancel,
        WizBack,
        WizNext,
        // The Shared sites row pinned under the list (D19).
        SharedRow,
        SharedSwitch,
        SharedEnd,       // the address, or the Download button
        SharedOpen,      // Open folder
        SharedGear,      // its settings page
        // The Shared sites settings page; `row` is the field's index.
        WwwSwitch,
        WwwChange,
        WwwOpen,
        WwwField,
        WwwCopyEnv,
        WwwClose,
    };
    enum class PanelAction : std::uint8_t { Browser, Folder, Terminal, Studio, Download };
    [[nodiscard]] static constexpr bool is_panel_part(Part part) {
        return part == Part::PanelClose || part == Part::PanelAddress || part == Part::PanelTab ||
               part == Part::PanelButton || part == Part::PanelField || part == Part::PanelCopyEnv ||
               part == Part::PanelPhpMyAdmin || part == Part::PanelKeep || is_php_part(part);
    }
    [[nodiscard]] static constexpr bool is_php_part(Part part) {
        return part == Part::PanelPhpSettings || part == Part::PanelPhpChange || part == Part::PanelPhpField ||
               part == Part::PanelPhpReset || part == Part::PanelPhpDiscard || part == Part::PanelPhpSave;
    }
    // What a "Save" chosen on the way out of the PHP tab was for: done once
    // the save is answered without a refusal.
    struct PanelAct {
        enum class Kind : std::uint8_t { None, Tab, Project, Close } kind = Kind::None;
        PanelTab tab = PanelTab::General;
        std::string project;
    };
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
        // The state label, or a Download button, or an install's progress.
        enum class End : std::uint8_t { State, Download, Progress } end = End::State;
        std::string end_text;
    };
    // The panel's pieces, from the last paint (logical coordinates). The
    // header and the tab bar stay put; the tab's content scrolls inside
    // `content` (its pieces are where they are drawn, scroll included).
    struct PanelGeometry {
        std::string project;
        RECT panel{};
        RECT close{};
        RECT title{};
        RECT line2{};    // the address, or the folder
        RECT address{};  // the address text itself; empty when there's nothing to open
        RECT tab_bar{};
        std::vector<std::pair<RECT, PanelTab>> tabs;
        RECT content{};      // the scrolling part, under the tab bar
        int content_h = 0;   // the shown tab's content, unscrolled (General, Database, Log)
        RECT folder_label{};
        RECT folder{};
        std::vector<std::pair<RECT, PanelAction>> buttons;
        RECT versions{};
        std::vector<std::pair<RECT, RECT>> fields;  // label, box; one per fields_ entry
        RECT php_note{};  // empty unless PHP is off and the folder has .php files
        RECT saved_in{};
        RECT db_box{};
        RECT db_title{};
        RECT db_detail{};
        RECT copy_env{};  // empty when the project has no database
        RECT pma{};       // Open in phpMyAdmin
        RECT pma_hint{};  // Download phpMyAdmin in Settings → Runtimes, when it isn't installed
        RECT keep_title{};
        RECT keep_detail{};
        RECT keep_switch{};
        RECT log_label{};
        RECT log_box{};
    };
    // The panel's PHP tab, from the last paint (or layout()), scroll included.
    struct PhpPanelGeometry {
        std::string project;
        RECT panel{};
        RECT content{};     // the panel's scrolling part
        int content_h = 0;  // unscrolled
        RECT line2{};       // the PHP version it uses
        RECT open_settings{};
        RECT ext_title{};
        RECT ext_hint{};
        std::vector<RECT> chips;           // one per php_chips()
        RECT no_changes{};
        RECT change{};
        RECT ini_title{};
        RECT ini_hint{};
        std::vector<RECT> labels;          // one per php_keys_
        std::vector<RECT> boxes;
        std::vector<int> warned;           // per php_keys_: index into warnings, or -1
        std::vector<std::pair<RECT, std::string>> warnings;  // names PHP doesn't know
        RECT info{};
        RECT message{};
        RECT reset{};
        RECT discard{};
        RECT save{};
    };
    // The New project page's pieces, from the last paint.
    struct WizardGeometry {
        RECT area{};
        RECT steps{};
        RECT title{};
        RECT footer{};
        RECT next{};
        Str next_label = Str::WizardNext;
        RECT back{};
        RECT cancel{};
        RECT add_folder{};
        RECT message{};
        std::vector<RECT> cards;
        RECT name_label{};
        RECT name_box{};
        RECT name_hint{};
        RECT folder_label{};
        RECT folder_box{};
        RECT browse{};
        RECT folder_hint{};
        std::vector<std::pair<RECT, RECT>> fields;  // label, box
        RECT log_box{};
        int columns = 1;  // of the cards, for the arrow keys
    };
    struct WizardState {
        bool open = false;
        int step = 0;  // 0 template, 1 name and folder, 2 versions, 3 making it
        std::size_t card = 0;
        std::string name;
        std::string parent;
        std::vector<VersionField> fields;
        std::map<std::string, std::size_t> picks;  // field key -> chosen index
        // Kept from the card at step 0 -> 1: the card list may change later.
        std::string template_id;
        bool studio = false;
    };
    // The Shared sites row's pieces, from the last paint; empty when it isn't shown.
    struct SharedGeometry {
        RECT row{};
        RECT toggle{};
        RECT icon{};
        RECT name{};
        RECT state{};  // the pill after the title
        RECT line2{};
        std::vector<RECT> chips;  // the first chips.size() of the row's chips that fit
        // The address, a Download button or an install's progress; empty when there's none.
        enum class End : std::uint8_t { None, Address, Download, Progress } end = End::None;
        RECT end_rect{};
        std::string end_text;
        RECT open{};
        RECT gear{};
    };
    // The Shared sites settings page's pieces, from the last paint.
    struct WwwGeometry {
        RECT area{};
        RECT title{};
        RECT toggle{};
        RECT hint{};
        RECT folder_label{};
        RECT folder_box{};
        RECT change{};
        RECT open{};
        std::vector<std::pair<RECT, RECT>> fields;  // label, box; one per www_fields_ entry
        RECT db_box{};
        RECT db_title{};
        RECT db_detail{};
        RECT copy_env{};  // empty when there's no database
        RECT footer{};
        RECT close{};
    };
    // Step 3 and the job not finished yet (also before its first answer).
    [[nodiscard]] bool wizard_running() const;
    // False for a template with nothing to choose (no Versions page).
    [[nodiscard]] bool wizard_has_versions() const;
    using Fonts = UiFonts;
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
    void open_phpmyadmin_links(std::vector<Snapshot::DbAnswer>& answers);
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
    void place_scroll_bars();
    // A mouse message for a scroll bar; true when one took it.
    bool scroll_bar_mouse(UINT msg, int x, int y);
    void ensure_visible(int row);
    [[nodiscard]] int list_height() const;
    [[nodiscard]] Hit hit_test(int x, int y) const;
    [[nodiscard]] BannerText banner() const;
    [[nodiscard]] bool shows_rows() const;
    [[nodiscard]] bool switch_value(const ProjectRow& row) const;
    void toggle(int visible_row);
    void start_downloads(int visible_row);
    void start_downloads_for(const std::string& project);
    [[nodiscard]] std::string panel_button_text(PanelAction action) const;
    // What a row shows at its end (see RowGeometry::End).
    [[nodiscard]] RowGeometry::End row_end(const ProjectRow& row, std::string& end_text) const;
    void open_address(int visible_row);
    [[nodiscard]] bool is_settled(const std::string& key) const;
    void settle(const std::vector<std::string>& keys);
    void show_notice(std::string text, bool error);
    void put_on_clipboard(std::string& text);

    // --- the project panel (main_window_panel.cpp) ---
    [[nodiscard]] const ProjectRow* selected_row() const;
    [[nodiscard]] bool panel_open() const;
    // The tab shown for the selected project (General when no panel is open).
    [[nodiscard]] PanelTab shown_tab() const;
    // The tab that should be shown: panel_tab_, or General when the project lacks it.
    [[nodiscard]] PanelTab wanted_tab() const;
    // Shows wanted_tab() when it isn't shown: the project changed (a version
    // picked, floserver.json edited) and a tab came or went. Leaving the PHP
    // tab asks about changes not saved, as the user leaving it would.
    void sync_panel_tab();
    // Shows `tab`, asking first about PHP changes not saved when leaving the PHP tab.
    void select_tab(PanelTab tab);
    // The next tab (`by` 1) or the one before (-1), the keys on the tab bar.
    void step_panel_tab(int by);
    void run_panel_act(const PanelAct& act);
    // The panel's pieces for this paint: header, tabs, the shown tab's content,
    // with panel_scroll_ kept inside the content.
    void update_panel_geometry(HDC dc);
    [[nodiscard]] int panel_content_h() const;
    void panel_scroll_to(int target);
    // Scrolls the content so `hit` (a panel part) is in view.
    void ensure_panel_visible(const Hit& hit);
    [[nodiscard]] RECT panel_rect_of(const Hit& hit) const;
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
    // 16 px, centred in `r`: Windows' Settings glyph in the text pass, or
    // drawn by hand in the shapes pass when the icon font isn't there.
    void paint_gear(Canvas& c, const RECT& r, Rgb color, bool text_pass) const;

    // --- the panel's PHP tab (main_window_php.cpp) ---
    // The tab is shown for selected_: php.get is asked the first time for this
    // project (never while the panel or tab is closed).
    void open_php_part();
    // Leaves it, asking first about changes not saved when `ask`; false: stay.
    // A Save chosen then waits for its answer and does `then` after.
    bool leave_php_part(bool ask, const PanelAct& then);
    bool leave_php_part(bool ask);
    // The project changed or the panel closed: what php.get said is dropped.
    void forget_php_info();
    void ask_php_info();
    [[nodiscard]] bool php_dirty() const;
    [[nodiscard]] std::vector<std::pair<std::string, bool>> php_chips() const;  // name, on
    [[nodiscard]] PhpPanelGeometry php_panel_geometry(HDC dc) const;
    [[nodiscard]] Hit php_panel_hit_test(int x, int y) const;
    void add_php_focus_order(std::vector<Hit>& order) const;
    void on_php_panel_click(const Hit& hit);
    void php_change_menu();
    void php_display_menu(std::size_t index);
    bool save_project_php();
    void set_project_php_read_only(bool read_only);
    void on_project_php_answers();
    void sync_project_php();
    void make_project_php_edits();
    void place_project_php_edits();
    void on_php_edit_change(int id);
    void on_php_edit_focus(int id);
    bool on_php_edit_key(int id, WPARAM key);
    [[nodiscard]] FieldEdit* php_edit_of(std::size_t field) const;
    void paint_php_panel(Canvas& c, bool text_pass) const;
    // Settings opened on PHP for `series`.
    void open_settings_php(const std::string& series);

    // --- New project (main_window_wizard.cpp) ---
    [[nodiscard]] bool wizard_open() const;
    [[nodiscard]] const TemplateCard* wizard_card() const;
    [[nodiscard]] RECT wizard_rect() const;
    [[nodiscard]] std::string wizard_name() const;
    [[nodiscard]] WizardGeometry wizard_geometry(HDC dc) const;
    [[nodiscard]] Hit wizard_hit_test(int x, int y) const;
    [[nodiscard]] std::vector<RECT> card_chips(HDC dc, const RECT& card, const TemplateCard& t) const;
    void open_wizard();
    void close_wizard();
    void wizard_next();
    void wizard_back();
    void wizard_browse();
    void wizard_field_menu(std::size_t index);
    void on_wizard_click(const Hit& hit);
    void on_wizard_key(WPARAM key);
    void paint_wizard(Canvas& c, HDC dc, bool text_pass) const;

    // --- Shared sites (main_window_shared.cpp) ---
    [[nodiscard]] bool shows_shared() const;  // the row is there: shared.get has answered
    [[nodiscard]] bool shared_switch_value() const;
    [[nodiscard]] SharedGeometry shared_geometry(HDC dc) const;
    [[nodiscard]] Hit shared_hit_test(int x, int y) const;
    [[nodiscard]] std::vector<Part> shared_focus_order() const;
    void toggle_shared();
    void run_shared_part(Part part);
    void start_shared_downloads();
    void open_shared_address();
    void on_shared_key(WPARAM key);
    void paint_shared_row(Canvas& c, HDC dc, bool text_pass) const;
    [[nodiscard]] bool www_open() const { return www_open_; }
    [[nodiscard]] WwwGeometry www_geometry(HDC dc) const;
    [[nodiscard]] Hit www_hit_test(int x, int y) const;
    [[nodiscard]] std::vector<Hit> www_focus_order() const;
    [[nodiscard]] std::size_t www_field_value(const VersionField& field) const;
    void open_www();
    void close_www();
    void www_change_folder();
    void www_field_menu(std::size_t index);
    void on_www_click(const Hit& hit);
    void on_www_key(WPARAM key);
    void paint_www(Canvas& c, HDC dc, bool text_pass) const;
    static LRESULT CALLBACK name_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR self);
    // Settings (D20): its own window, one at a time.
    void open_settings();
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
    std::map<std::string, bool> installing_;    // Download clicked; the worker hasn't reported yet
    std::string change_error_;
    bool pma_busy_ = false;  // the panel's phpmyadmin.login is on its way
    Notice notice_;  // a short message in the banner ("copied")

    WizardState wizard_;
    mutable WizardGeometry wizard_geometry_;
    HWND name_edit_ = nullptr;  // the New project name box

    std::string selected_;  // the project whose panel is open
    std::string pending_added_;  // a folder just added; its panel opens once its row is listed
    bool picking_folder_ = false;  // the folder picker is open
    std::vector<VersionField> fields_;
    mutable PanelGeometry panel_geometry_;  // from the last paint
    int panel_focus_ = -1;  // keyboard focus among panel_focus_order(); -1: in the list
    std::string query_;
    std::vector<std::size_t> visible_;  // indexes into snap_.rows that match query_
    mutable std::vector<RowGeometry> geometry_;  // from the last paint

    // Shared sites (D19): the row's geometry and keyboard focus, and the settings page.
    mutable SharedGeometry shared_geometry_;
    bool shared_focused_ = false;  // the keys are on the row (focus_ is then -1)
    int shared_part_ = 0;          // index into shared_focus_order()
    bool www_open_ = false;
    int www_focus_ = -1;           // index into www_focus_order(); -1: none shown
    std::vector<VersionField> www_fields_;
    mutable WwwGeometry www_geometry_;

    // The panel's tabs: the one the user chose (kept across projects) and
    // how far the shown one's content is scrolled.
    PanelTab panel_tab_ = PanelTab::General;
    PanelTab panel_shown_ = PanelTab::General;  // the tab shown now (its enter has run)
    bool tab_ask_declined_ = false;  // the PHP tab went away and the user chose to stay: not asked again
    int panel_scroll_ = 0;

    // The panel's PHP tab (D20), for selected_ while php_open_ (the tab is shown).
    static constexpr int kPhpEditBase = 2000;                // php_edits_[i] has id kPhpEditBase + i
    static constexpr UINT kPhpBackMessage = WM_APP + 5;      // Escape in one of its boxes: back to the tabs
    static constexpr UINT kStepTabMessage = WM_APP + 6;      // Ctrl+Tab in one: wparam 1 the next tab, 0 the one before
    bool php_open_ = false;
    std::string php_series_;             // the series its PHP comes from
    std::optional<PhpSeries> php_info_;  // php.get's answer: placeholders, the extension list
    std::string php_info_project_;       // the project and series php_info_ (or the request on its way) is for
    std::string php_info_series_;
    bool php_info_wanted_ = false;       // not asked: the service wasn't there; asked once it is
    bool php_loading_ = false;
    bool php_saving_ = false;
    ProjectPhp php_before_;              // what floserver.json has
    ProjectPhp php_after_;               // with the user's changes
    std::string php_another_;            // "Another setting", as typed
    std::string php_message_;
    bool php_message_error_ = false;
    std::string php_error_key_;          // the field Save was refused for
    std::vector<std::string> php_keys_;  // the fields: the common ones, the project's others, "" (Another setting)
    std::vector<std::unique_ptr<FieldEdit>> php_edits_;
    std::vector<int> php_field_edit_;    // field -> index in php_edits_, or -1 (a picker)
    mutable PhpPanelGeometry php_geometry_;
    bool php_leave_after_save_ = false;  // "Save" chosen on the way out: leave once it's answered
    PanelAct php_then_;                  // and what for
    bool asking_ = false;                // the panel's save question is open
    bool quit_after_ask_ = false;        // Quit came meanwhile
    win::UniqueBrush input_set_brush_;

    // Settings (D20): only while it's open; where it was, for next time.
    std::unique_ptr<SettingsWindow> settings_;
    std::optional<WINDOWPLACEMENT> settings_placement_;

    int scroll_ = 0;
    // The list's and the panel's scroll bars (scroll_bar.h), placed from the
    // last layout before each mouse message and paint.
    static constexpr UINT_PTR kListScrollTimer = 3;
    static constexpr UINT_PTR kPanelScrollTimer = 4;
    ScrollBar list_bar_{kListScrollTimer, [this](int offset) { scroll_to(offset); }};
    ScrollBar panel_bar_{kPanelScrollTimer, [this](int offset) { panel_scroll_to(offset); }};
    Hit hover_;
    Hit pressed_;
    int focus_ = -1;  // keyboard focus in the list
    bool show_focus_ = false;
    bool tracking_mouse_ = false;
};

}  // namespace floserver::ui

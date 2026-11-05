#pragma once

// FloServer's main window: the project list with its On/Off switches, the
// top bar and the footer (step 5a), and the project panel beside the list
// (step 5b), in a tray icon's keeping (D2). Everything it shows comes from
// floserverd through ServiceWorker.

#include "access_provider.h"
#include "canvas.h"
#include "form_parts.h"
#include "model/code_editor.h"
#include "model/first_run.h"
#include "model/php_page.h"
#include "model/project_menu.h"
#include "model/toast.h"
#include "model/ui_strings.h"
#include "recycle_bin.h"
#include "scroll_bar.h"
#include "toast_view.h"
#include "uninstall.h"
#include "service_worker.h"
#include "settings_window.h"
#include "win_util.h"
#include "window_style.h"

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace floserver::ui {

// Posted by a second FloServer.exe to bring this one forward.
inline constexpr UINT kShowWindowMessage = WM_APP + 1;

// Screen readers see it through AccessSource (main_window_access.cpp).
class MainWindow final : private AccessSource {
public:
    // `prefs`: window.json (Settings > General); `lang` is what they resolve to.
    MainWindow(HINSTANCE instance, Lang lang, const WindowPrefs& prefs, std::wstring class_name);
    ~MainWindow() override = default;
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
        Banner,      // the service's state: the clickable status line (Start FloServer)
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
        PanelRemove,   // Remove from FloServer…
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
        WizName,  // the name box (a real EDIT): a stop in the page's keyboard order
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
        SharedGear,      // its settings: Settings > Shared sites
        // The first-run page; `row` is the language's, runtime's or download's index.
        FrLang,
        FrTick,
        FrCancel,
        FrRetry,
        FrChange,
        FrCreate,
        FrAdd,
        FrBack,
        FrNext,
        FrSkipSetup,  // every step's Skip setup
        FrWithout,    // step 2's Continue without downloading
    };
    enum class PanelAction : std::uint8_t { Browser, Folder, Terminal, Editor, Studio, Download };
    // The panel's square icon buttons (approved 3 and 4 Nov), in their order.
    static constexpr std::array<PanelAction, 5> kIconActions = {PanelAction::Browser, PanelAction::Folder,
                                                                PanelAction::Terminal, PanelAction::Editor,
                                                                PanelAction::Studio};
    [[nodiscard]] static constexpr bool is_icon_action(PanelAction a) { return a != PanelAction::Download; }
    [[nodiscard]] static constexpr bool is_panel_part(Part part) {
        return part == Part::PanelClose || part == Part::PanelAddress || part == Part::PanelTab ||
               part == Part::PanelButton || part == Part::PanelField || part == Part::PanelCopyEnv ||
               part == Part::PanelPhpMyAdmin || part == Part::PanelKeep || part == Part::PanelRemove ||
               is_php_part(part);
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
        RECT remove{};  // Remove from FloServer…, at the General tab's end
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
        std::vector<RECT> step_rects;  // each step's words in the strip
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
        // Where the keys are on the page (wizard_focus_order()); Part::None:
        // the step's first stop (wizard_focus()).
        Hit focus;
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
    // The first-run page (approved 2 Nov): what's chosen and how far it got.
    // It belongs to this object, not the window, so it's kept when the
    // window is made again in another language.
    struct FirstRunState {
        bool decided = false;  // shown or not: asked once
        bool open = false;
        FirstRunStep step = FirstRunStep::Language;
        Lang system_lang = Lang::En;  // what "Same as Windows" gives
        // Step 2: runtimes.list's answer, asked when the step is first shown.
        using List = FirstRunList;
        List list = List::Idle;
        bool refreshing = false;  // a fresh list was asked for
        std::string list_error;
        std::vector<FirstRunOffer> offers;
        std::vector<bool> ticks;
        // "Download and continue": what was asked for, and how the ones
        // that weren't installed ended.
        bool started = false;
        std::vector<Download> wanted;
        std::map<std::string, FirstRunEnd> ends;
        FirstRunTrust trust = FirstRunTrust::NotAsked;
        std::string trust_error;
        std::string ca_der;  // base64, from certs.ensure_ca
        bool ca_checked = false;  // web.settings.get asked for the CA on step 3 (skipped downloads)
        bool without = false;     // "Continue without downloading" was chosen
        // Step 3: the projects folder chosen with Change… (folders.set).
        std::string folder;
        std::string folder_ask;  // the folders.set on its way (its purpose)
        std::string folder_picked;
        int scroll = 0;
        // The stop the keys are on (one of first_run_focus_order()), kept by
        // what it is, not its place: the stops come and go as the page changes.
        // Part::None: none. `focus_order` is the list it was last seen in.
        Hit focus;
        std::vector<Hit> focus_order;
    };
    // Its pieces, from the last paint.
    struct FirstRunGeometry {
        RECT area{};
        RECT steps{};
        std::vector<RECT> step_rects;  // each step's words in the strip (as many as fit)
        RECT content{};  // the part that scrolls, between the steps and the footer
        int content_h = 0;
        RECT title{};
        RECT intro{};
        RECT legend{};
        RECT card{};
        std::vector<RECT> rows;
        std::vector<RECT> cancels;   // a download row's Cancel; empty when it has none
        std::vector<RECT> details;   // step 3: each row's second line
        RECT note{};
        RECT info{};       // the certificate's box
        RECT info_note{};  // the line under it
        RECT message{};    // loading, or why the list couldn't be had
        RECT retry{};
        RECT change{};
        RECT create{};
        RECT add{};
        RECT footer{};
        RECT footer_text{};
        RECT skip{};     // Skip setup, at the reading start
        RECT back{};
        RECT without{};  // Continue without downloading
        RECT next{};
        Str next_label = Str::WizardNext;
        bool next_primary = true;
    };
    // Step 3 and the job not finished yet (also before its first answer).
    [[nodiscard]] bool wizard_running() const;
    // False for a template with nothing to choose (no Versions page).
    [[nodiscard]] bool wizard_has_versions() const;
    using Fonts = UiFonts;
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
    // The service's state, for the status line at the bottom (the first-run
    // page: over it): connecting, stopped (clickable), stopped answering...
    // Empty while it's connected and well.
    [[nodiscard]] BannerText status_line() const;
    [[nodiscard]] bool shows_rows() const;
    [[nodiscard]] bool switch_value(const ProjectRow& row) const;
    void toggle(int visible_row);
    void toggle_project(const std::string& project);
    // Being removed, or removed and still in the last list: nothing may change it.
    [[nodiscard]] bool leaving(const std::string& project) const;
    void start_downloads(int visible_row);
    void start_downloads_for(const std::string& project);
    [[nodiscard]] std::string panel_button_text(PanelAction action) const;
    // What a row shows at its end (see RowGeometry::End).
    [[nodiscard]] RowGeometry::End row_end(const ProjectRow& row, std::string& end_text) const;
    void open_address(int visible_row);

    // --- a row's right-click menu and removing a project (main_window_menu.cpp) ---
    // WM_CONTEXTMENU: a right-click on a row (selected first), or the menu
    // key or Shift+F10 at the row (or panel) the keys are on.
    void on_context_menu(LPARAM lparam);
    void show_project_menu(const std::string& project, POINT at);
    void show_shared_menu(POINT at);
    void run_project_command(const std::string& project, MenuCommand command);
    // Asks first (with the database box when it has one, the Recycle Bin box
    // when its folder may go there), then projects.remove; its answer comes
    // in on_remove_answers(). The folder goes only once the project is removed.
    void ask_remove(const std::string& project);
    // The question itself, once ask_remove()'s check has said whether `folder` may be offered.
    void ask_remove_now(const std::string& project, const std::string& folder, FolderOffer offer);
    struct Removed {
        std::string project;
        bool dropped = false;   // its database was deleted
        bool trash = false;     // its folder goes to the Recycle Bin now
        std::string path;       // that folder, as the service confirmed it
        bool refused = false;   // asked for, but the service had another folder for it
    };
    // projects.remove's answers, taken out of `answers`; the projects removed.
    [[nodiscard]] std::vector<Removed> on_remove_answers(std::vector<Snapshot::DbAnswer>& answers);
    // The folders a project's folder is never moved with (model/folder_removal.h):
    // Windows' own, FloServer's, the other projects'.
    [[nodiscard]] FolderGuard folder_guard(const std::string& project) const;
    // The Recycle Bin moves that finished: what became of each folder, said.
    void on_recycled();
    void copy_address(const std::string& address);
    [[nodiscard]] bool is_settled(const std::string& key) const;
    void settle(const std::vector<std::string>& keys);
    // A toast (model/toast.h): information goes by itself, an error stays until closed.
    void show_notice(std::string text, bool error);
    // An error shown from before goes as the user tries again.
    void dismiss_error();
    void toast_event(ToastEvent event);
    void sync_toast_timer();
    // The toast's bottom edge: above the footer (the first-run page's buttons).
    [[nodiscard]] int toast_bottom() const;
    // True when it's there.
    bool put_on_clipboard(std::string& text);

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
    void toggle_keep_running_for(const std::string& project);
    void open_folder(const std::string& path);
    void add_existing_folder();
    void open_terminal(const std::string& path);
    void paint_panel(Canvas& c, HDC dc, bool text_pass) const;
    void paint_switch(Canvas& c, const RECT& r, bool on, bool hover) const;
    void paint_button(Canvas& c, const RECT& r, bool hover, bool focused) const;
    // A square open button's stroke icon (globe, folder, >_, </>, Studio's layers), 16 px, centred in `r`.
    void paint_open_icon(Canvas& c, const RECT& r, PanelAction action, Rgb color) const;
    // An icon button's tooltip, which is also its accessible name: "Open
    // folder", "Open in VS Code", "Choose a code editor…".
    [[nodiscard]] std::string panel_button_tip(PanelAction action) const;
    // Open in browser only while the service is there; the others always.
    [[nodiscard]] bool panel_action_enabled(PanelAction action) const;
    // The tooltips follow the icon buttons (after each paint's geometry).
    void place_panel_tips();
    // FloCMS Studio with the project's folder (the window has it: no
    // request to the service). Quietly: nothing said when Studio isn't installed.
    void open_studio(const std::string& path, bool quietly);
    // The code editor with the project's folder; Settings → General's
    // Code editor when there's none.
    void open_editor(const std::string& path);
    void open_settings_editor();
    // The code editor projects open in, as last looked for (painting only
    // reads it). ensure_editor() looks once a window session, never while
    // painting: when the panel is laid out, before the row menu, on a click.
    // forget_editor() (General's choice changed, Settings closed) asks again.
    [[nodiscard]] const std::optional<EditorTarget>& editor() const { return editor_; }
    void ensure_editor();
    void forget_editor();
    // A mouse button went down: the keyboard focus frames hide (Windows' rule).
    void mouse_used();
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
    void make_name_edit();
    // After the window was made again: the name box back, with `name` in it.
    void remake_wizard_edit(const std::string& name);
    void close_wizard();
    void wizard_next();
    void wizard_back();
    void wizard_browse();
    void wizard_field_menu(std::size_t index);
    void on_wizard_click(const Hit& hit);
    void on_wizard_key(WPARAM key);
    // The page's keyboard stops, in reading order: the cards (one stop, the
    // arrows choose), the name box, Browse, the pickers, the footer's buttons.
    [[nodiscard]] std::vector<Hit> wizard_focus_order() const;
    // The stop the keys are on now: wizard_.focus, or the step's first.
    [[nodiscard]] Hit wizard_focus() const;
    // The keys to `hit` (the name box takes the real focus); `keyboard`: its frame shows.
    void wizard_focus_to(const Hit& hit, bool keyboard);
    // Tab (`back`: Shift+Tab) from where the keys are.
    void wizard_tab(bool back);
    // The name box's help for screen readers: its address or its rule.
    void sync_name_help();
    void paint_wizard(Canvas& c, HDC dc, bool text_pass) const;

    // --- the first-run page (main_window_first_run.cpp) ---
    [[nodiscard]] bool first_run_open() const { return first_run_.open; }
    // Shown when it's wanted (first_run_wanted), once the service has
    // answered; FLOSERVER_FIRST_RUN=1 shows it at once.
    void decide_first_run();
    void open_first_run();
    // Done: finished, skipped or the window closed. Marked in window.json.
    void finish_first_run();
    void first_run_ask_list();
    // The snapshot's answers for the page (the list, the CA, folders.set, how installs ended).
    void on_first_run_answers(Snapshot& s);
    void first_run_start_downloads();
    // On to step 3 without (more) downloads; the CA's state is asked once.
    void first_run_to_ready();
    // Windows' own question, after certs.ensure_ca's answer (kFirstRunTrustMessage).
    void first_run_trust_now();
    // A question or picker (asking_) was just answered: Windows' trust question, if it waited, now.
    void first_run_after_ask();
    void first_run_change_folder();
    [[nodiscard]] std::string first_run_folder() const;
    [[nodiscard]] std::vector<FirstRunRow> first_run_rows_now() const;
    [[nodiscard]] FirstRunGeometry first_run_geometry(HDC dc, int scroll) const;
    [[nodiscard]] Hit first_run_hit_test(int x, int y) const;
    [[nodiscard]] std::vector<Hit> first_run_focus_order() const;
    [[nodiscard]] RECT first_run_rect_of(const Hit& hit) const;
    void first_run_scroll_to(int target);
    // The keys to stop `index` of first_run_focus_order(), scrolled into view.
    void first_run_focus_at(int index);
    // Where first_run_.focus is in first_run_focus_order(); -1: none, or gone.
    [[nodiscard]] int first_run_focus_index() const;
    // After each layout: a stop that went (a download's Cancel when it
    // finished) hands the keys to the next one still there, or the one before.
    void first_run_sync_focus();
    // The footer's words: the privacy line, the total to download, what comes next.
    [[nodiscard]] std::string first_run_footer_words() const;
    void on_first_run_click(const Hit& hit);
    void on_first_run_key(WPARAM key);
    void paint_first_run(Canvas& c, HDC dc, bool text_pass) const;
    // Documents\FloServer: where new projects go when the service has no projects folder yet.
    [[nodiscard]] static std::string default_projects_parent();

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
    // The row's gear: Settings, at its Shared sites section.
    void open_settings_shared();
    static LRESULT CALLBACK name_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR self);
    // Settings (D20): its own window, one at a time.
    void open_settings();
    // Settings > General changed one of the window's preferences (PrefChange, the choice's index).
    void on_pref_change(WPARAM change, LPARAM value);

    // --- Remove FloServer… (main_window_uninstall.cpp, D21) ---
    // Settings > General's question was answered: Settings closes, and the
    // steps run in order (uninstall.h), each said here as it starts.
    void start_uninstall(const UninstallTicks& ticks);
    // A step finished on its thread: the next one, or what went wrong.
    void on_uninstall();
    // The worker stopped the service (`error`: not within its few seconds): the clean-up next.
    void uninstall_stopped(const std::string& error);
    // Stopped before anything was deleted: said, and FloServer goes on as it was.
    void uninstall_failed(const std::string& message);
    // The end: Windows' uninstall (or what was left said), then quit.
    void uninstall_done(const Uninstaller::Answer& answer);
    // Windows' own folders and the data folder, never moved to the Recycle Bin.
    [[nodiscard]] static FolderGuard windows_folder_guard();
    // Both windows made again in `lang` (its direction set at creation),
    // keeping where they are, what's open and what's typed.
    void change_language(Lang lang);
    // The top-level window, at `placement` when there's one (shown as
    // `show` says), else centred on its screen.
    [[nodiscard]] bool make_window(int show, const WINDOWPLACEMENT* placement);
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

    // --- screen readers (main_window_access.cpp, approved 5 Nov) ---
    // AccessSource: the tree as the window is now; a screen reader's request, run later.
    [[nodiscard]] const AccessTree& access_tree() override;
    void access_request(const std::string& id, AccessAction action) override;
    // What the last paint showed, as elements; the focus where the keys are.
    [[nodiscard]] AccessTree build_access_tree() const;
    void add_list_access(AccessTree& t) const;
    void add_shared_access(AccessTree& t) const;
    void add_panel_access(AccessTree& t) const;
    void add_php_access(AccessTree& t, int content, const AccessRect& clip) const;
    void add_wizard_access(AccessTree& t) const;
    void add_first_run_access(AccessTree& t) const;
    // The element id of a part ("row/my-shop/switch"); empty for the name box (a real EDIT).
    [[nodiscard]] std::string access_id(const Hit& hit) const;
    [[nodiscard]] std::string access_focus_id() const;
    // After each paint, while a screen reader listens: what changed is told.
    void access_after_paint();
    void run_access_requests();
    void run_access(const AccessNode& node, AccessAction action);
    void access_focus(const Hit& hit);

    HINSTANCE instance_;
    Lang lang_;
    bool rtl_;
    WindowPrefs prefs_;  // window.json: language, theme, closing, starting in the tray
    bool rebuilding_ = false;  // the window is being made again in another language
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
    bool pma_busy_ = false;  // the panel's phpmyadmin.login is on its way
    struct Removing {
        bool drop = false;   // with its database
        bool trash = false;  // and then its folder to the Recycle Bin
        std::string path;    // the folder the question showed
    };
    std::map<std::string, Removing> removing_;  // projects.remove on its way, per project
    RecycleBin recycle_bin_;
    Uninstaller uninstaller_;  // Remove FloServer…'s steps
    bool uninstalling_ = false;  // they're running: no Quit, no Settings meanwhile
    UninstallTicks uninstall_ticks_;
    std::string highlighted_;  // a row right-clicked while the panel is closed: shown selected
    std::string remove_checking_;     // the project whose remove question waits for its folder's check
    bool quit_after_folder_ = false;  // Quit waits for a folder's move (Windows' question needs its owner)
    std::set<std::string> removed_;         // removed; hidden until the list no longer has them
    Toast toast_;  // the message shown (model/toast.h)
    ToastGeometry toast_geometry_;
    ToastPopup toast_popup_;  // where it's shown: a popup of its own over the content

    WizardState wizard_;
    mutable WizardGeometry wizard_geometry_;
    FirstRunState first_run_;
    mutable FirstRunGeometry first_run_geometry_;
    HWND name_edit_ = nullptr;  // the New project name box
    std::string name_help_;     // what screen readers were told is its help

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
    static constexpr UINT_PTR kFirstRunScrollTimer = 5;
    // Posted after certs.ensure_ca's answer: Windows' question is asked outside on_snapshot.
    static constexpr UINT kFirstRunTrustMessage = WM_APP + 8;
    static constexpr UINT kRecycledMessage = WM_APP + 9;  // a folder's move to the Recycle Bin finished
    static constexpr UINT kToastMessage = WM_APP + 10;    // the toast's popup: the mouse, its close mark
    // Settings > General (or the first-run page): a preference changed; wparam PrefChange, lparam the index.
    static constexpr UINT kPrefsMessage = WM_APP + 7;
    ScrollBar list_bar_{kListScrollTimer, [this](int offset) { scroll_to(offset); }};
    ScrollBar panel_bar_{kPanelScrollTimer, [this](int offset) { panel_scroll_to(offset); }};
    ScrollBar first_run_bar_{kFirstRunScrollTimer, [this](int offset) { first_run_scroll_to(offset); }};
    Hit hover_;
    Hit pressed_;
    int focus_ = -1;  // keyboard focus in the list
    // Focus frames show only after the keyboard was used (Tab, the arrows,
    // the menu key, Alt); any mouse click hides them again, as Windows does.
    bool show_focus_ = false;
    bool tracking_mouse_ = false;
    // The code editor (editor()): found once, kept for the window session.
    bool editor_known_ = false;
    std::optional<EditorTarget> editor_;
    // The icon buttons' tooltips, by kIconActions' order: their words and where they are.
    std::array<std::wstring, kIconActions.size()> panel_tips_;
    std::array<RECT, kIconActions.size()> panel_tip_rects_{};

    // Screen readers: the provider, the tree as it is now (made again after
    // any message), the one the last events were raised from (only while a
    // screen reader listens), and the requests waiting for kAccessMessage.
    static constexpr UINT kAccessMessage = WM_APP + 11;
    static constexpr UINT kUninstallMessage = WM_APP + 12;  // a Remove FloServer… step finished
    AccessRoot access_;
    AccessTree access_now_;
    bool access_dirty_ = true;
    std::optional<AccessTree> access_said_;
    std::vector<std::pair<std::string, AccessAction>> access_queue_;
    bool access_running_ = false;  // a request is being run (it may have a menu open)
    // A page was just opened (not a window made again in another language):
    // its step is said even when it's the first thing a screen reader is told.
    bool access_page_opened_ = false;
};

}  // namespace floserver::ui

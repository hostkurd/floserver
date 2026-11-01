#pragma once

// FloServer Settings (D20): its own top-level window, not modal, one at a
// time, opened from the main window's gear. A sidebar of sections; Runtimes
// is the first (download, remove, the default for new projects).
//
// PHP is the second section: extensions and php.ini values per series. It
// asks php.get once when it's opened or the version changes, and changes
// wait for Save (one php.set).
//
// Ports & HTTPS is the third: the web ports, HTTPS for projects and for
// shared sites, and FloServer's certificate authority. It asks
// web.settings.get once when it's opened (the service checks the ports then,
// and names who holds one); each change is sent at once (web.settings.set).
//
// Database (MariaDB) is the fourth, built as PHP is: database.get once,
// database.set on Save; the server's state comes from the main window's
// databases.list snapshot.
//
// Apache is the fifth: the shared Apache's version, its modules and the
// user's own httpd.conf lines. apache.get once when it's opened (the service
// reads the modules folder then), apache.set on Save.
//
// Folders is the sixth: where new projects are made, the www folder, and
// FloServer's data folder with how much it uses and two clean-ups.
// folders.get once when it's opened (the service counts the data folder
// then, off the polling thread); each change is sent at once.
//
// Shared sites is the seventh (D19's settings page, moved here): the switch,
// the www folder, PHP, database and web server, and the www login. It asks
// nothing: what it shows is the main window's shared.get snapshot. The gear
// on the main window's Shared sites row opens Settings here.
//
// General is the last: the window's language and theme, starting with
// Windows, what closing the window does, and About. Opening it reads the
// Run entry once (no request to the service). The language, theme and
// closing choices belong to FloServer's window: they're posted to the owner
// (`prefs_message`), which keeps and saves them and applies them at once.
//
// Nothing runs while it's closed: it's destroyed on close, and while open it
// adds no timer or poll. Opening asks runtimes.list {"cached": true} once (no
// network); what's installed then comes from the main window's 2 s
// snapshot, and the full list is asked again only after a change is done or
// "Check for updates".

#include "canvas.h"
#include "form_parts.h"
#include "model/apache_page.h"
#include "model/database_page.h"
#include "model/general_page.h"
#include "model/folders_page.h"
#include "model/php_page.h"
#include "model/ports_page.h"
#include "model/runtimes_page.h"
#include "model/ui_strings.h"
#include "scroll_bar.h"
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

// What Settings > General asks FloServer's window to change: the wparam of
// its `prefs_message`; the lparam is the choice's index (kLangChoices,
// kThemeChoices, CloseAction, or 1 for "start in the tray").
enum class PrefChange : std::uint8_t { Language, Theme, Close, StartInTray };

class SettingsWindow {
public:
    // `closed_message` is posted to `owner` once the window is gone (wparam 0),
    // or with wparam 1 when the owner asked to quit while Settings was asking
    // whether to save (quit_when_asked()): the owner quits then. So the
    // owner can let go of this object outside its window procedure.
    // `prefs` are the window's preferences as they are now; a change made in
    // General is posted to `owner` as `prefs_message` (see PrefChange).
    SettingsWindow(HINSTANCE instance, Lang lang, const WindowPrefs& prefs, ServiceWorker& worker, HWND owner,
                   UINT closed_message, UINT prefs_message);
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
    // Opens the Shared sites section, from the main window's Shared sites row.
    void show_shared();
    // Shared sites' .env lines went on the clipboard (or couldn't): said
    // here when Copy .env was pressed here.
    void env_copied(bool done);
    // The Save / Don't save / Cancel question (or a folder picker) is open:
    // the owner mustn't destroy this object or ask it to close meanwhile.
    [[nodiscard]] bool asking() const { return asking_; }
    // FloServer is quitting while asking(): the owner is told to go on once
    // the question is answered.
    void quit_when_asked() { quit_after_ask_ = true; }
    // Opens the General section (after the windows were made again in another language).
    void show_general();
    // The owner's preferences after a change (or a failed save: `error`).
    void set_prefs(const WindowPrefs& prefs, const std::string& error);
    // The theme changed (Settings > General, or Windows'): drawn again.
    void refresh_theme();
    // Closing FloServer's window quits (Settings > General): closes as
    // close() does, and once it's gone the owner is told to quit (the closed
    // message with wparam 1), also after a Save chosen on the way is done.
    // False: it stays (Cancel, a refusal, or that Save on its way).
    bool close_and_quit();
    // Gone now, without asking (nothing in General waits for Save); the
    // closed message still follows. placement() then says where it was.
    void destroy_now();

private:
    // In the sidebar's order.
    enum class Section : std::uint8_t { Runtimes, Php, Ports, Database, Apache, Folders, Shared, General };
    static constexpr std::size_t kSections = 8;
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
        PhpFinalIni,
        PhpField,     // an edit box, or a picker
        PhpReset,
        PhpDiscard,
        PhpSave,
        // Settings > Ports & HTTPS; `row` is the choice's or switch's index.
        PortsChoice,
        PortsHttpEdit,
        PortsHttpsEdit,
        PortsUse,
        PortsSwitch,  // row 0: HTTPS for projects, 1: shared sites
        PortsTrust,
        PortsRemove,
        PortsOpenCert,
        // Settings > Database; `row` is the field's index.
        DbVersion,
        DbRuntimes,   // "Go to Runtimes" when no MariaDB is installed
        DbStart,
        DbStop,
        DbRestart,
        DbPort,       // an edit
        DbKeep,       // the Keep running checkbox
        DbFinalIni,
        DbEditIni,
        DbField,      // an edit
        DbReset,
        DbRootShow,
        DbRootCopy,
        DbNewPassword,  // an edit
        DbSetPassword,
        DbDiscard,
        DbSave,
        DbPhpMyAdmin,   // Open phpMyAdmin (as root, this series)
        // Settings > Apache; `row` is the module's cell.
        ApacheVersion,
        ApacheRuntimes,  // "Go to Runtimes" when no Apache is installed
        ApacheFind,      // the find box (an edit)
        ApacheModule,
        ApacheMore,      // "show all" / "show fewer"
        ApacheFinalConf,
        ApacheEditConf,
        ApacheDiscard,
        ApacheSave,
        // Settings > Folders.
        FoldersProjectsChange,
        FoldersProjectsOpen,
        FoldersProjectsDefault,
        FoldersWwwChange,
        FoldersWwwOpen,
        FoldersDataOpen,
        FoldersClearLogs,
        FoldersLeftovers,
        // Settings > Shared sites; `row` is the picker's index.
        SharedSwitch,
        SharedChange,
        SharedOpen,
        SharedField,
        SharedCopyEnv,
        // Settings > General; `row` is the choice's index.
        GeneralLang,
        GeneralTheme,
        GeneralStart,      // the Start with Windows switch
        GeneralStartMode,  // row 0: open the window, 1: start in the tray
        GeneralClose,      // row 0: keep running in the tray, 1: quit
        GeneralUpdates,
        GeneralLicences,
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
        RECT final_ini{};
        std::vector<RECT> labels;   // one per settings_php_ini_fields()
        std::vector<RECT> boxes;
        std::vector<RECT> resets;   // empty when the field isn't marked
        int error_field = -1;       // the field the last refusal was about
        RECT field_error{};         // its words, under it
        std::vector<bool> warned;   // per field: PHP doesn't know its name
        std::vector<std::pair<RECT, std::string>> warnings;  // and what's said, under its line
        RECT custom_note{};
        RECT footer_text{};
        RECT discard{};
        RECT save{};
    };
    struct PortsGeometry {
        RECT note{};  // Loading…
        RECT web_card{};
        RECT web_title{};
        std::array<RECT, kPortChoices.size()> choices{};  // the whole row
        std::array<RECT, kPortChoices.size()> choice_labels{};
        std::array<RECT, kPortChoices.size()> choice_notes{};
        RECT http_label{};
        RECT https_label{};
        RECT http_box{};   // empty unless "Other ports" is picked
        RECT https_box{};
        RECT use{};
        RECT problem{};
        RECT in_use{};
        std::vector<std::pair<RECT, std::string>> taken;
        RECT https_card{};
        RECT https_title{};
        std::array<RECT, 2> switch_rows{};
        std::array<RECT, 2> switches{};
        std::array<RECT, 2> switch_labels{};
        std::array<RECT, 2> switch_notes{};
        RECT needs_trust{};
        RECT ca_card{};
        RECT ca_title{};
        RECT ca_status{};
        RECT ca_safe{};
        RECT trust{};   // empty when there's nothing to trust
        RECT remove{};
        RECT open{};
    };
    struct DbGeometry {
        RECT version_label{};
        RECT version{};
        RECT note{};       // Loading…, or no MariaDB installed
        RECT runtimes{};   // Go to Runtimes
        RECT problems{};
        RECT server_card{};
        RECT server_title{};
        RECT state{};      // the server's state, in a line
        RECT start{};
        RECT stop{};
        RECT restart{};
        RECT port_label{};
        RECT port{};
        RECT port_note{};  // the port can't be used, or .env files keep the old one
        RECT keep{};       // the checkbox and its label
        RECT keep_note{};
        RECT used_by{};
        RECT ini_card{};
        RECT ini_title{};
        RECT ini_hint{};
        RECT final_ini{};
        RECT edit_ini{};
        std::vector<RECT> labels;  // one per settings_db_ini_fields()
        std::vector<RECT> boxes;
        std::vector<RECT> resets;
        int error_field = -1;
        RECT field_error{};
        RECT custom_problems{};
        std::vector<std::pair<RECT, std::string>> warnings;  // names in custom.ini MariaDB doesn't know
        RECT custom_note{};
        RECT root_card{};
        RECT root_title{};
        RECT root_label{};
        RECT root_value{};
        RECT root_show{};
        RECT root_copy{};
        RECT pma{};       // Open phpMyAdmin
        RECT pma_hint{};  // Download phpMyAdmin in Settings → Runtimes, when it isn't installed
        RECT new_label{};
        RECT new_password{};
        RECT set_password{};
        RECT password_error{};
        RECT root_note{};
        RECT databases{};
        RECT footer_text{};
        RECT discard{};
        RECT save{};
    };
    struct ApacheGeometry {
        RECT version_label{};
        RECT version{};     // empty while there's nothing to pick
        RECT note{};        // Loading…, or no Apache installed
        RECT runtimes{};    // Go to Runtimes
        RECT refused{};     // what httpd -t said about the last settings
        RECT problems{};
        RECT modules_card{};
        RECT modules_title{};
        RECT count{};
        RECT find{};
        std::vector<RECT> cells;
        std::vector<RECT> extras;        // the note after the name; empty when none fits
        std::vector<std::string> names;  // what each cell shows
        std::vector<std::string> notes;
        RECT modules_note{};
        RECT more{};
        RECT conf_card{};
        RECT conf_title{};
        RECT conf_hint{};
        RECT final_conf{};
        RECT edit_conf{};
        RECT custom_problems{};
        RECT conf_note{};
        RECT footer_text{};
        RECT discard{};
        RECT save{};
    };
    // A radio button's or a switch's row: the whole row, its label and the note under it.
    struct ChoiceGeometry {
        RECT row{};
        RECT label{};
        RECT note{};  // empty when there's none
    };
    struct GeneralGeometry {
        RECT lang_card{};
        RECT lang_title{};
        std::array<ChoiceGeometry, kLangChoices.size()> langs{};
        RECT lang_note{};
        RECT theme_card{};
        RECT theme_title{};
        std::array<ChoiceGeometry, kThemeChoices.size()> themes{};
        RECT start_card{};
        RECT start_title{};
        ChoiceGeometry start;
        RECT start_switch{};
        std::array<ChoiceGeometry, 2> modes{};  // open the window, start in the tray
        RECT elsewhere{};  // the Run entry starts another FloServer.exe
        RECT close_card{};
        RECT close_title{};
        std::array<ChoiceGeometry, 2> closes{};  // keep running in the tray, quit
        RECT about_card{};
        RECT about_title{};
        RECT version{};
        RECT updates{};
        RECT licences{};
        RECT updates_note{};
    };
    struct FoldersGeometry {
        RECT note{};  // Loading…
        RECT projects_card{};
        RECT projects_title{};
        RECT projects_note{};
        RECT projects_box{};
        RECT projects_change{};
        RECT projects_open{};
        RECT projects_default{};  // empty unless a folder of the user's is set
        RECT www_card{};          // empty while shared.get hasn't answered
        RECT www_title{};
        RECT www_note{};
        RECT www_box{};
        RECT www_change{};
        RECT www_open{};
        RECT data_card{};
        RECT data_title{};
        RECT data_note{};
        RECT data_box{};
        RECT data_open{};
        RECT data_usage{};
        RECT clean_card{};
        RECT clean_title{};
        RECT logs{};         // the button
        RECT logs_size{};
        RECT logs_note{};
        RECT leftovers{};    // the button
        RECT leftovers_size{};
        RECT leftovers_note{};
        RECT never_note{};
    };
    struct SharedGeometry {
        RECT toggle{};
        RECT note{};  // while shared.get hasn't answered
        RECT card{};
        RECT folder_label{};
        RECT folder_box{};
        RECT change{};
        RECT open{};
        std::vector<std::pair<RECT, RECT>> fields;  // label, box; one per picker
        RECT db_box{};   // empty when there's no login to show
        RECT db_title{};
        RECT db_detail{};
        RECT copy_env{};
    };
    struct Geometry {
        std::array<RECT, kSections> navs{};  // Runtimes, PHP, Ports & HTTPS, Database, Apache, Folders, Shared sites, General
        RECT title{};
        RECT hint{};
        RECT check{};
        RECT message{};
        RECT note{};  // "Loading…" before the first answer
        std::vector<GroupGeometry> groups;
        PhpGeometry php;
        PortsGeometry ports;
        DbGeometry db;
        ApacheGeometry apache;
        FoldersGeometry folders;
        SharedGeometry shared;
        GeneralGeometry general;
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
    // The bar at the window's end follows the page (after each layout).
    void place_scroll_bar();
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

    // --- Settings > Ports & HTTPS (settings_ports.cpp) ---
    void load_ports();
    void on_ports_answers(const Snapshot& snap);
    void make_port_edits();
    void fill_port_edits();
    void place_port_edits();
    void layout_ports(HDC dc, Geometry& g, int x0, int x1, int y) const;
    [[nodiscard]] const RECT* ports_rect_of(const Hit& hit) const;
    void ports_focus_order(std::vector<Hit>& order) const;
    void on_ports_click(const Hit& hit);
    [[nodiscard]] std::size_t shown_port_choice() const;
    [[nodiscard]] bool shown_switch(int index) const;
    void send_ports(const nlohmann::json& params);
    void use_custom_ports();
    void trust_ca();
    void trust_ca_now();
    void remove_ca();
    void open_certificate();
    void paint_ports_shapes(Canvas& c) const;
    void paint_ports_text(Canvas& c) const;

    // --- Settings > PHP (settings_php.cpp) ---
    // Leaves PHP's unsaved changes as the user says; false: stay (Cancel).
    // `then` is what a Save waits for (done once it's answered without a
    // refusal); `series` goes with AfterSave::Version.
    [[nodiscard]] bool settle_php_changes(AfterSave then, const std::string& series = {});
    void set_php_read_only(bool read_only);
    // port_edits_[i] has id kPortEditBase + i (edits_ start at 1000).
    static constexpr int kPortEditBase = 2000;
    void try_close();
    // `series`: the PHP version to show; empty for the newest.
    void open_section(Section section, const std::string& series = {});
    void load_php(const std::string& series);
    // True when a Save chosen on the way out is done and the window should close.
    [[nodiscard]] bool on_php_answers(const Snapshot& snap);
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
    // False when nothing was sent (not connected, a save already on its way).
    bool save_php();
    void discard_php();
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> php_versions() const;
    void paint_php_shapes(Canvas& c) const;
    void paint_php_text(Canvas& c) const;

    // --- Settings > Database (settings_database.cpp) ---
    [[nodiscard]] bool settle_db_changes(AfterSave then, const std::string& series = {});
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> db_versions() const;
    void load_db(const std::string& series);
    // True when a Save chosen on the way out is done and the window should close.
    [[nodiscard]] bool on_db_answers(const Snapshot& snap);
    void make_db_edits();
    void fill_db_edits();
    void place_db_edits();
    void set_db_read_only(bool read_only);
    void on_db_edit_change(const Hit& hit, FieldEdit& edit);
    void layout_db(HDC dc, Geometry& g, int x0, int x1, int y) const;
    [[nodiscard]] const RECT* db_rect_of(const Hit& hit) const;
    void db_focus_order(std::vector<Hit>& order) const;
    void on_db_click(const Hit& hit);
    void db_version_menu();
    bool save_db();
    void discard_db();
    void set_root_password();
    void paint_db_shapes(Canvas& c) const;
    void paint_db_text(Canvas& c) const;
    [[nodiscard]] static bool is_db_part(Part part) { return part >= Part::DbVersion && part <= Part::DbPhpMyAdmin; }

    // --- Settings > Apache (settings_apache.cpp) ---
    [[nodiscard]] bool settle_apache_changes(AfterSave then);
    void load_apache();
    // True when a Save chosen on the way out is done and the window should close.
    [[nodiscard]] bool on_apache_answers(const Snapshot& snap);
    void make_apache_edits();
    void place_apache_edits();
    void layout_apache(HDC dc, Geometry& g, int x0, int x1, int y) const;
    [[nodiscard]] std::vector<ApacheModule> shown_apache_modules(std::size_t& hidden) const;
    [[nodiscard]] const RECT* apache_rect_of(const Hit& hit) const;
    void apache_focus_order(std::vector<Hit>& order) const;
    void on_apache_click(const Hit& hit);
    void apache_version_menu();
    bool save_apache();
    void discard_apache();
    void paint_apache_shapes(Canvas& c) const;
    void paint_apache_text(Canvas& c) const;
    [[nodiscard]] static bool is_apache_part(Part part) {
        return part >= Part::ApacheVersion && part <= Part::ApacheSave;
    }

    // --- Settings > Folders (settings_folders.cpp) ---
    void load_folders();
    void on_folders_answers(const Snapshot& snap);
    void layout_folders(HDC dc, Geometry& g, int x0, int x1, int y) const;
    [[nodiscard]] const RECT* folders_rect_of(const Hit& hit) const;
    void folders_focus_order(std::vector<Hit>& order) const;
    void on_folders_click(const Hit& hit);
    void change_projects_folder();
    void clean_up(const std::string& what);
    void paint_folders_shapes(Canvas& c) const;
    void paint_folders_text(Canvas& c) const;
    // A folder box: its outline (shapes) or the path inside it (text).
    void paint_folder_box(Canvas& c, const RECT& box, const std::string& path, bool text_pass) const;
    // Opens a folder in Explorer, or says why it couldn't.
    void explore(const std::string& folder);
    [[nodiscard]] static bool is_folders_part(Part part) {
        return part >= Part::FoldersProjectsChange && part <= Part::FoldersLeftovers;
    }

    // --- Settings > Shared sites (settings_shared.cpp) ---
    void on_shared_answers(const Snapshot& snap);
    // The www folder picker (from either section); sent at once.
    void change_www_folder();
    [[nodiscard]] std::string shown_www_dir() const;
    [[nodiscard]] bool shown_shared_on() const;
    [[nodiscard]] std::size_t shared_field_value(const VersionField& field) const;
    void send_shared(const nlohmann::json& changes, const std::string& key);
    void toggle_shared();
    void shared_field_menu(std::size_t index);
    void layout_shared(HDC dc, Geometry& g, int x0, int x1, int y) const;
    [[nodiscard]] const RECT* shared_rect_of(const Hit& hit) const;
    void shared_focus_order(std::vector<Hit>& order) const;
    void on_shared_click(const Hit& hit);
    void paint_shared_shapes(Canvas& c) const;
    void paint_shared_text(Canvas& c) const;
    void paint_switch(Canvas& c, const RECT& r, bool on, bool hover) const;
    [[nodiscard]] static bool is_shared_part(Part part) { return part >= Part::SharedSwitch && part <= Part::SharedCopyEnv; }

    // --- Settings > General (settings_general.cpp) ---
    void load_general();  // the Run entry, read once as the section opens
    void layout_general(HDC dc, Geometry& g, int x0, int x1, int y) const;
    [[nodiscard]] const RECT* general_rect_of(const Hit& hit) const;
    void general_focus_order(std::vector<Hit>& order) const;
    void on_general_click(const Hit& hit);
    void post_pref(PrefChange change, std::size_t value);
    void set_start(bool on, bool tray);
    [[nodiscard]] bool shown_start_tray() const;
    [[nodiscard]] bool run_elsewhere() const;
    void open_licences();
    void paint_general_shapes(Canvas& c) const;
    void paint_general_text(Canvas& c) const;
    void paint_radio(Canvas& c, const ChoiceGeometry& choice, bool on, const Hit& hit) const;
    [[nodiscard]] static bool is_general_part(Part part) { return part >= Part::GeneralLang; }

    HINSTANCE instance_;
    Lang lang_;
    bool rtl_;
    ServiceWorker& worker_;
    HWND owner_;
    UINT closed_message_;
    UINT prefs_message_;
    WindowPrefs prefs_;
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
    bool auto_checked_ = false;      // the first-use fetch was made (once per opening)
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
    bool asking_ = false;
    bool quit_after_ask_ = false;
    bool quit_on_close_ = false;  // close_and_quit(): the owner quits once this window is gone
    AfterSave after_save_ = AfterSave::Stay;
    std::string after_save_series_;
    PhpChanges php_changes_;
    std::string php_query_;
    std::string php_error_key_;  // the php.ini key Save was refused for
    std::string php_error_;      // and why, in the service's words
    bool php_all_ = false;  // "show all" extensions
    // The find box, then one per text field; only while the section is shown.
    std::vector<std::unique_ptr<FieldEdit>> edits_;
    std::vector<int> field_edit_;  // field index -> index in edits_, or -1 (a picker)
    // Settings > Ports & HTTPS: web.settings.get's answer, and what the user
    // picked while its web.settings.set is on its way.
    std::optional<PortsSettings> ports_;
    bool ports_loading_ = false;
    int ports_waiting_ = 0;                 // changes, CA requests on their way
    std::optional<std::size_t> pending_choice_;
    std::array<std::optional<bool>, 2> pending_switch_{};
    bool custom_open_ = false;              // "Other ports" picked here, not sent yet
    Str ports_problem_ = Str::Count_;       // what's wrong with the typed ports
    bool trust_after_ca_ = false;           // Trust asked for a CA first
    std::vector<std::unique_ptr<FieldEdit>> port_edits_;  // http, https; only while the section is shown
    // Where a Save chosen on the way to another section goes once it's done.
    Section after_save_section_ = Section::Runtimes;

    // Settings > Database: the series shown, its database.get answer, the
    // server's state from the 2 s snapshot, and the changes not saved yet.
    std::string db_series_;
    std::optional<DbSeries> db_;
    DbServer db_server_;
    bool db_loading_ = false;
    bool db_saving_ = false;
    DbChanges db_changes_;
    std::string db_error_key_;  // the my.ini key Save was refused for
    std::string db_error_;
    std::string root_shown_;    // root's password, after Show; wiped when hidden or left
    bool root_busy_ = false;    // a root password request is on its way
    bool pma_busy_ = false;     // phpmyadmin.login is on its way
    std::string password_error_;
    // The port, the my.ini fields, then the new root password; only while the section is shown.
    std::vector<std::unique_ptr<FieldEdit>> db_edits_;
    // Settings > Apache: apache.get's answer and the changes not saved yet.
    std::optional<ApacheInfo> apache_;
    bool apache_loading_ = false;
    bool apache_saving_ = false;
    ApacheChanges apache_changes_;
    std::string apache_query_;
    bool apache_all_ = false;  // "show all" modules
    std::vector<std::unique_ptr<FieldEdit>> apache_edits_;  // the find box; only while the section is shown
    // Settings > General: the Run entry as read when the section opened (or
    // as written since), and this FloServer.exe's path.
    std::optional<RunEntry> run_entry_;
    std::string own_exe_;
    // Settings > Folders: folders.get's answer, and what's on its way.
    std::optional<FoldersInfo> folders_;
    bool folders_loading_ = false;
    bool folders_setting_ = false;  // folders.set
    std::string cleaning_;          // "logs" or "leftovers" while folders.clean is on its way
    bool picking_folder_ = false;   // a folder picker is open (it runs its own message loop)
    // Shared sites (both sections): the main window's shared.get snapshot,
    // and the user's changes until the worker reports them done (the keys
    // ServiceWorker::on_key / use_key give, counted).
    SharedSites shared_;
    SharedPage shared_page_;
    std::map<std::string, int> shared_waiting_;
    std::optional<bool> pending_shared_on_;
    std::map<std::string, std::optional<std::string>> pending_shared_use_;  // field key -> value
    bool copying_env_ = false;
    win::UniqueBrush input_brush_;
    win::UniqueBrush input_set_brush_;

    Geometry geometry_;
    int scroll_ = 0;
    static constexpr UINT_PTR kScrollTimer = 2;  // paging while the button is held on the bar
    ScrollBar scroll_bar_{kScrollTimer, [this](int offset) { scroll_to(offset); }};
    Hit hover_;
    Hit pressed_;
    int focus_ = -1;  // index into focus_order()
    bool show_focus_ = false;
    bool tracking_mouse_ = false;
};

}  // namespace floserver::ui

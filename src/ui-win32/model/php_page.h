#pragma once

// Settings > PHP and the project panel's PHP part (D20): php.get's answer,
// the changes the user has made but not saved yet, and what Save sends.
// Plain C++ so it's tested on every platform; the windows only draw it.

#include "model/ui_strings.h"
#include "model/view_model.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace floserver::ui {

struct PhpExtension {
    std::string name;
    bool on = false;
    bool base = false;     // on for every project unless turned off
    bool missing = false;  // named in the settings but not in this install
};

// php.get's (and php.set's) answer for one series.
struct PhpSeries {
    std::string series;   // "8.5"
    std::string version;  // the install used ("8.5.12"); empty when none is
    bool extensions_fixed = false;  // they come with the build: shown, not changed
    std::vector<PhpExtension> extensions;
    std::map<std::string, std::string> ini;      // FloServer's defaults and the series' values
    std::map<std::string, std::string> changed;  // the series' own values
    std::string custom_ini;
    bool custom_ini_exists = false;
    std::vector<std::string> used_by;  // project names, and "_shared" for shared sites
    std::vector<std::string> problems;
    std::vector<std::string> known_ini;  // every php.ini name the install knows, sorted; empty: unknown
};

// Nothing when the answer has no series.
[[nodiscard]] std::optional<PhpSeries> php_series_from_reply(const nlohmann::json& reply);

// The installed PHP series, newest first, each with its newest version:
// {"8.5", "8.5.12"}, {"8.4", "8.4.27"}.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> installed_php_series(const Installed& installed);

// Settings > PHP's version list: the installed series as the 2 s poll
// says (`has_installed`), else as Settings' own runtimes.list answer says
// (its "installed", read from this computer with no network), so the
// section never waits on anything online.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> settings_php_versions(bool has_installed,
                                                                                     const Installed& installed,
                                                                                     const nlohmann::json& runtimes_reply);

// A php.ini name PHP doesn't know is ignored by PHP without a word: the
// window warns (Save is still allowed). Nothing when `known` is empty (PHP
// couldn't tell) or knows `key`; else the closest known name, at most two
// edits away and ignoring case ("disable_function" -> "disable_functions"),
// or "" when none is that close.
[[nodiscard]] std::optional<std::string> unknown_ini_name(const std::vector<std::string>& known, std::string_view key);
// "PHP 8.5 has no setting named khalat." with "Did you mean …?" when
// there's a close one.
[[nodiscard]] std::string unknown_ini_text(std::string_view series, std::string_view key,
                                           const std::string& suggestion, Lang lang);

// The php.ini settings shown as fields. `choices` set: a picker (with what
// the current value is added when it isn't one of them), else an edit box.
struct PhpIniField {
    std::string key;
    std::vector<std::string> choices;
};
// Settings > PHP: the eight people change most.
[[nodiscard]] const std::vector<PhpIniField>& settings_php_ini_fields();
// The project panel: the five of them a project changes most.
[[nodiscard]] const std::vector<PhpIniField>& project_php_ini_fields();

// The extensions in the order they're shown: those on first (as listed),
// then the ones people turn on most, then the rest by name.
[[nodiscard]] std::vector<PhpExtension> ordered_extensions(const PhpSeries& series);
// How many the grid shows before "show all".
inline constexpr std::size_t kShownExtensions = 20;
// Case-insensitive part of the name; empty matches all.
[[nodiscard]] bool extension_matches(std::string_view name, std::string_view query);
// xdebug: "slows PHP".
[[nodiscard]] bool is_slow_extension(std::string_view name);

// --- Settings > PHP: changes to one series, until Save ---------------------------

struct PhpChanges {
    std::map<std::string, bool> extensions;                   // name -> on
    std::map<std::string, std::optional<std::string>> ini;    // nothing: back to FloServer's default
    [[nodiscard]] bool empty() const { return extensions.empty() && ini.empty(); }
};

[[nodiscard]] bool extension_on(const PhpSeries& series, const PhpChanges& changes, const std::string& name);
// Turns it on or off; a change back to what the series has is dropped.
void toggle_extension(const PhpSeries& series, PhpChanges& changes, const std::string& name);
[[nodiscard]] std::size_t extensions_on(const PhpSeries& series, const PhpChanges& changes);

// The text of a field: the change, else the series' value (empty after a
// reset until Save shows the default).
[[nodiscard]] std::string ini_text(const PhpSeries& series, const PhpChanges& changes, const std::string& key);
// The series has its own value there (or will once saved): the field is marked.
[[nodiscard]] bool ini_marked(const PhpSeries& series, const PhpChanges& changes, const std::string& key);
// What the user typed (or picked). The same text as before is no change;
// empty text is the default.
void set_ini_text(const PhpSeries& series, PhpChanges& changes, const std::string& key, const std::string& text);
// Back to FloServer's default.
void reset_ini(const PhpSeries& series, PhpChanges& changes, const std::string& key);

// php.set's parameters: only what changed.
[[nodiscard]] nlohmann::json php_set_params(const std::string& series, const PhpChanges& changes);

// The footer: "Applies when saved: PHP for 3 projects and shared sites
// restarts (under a second)."
[[nodiscard]] std::string php_restart_text(const PhpSeries& series, Lang lang);

// Whether an answer belongs to what's shown now: the same receiver
// (Settings, or the panel's project) and, when the answer names the series
// it was asked for, the same series. A late answer for a version or project
// the user has left is dropped, errors as well as values.
[[nodiscard]] bool php_answer_matches(std::string_view to, std::string_view series, std::string_view shown_to,
                                      std::string_view shown_series);

// What a "Save" chosen on the way out waits for: it's done only once the
// save is answered without a refusal.
enum class AfterSave : std::uint8_t { Stay, Close, Runtimes, Version };

// The php.ini key a refusal from php.set or projects.php is about
// ("memory_limit: put the value in double quotes when…", "opcache.enable is
// set by FloServer…"), when it's one of `keys`; else empty. The longest
// matching key wins.
[[nodiscard]] std::string ini_error_key(std::string_view message, const std::vector<std::string>& keys);

// --- the project panel's tabs ---------------------------------------------------

enum class PanelTab : std::uint8_t { General, Php, Database, Log };

// The tabs the panel shows for a project, in order: General and Log always,
// PHP when it uses PHP, Database when it has (or asks for) one.
[[nodiscard]] std::vector<PanelTab> panel_tabs(const ProjectRow& row);
// The tab shown: the one the user chose last (on any project), else
// General when this project hasn't that tab.
[[nodiscard]] PanelTab shown_panel_tab(const std::vector<PanelTab>& tabs, PanelTab chosen);
// The space each side of a tab's label: `most` when the labels fit `width`
// with it, else as much as fits down to `least` (then the labels are cut).
[[nodiscard]] int tab_padding(const std::vector<int>& label_widths, int width, int most, int least);

// --- the project panel's PHP part ---------------------------------------------

// Whether the panel offers PHP settings for the project.
[[nodiscard]] bool project_uses_php(const ProjectRow& row);
// The series its PHP comes from: the version it runs, what floserver.json
// asks for, else the newest installed. Nothing when there's none.
[[nodiscard]] std::optional<std::string> project_php_series(const ProjectRow& row, const Installed& installed);

// A project's own PHP changes (floserver.json "extensions" and "php_ini").
struct ProjectPhp {
    std::map<std::string, bool> extensions;  // name -> on
    std::map<std::string, std::string> ini;
    bool operator==(const ProjectPhp&) const = default;
};
[[nodiscard]] ProjectPhp project_php_of(const ProjectRow& row);

// On for the project: its change, else the series' (`series` may be null
// before php.get has answered: then only the base list is known to be on).
[[nodiscard]] bool project_extension_on(const PhpSeries* series, const ProjectPhp& php, const std::string& name);
// Turns it on or off; the same as the series' drops the project's change.
void toggle_project_extension(const PhpSeries* series, ProjectPhp& php, const std::string& name);
// A field's text: empty is the series' value, which is the placeholder.
void set_project_ini(ProjectPhp& php, const std::string& key, const std::string& text);
// "name = value" from "Another setting": the name and the value, trimmed.
// Nothing when there's no '=' or no name.
[[nodiscard]] std::optional<std::pair<std::string, std::string>> parse_ini_line(std::string_view line);

// projects.php's parameters from what the file had to what it should have.
[[nodiscard]] nlohmann::json project_php_params(const std::string& project, const ProjectPhp& before,
                                                const ProjectPhp& after);

}  // namespace floserver::ui

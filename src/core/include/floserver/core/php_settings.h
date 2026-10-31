#pragma once

// PHP extensions and php.ini values (D20, Settings → PHP), at two levels:
//
// Per PHP series ("8.5"), in config.json; they apply to every project on
// that series and to shared sites:
//
//   "php": {"8.5": {"extensions": {"bcmath": true, "exif": false},
//                   "ini": {"memory_limit": "1G"}}}
//
// "extensions" holds only the changes from the base list (base_php_extensions():
// on unless turned off; any other is off unless turned on). Each series may
// also have a user-owned <data>/php/<series>/custom.ini whose text is added
// to every php.ini FloServer writes for that series, since those files are
// rewritten and edits in them would be lost.
//
// Per project, in floserver.json, so they travel with the project:
//
//   "extensions": ["bcmath", "-exif"],    a leading - turns one off
//   "php_ini": {"memory_limit": "1024M"}
//
// php.ini is read top to bottom and a later value wins, so the generated file
// is: FloServer's defaults, the series' values, the series' custom.ini, the
// project's values. Extensions: the base list, then the series' changes, then
// the project's. opcache is always on and isn't offered.

#include "floserver/core/project_config.h"
#include "floserver/core/project_registry.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace floserver::core {

// Extensions every project gets when the PHP install has them, so the
// common frameworks (FloCMS, Laravel, WordPress) run without listing any.
[[nodiscard]] const std::vector<std::string>& base_php_extensions();

// FloServer's own php.ini values (development settings), in the order
// they're written. error_log and extension_dir are added per pool.
[[nodiscard]] const std::vector<std::pair<std::string, std::string>>& php_ini_defaults();

// a-z, 0-9 and _, at most 32 characters.
[[nodiscard]] bool is_php_extension_name(std::string_view name);

// Why a php.ini key can't be set, or nothing when it can. Keys FloServer
// manages itself (extension, zend_extension, extension_dir, error_log) are
// refused, as is anything that isn't a plain ini name.
[[nodiscard]] std::optional<std::string> php_ini_key_problem(std::string_view key);
// Why a value can't be set: one line, no control characters, at most 1000 bytes.
[[nodiscard]] std::optional<std::string> php_ini_value_problem(std::string_view value);

// "8.5" from "8.5", "8.5.12" or "08.5"; nothing unless there are at least
// two parts.
[[nodiscard]] std::optional<std::string> php_series_of(std::string_view version);

struct PhpSeriesSettings {
    std::map<std::string, bool> extensions;   // only the changes from the base list
    std::map<std::string, std::string> ini;   // key -> value

    bool operator==(const PhpSeriesSettings&) const = default;
};

// By series ("8.5").
using PhpSettings = std::map<std::string, PhpSeriesSettings>;

// config.json's "php". Entries that can't be used (a series that isn't
// two numbers, a bad name, key or value) are left out and described in
// `problems`, so one typo by hand doesn't stop every PHP site.
[[nodiscard]] PhpSettings read_php_settings(const AppConfig& app, std::vector<std::string>* problems = nullptr);

// Writes one series' settings into config.json's "php" (AppConfig::other),
// keeping keys this version doesn't know; empty settings remove the series.
void write_php_series(AppConfig& app, const std::string& series, const PhpSeriesSettings& settings);

// <data>/php/<series>/custom.ini.
[[nodiscard]] std::filesystem::path php_custom_ini_file(const std::filesystem::path& data, const std::string& series);
// The comment a new custom.ini starts with.
[[nodiscard]] std::string php_custom_ini_header(const std::string& series);
// Makes the file with its header when it isn't there; false (with `error`) when it can't.
[[nodiscard]] bool ensure_php_custom_ini(const std::filesystem::path& data, const std::string& series,
                                         std::string& error);
// The file's text: empty when it's missing, unreadable (over 256 KiB), or
// still only the header, so making the file doesn't restart any PHP.
[[nodiscard]] std::string read_php_custom_ini(const std::filesystem::path& data, const std::string& series);

// What one pool's php.ini holds beyond FloServer's defaults.
struct PhpIniSettings {
    std::string series;                              // "8.5", for the file's comments
    std::vector<std::string> extensions;             // on beyond the base list, sorted
    std::vector<std::string> removed;                // base ones turned off, sorted
    std::map<std::string, std::string> series_ini;
    std::string custom_ini;                          // the series' custom.ini text
    std::map<std::string, std::string> project_ini;

    bool operator==(const PhpIniSettings&) const = default;
};

// The base list with the series' and then the project's changes applied,
// and the ini values of both. Either may be absent (shared sites have no
// project; a series without settings has none). Managed keys and opcache
// are left out whatever the files say.
[[nodiscard]] PhpIniSettings effective_php_settings(const std::string& series, const PhpSeriesSettings* series_settings,
                                                    std::string custom_ini, const ProjectConfig* project);

// The part of a pool's key after "php-<version>": "+gd+intl" for the extra
// extensions, then "-<8 hex digits>" (a hash of the rest) when anything else
// differs from the defaults. The same settings always give the same text, so
// a pool keeps its key (and its port) until its settings change.
[[nodiscard]] std::string php_settings_key(const PhpIniSettings& settings);

}  // namespace floserver::core

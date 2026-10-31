#pragma once

// Settings → Apache: the shared Apache behind nginx (D6, apache_config.h).
// Kept in config.json beside "php", "mariadb" and "web":
//
//   "apache": {"version": "2.4.69", "modules": {"expires": false, "cache": true}}
//
// "version": the install the shared Apache runs (absent, or not installed:
// the newest). "modules" holds only the changes from what FloServer loads by
// itself (apache_module_info()): those its own config needs are always
// loaded; the ones .htaccess files commonly use are on unless turned off;
// any other module in the install's modules folder is off unless turned on.
// A module another one needs is loaded with it (deflate: filter). Keys this
// version doesn't know are kept.
//
// <data>/apache/custom.conf is the user's own httpd.conf lines, added at the
// end of the config FloServer writes once filter_apache_custom_conf() has
// checked them: Apache stays on 127.0.0.1 behind nginx, with FloServer's own
// files, whatever the file says. The new config is tested with `httpd -t`
// before it's used; one Apache refuses leaves the running Apache as it was.

#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_service.h"  // ServiceRefusal

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

class ProjectService;

struct ApacheSettings {
    std::optional<std::string> version;   // "2.4.69"; nothing: the newest installed
    std::map<std::string, bool> modules;  // short name ("expires") -> on; only changes

    bool operator==(const ApacheSettings&) const = default;
};

// config.json's "apache". Entries that can't be used are left out and
// described in `problems`.
[[nodiscard]] ApacheSettings read_apache_settings(const AppConfig& app, std::vector<std::string>* problems = nullptr);
// Writes them into AppConfig::other, keeping keys this version doesn't know;
// empty settings remove "version" and "modules" (and "apache" when nothing is left).
void write_apache_settings(AppConfig& app, const ApacheSettings& settings);

// a-z, 0-9 and _, at most 40 characters: "expires", "proxy_fcgi".
[[nodiscard]] bool is_apache_module_name(std::string_view name);

enum class ApacheModuleKind : std::uint8_t {
    Needed,    // FloServer's own config uses it: always loaded
    Default,   // what .htaccess files commonly use: on unless turned off
    Optional,  // off unless turned on
    Off,       // doesn't apply behind nginx (ssl, http2, ...): never loaded
};

struct ApacheModuleInfo {
    ApacheModuleKind kind = ApacheModuleKind::Optional;
    std::vector<std::string> needs;  // loaded before it, with it
    std::string why;                 // Needed / Off: "fcgi", "https", "http2", "mpm", ...
};

// What FloServer knows about a module by its short name; an unknown one is Optional.
[[nodiscard]] ApacheModuleInfo apache_module_info(std::string_view name, bool posix);

// The short names of the modules in an Apache install's modules folder
// (mod_<name>.so), sorted. Read for a request, never on a timer.
[[nodiscard]] std::vector<std::string> apache_modules_in(const std::filesystem::path& modules_dir);

struct ApacheModulePlan {
    std::vector<std::string> load;                               // short names, in LoadModule order
    std::map<std::string, std::vector<std::string>> needed_by;   // a module loaded for others -> them
    std::vector<std::string> missing;                            // turned on, not in the install: left out
};

// What the generated config loads: FloServer's own modules, the default
// ones not turned off and the ones turned on, each after the modules it
// needs. `present` says whether the install has mod_<name>.so; modules it
// lacks are left out (FloServer's own are always written). On POSIX the MPM
// and unixd come first, in apache_config.h, and aren't in this list.
[[nodiscard]] ApacheModulePlan plan_apache_modules(const std::map<std::string, bool>& changes,
                                                   const std::function<bool(const std::string&)>& present,
                                                   bool posix);

// <data>/apache/custom.conf.
[[nodiscard]] std::filesystem::path apache_custom_conf_file(const std::filesystem::path& data);
[[nodiscard]] std::string apache_custom_conf_header();
// Makes the file with its header when it isn't there.
[[nodiscard]] bool ensure_apache_custom_conf(const std::filesystem::path& data, std::string& error);
// The file's text: empty when it's missing, too big (over 256 KiB), or only the header.
[[nodiscard]] std::string read_apache_custom_conf(const std::filesystem::path& data);

// Why one directive can't be used in custom.conf, in plain words, or
// nothing when it can. `name` is the directive (any case), with a leading
// '<' or '</' for a section, `args` what follows it. `logs_dir`: log files
// may only be written there.
[[nodiscard]] std::optional<std::string> apache_directive_problem(std::string_view name, std::string_view args,
                                                                  const std::filesystem::path& logs_dir);

// custom.conf as it goes into httpd.conf. Lines are joined where Apache
// joins them (a '\' at the end), so a directive split over lines is checked
// whole. Comments are never copied; each line written is one checked
// directive, trimmed, never ending in '\'. Left out, with "line N: why" in
// `problems`: lines over 1000 bytes, control characters, a directive name
// in quotes or with anything but letters, digits and '_', and the
// directives apache_directive_problem() refuses; a refused section
// (<VirtualHost>) is left out up to its end.
struct FilteredApacheConf {
    std::string text;
    std::vector<std::string> problems;
};
[[nodiscard]] FilteredApacheConf filter_apache_custom_conf(std::string_view text,
                                                           const std::filesystem::path& logs_dir);

// Settings → Apache, for the daemon's apache.* requests. Nothing here runs
// on a timer: the modules folder is read for apache.get and apache.set.
class ApacheSettingsService {
public:
    struct Hooks {
        std::function<void()> reload;              // bring the web server in line now
        std::function<nlohmann::json()> status;    // web.status's "apache" (or null)
        std::function<nlohmann::json()> sites;     // web.status's "sites", for who uses Apache
        // The httpd.conf Apache runs (or would), or nothing with `error`.
        std::function<std::optional<std::string>(std::string& error)> final_config;
        bool posix = false;
    };
    ApacheSettingsService(std::filesystem::path data_dir, ProjectService& projects, Hooks hooks);

    //   {"version": "2.4.69" | null (the install used), "chosen": "2.4.69" | null (config.json's),
    //    "versions": ["2.4.69", ...] (installed, newest first),
    //    "modules": [{"name", "on", "kind": "needed"|"default"|"optional"|"off", "why"?,
    //                 "needs"?: [...], "needed_by"?: [...]}],  (the install's modules, by name)
    //    "custom_conf": path, "custom_conf_exists": bool, "custom_problems"?: ["line 3: ..."],
    //    "used_by": ["wp", "_shared"], "running": bool,
    //    "refused"?: (what httpd -t said; the last config it accepted runs), "problems"?: [...]}
    [[nodiscard]] std::optional<nlohmann::json> get(ServiceRefusal& refusal) const;
    // `changes`: {"version"?: "2.4.69"|null, "modules"?: {"expires": true|false|null}} (null: back
    // to FloServer's default). Only modules the install has can be turned on, nor
    // those FloServer needs turned off. Saves, reloads (a running Apache restarts
    // with the new config once `httpd -t` passed it) and replies as get() does,
    // with "restarting": true when it was running and its config changed.
    [[nodiscard]] std::optional<nlohmann::json> set(const nlohmann::json& changes, ServiceRefusal& refusal);
    // custom.conf, made with a header when it isn't there.
    [[nodiscard]] std::optional<std::filesystem::path> custom_conf(ServiceRefusal& refusal) const;
    // The final httpd.conf, written to <data>/apache/final-httpd.conf (read-only) to look at.
    [[nodiscard]] std::optional<std::filesystem::path> final_conf(ServiceRefusal& refusal) const;

private:
    [[nodiscard]] std::optional<nlohmann::json> describe(const AppConfig& config, ServiceRefusal& refusal) const;

    std::filesystem::path data_;
    ProjectService& projects_;
    Hooks hooks_;
};

}  // namespace floserver::core

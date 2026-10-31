#pragma once

// Settings > Runtimes (D20): runtimes.list's full answer as groups of rows,
// one row per version. Plain C++ so it's tested on every platform; the
// Settings window only draws it and sends what the user picks.

#include "model/ui_strings.h"
#include "model/view_model.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// An install the window follows (jobs.get), for the row of its runtime.
struct RuntimeJob {
    std::int64_t id = 0;
    std::string name;     // "php"
    std::string version;  // the version, or what was asked for until it's known
    std::string state;    // "checking", "downloading", "installing"...
    std::uint64_t received = 0;
    std::uint64_t total = 0;
};

// jobs.get's answer, or nothing when it has no id or name.
[[nodiscard]] std::optional<RuntimeJob> runtime_job(const nlohmann::json& job);

enum class RowAction : std::uint8_t { None, Remove, Download };

struct RuntimeRow {
    std::string name;     // "php"
    std::string version;  // "8.5.1"
    std::string folder;   // runtimes.remove's "version"; set when it's installed
    std::string label;    // "8.5.1"; "nginx 1.30.6" under Web servers
    bool installed = false;
    bool is_default = false;   // what new projects get
    bool always_used = false;  // the newest nginx: every project is served through it
    // "3 projects · Shared sites", in the window's language; empty when unused.
    std::string usage;
    // Who uses it, for "Used by …": "blog, shop, Shared sites"; empty when nobody.
    std::string used_by;
    std::string note;  // ".htaccess" for Apache
    std::uint64_t size = 0;  // the download, when it isn't installed
    RowAction action = RowAction::None;
};

struct RuntimeGroup {
    std::string title;    // "PHP", "MariaDB", "Node.js", "Python", "Web servers"
    std::string runtime;  // runtimes.default's name; empty for Web servers
    bool wide = false;    // PHP and MariaDB take the full width; the rest share a row
    // The default for new projects: "Newest installed" and each installed
    // series (PHP and MariaDB). Its key is the runtime's name.
    std::optional<VersionField> picker;
    std::vector<RuntimeRow> rows;
};

struct RuntimesPage {
    bool has_list = false;  // the signed list was there: versions to download are shown
    std::string hint;       // under the title: what the page does and the list's state
    std::vector<RuntimeGroup> groups;  // only groups with a row
};

// `reply` is runtimes.list's full answer. `installed` is what the window's
// 2 s poll saw last, which wins over the reply's (nullptr: the reply's).
// `databases` is databases.list's answer, for MariaDB's port.
[[nodiscard]] RuntimesPage runtimes_page(const nlohmann::json& reply, const Installed* installed,
                                         const nlohmann::json& databases, Lang lang);

// The Tools card's line for phpMyAdmin from runtimes.list's "phpmyadmin"
// (web.status's): "Opens at https://phpmyadmin.localhost", or why it isn't
// served. Empty when there's nothing to say.
[[nodiscard]] std::string phpmyadmin_line(const nlohmann::json& served, Lang lang);

// Whether phpMyAdmin is installed (the window's poll): "Open phpMyAdmin"
// buttons are disabled with a hint until it is.
[[nodiscard]] bool phpmyadmin_installed(const Installed& installed);

// True for http(s)://phpmyadmin.localhost followed by '/', ':' or nothing:
// the only links phpmyadmin.login's answer may open.
[[nodiscard]] bool is_phpmyadmin_url(std::string_view url);

// After "Check for updates" fails the answer has no list: the versions on
// offer stay those of `before`, the rest (installed, defaults) is `fresh`'s.
[[nodiscard]] nlohmann::json merge_runtimes_reply(const nlohmann::json& before, const nlohmann::json& fresh);

// "30 MB", "8.5 MB", "640 KB": whole numbers from 10 up, Western digits.
[[nodiscard]] std::string size_text(std::uint64_t bytes);

// "Downloading… 19 MB of 31 MB", "Installing…", "Starting…".
[[nodiscard]] std::string job_text(const RuntimeJob& job, Lang lang);
// How far the download is, 0 to 1; nothing before its size is known.
[[nodiscard]] std::optional<double> job_fraction(const RuntimeJob& job);

// The install running for a row, or nullptr.
[[nodiscard]] const RuntimeJob* job_for(const RuntimeRow& row, const std::vector<RuntimeJob>& jobs);

// The key Settings' downloads go under in the worker ("_settings/php/8.3.29");
// project names can't have '_' or '/', so it's never a project's.
[[nodiscard]] std::string settings_install_key(const std::string& name, const std::string& version);
[[nodiscard]] bool is_settings_key(const std::string& key);

}  // namespace floserver::ui

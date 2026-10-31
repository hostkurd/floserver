#pragma once

// What the service should run for the current project list (D6, D10, D11):
// for each project, its document root, the installed PHP it uses and what's
// missing; and the PHP FastCGI pools those projects need.
//
// Pools: D10 gives each PHP version in use its own pool. A php-cgi process
// reads one php.ini, so projects asking for different extensions or php.ini
// values on the same version can't share one; a pool is therefore one PHP
// install plus one php.ini. Projects with the same pair share it, and a shared pool may
// sleep only when none of its projects has "Keep running" on (D11).
//
// Planning reads no files and starts nothing: the caller passes in each
// project's floserver.json result, each PHP series' custom.ini and the
// installed runtimes, so it can be tested with plain values.

#include "floserver/core/php_settings.h"
#include "floserver/core/ports.h"
#include "floserver/core/project_config.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace floserver::core {

enum class SiteState : std::uint8_t {
    Off,            // the user turned it off
    Ready,          // everything it needs is installed
    NeedsDownload,  // a runtime it asks for isn't installed (see `missing`)
    Problem,        // floserver.json has an error, the folder is gone, or it asks for what this version can't run
};

// Whether a document root holds a .php file at its top level; true when it
// can't be read (a folder not made yet counts as PHP, as before).
[[nodiscard]] bool has_php_files(const std::filesystem::path& doc_root);
// The same, but false when it can't be read: only a .php file seen counts.
[[nodiscard]] bool has_php_files_seen(const std::filesystem::path& doc_root);

// The newest install of `name` matching `wanted` ("8.2"), or the newest of
// all when `wanted` is empty; PHP only counts NTS builds (the pools' kind).
[[nodiscard]] const InstalledRuntime* best_installed(const std::vector<InstalledRuntime>& installed,
                                                     const std::string& name, const std::string& wanted);

struct PlannedSite {
    std::string name;
    std::filesystem::path dir;
    std::filesystem::path doc_root;     // dir / root from floserver.json
    SiteState state = SiteState::Off;
    std::optional<std::size_t> pool;    // index into WebPlan::pools; set when Ready (PHP)
    std::optional<std::size_t> app;     // index into WebPlan::apps; set when Ready (own server)
    bool apache = false;                // served by the shared Apache (D6), PHP from `pool`
    bool static_site = false;           // nginx serves its files as they are; no PHP, no server of its own
    // PHP is off ("php": false) and the document root has a .php file, which
    // is sent as it is. Only looked for when PHP is off.
    bool php_files = false;
    std::vector<std::string> missing;   // "php 8.2", "mariadb 11.4"
    std::string problem;                // plain English, when state is Problem
    std::optional<DatabaseSpec> database;
    // What floserver.json asks for, when it could be read: the window shows
    // and changes these (versions as written there, "8.4", not "8.4.27").
    std::optional<ProjectConfig> config;
};

struct PhpPool {
    // "php-8.4.27+gd+intl", with "-<hash>" when other settings differ
    // (php_settings_key()): stable while nothing changes.
    std::string key;
    std::string version;                   // "8.4.27"
    std::filesystem::path php_dir;         // the install folder
    PhpIniSettings settings;               // extensions and php.ini values
    std::vector<std::string> sites;        // project names, in list order
    bool keep_running = false;             // any of its projects has Keep running on
    // One port per php-cgi worker, given by assign_pool_ports(). One process
    // per socket: on Windows, several processes accepting on one shared
    // socket lose connections (found on a real PC, 29 Oct).
    std::vector<Port> ports;
};

enum class AppKind : std::uint8_t { Node, Python, Apache };

// A project that runs its own server, which nginx forwards to: a Python
// project (one that asks for Python and not PHP, D17) or a Node project (one
// that asks for Node and neither PHP nor Python, D11).
struct AppSpec {
    std::string name;                   // the project's
    AppKind kind = AppKind::Node;
    std::filesystem::path dir;          // the project folder
    std::string version;                // the runtime's: "22.23.4"
    std::filesystem::path runtime_dir;  // its install folder
    std::optional<std::string> command; // from floserver.json
    bool keep_running = false;
    Port port = 0;                      // held by the service; given by assign_app_ports()
    // A Python app that names Node too: that Node is on its PATH (its
    // build tools), as "24.22.1" in the status.
    std::filesystem::path node_dir;
    std::string node_version;
    // The shared Apache: its config, with kApachePortPlaceholder for the
    // port it's started on (apache_config.h); written to dir/httpd.conf.
    std::string server_config;
};

struct WebPlan {
    std::vector<PlannedSite> sites;  // same order as the project list
    // Shared sites (D19), named kSharedSiteName, when they're on (or their
    // settings can't be read: then a Problem). doc_root is the www folder.
    std::optional<PlannedSite> shared;
    // phpMyAdmin (phpmyadmin.h), named kPhpMyAdminSiteName, when it's
    // installed: the newest install, PHP from a pool of its own on the
    // newest installed PHP. A Problem when a project has its address.
    std::optional<PlannedSite> phpmyadmin;
    std::string phpmyadmin_version;
    std::vector<PhpPool> pools;      // in order of first use
    std::vector<AppSpec> apps;       // in list order
    // The newest installed Apache, when a project needs it.
    std::filesystem::path apache_dir;
    std::string apache_version;
};

// The floserver.json result for a project. A project without the file reads
// as an empty config (the defaults).
using ConfigReader = std::function<ConfigResult(const Project&)>;

// Reads <dir>/floserver.json from the disk (at most 64 KiB). A missing file
// gives the defaults; a missing folder or an unreadable file gives an error.
[[nodiscard]] ConfigResult read_project_config_file(const Project& project);

// The text of a PHP series' custom.ini ("8.5"), read_php_custom_ini().
using CustomIniReader = std::function<std::string(const std::string& series)>;

// `default_php` is the version to use for projects whose floserver.json names
// none ("8.5"); nothing means the newest installed PHP. Shared sites are
// planned from config.json's "shared" (shared_sites.h). PHP settings come
// from config.json's "php" and each project's floserver.json; `custom_ini`
// is asked once per series in use (none: no custom.ini).
[[nodiscard]] WebPlan plan_sites(const AppConfig& config, const ConfigReader& read_config,
                                 const std::vector<InstalledRuntime>& installed,
                                 const std::optional<std::string>& default_php,
                                 const CustomIniReader& custom_ini = {});

// Which installed runtimes the projects and shared sites use (D20), so
// Settings can show it and a removal can be refused: "php/8.5.12" (name /
// install folder) -> {"blog", "shop", "_shared"}, sorted. Projects that are
// off count too (turning one on would need it again), so they're planned
// as if on; shared sites count only while they're on. The newest nginx is
// always in use, by "FloServer". Reads every project's floserver.json, so
// it's for a request, not a timer.
[[nodiscard]] std::map<std::string, std::vector<std::string>> runtime_users(
    const AppConfig& config, const ConfigReader& read_config, const std::vector<InstalledRuntime>& installed,
    const std::optional<std::string>& default_php);

inline constexpr Port kFirstPoolPort = 9100;
inline constexpr std::size_t kPhpWorkers = 4;  // one slow request doesn't hold up the rest

// Gives each pool `workers` loopback ports from 9100 upwards. A pool listed
// in `previous` (key -> ports) keeps its ports without a check, because the
// service itself holds that socket (D11); a reload never moves a pool that
// didn't change. New pools get the first port that `probe` says is free and
// no other pool uses. False when ports run out.
[[nodiscard]] bool assign_pool_ports(WebPlan& plan, const std::map<std::string, std::vector<Port>>& previous,
                                     const PortProbe& probe, std::size_t workers = kPhpWorkers);

inline constexpr Port kFirstAppPort = 9300;

// Gives each app a port from 9300 upwards, after assign_pool_ports(): an app
// in `previous` (project name -> port) keeps its port, new ones get the first
// that `probe` says is free and no pool or app uses. False when ports run out.
[[nodiscard]] bool assign_app_ports(WebPlan& plan, const std::map<std::string, Port>& previous, const PortProbe& probe);

}  // namespace floserver::core

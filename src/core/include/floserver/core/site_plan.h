#pragma once

// What the service should run for the current project list (D6, D10, D11):
// for each project, its document root, the installed PHP it uses and what's
// missing; and the PHP FastCGI pools those projects need.
//
// Pools: D10 gives each PHP version in use its own pool. A php-cgi process
// reads one php.ini, so projects asking for different extensions on the same
// version can't share one; a pool is therefore one PHP install plus one set
// of extensions. Projects with the same pair share it, and a shared pool may
// sleep only when none of its projects has "Keep running" on (D11).
//
// Planning reads no files and starts nothing: the caller passes in each
// project's floserver.json result and the installed runtimes, so it can be
// tested with plain values.

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

struct PlannedSite {
    std::string name;
    std::filesystem::path dir;
    std::filesystem::path doc_root;     // dir / root from floserver.json
    SiteState state = SiteState::Off;
    std::optional<std::size_t> pool;    // index into WebPlan::pools; set when Ready (PHP)
    std::optional<std::size_t> app;     // index into WebPlan::apps; set when Ready (own server)
    std::vector<std::string> missing;   // "php 8.2", "mariadb 11.4"
    std::string problem;                // plain English, when state is Problem
    std::optional<DatabaseSpec> database;
};

struct PhpPool {
    std::string key;                       // "php-8.4.27+gd+intl": stable while nothing changes
    std::string version;                   // "8.4.27"
    std::filesystem::path php_dir;         // the install folder
    std::vector<std::string> extensions;   // sorted, no duplicates
    std::vector<std::string> sites;        // project names, in list order
    bool keep_running = false;             // any of its projects has Keep running on
    // One port per php-cgi worker, given by assign_pool_ports(). One process
    // per socket: on Windows, several processes accepting on one shared
    // socket lose connections (found on a real PC, 29 Oct).
    std::vector<Port> ports;
};

enum class AppKind : std::uint8_t { Node, Python };

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
};

struct WebPlan {
    std::vector<PlannedSite> sites;  // same order as the project list
    std::vector<PhpPool> pools;      // in order of first use
    std::vector<AppSpec> apps;       // in list order
};

// The floserver.json result for a project. A project without the file reads
// as an empty config (the defaults).
using ConfigReader = std::function<ConfigResult(const Project&)>;

// Reads <dir>/floserver.json from the disk (at most 64 KiB). A missing file
// gives the defaults; a missing folder or an unreadable file gives an error.
[[nodiscard]] ConfigResult read_project_config_file(const Project& project);

// `default_php` is the version to use for projects whose floserver.json names
// none ("8.5"); nothing means the newest installed PHP.
[[nodiscard]] WebPlan plan_sites(const AppConfig& config, const ConfigReader& read_config,
                                 const std::vector<InstalledRuntime>& installed,
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

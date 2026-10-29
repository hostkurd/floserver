#include "floserver/core/site_plan.h"

#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <set>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxProjectConfigBytes = std::size_t{64} * 1024;

// PHP installs this build can run as a pool: the usual NTS builds.
bool is_pool_php(const InstalledRuntime& r) {
    return r.name == "php" && (r.variant.empty() || r.variant == "nts");
}

// The newest install of `name` matching `wanted` ("8.2"), or the newest of all
// when `wanted` is empty.
const InstalledRuntime* best_installed(const std::vector<InstalledRuntime>& installed, const std::string& name,
                                       const std::string& wanted) {
    const std::optional<Version> want = wanted.empty() ? std::nullopt : Version::parse(wanted);
    if (!wanted.empty() && !want) {
        return nullptr;
    }
    const InstalledRuntime* best = nullptr;
    std::optional<Version> best_version;
    for (const InstalledRuntime& r : installed) {
        if (r.name != name || (name == "php" && !is_pool_php(r))) {
            continue;
        }
        const auto v = Version::parse(r.version);
        if (!v || (want && !want->matches(*v))) {
            continue;
        }
        if (!best_version || *v > *best_version) {
            best = &r;
            best_version = v;
        }
    }
    return best;
}

std::string pool_key(const std::string& version, const std::vector<std::string>& extensions) {
    std::string key = "php-" + version;
    for (const std::string& e : extensions) {
        key += "+" + e;
    }
    return key;
}

}  // namespace

ConfigResult read_project_config_file(const Project& project) {
    std::error_code ec;
    if (!fs::is_directory(project.dir, ec)) {
        ConfigResult r;
        r.errors.push_back({"", "the folder " + path_to_utf8(project.dir) + " is missing"});
        return r;
    }
    const fs::path file = project.dir / kProjectConfigFileName;
    if (!fs::exists(file, ec) && !ec) {
        ConfigResult r;
        r.config = ProjectConfig{};
        return r;
    }
    // Only a plain file: a link to a pipe or a device would hang the read.
    if (!fs::is_regular_file(file, ec)) {
        ConfigResult r;
        r.errors.push_back({"", "floserver.json isn't a plain file"});
        return r;
    }
    const auto text = read_small_file(file, kMaxProjectConfigBytes);
    if (!text) {
        ConfigResult r;
        r.errors.push_back({"", "floserver.json can't be read (or is larger than 64 KB)"});
        return r;
    }
    ConfigResult result = parse_project_config(*text);
    for (ConfigProblem& e : result.errors) {
        e.message = "floserver.json: " + (e.key.empty() ? std::string() : e.key + " ") + e.message;
        e.key.clear();
    }
    // The root is checked as text by the parser; a link inside the project
    // (git can store one) could still lead outside it.
    if (result.config && result.config->root != ".") {
        const fs::path real_dir = fs::weakly_canonical(project.dir, ec);
        const fs::path real_root = ec ? fs::path() : fs::weakly_canonical(project.dir / path_from_utf8(result.config->root), ec);
        const fs::path inside = real_root.lexically_relative(real_dir);
        if (ec || inside.empty() || *inside.begin() == "..") {
            result.errors.push_back({"", "floserver.json: root leads outside the project folder (through a link)"});
            result.config.reset();
        }
    }
    return result;
}

WebPlan plan_sites(const AppConfig& config, const ConfigReader& read_config,
                   const std::vector<InstalledRuntime>& installed, const std::optional<std::string>& default_php) {
    WebPlan plan;
    for (const Project& project : config.projects) {
        PlannedSite site;
        site.name = project.name;
        site.dir = project.dir;
        site.doc_root = project.dir;
        if (!project.on) {
            plan.sites.push_back(std::move(site));
            continue;
        }

        const ConfigResult result = read_config(project);
        if (!result.config) {
            site.state = SiteState::Problem;
            const ConfigProblem& first = result.errors.empty() ? ConfigProblem{"", "can't be read"} : result.errors.front();
            site.problem = (first.key.empty() ? std::string() : first.key + " ") + first.message;
            plan.sites.push_back(std::move(site));
            continue;
        }
        const ProjectConfig& pc = *result.config;
        site.doc_root = pc.root == "." ? project.dir : (project.dir / path_from_utf8(pc.root)).lexically_normal();
        site.database = pc.database;
        // One unusable folder name must not stop nginx for every project.
        if (!nginx_quoted_path(site.doc_root)) {
            site.state = SiteState::Problem;
            site.problem = "the folder path has a $ or \" in it, which nginx can't use; rename the folder";
            plan.sites.push_back(std::move(site));
            continue;
        }

        // Not served by this build yet: Apache projects (D6) and Node-only
        // projects, which nginx will proxy to their dev server (D11).
        if (pc.web == WebServer::Apache) {
            site.state = SiteState::Problem;
            site.problem = "Apache projects can't run in this FloServer build yet";
            plan.sites.push_back(std::move(site));
            continue;
        }
        if (pc.node && !pc.php) {
            site.state = SiteState::Problem;
            site.problem = "Node projects can't run in this FloServer build yet";
            plan.sites.push_back(std::move(site));
            continue;
        }

        const std::string wanted_php = pc.php.value_or(default_php.value_or(""));
        const InstalledRuntime* php = best_installed(installed, "php", wanted_php);
        if (php == nullptr) {
            site.missing.push_back(wanted_php.empty() ? "php" : "php " + wanted_php);
        }
        if (pc.node && best_installed(installed, "node", *pc.node) == nullptr) {
            site.missing.push_back("node " + *pc.node);
        }
        if (pc.database && best_installed(installed, pc.database->family, pc.database->version) == nullptr) {
            site.missing.push_back(pc.database->family + " " + pc.database->version);
        }
        if (!site.missing.empty() || php == nullptr) {
            site.state = SiteState::NeedsDownload;
            plan.sites.push_back(std::move(site));
            continue;
        }

        std::vector<std::string> extensions = pc.extensions;
        std::sort(extensions.begin(), extensions.end());
        extensions.erase(std::unique(extensions.begin(), extensions.end()), extensions.end());
        const std::string key = pool_key(php->version, extensions);
        auto it = std::find_if(plan.pools.begin(), plan.pools.end(), [&key](const PhpPool& p) { return p.key == key; });
        if (it == plan.pools.end()) {
            plan.pools.push_back({key, php->version, php->dir, extensions, {}, false, {}});
            it = std::prev(plan.pools.end());
        }
        it->sites.push_back(project.name);
        it->keep_running = it->keep_running || project.keep_running;
        site.pool = static_cast<std::size_t>(it - plan.pools.begin());
        site.state = SiteState::Ready;
        plan.sites.push_back(std::move(site));
    }
    return plan;
}

bool assign_pool_ports(WebPlan& plan, const std::map<std::string, std::vector<Port>>& previous,
                       const PortProbe& probe, std::size_t workers) {
    std::set<Port> used;
    for (PhpPool& pool : plan.pools) {
        pool.ports.clear();
        const auto it = previous.find(pool.key);
        if (it == previous.end() || it->second.size() != workers) {
            continue;
        }
        if (std::none_of(it->second.begin(), it->second.end(), [&used](Port p) { return used.contains(p); })) {
            pool.ports = it->second;
            used.insert(it->second.begin(), it->second.end());
        }
    }
    // Ports other pools held before stay out of reach for new pools, so an
    // old pool being retired can't hand its port to a new one mid-reload.
    for (const auto& [key, ports] : previous) {
        used.insert(ports.begin(), ports.end());
    }
    for (PhpPool& pool : plan.pools) {
        while (pool.ports.size() < workers) {
            const auto port = pick_port_from(kFirstPoolPort, used, probe);
            if (!port) {
                return false;
            }
            pool.ports.push_back(*port);
            used.insert(*port);
        }
    }
    return true;
}

}  // namespace floserver::core

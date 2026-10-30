#include "floserver/core/site_plan.h"

#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/shared_sites.h"
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

std::string pool_key(const std::string& version, const std::vector<std::string>& extensions) {
    std::string key = "php-" + version;
    for (const std::string& e : extensions) {
        key += "+" + e;
    }
    return key;
}

}  // namespace

bool has_php_files(const fs::path& doc_root) {
    std::error_code ec;
    fs::directory_iterator it(doc_root, ec);
    if (ec) {
        return true;  // can't tell (no such folder yet): a PHP project, as before
    }
    std::size_t looked = 0;
    for (; it != fs::directory_iterator() && looked < 2000; it.increment(ec), ++looked) {
        if (ec) {
            return true;
        }
        std::string ext = path_to_utf8(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
        if (ext == ".php" && !it->is_directory(ec)) {
            return true;
        }
    }
    return false;
}

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

namespace {

// The pool for this PHP install and these extensions, made when new; the
// site joins it.
std::size_t add_to_pool(WebPlan& plan, const InstalledRuntime& php, std::vector<std::string> extensions,
                        const std::string& site, bool keep_running) {
    std::sort(extensions.begin(), extensions.end());
    extensions.erase(std::unique(extensions.begin(), extensions.end()), extensions.end());
    const std::string key = pool_key(php.version, extensions);
    auto it = std::find_if(plan.pools.begin(), plan.pools.end(), [&key](const PhpPool& p) { return p.key == key; });
    if (it == plan.pools.end()) {
        plan.pools.push_back({key, php.version, php.dir, extensions, {}, false, {}});
        it = std::prev(plan.pools.end());
    }
    it->sites.push_back(site);
    it->keep_running = it->keep_running || keep_running;
    return static_cast<std::size_t>(it - plan.pools.begin());
}

// Shared sites (D19): one site for the whole www folder, PHP from a pool
// like any project's, Apache or nginx as the settings say.
void plan_shared(WebPlan& plan, const AppConfig& config, const std::vector<InstalledRuntime>& installed,
                 const std::optional<std::string>& default_php) {
    PlannedSite site;
    site.name = std::string(kSharedSiteName);
    std::string error;
    const auto shared = read_shared_config(config, error);
    if (!shared) {
        site.state = SiteState::Problem;
        site.problem = "config.json: " + error;
        plan.shared = std::move(site);
        return;
    }
    if (!shared->on) {
        return;
    }
    const auto dir = shared_dir(*shared);
    if (!dir) {
        site.state = SiteState::Problem;
        site.problem = "there's no home folder for the www folder; choose one in Settings";
        plan.shared = std::move(site);
        return;
    }
    site.dir = *dir;
    site.doc_root = *dir;
    site.database = shared->database;
    const bool apache = shared->web == WebServer::Apache;
    if (!nginx_quoted_path(site.doc_root)) {
        site.state = SiteState::Problem;
        site.problem = "the www folder's path has a $ or \" in it, which nginx can't use; choose another folder";
        plan.shared = std::move(site);
        return;
    }
    if (apache && path_to_utf8(site.doc_root).find_first_of("[]*?|%") != std::string::npos) {
        site.state = SiteState::Problem;
        site.problem = "the www folder's path has one of [ ] * ? | % in it, which Apache can't use; choose another folder";
        plan.shared = std::move(site);
        return;
    }
    const std::string wanted_php = shared->php.value_or(default_php.value_or(""));
    const InstalledRuntime* php = best_installed(installed, "php", wanted_php);
    if (php == nullptr) {
        site.missing.push_back(wanted_php.empty() ? "php" : "php " + wanted_php);
    }
    const InstalledRuntime* apache_runtime = apache ? best_installed(installed, "apache", "") : nullptr;
    if (apache && apache_runtime == nullptr) {
        site.missing.emplace_back("apache");
    }
    if (shared->database && best_installed(installed, shared->database->family, shared->database->version) == nullptr) {
        site.missing.push_back(shared->database->family + " " + shared->database->version);
    }
    site.config = shared_project_config(*shared, php != nullptr ? php->version : wanted_php);
    if (!site.missing.empty() || php == nullptr) {
        site.state = SiteState::NeedsDownload;
        plan.shared = std::move(site);
        return;
    }
    site.pool = add_to_pool(plan, *php, {}, site.name, false);
    site.state = SiteState::Ready;
    if (apache) {
        site.apache = true;
        plan.apache_dir = apache_runtime->dir;
        plan.apache_version = apache_runtime->version;
    }
    plan.shared = std::move(site);
}

}  // namespace

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
        site.config = pc;
        site.doc_root = pc.root == "." ? project.dir : (project.dir / path_from_utf8(pc.root)).lexically_normal();
        site.database = pc.database;
        // One unusable folder name must not stop nginx for every project.
        if (!nginx_quoted_path(site.doc_root)) {
            site.state = SiteState::Problem;
            site.problem = "the folder path has a $ or \" in it, which nginx can't use; rename the folder";
            plan.sites.push_back(std::move(site));
            continue;
        }

        // Apache projects (D6) are PHP projects served by the shared Apache:
        // the same pools, Apache for .htaccess. Apache reads [ ] * ? in a
        // folder path as a pattern, and | can't go into its regex.
        const bool apache = pc.web == WebServer::Apache;
        if (apache && path_to_utf8(site.doc_root).find_first_of("[]*?|") != std::string::npos) {
            site.state = SiteState::Problem;
            site.problem = "the folder path has one of [ ] * ? | in it, which Apache can't use; rename the folder";
            plan.sites.push_back(std::move(site));
            continue;
        }
        // Python or Node without PHP: the project's own server, forwarded to
        // (D11, D17). Python serves when both are named (a Django app with a
        // Vite front end); with PHP, they're only there for their tools.
        if ((pc.node || pc.python) && !pc.php && !apache) {
            const AppKind kind = pc.python ? AppKind::Python : AppKind::Node;
            const std::string runtime_name = pc.python ? "python" : "node";
            const std::string wanted = pc.python ? *pc.python : *pc.node;
            const InstalledRuntime* runtime = best_installed(installed, runtime_name, wanted);
            if (runtime == nullptr) {
                site.missing.push_back(runtime_name);
                site.missing.back().append(" ").append(wanted);
            }
            const InstalledRuntime* tools =
                pc.python && pc.node ? best_installed(installed, "node", *pc.node) : nullptr;
            if (pc.python && pc.node && tools == nullptr) {
                site.missing.push_back("node " + *pc.node);
            }
            if (pc.database && best_installed(installed, pc.database->family, pc.database->version) == nullptr) {
                site.missing.push_back(pc.database->family + " " + pc.database->version);
            }
            if (!site.missing.empty() || runtime == nullptr) {
                site.state = SiteState::NeedsDownload;
                plan.sites.push_back(std::move(site));
                continue;
            }
            plan.apps.push_back({project.name, kind, project.dir, runtime->version, runtime->dir, pc.command,
                                 project.keep_running, 0, tools != nullptr ? tools->dir : fs::path(),
                                 tools != nullptr ? tools->version : std::string(), {}});
            site.app = plan.apps.size() - 1;
            site.state = SiteState::Ready;
            plan.sites.push_back(std::move(site));
            continue;
        }

        // A static site: no PHP, Node or Python asked for and no .php file in
        // the document root. nginx serves its files as they are; nothing else
        // runs. A .php file (or a PHP version) later makes it a PHP project.
        if (!pc.php && !pc.node && !pc.python && !apache && !has_php_files(site.doc_root)) {
            if (pc.database && best_installed(installed, pc.database->family, pc.database->version) == nullptr) {
                site.missing.push_back(pc.database->family + " " + pc.database->version);
                site.state = SiteState::NeedsDownload;
            } else {
                site.state = SiteState::Ready;
            }
            site.static_site = true;
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
        if (pc.python && best_installed(installed, "python", *pc.python) == nullptr) {
            site.missing.push_back("python " + *pc.python);
        }
        const InstalledRuntime* apache_runtime = apache ? best_installed(installed, "apache", "") : nullptr;
        if (apache && apache_runtime == nullptr) {
            site.missing.emplace_back("apache");
        }
        if (pc.database && best_installed(installed, pc.database->family, pc.database->version) == nullptr) {
            site.missing.push_back(pc.database->family + " " + pc.database->version);
        }
        if (!site.missing.empty() || php == nullptr) {
            site.state = SiteState::NeedsDownload;
            plan.sites.push_back(std::move(site));
            continue;
        }

        site.pool = add_to_pool(plan, *php, pc.extensions, project.name, project.keep_running);
        site.state = SiteState::Ready;
        if (apache) {
            site.apache = true;
            plan.apache_dir = apache_runtime->dir;
            plan.apache_version = apache_runtime->version;
        }
        plan.sites.push_back(std::move(site));
    }
    plan_shared(plan, config, installed, default_php);
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

bool assign_app_ports(WebPlan& plan, const std::map<std::string, Port>& previous, const PortProbe& probe) {
    std::set<Port> used;
    for (const PhpPool& pool : plan.pools) {
        used.insert(pool.ports.begin(), pool.ports.end());
    }
    for (AppSpec& app : plan.apps) {
        app.port = 0;
        const auto it = previous.find(app.name);
        if (it != previous.end() && !used.contains(it->second)) {
            app.port = it->second;
            used.insert(it->second);
        }
    }
    // As with pools: a port an app held before isn't handed to another one
    // in the same reload.
    for (const auto& [name, port] : previous) {
        used.insert(port);
    }
    for (AppSpec& app : plan.apps) {
        if (app.port == 0) {
            const auto port = pick_port_from(kFirstAppPort, used, probe);
            if (!port) {
                return false;
            }
            app.port = *port;
            used.insert(*port);
        }
    }
    return true;
}

}  // namespace floserver::core

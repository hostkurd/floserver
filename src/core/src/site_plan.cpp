#include "floserver/core/site_plan.h"

#include "floserver/core/apache_settings.h"

#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/phpmyadmin.h"
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

}  // namespace

namespace {

// `unreadable`: the answer when the folder can't be read.
bool find_php_files(const fs::path& doc_root, bool unreadable) {
    std::error_code ec;
    fs::directory_iterator it(doc_root, ec);
    if (ec) {
        return unreadable;
    }
    std::size_t looked = 0;
    for (; it != fs::directory_iterator() && looked < 2000; it.increment(ec), ++looked) {
        if (ec) {
            return unreadable;
        }
        std::string ext = path_to_utf8(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
        if (ext == ".php" && !it->is_directory(ec)) {
            return true;
        }
    }
    return false;
}

}  // namespace

// Can't tell (no such folder yet): a PHP project, as before.
bool has_php_files(const fs::path& doc_root) { return find_php_files(doc_root, true); }

bool has_php_files_seen(const fs::path& doc_root) { return find_php_files(doc_root, false); }

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

const InstalledRuntime* chosen_apache(const std::vector<InstalledRuntime>& installed, const AppConfig& config) {
    if (const auto wanted = read_apache_settings(config).version) {
        for (const InstalledRuntime& r : installed) {
            if (r.name == "apache" && r.version == *wanted) {
                return &r;
            }
        }
    }
    return best_installed(installed, "apache", "");
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

// The PHP settings (D20) for a site on this install: the series' from
// config.json, its custom.ini (read once per series), and the project's.
class PhpSettingsSource {
public:
    PhpSettingsSource(const AppConfig& config, const CustomIniReader& custom_ini)
        : series_(read_php_settings(config)), custom_ini_(&custom_ini) {}

    PhpIniSettings operator()(const InstalledRuntime& php, const ProjectConfig* project) {
        const std::string series = php_series_of(php.version).value_or(php.version);
        auto text = custom_.find(series);
        if (text == custom_.end()) {
            text = custom_.emplace(series, *custom_ini_ ? (*custom_ini_)(series) : std::string()).first;
        }
        const auto found = series_.find(series);
        return effective_php_settings(series, found != series_.end() ? &found->second : nullptr, text->second, project);
    }

private:
    PhpSettings series_;
    const CustomIniReader* custom_ini_;
    std::map<std::string, std::string> custom_;
};

// The pool for this PHP install and these settings, made when new; the
// site joins it.
std::size_t add_to_pool(WebPlan& plan, const InstalledRuntime& php, PhpIniSettings settings,
                        const std::string& site, bool keep_running) {
    std::string key = "php-" + php.version + php_settings_key(settings);
    if (site == kPhpMyAdminSiteName) {
        key += "-phpmyadmin";  // always a pool of its own, whatever a project's settings
    }
    auto it = std::find_if(plan.pools.begin(), plan.pools.end(), [&key](const PhpPool& p) { return p.key == key; });
    if (it == plan.pools.end()) {
        plan.pools.push_back({key, php.version, php.dir, std::move(settings), {}, false, {}});
        it = std::prev(plan.pools.end());
    }
    it->sites.push_back(site);
    it->keep_running = it->keep_running || keep_running;
    return static_cast<std::size_t>(it - plan.pools.begin());
}

// Shared sites (D19): one site for the whole www folder, PHP from a pool
// like any project's, Apache or nginx as the settings say.
void plan_shared(WebPlan& plan, const AppConfig& config, const std::vector<InstalledRuntime>& installed,
                 const std::optional<std::string>& default_php, PhpSettingsSource& php_settings) {
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
    const InstalledRuntime* apache_runtime = apache ? chosen_apache(installed, config) : nullptr;
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
    // The series' settings; shared sites have no project's own.
    site.pool = add_to_pool(plan, *php, php_settings(*php, nullptr), site.name, false);
    site.state = SiteState::Ready;
    if (apache) {
        site.apache = true;
        plan.apache_dir = apache_runtime->dir;
        plan.apache_version = apache_runtime->version;
    }
    plan.shared = std::move(site);
}

// phpMyAdmin: its own pool on the newest PHP, so its php.ini can hide what
// newer PHP versions say about phpMyAdmin's older code.
void plan_phpmyadmin(WebPlan& plan, const AppConfig& config, const std::vector<InstalledRuntime>& installed,
                     PhpSettingsSource& php_settings) {
    const InstalledRuntime* pma = best_installed(installed, std::string(kPhpMyAdminRuntime), "");
    if (pma == nullptr) {
        return;  // not installed: not served
    }
    PlannedSite site;
    site.name = std::string(kPhpMyAdminSiteName);
    site.dir = pma->dir;
    site.doc_root = pma->dir;
    plan.phpmyadmin_version = pma->version;
    if (find_project(config, kPhpMyAdminHostName) != nullptr) {
        site.state = SiteState::Problem;
        site.problem = "the project " + std::string(kPhpMyAdminHostName) + " has the address " +
                       std::string(kPhpMyAdminHostName) + ".localhost, so phpMyAdmin can't use it";
        plan.phpmyadmin = std::move(site);
        return;
    }
    if (!nginx_quoted_path(site.doc_root)) {
        site.state = SiteState::Problem;
        site.problem = "phpMyAdmin's folder path has a $ or \" in it, which nginx can't use";
        plan.phpmyadmin = std::move(site);
        return;
    }
    const InstalledRuntime* php = best_installed(installed, "php", "");
    if (php == nullptr) {
        site.state = SiteState::NeedsDownload;
        site.missing.emplace_back("php");
        plan.phpmyadmin = std::move(site);
        return;
    }
    PhpIniSettings settings = php_settings(*php, nullptr);
    // Errors go to the pool's log, not into phpMyAdmin's pages; from PHP 8.4
    // on, deprecation notices aren't reported at all (phpMyAdmin 5.2 is made
    // for PHP up to 8.3 and runs on later ones with such notices).
    settings.project_ini["display_errors"] = "Off";
    settings.project_ini["display_startup_errors"] = "Off";
    const auto version = Version::parse(php->version);
    const auto first_noisy = Version::parse("8.4");
    if (version && first_noisy && !(*version < *first_noisy)) {
        settings.project_ini["error_reporting"] = "E_ALL & ~E_DEPRECATED";
    }
    site.pool = add_to_pool(plan, *php, std::move(settings), site.name, false);
    site.state = SiteState::Ready;
    plan.phpmyadmin = std::move(site);
}

}  // namespace

WebPlan plan_sites(const AppConfig& config, const ConfigReader& read_config,
                   const std::vector<InstalledRuntime>& installed, const std::optional<std::string>& default_php,
                   const CustomIniReader& custom_ini) {
    WebPlan plan;
    PhpSettingsSource php_settings(config, custom_ini);
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
        if ((pc.node || pc.python) && !php_asked(pc) && !apache) {
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
        // the document root, or PHP turned off ("php": false). nginx serves
        // its files as they are; nothing else runs. Without the key, a .php
        // file (or a PHP version) later makes it a PHP project.
        const bool off = php_off(pc);
        if (off) {
            site.php_files = has_php_files_seen(site.doc_root);
        }
        if (!php_asked(pc) && !pc.node && !pc.python && !apache && (off || !has_php_files(site.doc_root))) {
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
        // Apache with PHP off: Apache alone (for .htaccess), no pool.
        if (apache && off) {
            const InstalledRuntime* apache_runtime = chosen_apache(installed, config);
            if (apache_runtime == nullptr) {
                site.missing.emplace_back("apache");
            }
            if (pc.node && best_installed(installed, "node", *pc.node) == nullptr) {
                site.missing.push_back("node " + *pc.node);
            }
            if (pc.python && best_installed(installed, "python", *pc.python) == nullptr) {
                site.missing.push_back("python " + *pc.python);
            }
            if (pc.database && best_installed(installed, pc.database->family, pc.database->version) == nullptr) {
                site.missing.push_back(pc.database->family + " " + pc.database->version);
            }
            if (!site.missing.empty() || apache_runtime == nullptr) {
                site.state = SiteState::NeedsDownload;
            } else {
                site.state = SiteState::Ready;
                site.apache = true;
                plan.apache_dir = apache_runtime->dir;
                plan.apache_version = apache_runtime->version;
            }
            plan.sites.push_back(std::move(site));
            continue;
        }

        // "newest" means the newest installed, whatever the default.
        const std::string wanted_php =
            pc.php.value_or(pc.php_choice == PhpChoice::Newest ? std::string() : default_php.value_or(""));
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
        const InstalledRuntime* apache_runtime = apache ? chosen_apache(installed, config) : nullptr;
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

        site.pool = add_to_pool(plan, *php, php_settings(*php, &pc), project.name, project.keep_running);
        site.state = SiteState::Ready;
        if (apache) {
            site.apache = true;
            plan.apache_dir = apache_runtime->dir;
            plan.apache_version = apache_runtime->version;
        }
        plan.sites.push_back(std::move(site));
    }
    plan_shared(plan, config, installed, default_php, php_settings);
    plan_phpmyadmin(plan, config, installed, php_settings);
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

std::map<std::string, std::vector<std::string>> runtime_users(const AppConfig& config, const ConfigReader& read_config,
                                                              const std::vector<InstalledRuntime>& installed,
                                                              const std::optional<std::string>& default_php) {
    AppConfig all_on = config;
    for (Project& p : all_on.projects) {
        p.on = true;
    }
    const WebPlan plan = plan_sites(all_on, read_config, installed, default_php);

    std::map<std::string, std::set<std::string>> users;
    const auto use = [&users](const std::string& name, const fs::path& dir, const std::string& who) {
        if (!dir.empty()) {
            users[name + "/" + path_to_utf8(dir.filename())].insert(who);
        }
    };
    for (const PhpPool& pool : plan.pools) {
        for (const std::string& site : pool.sites) {
            if (site == kPhpMyAdminSiteName) {
                continue;  // it moves to the next newest PHP when this one goes
            }
            use("php", pool.php_dir, site);
        }
    }
    for (const AppSpec& app : plan.apps) {
        if (app.kind == AppKind::Node) {
            use("node", app.runtime_dir, app.name);
        } else if (app.kind == AppKind::Python) {
            use("python", app.runtime_dir, app.name);
            use("node", app.node_dir, app.name);
        }
    }
    // What each project names, whatever state the plan left it in: one that
    // waits for a download (or has a problem such as a missing folder) still
    // needs the rest, and removing one of those would mean two downloads.
    const auto named = [&](const ProjectConfig& pc, const std::string& who) {
        if (php_asked(pc)) {
            if (const InstalledRuntime* r = best_installed(installed, "php", pc.php.value_or(""))) {
                use("php", r->dir, who);
            }
        }
        if (pc.node) {
            if (const InstalledRuntime* r = best_installed(installed, "node", *pc.node)) {
                use("node", r->dir, who);
            }
        }
        if (pc.python) {
            if (const InstalledRuntime* r = best_installed(installed, "python", *pc.python)) {
                use("python", r->dir, who);
            }
        }
        if (pc.database && pc.database->family == "mariadb") {
            if (const InstalledRuntime* r = best_installed(installed, "mariadb", pc.database->version)) {
                use("mariadb", r->dir, who);
            }
        }
        if (pc.web == WebServer::Apache) {
            if (const InstalledRuntime* r = chosen_apache(installed, config)) {
                use("apache", r->dir, who);
            }
        }
    };
    for (const PlannedSite& site : plan.sites) {
        if (site.config) {
            named(*site.config, site.name);
        }
    }
    // Shared sites while on, from config.json's own settings (the plan
    // doesn't record them when one is missing). No PHP named: the newest.
    std::string shared_error;
    if (const auto shared = read_shared_config(config, shared_error); shared && shared->on) {
        named(shared_project_config(*shared, shared->php.value_or("")), std::string(kSharedSiteName));
    }
    if (const InstalledRuntime* nginx = best_installed(installed, "nginx", "")) {
        use("nginx", nginx->dir, "FloServer");
    }

    std::map<std::string, std::vector<std::string>> out;
    for (auto& [key, who] : users) {
        out[key] = {who.begin(), who.end()};
    }
    return out;
}

}  // namespace floserver::core

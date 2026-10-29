#include "floserver/core/web_service.h"

#include "floserver/core/apache_config.h"
#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/encoding.h"
#include "floserver/core/sha256.h"
#include "floserver/core/trust_store.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <system_error>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kMaxLogTail = 2000;
constexpr auto kNginxRetry = 10s;

// The newest install of `name`, or nothing.
const InstalledRuntime* newest(const std::vector<InstalledRuntime>& installed, const std::string& name) {
    const InstalledRuntime* best = nullptr;
    std::optional<Version> best_v;
    for (const InstalledRuntime& r : installed) {
        const auto v = Version::parse(r.version);
        if (r.name == name && v && (!best_v || *v > *best_v)) {
            best = &r;
            best_v = v;
        }
    }
    return best;
}

std::string tail_of(const fs::path& file) {
    auto text = read_small_file(file, std::size_t{256} * 1024).value_or("");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.size() > kMaxLogTail) {
        text = "..." + text.substr(text.size() - kMaxLogTail);
    }
    return text;
}

bool running_as_root() {
#ifdef _WIN32
    return false;
#else
    return geteuid() == 0;
#endif
}

const char* pool_state_name(PoolState s) {
    switch (s) {
        case PoolState::Sleeping:
            return "sleeping";
        case PoolState::Running:
            return "running";
        case PoolState::Failed:
            return "failed";
    }
    return "failed";
}

}  // namespace

fs::path web_dir(const fs::path& data) { return data / "web"; }

std::optional<fs::path> nginx_program(const fs::path& nginx_dir) {
#ifdef _WIN32
    const char* names[] = {"nginx.exe"};  // NOLINT(modernize-avoid-c-arrays)
#else
    const char* names[] = {"nginx", "sbin/nginx"};  // NOLINT(modernize-avoid-c-arrays)
#endif
    for (const char* n : names) {
        std::error_code ec;
        fs::path p = nginx_dir / n;
        if (fs::is_regular_file(p, ec)) {
            return p;
        }
    }
    return std::nullopt;
}

WebService::WebService(WebServiceConfig config, CertStore& certs)
    : config_(std::move(config)), certs_(certs), work_(web_dir(config_.data_dir)) {
    if (!config_.probe) {
        config_.probe = can_listen_on_loopback;
    }
}

WebService::~WebService() { stop(); }

void WebService::start() {
    {
        const std::lock_guard lock(mutex_);
        reload_locked();
    }
    thread_ = std::thread([this] { supervise(); });
}

void WebService::stop() {
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    const std::lock_guard lock(mutex_);
    stopped_ = true;  // a reload still on its way (a client mid-request) starts nothing
    stop_locked();
}

void WebService::stop_locked() {
    stop_nginx_locked();
    pools_.clear();
    // All at once, then wait: each may take a few seconds to shut down.
    const auto apps = std::move(apps_);
    apps_.clear();
    for (const auto& [name, runner] : apps) {
        runner->close_soon();
    }
    for (const auto& [name, runner] : apps) {
        runner->close();
    }
    sites_.clear();
}

void WebService::replace_apps_locked(std::map<std::string, std::shared_ptr<AppRunner>> apps) {
    for (const auto& [name, runner] : apps_) {
        const auto it = apps.find(name);
        if (it == apps.end() || it->second != runner) {
            runner->close_soon();
        }
    }
    apps_ = std::move(apps);
}

json WebService::reload() {
    const std::lock_guard lock(mutex_);
    reload_locked();
    return status_locked();
}

json WebService::status() const {
    const std::lock_guard lock(mutex_);
    return status_locked();
}

// Runs nginx once in the web folder (-t, -s reload, -s stop) and waits for
// it. Paths on its command line are relative to that folder: nginx for
// Windows reads its arguments in the ANSI code page, which can't hold every
// user folder name; the folder itself is passed as a wide string.
std::optional<int> WebService::run_nginx(std::vector<std::string> args, const fs::path& log, std::string& error) const {
    const auto program = nginx_program(nginx_dir_);
    if (!program) {
        error = "nginx is missing from " + path_to_utf8(nginx_dir_);
        return std::nullopt;
    }
    std::error_code ec;
    fs::remove(log, ec);
    ChildOptions o;
    o.program = *program;
    o.args = {"-p", "./"};
    o.args.insert(o.args.end(), args.begin(), args.end());
    o.working_dir = work_;
    o.output_log = log;
    auto child = ChildProcess::start(o, error);
    if (!child) {
        return std::nullopt;
    }
    const auto code = child->wait(15s);
    if (!code) {
        error = "nginx " + args.back() + " didn't finish";
    }
    return code;
}

std::optional<std::string> WebService::test_nginx_config(const std::string& conf_name, std::string& error) const {
    const fs::path log = work_ / "logs" / "nginx-test.log";
    const auto code = run_nginx({"-t", "-q", "-c", conf_name}, log, error);
    if (!code) {
        return std::nullopt;
    }
    if (*code != 0) {
        error = tail_of(log);
        if (error.empty()) {
            error = "nginx -t failed (exit " + std::to_string(*code) + ")";
        }
        return std::nullopt;
    }
    return std::string();
}

bool WebService::start_nginx_locked(std::string& error) {
    const auto program = nginx_program(nginx_dir_);
    if (!program) {
        error = "nginx is missing from " + path_to_utf8(nginx_dir_);
        return false;
    }
    std::error_code ec;
    fs::create_directories(work_ / "logs", ec);
    fs::create_directories(work_ / "temp", ec);
    stop_leftover_nginx();
    ChildOptions o;
    o.program = *program;
    o.args = {"-p", "./", "-c", "nginx.conf"};
#ifndef _WIN32
    o.args.insert(o.args.end(), {"-g", "daemon off;"});  // stays our child, in our process group
#endif
    o.working_dir = work_;
    o.output_log = work_ / "logs" / "nginx-output.log";
    nginx_ = ChildProcess::start(o, error);
    if (!nginx_) {
        return false;
    }
    // A config that passed -t can still fail to start (a port taken since).
    if (nginx_->wait(500ms)) {
        nginx_.reset();
        error = tail_of(work_ / "logs" / "nginx-error.log");
        if (error.empty()) {
            error = tail_of(work_ / "logs" / "nginx-output.log");
        }
        if (error.empty()) {
            error = "nginx stopped right after starting";
        }
        return false;
    }
    return true;
}

// An nginx left from a service that didn't stop cleanly (a crash) holds the
// ports. Its pid file names it; it's stopped only when that pid really is our
// nginx, and the start waits until it's gone.
void WebService::stop_leftover_nginx() const {
    const fs::path pid_file = work_ / "nginx.pid";
    std::error_code ec;
    const auto text = read_small_file(pid_file, 64);
    if (!text) {
        return;
    }
    std::uint32_t pid = 0;
    try {
        pid = static_cast<std::uint32_t>(std::stoul(*text));
    } catch (const std::exception&) {
        fs::remove(pid_file, ec);
        return;
    }
    if (pid != 0 && process_is_program(pid, nginx_program(nginx_dir_).value_or(fs::path()))) {
        std::string ignored;
        (void)run_nginx({"-c", "nginx.conf", "-s", "stop"}, work_ / "logs" / "nginx-signal.log", ignored);
        (void)wait_for_process_exit(pid, 10s);
    }
    fs::remove(pid_file, ec);
}

void WebService::stop_nginx_locked() {
    nginx_.reset();  // ends the master and its worker
    std::error_code ec;
    fs::remove(work_ / "nginx.pid", ec);
    config_text_.clear();
    ports_.reset();
}

void WebService::reload_locked() {
    if (stopped_) {
        return;
    }
    error_.clear();
    std::string error;
    const auto app = load_app_config(app_config_file(config_.data_dir), error);
    if (!app) {
        error_ = "the project list can't be read: " + error;  // keep running what runs
        return;
    }
    const auto installed = list_installed(runtimes_dir(config_.data_dir));
    WebPlan plan = plan_sites(*app, read_project_config_file, installed, std::nullopt);

    const InstalledRuntime* nginx = newest(installed, "nginx");
    nginx_dir_ = nginx != nullptr ? nginx->dir : fs::path();
    nginx_version_ = nginx != nullptr ? nginx->version : std::string();

    // Ports: pools keep theirs; new pools get free ones.
    std::map<std::string, std::vector<Port>> previous;
    for (const auto& [key, runner] : pools_) {
        previous[key] = runner->pool().ports;
    }
    if (!assign_pool_ports(plan, previous, config_.probe, config_.php_workers)) {
        error_ = "there's no free port for PHP from 9100 upwards";
        return;
    }
    // The shared Apache (D6), for the projects that ask for it: one more
    // app, under a name no project can have, PHP from the same pools.
    std::string apache_error;
    {
        ApacheSpec as;
        bool keep = false;
        for (const PlannedSite& p : plan.sites) {
            if (p.state == SiteState::Ready && p.apache) {
                as.sites.push_back({p.name, p.doc_root, plan.pools.at(*p.pool).ports});
                const Project* project = find_project(*app, p.name);
                keep = keep || (project != nullptr && project->keep_running);
            }
        }
        if (!as.sites.empty()) {
            as.server_root = apache_server_root(plan.apache_dir);
            as.modules_dir = as.server_root / "modules";
            as.work_dir = work_;
#ifndef _WIN32
            as.posix = true;
#endif
            auto text = make_apache_config(as, apache_error);
            if (text) {
                if (running_as_root()) {
                    *text += "User #65534\nGroup #65534\n";  // Apache won't serve as root (containers)
                }
                AppSpec spec;
                spec.name = std::string(kApacheAppName);
                spec.kind = AppKind::Apache;
                spec.dir = work_ / "apache";
                spec.version = plan.apache_version;
                spec.runtime_dir = plan.apache_dir;
                spec.keep_running = keep;
                spec.server_config = std::move(*text);
                plan.apps.push_back(std::move(spec));
            }
        }
    }
    std::map<std::string, Port> previous_apps;
    for (const auto& [name, runner] : apps_) {
        previous_apps[name] = runner->listener().port();
    }
    if (!assign_app_ports(plan, previous_apps, config_.probe)) {
        error_ = "there's no free port for projects' own servers from 9300 upwards";
        return;
    }

    // The new pools and sites are built aside. They replace the current ones
    // only once nginx runs a config that uses them: until then the running
    // config may still send requests to pools the new plan drops.
    std::map<std::string, Site> sites;
    std::map<std::string, std::string> pool_errors;
    std::map<std::string, std::shared_ptr<PhpPoolRunner>> pools;
    for (const PhpPool& pool : plan.pools) {
        if (const auto it = pools_.find(pool.key); it != pools_.end()) {
            it->second->set_pool(pool);
            pools[pool.key] = it->second;
            continue;
        }
        std::string why;
        auto runner = PhpPoolRunner::open(pool, work_, why);
        if (!runner) {
            pool_errors[pool.key] = why;
            continue;
        }
        if (pool.keep_running) {
            runner->wake(Clock::now());
        }
        pools[pool.key] = std::move(runner);
    }
    std::map<std::string, std::string> app_errors;
    std::map<std::string, std::shared_ptr<AppRunner>> apps;
    for (const AppSpec& spec : plan.apps) {
        if (const auto it = apps_.find(spec.name); it != apps_.end() && it->second->listener().port() == spec.port) {
            it->second->set_app(spec);
            apps[spec.name] = it->second;
            continue;
        }
        std::string why;
        auto runner = AppRunner::open(spec, {work_, app_internal_ports_, config_.probe}, why);
        if (!runner) {
            app_errors[spec.name] = why;
            continue;
        }
        apps[spec.name] = std::move(runner);
    }
    // On any failure below: keep every pool (old and new) and, while nginx
    // runs, the sites it serves; new sites get no address yet.
    const auto keep_both = [this, &pools, &apps, &sites] {
        for (auto& [key, runner] : pools) {
            pools_.try_emplace(key, runner);
        }
        for (auto& [name, runner] : apps) {
            apps_.try_emplace(name, runner);
        }
        if (!nginx_) {
            for (auto& [name, site] : sites) {
                site.address.clear();
            }
            sites_ = std::move(sites);
        }
    };

    std::vector<NginxSite> serve;
    const auto ca = certs_.load_ca(error);
    tls_ = ca.has_value() && ca_trust_state(ca->der) != TrustState::NotTrusted;
    for (const PlannedSite& p : plan.sites) {
        if (p.state != SiteState::Ready) {
            continue;
        }
        Site site;
        if (nginx == nullptr) {
            site.state_override = "needs_download";
            site.missing = {"nginx"};
            sites[p.name] = site;
            continue;
        }
        std::optional<NginxTls> site_tls;
        if (tls_) {
            std::string cert_error;
            if (const auto cert = certs_.ensure_site(p.name, cert_error)) {
                site_tls = NginxTls{cert->certificate, cert->key};
            } else {
                error_ = "no certificate for " + p.name + ": " + cert_error;
            }
        }
        if (p.apache) {
            const auto spec = std::find_if(plan.apps.begin(), plan.apps.end(),
                                           [](const AppSpec& a) { return a.name == kApacheAppName; });
            const PhpPool& pool = plan.pools.at(*p.pool);
            std::string why = apache_error;
            if (why.empty() && spec != plan.apps.end()) {
                if (const auto e = app_errors.find(spec->name); e != app_errors.end()) {
                    why = e->second;
                }
            }
            if (const auto e = pool_errors.find(pool.key); why.empty() && e != pool_errors.end()) {
                why = e->second;
            }
            if (!why.empty() || spec == plan.apps.end()) {
                site.state_override = "problem";
                site.problem = why.empty() ? std::string("Apache can't be set up") : why;
                sites[p.name] = site;
                continue;
            }
            // nginx forwards everything to Apache and logs each request to the
            // pool's idle log, so the pool doesn't sleep while Apache uses it.
            NginxSite ns;
            ns.name = p.name;
            ns.doc_root = p.doc_root;
            ns.app_port = spec->port;
            ns.php_access_log = pools.at(pool.key)->access_log();
            ns.tls = site_tls;
            site.app = true;
            site.app_kind = "apache";
            site.app_name = spec->name;
            site.address = ns.tls ? "https" : "http";
            serve.push_back(std::move(ns));
            sites[p.name] = site;
            continue;
        }
        if (p.app) {
            const AppSpec& spec = plan.apps.at(*p.app);
            if (const auto e = app_errors.find(spec.name); e != app_errors.end()) {
                site.state_override = "problem";
                site.problem = e->second;
                sites[p.name] = site;
                continue;
            }
            NginxSite ns;
            ns.name = p.name;
            ns.doc_root = p.doc_root;
            ns.app_port = spec.port;
            ns.tls = site_tls;
            site.app = true;
            site.app_kind = spec.kind == AppKind::Python ? "python" : "node";
            site.app_name = spec.name;
            site.address = ns.tls ? "https" : "http";
            serve.push_back(std::move(ns));
            sites[p.name] = site;
            continue;
        }
        const PhpPool& pool = plan.pools.at(*p.pool);
        if (const auto e = pool_errors.find(pool.key); e != pool_errors.end()) {
            site.state_override = "problem";
            site.problem = e->second;
            sites[p.name] = site;
            continue;
        }
        const auto& runner = pools.at(pool.key);
        NginxSite ns;
        ns.name = p.name;
        ns.doc_root = p.doc_root;
        ns.php_ports = pool.ports;
        ns.php_access_log = runner->access_log();
        ns.tls = site_tls;
        site.pool_key = pool.key;
        site.address = ns.tls ? "https" : "http";  // filled in with the port below
        serve.push_back(std::move(ns));
        sites[p.name] = site;
    }

    if (serve.empty()) {
        stop_nginx_locked();  // nothing to serve: free the ports and the memory
        pools_ = std::move(pools);
        replace_apps_locked(std::move(apps));
        sites_ = std::move(sites);
        return;
    }
    if (nginx_ && !nginx_->running()) {
        nginx_.reset();
        config_text_.clear();
    }
    if (!nginx_ && Clock::now() < nginx_retry_after_) {
        keep_both();  // it failed a moment ago; the supervisor tries again
        return;
    }
    if (!ports_) {
        ports_ = config_.fixed_ports ? config_.fixed_ports : pick_web_ports(config_.probe);
        if (!ports_) {
            error_ = "ports 80/443 and 8080-8099/8443-8462 are all taken by other programs";
            keep_both();
            return;
        }
    }
    for (auto& [name, site] : sites) {
        if (!site.address.empty()) {
            site.address = site_address(name, *ports_, site.address == "https");
        }
    }

    NginxSpec spec;
    spec.nginx_dir = nginx_dir_;
    spec.work_dir = work_;
    spec.ports = *ports_;
    spec.ipv6 = loopback_ipv6_available();
    const auto v = Version::parse(nginx_version_);
    const auto http2_on = Version::parse("1.25.1");
    spec.http2_directive = !v || !http2_on || !(*v < *http2_on);
    spec.user = running_as_root() ? "root" : "";
    spec.sites = std::move(serve);
    // The id comes from the config itself, so the same config has the same
    // id and an unchanged one still compares equal.
    auto text = make_nginx_config(spec, error);
    if (text) {
        spec.config_id = to_hex(sha256(*text)).substr(0, 16);
        text = make_nginx_config(spec, error);
    }
    if (!text) {
        error_ = error;
        keep_both();
        return;
    }
    if (*text == config_text_ && nginx_) {
        pools_ = std::move(pools);  // nothing changed for nginx
        replace_apps_locked(std::move(apps));
        sites_ = std::move(sites);
        return;
    }
    if (*text == rejected_text_) {
        error_ = rejected_error_;  // already refused; don't ask nginx again every few seconds
        keep_both();
        return;
    }

    std::error_code ec;
    fs::create_directories(work_ / "logs", ec);
    fs::create_directories(work_ / "temp", ec);
    if (!write_file_atomically(work_ / "nginx.next.conf", *text, error)) {
        error_ = error;
        keep_both();
        return;
    }
    if (!test_nginx_config("nginx.next.conf", error)) {
        rejected_text_ = *text;
        rejected_error_ = "nginx refused the new settings, so the old ones stay: " + error;
        error_ = rejected_error_;
        keep_both();
        return;
    }
    rejected_text_.clear();
    fs::rename(work_ / "nginx.next.conf", work_ / "nginx.conf", ec);
    if (ec) {
        error_ = "can't replace nginx.conf: " + error_text(ec);
        keep_both();
        return;
    }
    if (nginx_) {
        const auto code = run_nginx({"-c", "nginx.conf", "-s", "reload"}, work_ / "logs" / "nginx-signal.log", error);
        if (!code || *code != 0) {
            nginx_error_ = "nginx didn't reload: " +
                           (error.empty() ? tail_of(work_ / "logs" / "nginx-signal.log") : error);
            keep_both();
            return;
        }
        nginx_error_.clear();
    } else if (!start_nginx_locked(error)) {
        nginx_error_ = error;
        nginx_retry_after_ = Clock::now() + kNginxRetry;
        ports_.reset();  // perhaps taken since: choose again next time
        keep_both();
        return;
    } else {
        nginx_error_.clear();
        nginx_started_ = Clock::now();
    }
    // -s reload returns before nginx has switched over: new workers start,
    // then the old ones stop taking connections. Reply only once the new
    // config answers several times in a row, so an address shown by
    // `flo start` works at once.
    const auto deadline = Clock::now() + 5s;
    for (int in_a_row = 0; in_a_row < 5 && Clock::now() < deadline;) {
        const bool current =
            loopback_http_get(ports_->http, "floserver-check.invalid", "/.floserver/config-id", 1s) == spec.config_id;
        in_a_row = current ? in_a_row + 1 : 0;
        std::this_thread::sleep_for(10ms);
    }
    config_text_ = std::move(*text);
    pools_ = std::move(pools);  // pools the new config no longer uses end here
    replace_apps_locked(std::move(apps));
    sites_ = std::move(sites);
}

void WebService::supervise() {
    auto last_tick = Clock::now();
    auto last_reload = Clock::now();
    while (!stopping_) {
        // Pools with no php-cgi running: a waiting connection wakes them (or,
        // for a pool that can't run, is refused at once).
        // Apps: every connection is taken here and relayed.
        std::vector<std::shared_ptr<PhpPoolRunner>> watched;
        std::vector<std::shared_ptr<AppRunner>> apps;
        {
            const std::lock_guard lock(mutex_);
            for (const auto& [key, runner] : pools_) {
                if (runner->state() != PoolState::Running) {
                    watched.push_back(runner);
                }
            }
            for (const auto& [name, runner] : apps_) {
                apps.push_back(runner);
            }
        }
        std::vector<const LoopbackListener*> listeners;
        std::vector<std::size_t> owner;  // listener -> index in watched
        for (std::size_t w = 0; w < watched.size(); ++w) {
            for (const LoopbackListener* l : watched[w]->listeners()) {
                listeners.push_back(l);
                owner.push_back(w);
            }
        }
        const std::size_t first_app = listeners.size();
        for (const auto& a : apps) {
            listeners.push_back(&a->listener());
        }
        const auto ready = waiting_connections(listeners, 200ms);
        if (stopping_) {
            break;
        }
        for (const std::size_t i : ready) {
            if (i >= first_app) {
                apps[i - first_app]->accept_waiting();  // a closed app answers that it's off
            }
        }
        const auto now = Clock::now();
        const std::lock_guard lock(mutex_);
        for (const std::size_t i : ready) {
            if (i >= first_app) {
                continue;
            }
            const auto& r = watched[owner[i]];
            const auto current = pools_.find(r->pool().key);
            if (current == pools_.end() || current->second != r || r->state() == PoolState::Running) {
                continue;  // dropped by a reload meanwhile, or already woken
            }
            if (r->wants_wake_on_connection(now)) {
                r->wake(now);  // every worker: nginx spreads requests over all of them
            } else {
                listeners[i]->drop_waiting_connection();
            }
        }
        if (now - last_tick >= 1s) {
            last_tick = now;
            for (const auto& [key, runner] : pools_) {
                runner->tick(now, config_.idle_after);
            }
            for (const auto& [name, runner] : apps_) {
                runner->tick(now, config_.idle_after);
            }
            if (nginx_ && !nginx_->running()) {
                nginx_error_ = "nginx stopped: " + tail_of(work_ / "logs" / "nginx-error.log");
                nginx_.reset();
                config_text_.clear();
                // Started again with the same ports, after a wait that grows
                // while it keeps ending (2 s, 4 s ... 60 s); one that ran for
                // a minute starts the count again.
                nginx_failures_ = now - nginx_started_ > 60s ? 1 : std::min(nginx_failures_ + 1, 5);
                nginx_retry_after_ = now + std::chrono::seconds(std::min(60, 1 << nginx_failures_));
            }
        }
        if (now - last_reload >= config_.reload_every) {
            last_reload = now;
            reload_locked();
        }
    }
}

json WebService::status_locked() const {
    json pools = json::array();
    for (const auto& [key, r] : pools_) {
        json p = {{"key", key},
                  {"version", r->pool().version},
                  {"ports", r->pool().ports},
                  {"state", pool_state_name(r->state())},
                  {"sites", r->pool().sites},
                  {"keep_running", r->pool().keep_running}};
        if (const auto pid = r->pid()) {
            p["pid"] = *pid;
            p["pids"] = r->pids();
        }
        if (const auto last = r->last_request(); last && r->state() == PoolState::Running) {
            p["idle_s"] = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - *last).count();
        }
        if (!r->last_error().empty()) {
            p["error"] = r->last_error();
        }
        pools.push_back(std::move(p));
    }
    json apps = json::object();
    for (const auto& [name, r] : apps_) {
        apps[name] = r->describe();
    }
    json sites = json::object();
    for (const auto& [name, s] : sites_) {
        json j = json::object();
        if (!s.address.empty()) {
            j["address"] = s.address;
        }
        if (s.app) {
            j["app"] = s.app_kind;
            if (apps.contains(s.app_name)) {
                j["run"] = apps[s.app_name]["state"];
                if (apps[s.app_name].contains("error")) {
                    j["error"] = apps[s.app_name]["error"];
                }
            } else {
                j["run"] = "sleeping";
            }
        }
        if (s.pool_key) {
            j["pool"] = *s.pool_key;
            const auto it = pools_.find(*s.pool_key);
            j["run"] = it != pools_.end() ? pool_state_name(it->second->state()) : "sleeping";
        }
        if (!s.state_override.empty()) {
            j["state"] = s.state_override;
        }
        if (!s.missing.empty()) {
            j["missing"] = s.missing;
        }
        if (!s.problem.empty()) {
            j["problem"] = s.problem;
        }
        sites[name] = std::move(j);
    }
    json nginx = {{"running", nginx_ != nullptr}, {"version", nginx_version_}, {"tls", tls_}};
    if (nginx_) {
        nginx["pid"] = nginx_->pid();
    }
    if (ports_) {
        nginx["http_port"] = ports_->http;
        nginx["https_port"] = ports_->https;
    }
    if (!nginx_error_.empty()) {
        nginx["error"] = nginx_error_;
    }
    json out = {{"nginx", nginx}, {"pools", pools}, {"apps", apps}, {"sites", sites}};
    if (!error_.empty()) {
        out["error"] = error_;
    }
    return out;
}

}  // namespace floserver::core

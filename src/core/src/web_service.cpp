#include "floserver/core/web_service.h"

#include "floserver/core/nginx_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/trust_store.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <system_error>

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

// Paths for nginx's command line, with '/' (nginx for Windows reads both, but
// its prefix must end in '/').
std::string nginx_arg(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return {u.begin(), u.end()};
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
    stop_locked();
}

void WebService::stop_locked() {
    stop_nginx_locked();
    pools_.clear();
    sites_.clear();
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

std::optional<std::string> WebService::test_nginx_config(const fs::path& conf, std::string& error) const {
    const auto program = nginx_program(nginx_dir_);
    if (!program) {
        error = "nginx is missing from " + path_to_utf8(nginx_dir_);
        return std::nullopt;
    }
    const fs::path log = work_ / "logs" / "nginx-test.log";
    std::error_code ec;
    fs::remove(log, ec);
    ChildOptions o;
    o.program = *program;
    o.args = {"-t", "-q", "-p", nginx_arg(work_) + "/", "-c", nginx_arg(conf)};
    o.working_dir = work_;
    o.output_log = log;
    auto child = ChildProcess::start(o, error);
    if (!child) {
        return std::nullopt;
    }
    const auto code = child->wait(15s);
    if (!code) {
        error = "nginx -t didn't finish";
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
    const fs::path conf = work_ / "nginx.conf";
    const std::string prefix = nginx_arg(work_) + "/";
    // An nginx left from a service that didn't stop cleanly holds the ports.
    if (fs::exists(work_ / "nginx.pid", ec)) {
        std::string ignored;
        (void)run_and_wait(*program, {"-p", prefix, "-c", nginx_arg(conf), "-s", "stop"}, 10s, ignored);
        fs::remove(work_ / "nginx.pid", ec);
    }
    ChildOptions o;
    o.program = *program;
    o.args = {"-p", prefix, "-c", nginx_arg(conf)};
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

void WebService::stop_nginx_locked() {
    nginx_.reset();  // ends the master and its worker
    std::error_code ec;
    fs::remove(work_ / "nginx.pid", ec);
    config_text_.clear();
    ports_.reset();
}

void WebService::reload_locked() {
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
    std::map<std::string, Port> previous;
    for (const auto& [key, runner] : pools_) {
        previous[key] = runner->pool().port;
    }
    if (!assign_pool_ports(plan, previous, config_.probe)) {
        error_ = "there's no free port for PHP from 9100 upwards";
        return;
    }

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
    pools_ = std::move(pools);  // pools no longer wanted end here (or when the supervisor lets go)

    std::vector<NginxSite> serve;
    const auto ca = certs_.load_ca(error);
    tls_ = ca.has_value() && ca_trust_state(ca->der) != TrustState::NotTrusted;
    for (const PlannedSite& p : plan.sites) {
        if (p.state != SiteState::Ready) {
            continue;
        }
        Site site;
        const PhpPool& pool = plan.pools.at(*p.pool);
        if (nginx == nullptr) {
            site.state_override = "needs_download";
            site.missing = {"nginx"};
            sites[p.name] = site;
            continue;
        }
        if (const auto e = pool_errors.find(pool.key); e != pool_errors.end()) {
            site.state_override = "problem";
            site.problem = e->second;
            sites[p.name] = site;
            continue;
        }
        const auto& runner = pools_.at(pool.key);
        NginxSite ns{p.name, p.doc_root, pool.port, runner->access_log(), std::nullopt};
        if (tls_) {
            std::string cert_error;
            if (const auto cert = certs_.ensure_site(p.name, cert_error)) {
                ns.tls = NginxTls{cert->certificate, cert->key};
            } else {
                error_ = "no certificate for " + p.name + ": " + cert_error;
            }
        }
        site.pool_key = pool.key;
        site.address = ns.tls ? "https" : "http";  // filled in with the port below
        serve.push_back(std::move(ns));
        sites[p.name] = site;
    }
    sites_ = std::move(sites);

    if (serve.empty()) {
        stop_nginx_locked();  // nothing to serve: free the ports and the memory
        return;
    }
    if (nginx_ && !nginx_->running()) {
        nginx_.reset();
        config_text_.clear();
        ports_.reset();
    }
    if (!nginx_ && Clock::now() < nginx_retry_after_) {
        return;  // it failed a moment ago; the supervisor tries again
    }
    if (!ports_) {
        ports_ = config_.fixed_ports ? config_.fixed_ports : pick_web_ports(config_.probe);
        if (!ports_) {
            error_ = "ports 80/443 and 8080-8099/8443-8462 are all taken by other programs";
            return;
        }
    }
    for (auto& [name, site] : sites_) {
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
    auto text = make_nginx_config(spec, error);
    if (!text) {
        error_ = error;
        return;
    }
    if (*text == config_text_ && nginx_) {
        return;  // nothing changed
    }

    std::error_code ec;
    fs::create_directories(work_ / "logs", ec);
    fs::create_directories(work_ / "temp", ec);
    const fs::path next = work_ / "nginx.next.conf";
    if (!write_file_atomically(next, *text, error)) {
        error_ = error;
        return;
    }
    if (!test_nginx_config(next, error)) {
        error_ = "nginx refused the new settings, so the old ones stay: " + error;
        return;
    }
    fs::rename(next, work_ / "nginx.conf", ec);
    if (ec) {
        error_ = "can't replace nginx.conf: " + error_text(ec);
        return;
    }
    if (nginx_) {
        const auto program = nginx_program(nginx_dir_);
        const auto code = run_and_wait(*program,
                                       {"-p", nginx_arg(work_) + "/", "-c", nginx_arg(work_ / "nginx.conf"), "-s", "reload"},
                                       15s, error);
        if (!code || *code != 0) {
            nginx_error_ = "nginx didn't reload: " + (error.empty() ? "exit " + std::to_string(code.value_or(-1)) : error);
        } else {
            nginx_error_.clear();
        }
    } else if (!start_nginx_locked(error)) {
        nginx_error_ = error;
        nginx_retry_after_ = Clock::now() + kNginxRetry;
        ports_.reset();
        return;
    } else {
        nginx_error_.clear();
    }
    config_text_ = std::move(*text);
}

void WebService::supervise() {
    auto last_tick = Clock::now();
    auto last_reload = Clock::now();
    while (!stopping_) {
        // Pools with no php-cgi running: a waiting connection wakes them (or,
        // for a pool that can't run, is refused at once).
        std::vector<std::shared_ptr<PhpPoolRunner>> watched;
        {
            const std::lock_guard lock(mutex_);
            for (const auto& [key, runner] : pools_) {
                if (runner->state() != PoolState::Running) {
                    watched.push_back(runner);
                }
            }
        }
        std::vector<const LoopbackListener*> listeners;
        listeners.reserve(watched.size());
        for (const auto& r : watched) {
            listeners.push_back(&r->listener());
        }
        const auto ready = waiting_connections(listeners, 200ms);
        if (stopping_) {
            break;
        }
        const auto now = Clock::now();
        const std::lock_guard lock(mutex_);
        for (const std::size_t i : ready) {
            const auto& r = watched[i];
            if (r->state() == PoolState::Running) {
                continue;
            }
            if (r->wants_wake_on_connection(now)) {
                r->wake(now);
            } else {
                r->listener().drop_waiting_connection();
            }
        }
        if (now - last_tick >= 1s) {
            last_tick = now;
            for (const auto& [key, runner] : pools_) {
                runner->tick(now, config_.idle_after);
            }
            if (nginx_ && !nginx_->running()) {
                nginx_error_ = "nginx stopped: " + tail_of(work_ / "logs" / "nginx-error.log");
                nginx_.reset();
                config_text_.clear();
                ports_.reset();
                last_reload = now - config_.reload_every;  // start it again now
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
                  {"port", r->pool().port},
                  {"state", pool_state_name(r->state())},
                  {"sites", r->pool().sites},
                  {"keep_running", r->pool().keep_running}};
        if (const auto pid = r->pid()) {
            p["pid"] = *pid;
        }
        if (const auto last = r->last_request(); last && r->state() == PoolState::Running) {
            p["idle_s"] = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - *last).count();
        }
        if (!r->last_error().empty()) {
            p["error"] = r->last_error();
        }
        pools.push_back(std::move(p));
    }
    json sites = json::object();
    for (const auto& [name, s] : sites_) {
        json j = json::object();
        if (!s.address.empty()) {
            j["address"] = s.address;
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
    json out = {{"nginx", nginx}, {"pools", pools}, {"sites", sites}};
    if (!error_.empty()) {
        out["error"] = error_;
    }
    return out;
}

}  // namespace floserver::core

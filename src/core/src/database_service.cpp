#include "floserver/core/database_service.h"

#include "floserver/core/crypto.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/site_plan.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <condition_variable>
#include <random>
#include <set>
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

constexpr int kMaxConnections = 256;
constexpr auto kFailedRetry = 30s;

bool running_as_root() {
#ifdef _WIN32
    return false;
#else
    return geteuid() == 0;
#endif
}

std::string tail(const fs::path& file, std::size_t max = 1500) {
    auto text = read_small_file(file, std::size_t{4} * 1024 * 1024).value_or("");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text.size() > max ? "..." + text.substr(text.size() - max) : text;
}

// The newest installed MariaDB matching `version` ("11.4").
const InstalledRuntime* best_mariadb(const std::vector<InstalledRuntime>& installed, const std::string& version) {
    const auto want = Version::parse(version);
    const InstalledRuntime* best = nullptr;
    std::optional<Version> best_v;
    for (const InstalledRuntime& r : installed) {
        const auto v = Version::parse(r.version);
        if (r.name == "mariadb" && v && want && want->matches(*v) && (!best_v || *v > *best_v)) {
            best = &r;
            best_v = v;
        }
    }
    return best;
}

// Overwrites a file holding a secret before removing it.
void remove_secret_file(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (!ec && size > 0) {
        const std::string zeros(static_cast<std::size_t>(size), '\0');
        std::string ignored;
        (void)write_private_file_atomically(file, zeros, ignored);
    }
    fs::remove(file, ec);
}

}  // namespace

// ------------------------------------------------------------------ server

class DatabaseService::Server {
public:
    enum class State : std::uint8_t { Sleeping, Starting, Running, Failed };

    Server(MariaDbFiles files, std::string version, Port port, Port internal, std::unique_ptr<LoopbackListener> listener,
           const SecretStore& secrets, std::chrono::seconds start_limit)
        : files_(std::move(files)),
          version_(std::move(version)),
          port_(port),
          internal_(internal),
          listener_(std::move(listener)),
          secrets_(secrets),
          start_limit_(start_limit) {}
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;
    ~Server() { stop(); }

    [[nodiscard]] const MariaDbFiles& files() const { return files_; }
    [[nodiscard]] Port port() const { return port_; }
    [[nodiscard]] Port internal_port() const { return internal_; }
    [[nodiscard]] const LoopbackListener& listener() const { return *listener_; }
    void set_projects(std::vector<std::string> projects, bool keep_running) {
        const std::lock_guard lock(mutex_);
        projects_ = std::move(projects);
        keep_running_ = keep_running;
    }

    // Starts mariadbd unless it runs; waits for it. False (with `error`)
    // when it can't run.
    bool ensure_running(std::string& error) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return state_ != State::Starting; });
        if (state_ == State::Running && process_ && process_->running()) {
            return true;
        }
        if (state_ == State::Failed && Clock::now() < retry_after_) {
            error = error_;
            return false;
        }
        state_ = State::Starting;
        lock.unlock();
        std::string why;
        auto process = start_server(why);
        lock.lock();
        if (process) {
            process_ = std::move(process);
            state_ = State::Running;
            last_activity_ = Clock::now();
            error_.clear();
        } else {
            state_ = State::Failed;
            error_ = why;
            retry_after_ = Clock::now() + kFailedRetry;
            error = why;
        }
        changed_.notify_all();
        return state_ == State::Running;
    }

    // A clean shutdown, then the process ends for sure.
    void stop() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return state_ != State::Starting; });
        if (!process_) {
            if (state_ == State::Running) {
                state_ = State::Sleeping;
            }
            return;
        }
        if (process_->running()) {
            if (const auto root = secrets_.read(root_name())) {
                std::string ignored;
                (void)run_tool(files_.admin, {"shutdown"}, *root, {}, 30s, ignored);
            }
            (void)process_->wait(30s);
        }
        process_.reset();  // ends it if the shutdown didn't
        state_ = State::Sleeping;
    }

    // SQL as root on the running server.
    bool run_sql(const std::string& sql, std::string& error) {
        const auto root = secrets_.read(root_name());
        if (!root) {
            error = "root's password for MariaDB " + files_.series + " is missing from the secure store";
            return false;
        }
        std::random_device rd;
        const fs::path file = files_.folder / ("query-" + std::to_string(rd()) + ".sql");
        if (!write_private_file_atomically(file, sql, error)) {
            return false;
        }
        const bool ok = run_tool(files_.client, {"--batch"}, *root, file, 60s, error);
        remove_secret_file(file);
        return ok;
    }

    void connection_opened() {
        ++active_;
        touch();
    }
    void connection_closed() {
        --active_;
        touch();
    }
    void touch() {
        const std::lock_guard lock(activity_mutex_);
        last_activity_ = Clock::now();
    }
    [[nodiscard]] int active() const { return active_; }

    // Shuts down after the idle time with no connection open.
    void tick(std::chrono::seconds idle_after) {
        {
            const std::lock_guard lock(mutex_);
            if (state_ == State::Running && process_ && !process_->running()) {
                error_ = "mariadbd stopped: " + tail(files_.error_log, 600);
                process_.reset();
                state_ = State::Sleeping;  // the next connection starts it again
                return;
            }
            if (state_ != State::Running || keep_running_ || active_ > 0) {
                return;
            }
        }
        Clock::time_point last;
        {
            const std::lock_guard lock(activity_mutex_);
            last = last_activity_;
        }
        if (Clock::now() - last >= idle_after) {
            stop();
        }
    }

    [[nodiscard]] json describe() const {
        const std::lock_guard lock(mutex_);
        static constexpr const char* kStates[] = {"sleeping", "starting", "running", "failed"};  // NOLINT
        json j = {{"series", files_.series},
                  {"version", version_},
                  {"port", port_},
                  {"internal_port", internal_},
                  {"state", kStates[static_cast<int>(state_)]},
                  {"connections", active_.load()},
                  {"projects", projects_},
                  {"keep_running", keep_running_}};
        if (process_) {
            j["pid"] = process_->pid();
        }
        if (!error_.empty()) {
            j["error"] = error_;
        }
        return j;
    }

    [[nodiscard]] std::string root_name() const { return "mariadb-" + files_.series + "/root"; }

private:
    // One of MariaDB's programs as a client of this server: TCP to the
    // internal port, as root, the password in MYSQL_PWD only.
    bool run_tool(const fs::path& program, std::vector<std::string> extra, const std::string& password,
                  const fs::path& input, std::chrono::seconds limit, std::string& error) const {
        ChildOptions o;
        o.program = program;
        o.args = {"--no-defaults", "--protocol=TCP", "--host=127.0.0.1", "--port=" + std::to_string(internal_), "--user=root"};
        o.args.insert(o.args.end(), extra.begin(), extra.end());
        o.working_dir = files_.folder;
        o.env = {{"MYSQL_PWD", password}};
        o.remove_env = {"MYSQL_HOST", "MYSQL_TCP_PORT", "MYSQL_UNIX_PORT"};
        o.stdin_file = input;
        const fs::path log = files_.folder / "client.log";
        std::error_code ec;
        fs::remove(log, ec);
        o.output_log = log;
        auto child = ChildProcess::start(o, error);
        if (!child) {
            return false;
        }
        const auto code = child->wait(limit);
        if (code == 0) {
            return true;
        }
        error = path_to_utf8(program.filename()) + ": " +
                (code ? tail(log, 600) : std::string("didn't finish in time"));
        return false;
    }

    // Setting up the data folder (the first time), the config, and
    // mariadbd, waiting until it answers and its pid file names it.
    std::unique_ptr<ChildProcess> start_server(std::string& error) {
        std::error_code ec;
        fs::create_directories(files_.folder, ec);
#ifndef _WIN32
        fs::permissions(files_.folder, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
        if (!fs::is_regular_file(files_.server, ec)) {
            error = "mariadbd is missing from " + path_to_utf8(files_.base_dir);
            return nullptr;
        }
        auto root = secrets_.read(root_name());
        const fs::path marker = files_.folder / "root-password-set";
        if (!mariadb_initialized(files_)) {
            if (!root) {
                root = generate_password();
                if (!root || !secrets_.save(root_name(), *root, error)) {
                    error = "can't keep root's password: " + error;
                    return nullptr;
                }
            }
            if (!set_up(error)) {
                return nullptr;
            }
            fs::remove(marker, ec);
        }
        if (!root) {
            error = "root's password for MariaDB " + files_.series + " is missing from the secure store";
            return nullptr;
        }
        const bool first = !fs::exists(marker, ec);
        // The first start sets root's password from an init file, read
        // before the server takes any connection.
        const fs::path init = files_.folder / "init.sql";
        std::string config = mariadb_config(files_, internal_, running_as_root());
        if (first) {
            if (!write_private_file_atomically(init, root_password_sql(*root), error)) {
                return nullptr;
            }
            const std::u8string u = init.generic_u8string();
            config.insert(config.find("character-set-server"), "init-file=\"" + std::string(u.begin(), u.end()) + "\"\n");
        }
        if (!write_file_atomically(files_.config, config, error)) {
            return nullptr;
        }
        fs::remove(files_.pid_file, ec);
        const auto log_from = fs::exists(files_.error_log, ec) ? fs::file_size(files_.error_log, ec) : 0;

        ChildOptions o;
        o.program = files_.server;
        o.args = {"--defaults-file=" + path_to_utf8(files_.config.filename())};  // relative: the folder is its cwd
        o.working_dir = files_.folder;
        o.output_log = files_.folder / "mariadbd.out";
        auto process = ChildProcess::start(o, error);
        if (!process) {
            if (first) {
                remove_secret_file(init);
            }
            return nullptr;
        }
        // Ready: its pid file names our process (not another server that
        // took the port meanwhile) and root can log in. The port alone isn't
        // enough: MariaDB listens before it has read the init file, so that
        // file must stay until a login with the new password works.
        const fs::path probe = files_.folder / "probe.sql";
        if (!write_private_file_atomically(probe, "SELECT 1;\n", error)) {
            return nullptr;
        }
        const auto deadline = Clock::now() + start_limit_;
        bool ready = false;
        std::string login_error;
        while (Clock::now() < deadline && process->running()) {
            const auto pid = read_small_file(files_.pid_file, 64);
            if (pid && std::to_string(process->pid()) == std::string(pid->substr(0, pid->find_first_of("\r\n")))) {
                if (const auto s = connect_loopback(internal_)) {
                    close_socket(*s);
                    login_error.clear();
                    if (run_tool(files_.client, {"--batch", "--connect-timeout=10"}, *root, probe, 30s, login_error)) {
                        ready = true;
                        break;
                    }
                    // Not first: a wrong password won't put itself right.
                    if (!first && login_error.find("1045") != std::string::npos) {
                        break;
                    }
                }
            }
            std::this_thread::sleep_for(200ms);
        }
        fs::remove(probe, ec);
        if (first) {
            remove_secret_file(init);
            // Next starts without the init file.
            std::string ignored;
            (void)write_file_atomically(files_.config, mariadb_config(files_, internal_, running_as_root()), ignored);
        }
        if (!ready) {
            if (!login_error.empty() && process->running()) {
                error = "root can't log in to MariaDB " + files_.series + ": " + login_error;
                return nullptr;  // ends it
            }
            error = "mariadbd didn't start: " + start_errors(log_from, o.output_log);
            return nullptr;
        }
        if (first) {
            (void)write_file_atomically(marker, "1\n", error);
        }
        return process;
    }

    // What this start wrote to the error log: its [ERROR] lines, or the
    // end of the log when there are none.
    [[nodiscard]] std::string start_errors(std::uintmax_t log_from, const fs::path& output_log) const {
        const auto log = read_small_file(files_.error_log, std::size_t{4} * 1024 * 1024).value_or("");
        const std::string fresh = log.size() > log_from ? log.substr(static_cast<std::size_t>(log_from)) : std::string();
        std::string errors;
        std::size_t pos = 0;
        while (pos < fresh.size()) {
            const auto end = fresh.find('\n', pos);
            const std::string line = fresh.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? fresh.size() : end + 1;
            if (line.find("[ERROR]") != std::string::npos && line.find("Aborting") == std::string::npos) {
                errors.append(errors.empty() ? "" : "\n").append(line);
            }
        }
        if (errors.empty()) {
            return fresh.empty() ? tail(output_log, 800) : tail(files_.error_log, 800);
        }
        return errors.size() > 1500 ? errors.substr(0, 1500) + "..." : errors;
    }

    // mariadb-install-db, once. A folder left half-way is moved aside,
    // never deleted: it may hold someone's databases.
    bool set_up(std::string& error) {
        std::error_code ec;
        if (fs::exists(files_.data_dir, ec)) {
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            fs::path aside = files_.data_dir;
            aside += ".failed-" + std::to_string(stamp);
            fs::rename(files_.data_dir, aside, ec);
            if (ec) {
                error = "can't move the unfinished data folder aside: " + error_text(ec);
                return false;
            }
        }
        ChildOptions o;
        o.program = files_.install_db;
#ifdef _WIN32
        // Relative to its folder: Windows programs read arguments in the
        // ANSI code page. No password here (root gets it from the init file).
        o.args = {"--datadir=data"};
#else
        o.args = {"--no-defaults", "--basedir=" + path_to_utf8(files_.base_dir), "--datadir=" + path_to_utf8(files_.data_dir),
                  "--auth-root-authentication-method=normal", "--skip-test-db", "--force"};
        if (running_as_root()) {
            o.args.emplace_back("--user=root");
        }
#endif
        o.working_dir = files_.folder;
        o.output_log = files_.folder / "install.log";
        auto child = ChildProcess::start(o, error);
        if (!child) {
            return false;
        }
        const auto code = child->wait(start_limit_);
        if (code != 0 || !mariadb_initialized(files_)) {
            error = "setting up MariaDB " + files_.series + " failed: " + tail(o.output_log, 800);
            return false;
        }
        return true;
    }

    MariaDbFiles files_;
    std::string version_;
    Port port_;
    Port internal_;
    std::unique_ptr<LoopbackListener> listener_;
    const SecretStore& secrets_;
    std::chrono::seconds start_limit_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    State state_ = State::Sleeping;
    std::unique_ptr<ChildProcess> process_;
    std::string error_;
    Clock::time_point retry_after_;
    std::vector<std::string> projects_;
    bool keep_running_ = false;

    std::atomic<int> active_{0};
    std::mutex activity_mutex_;
    Clock::time_point last_activity_;
};

// ------------------------------------------------------------------ service

DatabaseService::DatabaseService(DatabaseServiceConfig config, const SecretStore& secrets)
    : config_(std::move(config)), secrets_(secrets) {
    if (!config_.probe) {
        config_.probe = can_listen_on_loopback;
    }
}

DatabaseService::~DatabaseService() { stop(); }

void DatabaseService::start() {
    {
        const std::lock_guard lock(mutex_);
        reload_locked();
    }
    thread_ = std::thread([this] { supervise(); });
}

void DatabaseService::stop() {
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    std::map<std::string, std::shared_ptr<Server>> servers;
    {
        const std::lock_guard lock(mutex_);
        stopped_ = true;
        servers.swap(servers_);
    }
    for (auto& [series, server] : servers) {
        server->stop();
    }
}

json DatabaseService::reload() {
    const std::lock_guard lock(mutex_);
    reload_locked();
    json list = json::array();
    for (const auto& [series, server] : servers_) {
        list.push_back(server->describe());
    }
    return {{"servers", list}};
}

json DatabaseService::status() const {
    std::vector<std::shared_ptr<Server>> servers;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [series, server] : servers_) {
            servers.push_back(server);
        }
    }
    json list = json::array();
    for (const auto& s : servers) {
        list.push_back(s->describe());
    }
    return {{"servers", list}};
}

void DatabaseService::reload_locked() {
    if (stopped_) {
        return;
    }
    std::string error;
    const auto app = load_app_config(app_config_file(config_.data_dir), error);
    if (!app) {
        return;  // keep what runs
    }
    const auto installed = list_installed(runtimes_dir(config_.data_dir));
    const WebPlan plan = plan_sites(*app, read_project_config_file, installed, std::nullopt);

    struct Want {
        const InstalledRuntime* runtime = nullptr;
        std::vector<std::string> projects;
        bool keep_running = false;
    };
    std::map<std::string, Want> wanted;
    for (const PlannedSite& site : plan.sites) {
        if (site.state != SiteState::Ready || !site.database || site.database->family != "mariadb") {
            continue;
        }
        const InstalledRuntime* r = best_mariadb(installed, site.database->version);
        if (r == nullptr) {
            continue;
        }
        const std::string series = mariadb_series(r->version);
        Want& w = wanted[series];
        if (w.runtime == nullptr || Version::parse(r->version) > Version::parse(w.runtime->version)) {
            w.runtime = r;
        }
        w.projects.push_back(site.name);
        if (const Project* p = find_project(*app, site.name)) {
            w.keep_running = w.keep_running || p->keep_running;
        }
    }

    // Servers no longer wanted are shut down cleanly (outside the lock's
    // hot path: the supervisor holds its own reference while relaying).
    for (auto it = servers_.begin(); it != servers_.end();) {
        if (!wanted.contains(it->first)) {
            it->second->stop();
            it = servers_.erase(it);
        } else {
            ++it;
        }
    }
    std::set<Port> taken;
    for (const auto& [series, server] : servers_) {
        taken.insert(server->port());
    }
    for (auto& [series, want] : wanted) {
        if (const auto it = servers_.find(series); it != servers_.end()) {
            it->second->set_projects(want.projects, want.keep_running);
            continue;
        }
        // D12: 3306 when free, else 3307, 3308... never another stack's.
        const auto port = pick_port_from(kFirstDatabasePort, taken, config_.probe);
        if (!port) {
            continue;
        }
        std::set<Port> internal_taken = taken;
        internal_taken.insert(*port);
        const auto internal = pick_port_from(kFirstInternalDatabasePort, internal_taken, config_.probe);
        if (!internal) {
            continue;
        }
        auto listener = LoopbackListener::open(*port, error);
        if (!listener) {
            continue;
        }
        taken.insert(*port);
        taken.insert(*internal);
        auto server = std::make_shared<Server>(mariadb_files(want.runtime->dir, want.runtime->version, config_.data_dir),
                                               want.runtime->version, *port, *internal, std::move(listener), secrets_,
                                               config_.start_limit);
        server->set_projects(want.projects, want.keep_running);
        servers_[series] = std::move(server);
    }
}

void DatabaseService::supervise() {
    auto last_tick = Clock::now();
    auto last_reload = Clock::now();
    while (!stopping_) {
        std::vector<std::shared_ptr<Server>> servers;
        {
            const std::lock_guard lock(mutex_);
            for (const auto& [series, s] : servers_) {
                servers.push_back(s);
            }
        }
        std::vector<const LoopbackListener*> listeners;
        listeners.reserve(servers.size());
        for (const auto& s : servers) {
            listeners.push_back(&s->listener());
        }
        for (const std::size_t i : waiting_connections(listeners, 200ms)) {
            auto client = servers[i]->listener().accept_connection();
            if (!client) {
                continue;
            }
            if (servers[i]->active() >= kMaxConnections) {
                close_socket(*client);
                continue;
            }
            // Each connection on its own thread: the first one may wait
            // for mariadbd to start; the others queue behind it.
            servers[i]->connection_opened();
            try {
                std::thread([server = servers[i], c = *client] {
                    std::string error;
                    std::optional<std::uintptr_t> upstream;
                    if (server->ensure_running(error)) {
                        upstream = connect_loopback(server->internal_port());
                    }
                    if (upstream) {
                        relay_sockets(c, *upstream, [&server] { server->touch(); });
                    } else {
                        close_socket(c);  // the client sees the connection end
                    }
                    server->connection_closed();
                }).detach();
            } catch (const std::system_error&) {
                servers[i]->connection_closed();
                close_socket(*client);
            }
        }
        if (stopping_) {
            break;
        }
        const auto now = Clock::now();
        if (now - last_tick >= 1s) {
            last_tick = now;
            for (const auto& s : servers) {
                s->tick(config_.idle_after);
            }
        }
        if (now - last_reload >= config_.reload_every) {
            last_reload = now;
            const std::lock_guard lock(mutex_);
            reload_locked();
        }
    }
}

std::optional<DatabaseLogin> DatabaseService::project_login(const std::string& project, ServiceRefusal& refusal) {
    std::string error;
    const auto app = load_app_config(app_config_file(config_.data_dir), error);
    const Project* p = app ? find_project(*app, project) : nullptr;
    if (p == nullptr) {
        refusal = {"unknown_project", "there's no project called " + project};
        return std::nullopt;
    }
    const ConfigResult cfg = read_project_config_file(*p);
    if (!cfg.config || !cfg.config->database) {
        refusal = {"no_database", project + " has no database in its floserver.json (add one with: flo use mariadb 11.4)"};
        return std::nullopt;
    }
    std::shared_ptr<Server> server;
    if (p->on) {
        const auto installed = list_installed(runtimes_dir(config_.data_dir));
        const InstalledRuntime* r = best_mariadb(installed, cfg.config->database->version);
        const std::lock_guard lock(mutex_);
        reload_locked();
        if (r != nullptr) {
            if (const auto it = servers_.find(mariadb_series(r->version)); it != servers_.end()) {
                server = it->second;
            }
        }
    }
    if (!server) {
        refusal = {"database_off", project + "'s database runs while the project is on: flo start " + project};
        return std::nullopt;
    }
    if (!server->ensure_running(error)) {
        refusal = {"database_failed", error};
        return std::nullopt;
    }
    server->touch();
    DatabaseLogin login;
    login.port = server->port();
    login.database = project_database_name(project);
    login.user = login.database;
    const std::string secret = "mariadb-" + server->files().series + "/" + login.database;
    auto password = secrets_.read(secret);
    if (!password) {
        password = generate_password();
        if (!password || !secrets_.save(secret, *password, error)) {
            refusal = {"secret_failed", "can't keep the password: " + error};
            return std::nullopt;
        }
    }
    login.password = *password;
    if (!server->run_sql(project_database_sql(login.database, login.user, login.password), error)) {
        refusal = {"database_failed", error};
        return std::nullopt;
    }
    return login;
}

}  // namespace floserver::core

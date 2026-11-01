#include "floserver/core/database_service.h"

#include "floserver/core/shared_sites.h"

#include "floserver/core/mariadb_settings.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/site_plan.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <condition_variable>
#include <fstream>
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
// First in every SQL FloServer runs as root.
constexpr std::string_view kPlainSqlMode = "SET SESSION sql_mode='';\n";
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

std::string random_suffix() {
    std::random_device rd;
    return std::to_string(rd());
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

// Removes a file that held a secret, overwriting it in place first (best
// effort: an SSD or a copy-on-write file system may keep the old blocks).
void remove_secret_file(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (!ec && size > 0) {
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
        if (f) {
            const std::string zeros(static_cast<std::size_t>(size), '\0');
            f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
            f.flush();
        }
    }
    fs::remove(file, ec);
}

// The public port kept for a series, so .env files stay right.
std::optional<Port> saved_port(const fs::path& folder) {
    const auto text = read_small_file(folder / "port", 32);
    if (!text) {
        return std::nullopt;
    }
    try {
        const auto n = std::stoul(*text);
        return n > 0 && n < 65536 ? std::optional<Port>(static_cast<Port>(n)) : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Runs `f` when the scope ends.
template <class F>
class AtExit {
public:
    explicit AtExit(F f) : f_(std::move(f)) {}
    AtExit(const AtExit&) = delete;
    AtExit& operator=(const AtExit&) = delete;
    AtExit(AtExit&&) = delete;
    AtExit& operator=(AtExit&&) = delete;
    ~AtExit() { f_(); }

private:
    F f_;
};

ServiceRefusal not_installed(const std::string& version) {
    return {"not_installed",
            "MariaDB " + version + " isn't installed: download it in Settings → Runtimes (or: flo install mariadb " +
                version + ")"};
}

// Installed, but its server isn't there yet: its old one is still shutting
// down (a newer patch release, a new port), or no port is free.
ServiceRefusal no_server_yet(const std::string& series) {
    return {"database_off", "MariaDB " + series + " can't take connections yet (its port is busy); try again in a few seconds"};
}

}  // namespace

// The internal ports in use or chosen, across servers.
struct DatabaseService::InternalPorts {
    std::mutex mutex;
    std::set<Port> ports;

    // A port from 33100 up that nothing listens on and no other server has.
    std::optional<Port> pick(std::optional<Port> current, Port public_port, const PortProbe& probe) {
        const std::lock_guard lock(mutex);
        if (current) {
            ports.erase(*current);
        }
        std::set<Port> taken = ports;
        taken.insert(public_port);
        const auto port = pick_port_from(kFirstInternalDatabasePort, taken, probe);
        if (port) {
            ports.insert(*port);
        }
        return port;
    }
    void release(Port port) {
        const std::lock_guard lock(mutex);
        ports.erase(port);
    }
};

// ------------------------------------------------------------------ server

class DatabaseService::Server {
public:
    enum class State : std::uint8_t { Sleeping, Starting, Running, Stopping, Failed };

    Server(fs::path data_dir, MariaDbFiles files, std::string version, Port port, Port internal,
           std::unique_ptr<LoopbackListener> listener, SecretStore secrets, std::shared_ptr<InternalPorts> internal_ports,
           PortProbe probe, std::chrono::seconds start_limit)
        : data_dir_(std::move(data_dir)),
          files_(std::move(files)),
          version_(std::move(version)),
          port_(port),
          internal_(internal),
          listener_(std::move(listener)),
          secrets_(std::move(secrets)),
          internal_ports_(std::move(internal_ports)),
          probe_(std::move(probe)),
          start_limit_(start_limit) {
        sweep_secret_files();
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;
    ~Server() {
        stop(true);
        internal_ports_->release(internal_);
    }

    [[nodiscard]] const MariaDbFiles& files() const { return files_; }
    [[nodiscard]] const std::string& version() const { return version_; }
    [[nodiscard]] Port port() const { return port_; }
    [[nodiscard]] Port internal_port() const { return internal_; }
    [[nodiscard]] const LoopbackListener& listener() const { return *listener_; }
    [[nodiscard]] std::mutex& login_mutex() { return login_mutex_; }
    void set_projects(std::vector<std::string> projects, bool keep_running) {
        const std::lock_guard lock(mutex_);
        projects_ = std::move(projects);
        keep_running_ = keep_running;
    }

    // Starts mariadbd unless it runs; waits for it. False (with `error`)
    // when it can't run, or the server has been retired.
    bool ensure_running(std::string& error) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return state_ != State::Starting && state_ != State::Stopping; });
        if (retired_) {
            error = "this database server was turned off";
            return false;
        }
        if (state_ == State::Running && process_ && process_->running()) {
            lock.unlock();
            touch();
            return true;
        }
        if (state_ == State::Failed && Clock::now() < retry_after_) {
            error = error_;
            return false;
        }
        state_ = State::Starting;
        lock.unlock();
        std::string why;
        bool wrong_root = false;
        auto process = start_server(false, wrong_root, why);
        if (!process && wrong_root) {
            // root's saved password doesn't work (the saved copy was lost or
            // replaced): set a new one through the init file, once.
            why.clear();
            process = start_server(true, wrong_root, why);
        }
        touch();
        lock.lock();
        if (process) {
            process_ = std::move(process);
            state_ = State::Running;
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

    // A clean shutdown; the process ends for sure. `retire`: for good (it
    // won't start again). Waits for a start or stop under way.
    void stop(bool retire) {
        std::unique_lock lock(mutex_);
        if (retire) {
            retired_ = true;
        }
        changed_.wait(lock, [this] { return state_ != State::Starting && state_ != State::Stopping; });
        if (!process_) {
            if (state_ == State::Running) {
                state_ = State::Sleeping;
            }
            return;
        }
        auto process = std::move(process_);
        state_ = State::Stopping;
        lock.unlock();
        shut_down(*process);
        process.reset();
        lock.lock();
        state_ = State::Sleeping;
        changed_.notify_all();
    }

    // The port Settings asked for when it was made; `fallback`: that one
    // was taken, so it's on another.
    void set_chosen_port(std::optional<Port> port, bool fallback) {
        const std::lock_guard lock(mutex_);
        chosen_port_ = port;
        port_fallback_ = fallback;
    }
    [[nodiscard]] std::optional<Port> chosen_port() const {
        const std::lock_guard lock(mutex_);
        return chosen_port_;
    }

    [[nodiscard]] bool running() const {
        const std::lock_guard lock(mutex_);
        return state_ == State::Running || state_ == State::Starting;
    }

    // Stops it cleanly and starts it again (the my.ini is written afresh).
    bool restart(std::string& error) {
        // Counts as a connection: it doesn't go to sleep between the two.
        connection_opened();
        const AtExit closed([this] { connection_closed(); });
        stop(false);
        clear_failure();
        return ensure_running(error);
    }

    // Asked for by the user: a start that failed is tried again now, not in 30 s.
    void clear_failure() {
        const std::lock_guard lock(mutex_);
        if (state_ == State::Failed) {
            state_ = State::Sleeping;
        }
    }

    // A new password for root: set in MariaDB, then kept in the secure
    // store. Kept under "<root>-new" first, so a failure between the two
    // leaves it somewhere (a start whose saved password is refused sets a
    // new one by itself anyway). No shutdown runs meanwhile (it would log in
    // with the old one).
    bool change_root_password(const std::string& password, std::string& error) {
        connection_opened();
        const AtExit closed([this] { connection_closed(); });
        if (!ensure_running(error)) {
            return false;
        }
        // No project's login meanwhile (it runs SQL as root too).
        const std::lock_guard logins(login_mutex_);
        const std::lock_guard admin(admin_mutex_);
        const std::string pending = root_name() + "-new";
        if (!secrets_.save(pending, password, error)) {
            error = "can't keep the new password: " + error;
            return false;
        }
        if (!run_sql(root_password_sql(password), error)) {
            (void)secrets_.remove(pending);
            return false;
        }
        if (!secrets_.save(root_name(), password, error)) {
            error = "MariaDB has the new password, but it couldn't be kept (it's under " + pending + "): " + error;
            return false;
        }
        (void)secrets_.remove(pending);
        return true;
    }

    // The my.ini this server writes when it starts (`first`: with the init
    // file), from Settings > Database as config.json has it now.
    [[nodiscard]] std::string config_text(bool first) const {
        std::string error;
        const auto app = load_app_config(app_config_file(data_dir_), error);
        const MariaDbSettings all = app ? read_mariadb_settings(*app) : MariaDbSettings{};
        const auto it = all.find(files_.series);
        const MariaDbExtras extras = mariadb_extras(data_dir_, files_.series, it != all.end() ? &it->second : nullptr);
        return mariadb_config(files_, internal_, running_as_root(), first, &extras);
    }

    // SQL as root on the running server.
    bool run_sql(const std::string& sql, std::string& error) {
        const auto root = secrets_.read(root_name());
        if (!root) {
            error = "root's password for MariaDB " + files_.series + " is missing from the secure store";
            return false;
        }
        const fs::path file = files_.folder / ("query-" + random_suffix() + ".sql");
        // FloServer's quoting (sql_string) whatever sql_mode my.ini sets
        // (NO_BACKSLASH_ESCAPES would read a password's \ differently).
        if (!write_private_file_atomically(file, std::string(kPlainSqlMode) + sql, error)) {
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
        touch();
        --active_;
    }
    void touch() {
        const std::lock_guard lock(activity_mutex_);
        last_activity_ = Clock::now();
    }
    [[nodiscard]] int active() const { return active_; }

    // Due to sleep: true (and "stopping") when it has run with no
    // connection for the idle time; the caller then calls finish_stop().
    // A server that ended on its own is noted and sleeps.
    bool begin_idle_stop(std::chrono::seconds idle_after) {
        Clock::time_point last;
        {
            const std::lock_guard lock(activity_mutex_);
            last = last_activity_;
        }
        const std::lock_guard lock(mutex_);
        if (state_ != State::Running || !process_) {
            return false;
        }
        if (!process_->running()) {
            error_ = "mariadbd stopped: " + tail(files_.error_log, 600);
            process_.reset();
            state_ = State::Sleeping;  // the next connection starts it again
            return false;
        }
        if (keep_running_ || active_ > 0 || Clock::now() - last < idle_after) {
            return false;
        }
        state_ = State::Stopping;
        stopping_process_ = std::move(process_);
        return true;
    }
    void finish_stop() {
        std::unique_ptr<ChildProcess> process;
        {
            const std::lock_guard lock(mutex_);
            process = std::move(stopping_process_);
        }
        if (process) {
            shut_down(*process);
            process.reset();
        }
        const std::lock_guard lock(mutex_);
        state_ = State::Sleeping;
        changed_.notify_all();
    }

    [[nodiscard]] json describe() const {
        const std::lock_guard lock(mutex_);
        static constexpr const char* kStates[] = {"sleeping", "starting", "running", "stopping", "failed"};  // NOLINT
        json j = {{"series", files_.series},
                  {"version", version_},
                  {"port", port_},
                  {"internal_port", internal_.load()},
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
        if (port_fallback_ && chosen_port_) {
            j["port_wanted"] = *chosen_port_;  // taken by another program
        }
        return j;
    }

    [[nodiscard]] std::string root_name() const { return "mariadb-" + files_.series + "/root"; }

private:
    void shut_down(ChildProcess& process) const {
        // Not while root's password is being changed: the saved one would be refused.
        const std::lock_guard admin(admin_mutex_);
        if (process.running()) {
            if (const auto root = secrets_.read(root_name())) {
                std::string ignored;
                (void)run_tool(files_.admin, {"shutdown"}, *root, {}, 30s, ignored);
            }
            (void)process.wait(30s);
        }
        process.kill();  // if the shutdown didn't end it
    }

    // Secrets a crash may have left: the init file, query files.
    void sweep_secret_files() const {
        std::error_code ec;
        remove_secret_file(files_.init_file);
        for (fs::directory_iterator it(files_.folder, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = path_to_utf8(it->path().filename());
            if (name.ends_with(".sql") && (name.starts_with("query-") || name.starts_with("probe"))) {
                remove_secret_file(it->path());
            }
        }
    }

    // One of MariaDB's programs as a client of this server: TCP to the
    // internal port, as root, the password in MYSQL_PWD only.
    bool run_tool(const fs::path& program, const std::vector<std::string>& extra, const std::string& password,
                  const fs::path& input, std::chrono::seconds limit, std::string& error) const {
        ChildOptions o;
        o.program = program;
        o.args = {"--no-defaults", "--protocol=TCP", "--host=127.0.0.1", "--port=" + std::to_string(internal_.load()),
                  "--user=root"};
        o.args.insert(o.args.end(), extra.begin(), extra.end());
        o.working_dir = files_.folder;
        o.env = {{"MYSQL_PWD", password}};
        o.remove_env = {"MYSQL_HOST", "MYSQL_TCP_PORT", "MYSQL_UNIX_PORT"};
        o.stdin_file = input;
        o.output_log = files_.folder / ("client-" + random_suffix() + ".log");  // one per run: runs may overlap
        auto child = ChildProcess::start(o, error);
        bool ok = false;
        if (child) {
            const auto code = child->wait(limit);
            ok = code == 0;
            if (!ok) {
                error = path_to_utf8(program.filename()) + ": " +
                        (code ? tail(o.output_log, 600) : std::string("didn't finish in time"));
            }
        }
        std::error_code ec;
        fs::remove(o.output_log, ec);
        return ok;
    }

    // Setting up the data folder (the first time), the config, and
    // mariadbd, waiting until root can log in. `reset_root`: a new password
    // for root through the init file. `wrong_root` is set when root's saved
    // password was refused.
    std::unique_ptr<ChildProcess> start_server(bool reset_root, bool& wrong_root, std::string& error) {
        wrong_root = false;
        std::error_code ec;
        fs::create_directories(files_.folder, ec);
#ifndef _WIN32
        fs::permissions(files_.folder, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
        if (!fs::is_regular_file(files_.server, ec)) {
            error = "mariadbd is missing from " + path_to_utf8(files_.base_dir);
            return nullptr;
        }
        const fs::path marker = files_.folder / "root-password-set";
        if (!mariadb_initialized(files_)) {
            if (!set_up(error)) {
                return nullptr;
            }
            fs::remove(marker, ec);
        }
        auto root = reset_root ? std::nullopt : secrets_.read(root_name());
        if (!root) {
            // A new password (the first start, or the saved one is gone).
            root = generate_password();
            if (!root || !secrets_.save(root_name(), *root, error)) {
                error = "can't keep root's password: " + error;
                return nullptr;
            }
            fs::remove(marker, ec);
        }
        const bool first = !fs::exists(marker, ec);

        // The internal port may have been taken since it was chosen.
        if (!probe_(internal_)) {
            const auto port = internal_ports_->pick(internal_.load(), port_, probe_);
            if (!port) {
                error = "no free port for MariaDB " + files_.series;
                return nullptr;
            }
            internal_ = *port;
        }

        // The first start sets root's password from an init file, read
        // before the server takes any connection. It goes as soon as root
        // can log in, and on every way out of here.
        const AtExit remove_init([this, first] {
            if (first) {
                remove_secret_file(files_.init_file);
            }
        });
        if (first) {
            fs::create_directories(files_.data_dir, ec);
            if (!write_private_file_atomically(files_.init_file, std::string(kPlainSqlMode) + root_password_sql(*root),
                                               error)) {
                return nullptr;
            }
        }
        if (!write_file_atomically(files_.config, config_text(first), error)) {
            return nullptr;
        }
        fs::remove(files_.pid_file, ec);
        const auto log_from = fs::exists(files_.error_log, ec) ? fs::file_size(files_.error_log, ec) : 0;

        ChildOptions o;
        o.program = files_.server;
        o.args = {"--defaults-file=" + path_to_utf8(files_.config.filename())};  // relative: the folder is its cwd
        // After it, so they win over anything in the file.
        for (std::string& a : mariadb_command_line_guards(files_, internal_)) {
            o.args.push_back(std::move(a));
        }
        o.working_dir = files_.folder;
        o.output_log = files_.folder / "mariadbd.out";
        auto process = ChildProcess::start(o, error);
        if (!process) {
            return nullptr;
        }
        // Ready: its pid file names our process (not another server that
        // took the port meanwhile) and root can log in. The port alone isn't
        // enough: MariaDB listens before it has read the init file.
        const fs::path probe = files_.folder / ("probe-" + random_suffix() + ".sql");
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
                        // A password change that was set in MariaDB but not
                        // kept as root's: that one, kept now.
                        if (const auto pending = secrets_.read(root_name() + "-new")) {
                            std::string ignored;
                            if (run_tool(files_.client, {"--batch", "--connect-timeout=10"}, *pending, probe, 30s,
                                         ignored) &&
                                secrets_.save(root_name(), *pending, ignored)) {
                                (void)secrets_.remove(root_name() + "-new");
                                ready = true;
                                break;
                            }
                        }
                        wrong_root = true;
                        break;
                    }
                }
            }
            std::this_thread::sleep_for(200ms);
        }
        fs::remove(probe, ec);
        if (first) {
            // Next starts without the init file.
            std::string ignored;
            (void)write_file_atomically(files_.config, config_text(false), ignored);
        }
        if (!ready) {
            if (!login_error.empty() && process->running()) {
                error = "root can't log in to MariaDB " + files_.series + ": " + login_error;
            } else {
                error = "mariadbd didn't start: " + start_errors(log_from, o.output_log);
            }
            shut_down(*process);
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
    // never deleted: it may hold someone's databases. The marker is only
    // written once install-db has finished.
    bool set_up(std::string& error) {
        std::error_code ec;
        fs::remove(files_.installed_marker, ec);
        if (fs::exists(files_.data_dir, ec)) {
            fs::path aside = files_.data_dir;
            aside += ".failed-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
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
        o.args = {"--datadir=" + path_to_utf8(files_.data_dir.filename())};
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
        if (code != 0 || !fs::is_directory(files_.data_dir / "mysql", ec)) {
            child.reset();
            error = "setting up MariaDB " + files_.series + " failed: " + tail(o.output_log, 800);
            return false;
        }
        return write_file_atomically(files_.installed_marker, "1\n", error);
    }

    const fs::path data_dir_;
    const MariaDbFiles files_;
    const std::string version_;
    const Port port_;
    std::atomic<Port> internal_;
    const std::unique_ptr<LoopbackListener> listener_;
    const SecretStore secrets_;
    const std::shared_ptr<InternalPorts> internal_ports_;
    const PortProbe probe_;
    const std::chrono::seconds start_limit_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    State state_ = State::Sleeping;
    bool retired_ = false;
    std::unique_ptr<ChildProcess> process_;
    std::unique_ptr<ChildProcess> stopping_process_;
    std::string error_;
    Clock::time_point retry_after_;
    std::vector<std::string> projects_;
    bool keep_running_ = false;
    std::optional<Port> chosen_port_;
    bool port_fallback_ = false;

    std::atomic<int> active_{0};
    std::mutex activity_mutex_;
    Clock::time_point last_activity_ = Clock::now();
    std::mutex login_mutex_;
    mutable std::mutex admin_mutex_;  // a shutdown waits for a root password change
};

// ------------------------------------------------------------------ service

DatabaseService::DatabaseService(DatabaseServiceConfig config, SecretStore secrets)
    : config_(std::move(config)), secrets_(std::move(secrets)), internal_ports_(std::make_shared<InternalPorts>()) {
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
        server->stop(true);
    }
    reap_stoppers(true);
}

void DatabaseService::run_later(std::function<void()> work) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto shared = std::make_shared<std::function<void()>>(std::move(work));
    const std::lock_guard lock(stoppers_mutex_);
    try {
        stoppers_.push_back({std::thread([shared, done] {
                                 (*shared)();
                                 *done = true;
                             }),
                             done});
    } catch (const std::system_error&) {
        (*shared)();  // no thread: here (slower, but it's done)
    }
}

void DatabaseService::stop_later(std::shared_ptr<Server> server, bool retire) {
    run_later([server = std::move(server), retire] {
        if (retire) {
            server->stop(true);
        } else {
            server->finish_stop();
        }
    });
}

void DatabaseService::retire_later(std::shared_ptr<Server> server) {
    retired_.push_back(server);
    stop_later(std::move(server), true);
}

void DatabaseService::reap_stoppers(bool all) {
    std::vector<Stopper> finished;
    {
        const std::lock_guard lock(stoppers_mutex_);
        for (auto it = stoppers_.begin(); it != stoppers_.end();) {
            if (all || *it->done) {
                finished.push_back(std::move(*it));
                it = stoppers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (Stopper& s : finished) {
        if (s.thread.joinable()) {
            s.thread.join();
        }
    }
}

namespace {

// A server's status, shared sites (D19) shown as "shared": true rather than
// as a project named _shared.
json describe_server_json(json j) {
    if (j.contains("projects") && j["projects"].is_array()) {
        json projects = json::array();
        bool shared = false;
        for (const json& p : j["projects"]) {
            if (p.is_string() && p.get<std::string>() == kSharedSiteName) {
                shared = true;
            } else {
                projects.push_back(p);
            }
        }
        j["projects"] = std::move(projects);
        j["shared"] = shared;
    }
    return j;
}

}  // namespace

json DatabaseService::reload() {
    std::vector<std::shared_ptr<Server>> servers;
    {
        const std::lock_guard lock(mutex_);
        reload_locked();
        for (const auto& [series, server] : servers_) {
            servers.push_back(server);
        }
    }
    json list = json::array();
    for (const auto& s : servers) {
        list.push_back(describe_server_json(s->describe()));
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
        list.push_back(describe_server_json(s->describe()));
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
    // Shared sites' database (D19), when they're on and ask for one.
    if (plan.shared && plan.shared->state == SiteState::Ready && plan.shared->database &&
        plan.shared->database->family == "mariadb") {
        if (const InstalledRuntime* r = best_mariadb(installed, plan.shared->database->version)) {
            Want& w = wanted[mariadb_series(r->version)];
            if (w.runtime == nullptr || Version::parse(r->version) > Version::parse(w.runtime->version)) {
                w.runtime = r;
            }
            w.projects.push_back(plan.shared->name);
        }
    }
    // Settings > Database: "Keep running" runs a series even when nothing
    // uses it, and a chosen port moves its server there.
    const MariaDbSettings settings = read_mariadb_settings(*app);
    for (const auto& [series, s] : settings) {
        if (!s.keep_running) {
            continue;
        }
        if (const InstalledRuntime* r = best_mariadb(installed, series); r != nullptr && mariadb_series(r->version) == series) {
            Want& w = wanted[series];
            if (w.runtime == nullptr || Version::parse(r->version) > Version::parse(w.runtime->version)) {
                w.runtime = r;
            }
        }
    }
    // Every other installed series has a server too, asleep until something
    // connects: phpMyAdmin (its own login form or FloServer's one click) and
    // other clients reach any installed MariaDB, which then sleeps when idle.
    for (const InstalledRuntime& one : installed) {
        if (one.name != "mariadb") {
            continue;
        }
        const std::string series = mariadb_series(one.version);
        if (wanted.contains(series)) {
            continue;
        }
        if (const InstalledRuntime* r = best_mariadb(installed, series); r != nullptr && mariadb_series(r->version) == series) {
            wanted[series].runtime = r;
        }
    }
    for (const auto& [series, count] : held_) {
        wanted.erase(series);
    }
    const auto chosen_port = [&settings](const std::string& series) -> std::optional<Port> {
        const auto it = settings.find(series);
        if (it == settings.end()) {
            return std::nullopt;
        }
        return it->second.port;
    };
    for (auto& [series, want] : wanted) {
        const auto it = settings.find(series);
        want.keep_running = want.keep_running || (it != settings.end() && it->second.keep_running);
    }

    // Servers no longer wanted, whose series has a newer patch release
    // installed, or whose port was changed in Settings, are shut down in the
    // background. One is made again once the old one has let go of its port
    // and its data folder.
    for (auto it = servers_.begin(); it != servers_.end();) {
        const auto want = wanted.find(it->first);
        const auto port = chosen_port(it->first);
        if (want == wanted.end() || want->second.runtime->version != it->second->version() ||
            port != it->second->chosen_port()) {
            retire_later(it->second);
            it = servers_.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(retired_, [](const std::weak_ptr<Server>& w) { return w.expired(); });
    std::set<Port> taken;
    std::set<Port> retiring;
    std::set<std::string> retiring_series;
    for (const auto& [series, server] : servers_) {
        taken.insert(server->port());
    }
    for (const auto& w : retired_) {
        if (const auto s = w.lock()) {
            retiring.insert(s->port());
            retiring_series.insert(s->files().series);
        }
    }
    for (auto& [series, want] : wanted) {
        if (const auto it = servers_.find(series); it != servers_.end()) {
            it->second->set_projects(want.projects, want.keep_running);
            continue;
        }
        if (retiring_series.contains(series)) {
            continue;  // its old server still uses the data folder: next time
        }
        MariaDbFiles files = mariadb_files(want.runtime->dir, want.runtime->version, config_.data_dir);
        // The series keeps its port (.env files have it). D12: the first
        // time 3306 when free, else 3307, 3308... never another stack's.
        // A port chosen in Settings comes first.
        const std::optional<Port> chosen = chosen_port(series);
        std::optional<Port> port = chosen ? chosen : saved_port(files.folder);
        if (port && retiring.contains(*port)) {
            continue;  // our old server still holds it: next time
        }
        // The saved port is opened directly: the probe can read it as taken
        // while connections our relay closed are still in TIME_WAIT. Opening
        // fails if another program really listens there.
        std::unique_ptr<LoopbackListener> listener;
        if (port && !taken.contains(*port)) {
            listener = LoopbackListener::open(*port, error);
        }
        bool fallback = false;
        if (!listener && chosen) {
            // The chosen port is taken: the one it had, else the next free one.
            fallback = true;
            port = saved_port(files.folder);
            if (port && !taken.contains(*port) && !retiring.contains(*port) && *port != *chosen) {
                listener = LoopbackListener::open(*port, error);
            }
        }
        if (!listener) {
            std::set<Port> avoid = taken;
            avoid.insert(retiring.begin(), retiring.end());
            if (port) {
                avoid.insert(*port);
            }
            if (chosen) {
                avoid.insert(*chosen);
            }
            port = pick_port_from(kFirstDatabasePort, avoid, config_.probe);
            if (!port) {
                continue;
            }
            listener = LoopbackListener::open(*port, error);
            if (!listener) {
                continue;
            }
        }
        const auto internal = internal_ports_->pick(std::nullopt, *port, config_.probe);
        if (!internal) {
            continue;
        }
        std::error_code ec;
        fs::create_directories(files.folder, ec);
        (void)write_file_atomically(files.folder / "port", std::to_string(*port) + "\n", error);
        taken.insert(*port);
        auto server = std::make_shared<Server>(config_.data_dir, std::move(files), want.runtime->version, *port, *internal,
                                               std::move(listener), secrets_, internal_ports_, config_.probe,
                                               config_.start_limit);
        server->set_projects(want.projects, want.keep_running);
        server->set_chosen_port(chosen, fallback);
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
        if (listeners.empty()) {
            std::this_thread::sleep_for(200ms);
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
                if (s->begin_idle_stop(config_.idle_after)) {
                    stop_later(s, false);
                }
            }
            reap_stoppers(false);
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
    DatabaseLogin login;
    login.database = project_database_name(project);
    login.user = login.database;
    if (reserved_database_name(login.database)) {
        refusal = {"reserved_name", "\"" + login.database + "\" is one of MariaDB's or shared sites' names; rename the project "
                                    "(flo remove " + project + ", then flo add --name <other name>)"};
        return std::nullopt;
    }
    // Every installed series has a server, so this works with the project
    // off too (phpMyAdmin, .env lines).
    std::shared_ptr<Server> server;
    {
        const auto installed = list_installed(runtimes_dir(config_.data_dir));
        const InstalledRuntime* r = best_mariadb(installed, cfg.config->database->version);
        if (r == nullptr) {
            refusal = not_installed(cfg.config->database->version);
            return std::nullopt;
        }
        server = server_of(mariadb_series(r->version));
        if (!server) {
            refusal = no_server_yet(mariadb_series(r->version));
            return std::nullopt;
        }
    }
    // Counts as a connection: the server doesn't go to sleep meanwhile.
    server->connection_opened();
    const AtExit closed([&server] { server->connection_closed(); });
    if (!server->ensure_running(error)) {
        refusal = {"database_failed", error};
        return std::nullopt;
    }
    // One at a time per server: the saved password and the live one match.
    const std::lock_guard lock(server->login_mutex());
    login.port = server->port();
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

std::optional<DatabaseLogin> DatabaseService::shared_login(ServiceRefusal& refusal) {
    std::string error;
    const auto app = load_app_config(app_config_file(config_.data_dir), error);
    const auto shared = app ? read_shared_config(*app, error) : std::nullopt;
    if (!shared) {
        refusal = {"config_unreadable", "FloServer's settings can't be used: " + error};
        return std::nullopt;
    }
    if (!shared->on) {
        refusal = {"shared_off", "shared sites are off: flo shared on"};
        return std::nullopt;
    }
    if (!shared->database || shared->database->family != "mariadb") {
        refusal = {"no_database", "shared sites have no database (choose one in Settings, or: flo shared db 11.4)"};
        return std::nullopt;
    }
    // A project from before shared sites had a database (www, www-blog):
    // its data must not become the shared sites'.
    for (const Project& p : app ? app->projects : std::vector<Project>()) {
        if (reserved_database_name(project_database_name(p.name)) && p.name.starts_with("www")) {
            refusal = {"reserved_name", "the project " + p.name + " uses a name shared sites' database needs (" +
                                            std::string(kSharedDatabaseUser) + ", " + std::string(kSharedDatabasePrefix) +
                                            "...); rename it first (flo remove " + p.name +
                                            ", then flo add --name <other name>)"};
            return std::nullopt;
        }
    }
    std::shared_ptr<Server> server;
    {
        const auto installed = list_installed(runtimes_dir(config_.data_dir));
        const InstalledRuntime* r = best_mariadb(installed, shared->database->version);
        const std::lock_guard lock(mutex_);
        reload_locked();
        if (r != nullptr) {
            if (const auto it = servers_.find(mariadb_series(r->version)); it != servers_.end()) {
                server = it->second;
            }
        }
    }
    if (!server) {
        refusal = {"database_off", "the shared sites' MariaDB " + shared->database->version +
                                       " isn't installed or can't run yet (flo install mariadb " +
                                       shared->database->version + ")"};
        return std::nullopt;
    }
    server->connection_opened();
    const AtExit closed([&server] { server->connection_closed(); });
    if (!server->ensure_running(error)) {
        refusal = {"database_failed", error};
        return std::nullopt;
    }
    const std::lock_guard lock(server->login_mutex());
    DatabaseLogin login;
    login.database = std::string(kSharedDatabaseUser);
    login.user = std::string(kSharedDatabaseUser);
    login.port = server->port();
    const std::string secret = "mariadb-" + server->files().series + "/" + std::string(kSharedDatabaseUser);
    auto password = secrets_.read(secret);
    if (!password) {
        password = generate_password();
        if (!password || !secrets_.save(secret, *password, error)) {
            refusal = {"secret_failed", "can't keep the password: " + error};
            return std::nullopt;
        }
    }
    login.password = *password;
    if (!server->run_sql(shared_database_sql(login.password), error)) {
        refusal = {"database_failed", error};
        return std::nullopt;
    }
    return login;
}

// --------------------------------------------------------- Settings → Database

std::shared_ptr<DatabaseService::Server> DatabaseService::server_of(const std::string& series) {
    const std::lock_guard lock(mutex_);
    reload_locked();
    const auto it = servers_.find(series);
    return it != servers_.end() ? it->second : nullptr;
}

json DatabaseService::series_status(const std::string& series) const {
    json out = {{"server", nullptr}};
    {
        const std::lock_guard lock(mutex_);
        if (const auto it = servers_.find(series); it != servers_.end()) {
            out["server"] = describe_server_json(it->second->describe());
        }
    }
    // Whose database is there: the ones whose password was kept.
    std::string error;
    json databases = json::array();
    if (const auto app = load_app_config(app_config_file(config_.data_dir), error)) {
        for (const Project& p : app->projects) {
            const std::string db = project_database_name(p.name);
            if (!reserved_database_name(db) && secrets_.read(std::string("mariadb-").append(series).append("/").append(db))) {
                databases.push_back(p.name);
            }
        }
    }
    if (secrets_.read("mariadb-" + series + "/" + std::string(kSharedDatabaseUser))) {
        databases.push_back(std::string(kSharedSiteName));
    }
    out["databases"] = std::move(databases);
    return out;
}

namespace {

// Why a series has no server: it isn't installed (the usual reason: every
// installed series has one), or it can't have one just now.
ServiceRefusal no_server(const fs::path& data_dir, const std::string& series) {
    const auto installed = list_installed(runtimes_dir(data_dir));
    const InstalledRuntime* r = best_mariadb(installed, series);
    if (r == nullptr || mariadb_series(r->version) != series) {
        return not_installed(series);
    }
    return no_server_yet(series);
}

}  // namespace

bool DatabaseService::server_action(const std::string& series, const std::string& action, ServiceRefusal& refusal) {
    if (action != "start" && action != "stop" && action != "restart") {
        refusal = {"bad_params", "action is start, stop or restart"};
        return false;
    }
    auto server = server_of(series);
    if (!server) {
        refusal = no_server(config_.data_dir, series);
        return false;
    }
    run_later([server = std::move(server), action] {
        std::string error;
        if (action == "stop") {
            server->stop(false);  // a clean shutdown; a connection starts it again
        } else if (action == "restart") {
            (void)server->restart(error);
        } else {
            server->connection_opened();
            server->clear_failure();
            (void)server->ensure_running(error);
            server->connection_closed();
        }
    });
    return true;
}

bool DatabaseService::restart_if_running(const std::string& series) {
    std::shared_ptr<Server> server;
    {
        const std::lock_guard lock(mutex_);
        if (const auto it = servers_.find(series); it != servers_.end() && it->second->running()) {
            server = it->second;
        }
    }
    if (!server) {
        return false;
    }
    run_later([server = std::move(server)] {
        std::string error;
        (void)server->restart(error);
    });
    return true;
}

std::optional<std::string> DatabaseService::root_password(const std::string& series, ServiceRefusal& refusal) const {
    auto password = secrets_.read("mariadb-" + series + "/root");
    if (!password) {
        refusal = {"no_password", "MariaDB " + series + " has no root password yet: it gets one the first time it starts"};
    }
    return password;
}

void DatabaseService::hold_series(const std::string& series) {
    std::vector<std::shared_ptr<Server>> going;
    {
        const std::lock_guard lock(mutex_);
        ++held_[series];
        for (const auto& w : retired_) {
            if (const auto s = w.lock(); s && s->files().series == series) {
                going.push_back(s);
            }
        }
        if (const auto it = servers_.find(series); it != servers_.end()) {
            going.push_back(it->second);
        }
        reload_locked();  // retires it
    }
    for (const auto& s : going) {
        s->stop(true);  // waits for a stop under way too
    }
}

void DatabaseService::release_series(const std::string& series) {
    const std::lock_guard lock(mutex_);
    if (const auto it = held_.find(series); it != held_.end() && --it->second <= 0) {
        held_.erase(it);
    }
    reload_locked();
}

std::optional<DatabaseLogin> DatabaseService::root_login(const std::string& series, ServiceRefusal& refusal) {
    const auto server = server_of(series);
    if (!server) {
        refusal = no_server(config_.data_dir, series);
        return std::nullopt;
    }
    // Counts as a connection meanwhile; afterwards the idle time starts
    // from now, so it sleeps as usual unless it's kept running.
    server->connection_opened();
    const AtExit closed([&server] { server->connection_closed(); });
    server->clear_failure();  // asked for by the user: a failed start is tried again now
    std::string error;
    if (!server->ensure_running(error)) {
        refusal = {"database_failed", "MariaDB " + series + " didn't start: " + error};
        return std::nullopt;
    }
    auto password = secrets_.read(server->root_name());
    if (!password) {
        refusal = {"no_password", "root's password for MariaDB " + series + " is missing from the secure store"};
        return std::nullopt;
    }
    DatabaseLogin login;
    login.port = server->port();
    login.user = "root";
    login.password = std::move(*password);
    return login;
}

bool DatabaseService::set_root_password(const std::string& series, const std::string& password, ServiceRefusal& refusal) {
    if (const auto problem = root_password_problem(password)) {
        refusal = {"bad_params", *problem};
        return false;
    }
    const auto server = server_of(series);
    if (!server) {
        refusal = no_server(config_.data_dir, series);
        return false;
    }
    std::string error;
    if (!server->change_root_password(password, error)) {
        refusal = {"database_failed", error};
        return false;
    }
    return true;
}

std::optional<fs::path> DatabaseService::final_ini(const std::string& series, ServiceRefusal& refusal) const {
    std::shared_ptr<Server> server;
    {
        const std::lock_guard lock(mutex_);
        if (const auto it = servers_.find(series); it != servers_.end()) {
            server = it->second;
        }
    }
    std::string text;
    std::string port_note;
    if (server) {
        text = server->config_text(false);
        port_note = "# port=" + std::to_string(server->internal_port()) +
                    " below is FloServer's own: projects connect to " + std::to_string(server->port()) +
                    ", and FloServer passes each connection on (and starts MariaDB when it sleeps).\n";
    } else {
        const auto installed = list_installed(runtimes_dir(config_.data_dir));
        const InstalledRuntime* r = best_mariadb(installed, series);
        if (r == nullptr || mariadb_series(r->version) != series) {
            refusal = {"not_installed", "MariaDB " + series + " isn't installed"};
            return std::nullopt;
        }
        std::string error;
        const auto app = load_app_config(app_config_file(config_.data_dir), error);
        const MariaDbSettings all = app ? read_mariadb_settings(*app) : MariaDbSettings{};
        const auto it = all.find(series);
        const MariaDbExtras extras = mariadb_extras(config_.data_dir, series, it != all.end() ? &it->second : nullptr);
        text = mariadb_config(mariadb_files(r->dir, r->version, config_.data_dir), kFirstInternalDatabasePort,
                              running_as_root(), false, &extras);
        port_note = "# MariaDB " + series + " isn't running for any project now; the port below is chosen when it starts.\n";
    }
    text = "# What MariaDB " + series +
           " reads when it starts. Written by FloServer when you ask to see it: edits here are lost and do nothing.\n"
           "# Change values in Settings > Database, or add your own lines to custom.ini beside this file.\n" +
           port_note + "\n" + text;
    fs::path file = mariadb_custom_ini_file(config_.data_dir, series).parent_path() / "final-my.ini";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    // Read-only, so Notepad says so on Save; made writable again to replace it.
    fs::permissions(file, fs::perms::owner_write, fs::perm_options::add, ec);
    std::string error;
    if (!write_file_atomically(file, text, error)) {
        refusal = {"folder_unwritable", error};
        return std::nullopt;
    }
    fs::permissions(file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove, ec);
    return file;
}

}  // namespace floserver::core

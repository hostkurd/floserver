#include "floserver/core/app_runner.h"

#include "floserver/core/paths.h"
#include "floserver/core/process.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>
#include <thread>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr int kMaxConnections = 256;
constexpr auto kFailedRetry = 5s;          // a request after this tries again
constexpr auto kQuickExit = 10s;           // ended sooner than this after starting: Failed
constexpr std::uintmax_t kMaxLogBytes = std::uintmax_t{8} * 1024 * 1024;
constexpr std::size_t kMaxPackageJson = std::size_t{1024} * 1024;
constexpr std::size_t kTailBytes = 1500;

#ifdef _WIN32
constexpr char kPathSeparator = ';';
#else
constexpr char kPathSeparator = ':';
#endif

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// The end of a log file, without reading all of it.
std::string tail(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff from = std::max<std::streamoff>(0, size - static_cast<std::streamoff>(kTailBytes));
    in.seekg(from);
    std::string text(static_cast<std::size_t>(size - from), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return from > 0 ? "..." + text : text;
}

void append_line(const fs::path& file, const std::string& line) {
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out << line << "\n";
}

// The words of a shell command, split on spaces (quotes aren't needed here:
// only the command's name and the presence of a port option count).
std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::string word;
    for (const char c : text) {
        if (c == ' ' || c == '\t') {
            if (!word.empty()) {
                out.push_back(std::move(word));
                word.clear();
            }
        } else {
            word += c;
        }
    }
    if (!word.empty()) {
        out.push_back(std::move(word));
    }
    return out;
}

// What the environment of the app's processes adds: the port, loopback
// only, no browser opened by the dev server, plain text in the log, and the
// project's Node first on PATH.
std::vector<std::pair<std::string, std::string>> app_environment(const AppSpec& app, Port port) {
    const auto node = node_program(app.runtime_dir);
    std::string path = node ? path_to_utf8(node->parent_path()) : std::string();
    if (const auto current = get_env("PATH")) {
        path += path.empty() ? *current : kPathSeparator + *current;
    }
    return {{"PORT", std::to_string(port)},
            {"HOST", "127.0.0.1"},
            {"BROWSER", "none"},
            {"NO_COLOR", "1"},
            {"FORCE_COLOR", "0"},
            {"PATH", path}};
}

// package.json lists packages and node_modules isn't there yet.
bool needs_npm_install(const fs::path& dir) {
    std::error_code ec;
    if (fs::exists(dir / "node_modules", ec) || !is_file(dir / "package.json")) {
        return false;
    }
    const auto text = read_small_file(dir / "package.json", kMaxPackageJson);
    if (!text) {
        return false;
    }
    try {
        const json doc = json::parse(*text);
        for (const char* key : {"dependencies", "devDependencies"}) {
            if (doc.is_object() && doc.contains(key) && doc[key].is_object() && !doc[key].empty()) {
                return true;
            }
        }
    } catch (const json::exception&) {
        // app_launch() reports it
    }
    return false;
}

std::string replace_all(std::string text, std::string_view from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

}  // namespace

std::optional<fs::path> node_program(const fs::path& node_dir) {
#ifdef _WIN32
    fs::path p = node_dir / "node.exe";
#else
    fs::path p = node_dir / "bin" / "node";
#endif
    return is_file(p) ? std::optional<fs::path>(std::move(p)) : std::nullopt;
}

std::optional<fs::path> npm_script(const fs::path& node_dir) {
#ifdef _WIN32
    fs::path p = node_dir / "node_modules" / "npm" / "bin" / "npm-cli.js";
#else
    fs::path p = node_dir / "lib" / "node_modules" / "npm" / "bin" / "npm-cli.js";
#endif
    return is_file(p) ? std::optional<fs::path>(std::move(p)) : std::nullopt;
}

std::vector<std::string> port_arguments(const std::string& script, Port port) {
    // npm adds arguments at the end of the script, so they reach its last
    // command: "tsc && vite" → vite.
    std::string_view last(script);
    for (const std::string_view sep : {"&&", "||", ";", "|"}) {
        if (const auto at = last.rfind(sep); at != std::string_view::npos) {
            last = last.substr(at + sep.size());
        }
    }
    const std::vector<std::string> w = words(last);
    if (std::any_of(w.begin(), w.end(), [](const std::string& s) {
            return s == "--port" || s.starts_with("--port=") || s == "-p";
        })) {
        return {};
    }
    // The command's name, after any VAR=value and cross-env.
    std::string name;
    for (const std::string& s : w) {
        if (s.find('=') != std::string::npos || s == "cross-env" || s == "env" || s == "npx") {
            continue;
        }
        name = s.substr(s.find_last_of('/') + 1);
        break;
    }
    const std::string p = std::to_string(port);
    if (name == "vite") {
        return {"--port", p, "--strictPort", "--host", "127.0.0.1"};
    }
    if (name == "next") {
        return {"--port", p, "--hostname", "127.0.0.1"};
    }
    if (name == "nuxt" || name == "nuxi" || name == "astro" || name == "ng") {
        return {"--port", p, "--host", "127.0.0.1"};
    }
    return {};
}

std::optional<AppLaunch> app_launch(const AppSpec& app, Port port, std::string& error) {
    AppLaunch launch;
    launch.options.env = app_environment(app, port);
    if (app.command) {
        const std::string command = replace_all(*app.command, "{port}", std::to_string(port));
        launch.shown = command;
#ifdef _WIN32
        const std::string root = get_env("SystemRoot").value_or("C:\\Windows");
        launch.options.program = path_from_utf8(root) / "System32" / "cmd.exe";
        launch.options.windows_raw_args = "/d /s /c \"" + command + "\"";
#else
        launch.options.program = "/bin/sh";
        launch.options.args = {"-c", command};
#endif
        return launch;
    }

    const fs::path package = app.dir / "package.json";
    const auto text = is_file(package) ? read_small_file(package, kMaxPackageJson) : std::nullopt;
    if (!text) {
        error = "there's no package.json in the project folder; add one, or a \"command\" in floserver.json";
        return std::nullopt;
    }
    std::string script_name;
    std::string script;
    try {
        const json doc = json::parse(*text);
        const json* scripts = doc.is_object() && doc.contains("scripts") ? &doc["scripts"] : nullptr;
        for (const char* name : {"dev", "start"}) {
            if (scripts != nullptr && scripts->is_object() && scripts->contains(name) && (*scripts)[name].is_string()) {
                script_name = name;
                script = (*scripts)[name].get<std::string>();
                break;
            }
        }
    } catch (const json::exception&) {
        error = "package.json isn't valid JSON";
        return std::nullopt;
    }
    if (script_name.empty()) {
        error = "package.json has no \"dev\" or \"start\" script; add one, or a \"command\" in floserver.json";
        return std::nullopt;
    }
    const auto node = node_program(app.runtime_dir);
    const auto npm = npm_script(app.runtime_dir);
    if (!node || !npm) {
        error = "node or npm is missing from " + path_to_utf8(app.runtime_dir);
        return std::nullopt;
    }
    launch.options.program = *node;
    launch.options.args = {path_to_utf8(*npm), "run", script_name};
    launch.shown = "npm run " + script_name;
    if (const auto extra = port_arguments(script, port); !extra.empty()) {
        launch.options.args.emplace_back("--");
        launch.shown += " --";
        for (const std::string& a : extra) {
            launch.options.args.push_back(a);
            launch.shown += " " + a;
        }
    }
    return launch;
}

std::optional<Port> PortSet::take(Port first, const PortProbe& probe) {
    const std::lock_guard lock(mutex_);
    const auto port = pick_port_from(first, taken_, probe);
    if (port) {
        taken_.insert(*port);
    }
    return port;
}

void PortSet::give_back(Port port) {
    const std::lock_guard lock(mutex_);
    taken_.erase(port);
}

// ------------------------------------------------------------------ runner

std::shared_ptr<AppRunner> AppRunner::open(const AppSpec& app, AppRunnerConfig config, std::string& error) {
    auto listener = LoopbackListener::open(app.port, error);
    if (!listener) {
        return nullptr;
    }
    std::error_code ec;
    fs::create_directories(config.work_dir / "apps", ec);
    if (!config.internal_ports) {
        config.internal_ports = std::make_shared<PortSet>();
    }
    if (!config.probe) {
        config.probe = can_listen_on_loopback;
    }
    return std::shared_ptr<AppRunner>(new AppRunner(app, std::move(config), std::move(listener)));
}

AppRunner::AppRunner(const AppSpec& app, AppRunnerConfig config, std::unique_ptr<LoopbackListener> listener)
    : config_(std::move(config)),
      listener_(std::move(listener)),
      log_(config_.work_dir / "apps" / (app.name + ".log")),
      app_(app) {
    last_activity_ = Clock::now().time_since_epoch().count();
}

AppRunner::~AppRunner() {
    // Threads hold a reference, so none is left by now.
    const std::lock_guard lock(mutex_);
    process_.reset();
    if (internal_) {
        config_.internal_ports->give_back(*internal_);
    }
}

AppSpec AppRunner::app() const {
    const std::lock_guard lock(mutex_);
    return app_;
}

AppState AppRunner::state() const {
    const std::lock_guard lock(mutex_);
    return state_;
}

void AppRunner::set_app(const AppSpec& app) {
    std::unique_lock lock(mutex_);
    const bool other = app.kind != app_.kind || app.dir != app_.dir || app.runtime_dir != app_.runtime_dir ||
                       app.command != app_.command;
    app_ = app;
    if (other) {
        error_.clear();
        if (state_ == AppState::Running || state_ == AppState::Failed) {
            stop_process(lock);
        }
    }
}

void AppRunner::stop_process(std::unique_lock<std::mutex>& lock) {
    (void)lock;  // held by the caller
    if (process_) {
        process_->kill();
        process_.reset();
    }
    if (internal_) {
        config_.internal_ports->give_back(*internal_);
        internal_.reset();
    }
    state_ = AppState::Sleeping;
    changed_.notify_all();
}

void AppRunner::accept_waiting() {
    const auto client = listener_->accept_connection();
    if (!client) {
        return;
    }
    if (connections_ >= kMaxConnections) {
        close_socket(*client);
        return;
    }
    ++connections_;
    ++threads_;
    try {
        std::thread([self = shared_from_this(), c = *client] {
            self->serve(c);
            --self->connections_;
            --self->threads_;
        }).detach();
    } catch (const std::system_error&) {
        --connections_;
        --threads_;
        close_socket(*client);
    }
}

void AppRunner::serve(std::uintptr_t client) {
    const auto touch = [this] { last_activity_ = Clock::now().time_since_epoch().count(); };
    touch();
    std::string error;
    std::optional<std::uintptr_t> upstream;
    std::optional<Port> port;
    if (ensure_running(error)) {
        {
            const std::lock_guard lock(mutex_);
            port = internal_;
        }
        upstream = port ? connect_loopback(*port) : std::nullopt;
        if (!upstream) {
            error = "the server isn't answering on port " + (port ? std::to_string(*port) : std::string("?"));
        }
    }
    if (upstream) {
        relay_sockets(client, *upstream, touch);
    } else {
        answer_and_close(client, failure_page(error));
    }
    touch();
}

void AppRunner::wake() {
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || state_ != AppState::Sleeping) {
            return;
        }
    }
    ++threads_;
    try {
        std::thread([self = shared_from_this()] {
            std::string ignored;
            (void)self->ensure_running(ignored);
            --self->threads_;
        }).detach();
    } catch (const std::system_error&) {
        --threads_;
    }
}

bool AppRunner::ensure_running(std::string& error) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return state_ != AppState::Starting || closed_; });
    if (closed_) {
        error = "the project was turned off";
        return false;
    }
    if (state_ == AppState::Running && process_ && process_->running()) {
        return true;
    }
    const auto now = Clock::now();
    if (state_ == AppState::Failed && now < retry_after_) {
        error = error_;
        return false;
    }
    if (process_) {
        stop_process(lock);  // it ended; tick() hasn't noticed yet
    }
    state_ = AppState::Starting;
    const AppSpec app = app_;
    lock.unlock();

    std::string shown;
    std::string why;
    std::unique_ptr<ChildProcess> process;
    const auto port = config_.internal_ports->take(kFirstAppInternalPort, config_.probe);
    if (!port) {
        why = "there's no free port from " + std::to_string(kFirstAppInternalPort) + " upwards";
    } else {
        process = start_server(app, *port, shown, why);
    }

    lock.lock();
    if (process && !closed_) {
        process_ = std::move(process);
        internal_ = port;
        shown_ = shown;
        state_ = AppState::Running;
        started_ = Clock::now();
        error_.clear();
    } else {
        if (process) {
            process->kill();
        }
        if (port) {
            config_.internal_ports->give_back(*port);
        }
        state_ = closed_ ? AppState::Sleeping : AppState::Failed;
        error_ = closed_ ? std::string() : why;
        retry_after_ = Clock::now() + kFailedRetry;
        error = closed_ ? std::string("the project was turned off") : why;
    }
    changed_.notify_all();
    return state_ == AppState::Running;
}

std::unique_ptr<ChildProcess> AppRunner::start_server(const AppSpec& app, Port port, std::string& shown,
                                                      std::string& error) {
    std::error_code ec;
    if (fs::file_size(log_, ec) > kMaxLogBytes && !ec) {
        fs::remove(log_, ec);
    }
    const auto is_closing = [this] {
        const std::lock_guard lock(mutex_);
        return closed_;
    };
    // Waits for `child`, giving up when the runner closes or time runs out.
    const auto wait_for = [&is_closing](ChildProcess& child, std::chrono::seconds limit) -> std::optional<int> {
        const auto deadline = Clock::now() + limit;
        while (Clock::now() < deadline && !is_closing()) {
            if (const auto code = child.wait(200ms)) {
                return code;
            }
        }
        child.kill();
        return std::nullopt;
    };

    // The project's packages, the first time (a fresh clone has none).
    if (needs_npm_install(app.dir)) {
        const auto node = node_program(app.runtime_dir);
        const auto npm = npm_script(app.runtime_dir);
        if (!node || !npm) {
            error = "node or npm is missing from " + path_to_utf8(app.runtime_dir);
            return nullptr;
        }
        append_line(log_, "--- FloServer: npm install");
        ChildOptions o;
        o.program = *node;
        o.args = {path_to_utf8(*npm), "install"};
        o.env = app_environment(app, port);
        o.working_dir = app.dir;
        o.output_log = log_;
        auto install = ChildProcess::start(o, error);
        if (!install) {
            return nullptr;
        }
        const auto code = wait_for(*install, config_.install_limit);
        if (code != 0) {
            error = code ? "npm install failed:\n" + tail(log_) : "npm install didn't finish";
            return nullptr;
        }
    }

    auto launch = app_launch(app, port, error);
    if (!launch) {
        append_line(log_, "--- FloServer: can't start: " + error);
        return nullptr;
    }
    shown = launch->shown;
    append_line(log_, "--- FloServer: " + shown + " (port " + std::to_string(port) + ")");
    launch->options.working_dir = app.dir;
    launch->options.output_log = log_;
    auto child = ChildProcess::start(launch->options, error);
    if (!child) {
        return nullptr;
    }
    // Ready once it takes connections on its port.
    const auto deadline = Clock::now() + config_.start_limit;
    while (true) {
        if (is_closing()) {
            child->kill();
            error = "the project was turned off";
            return nullptr;
        }
        if (const auto code = child->wait(0ms)) {
            error = shown + " ended (exit " + std::to_string(*code) + ") before it took connections on port " +
                    std::to_string(port) + ":\n" + tail(log_);
            return nullptr;
        }
        if (const auto s = connect_loopback(port)) {
            close_socket(*s);
            return child;
        }
        if (Clock::now() >= deadline) {
            child->kill();
            error = shown + " didn't take connections on port " + std::to_string(port) + " within " +
                    std::to_string(config_.start_limit.count()) + " s (does it listen on the PORT it's given?):\n" +
                    tail(log_);
            return nullptr;
        }
        std::this_thread::sleep_for(100ms);
    }
}

void AppRunner::tick(Clock::time_point now, std::chrono::seconds idle_after) {
    std::unique_lock lock(mutex_);
    if (closed_) {
        return;
    }
    if (state_ == AppState::Running && process_ && !process_->running()) {
        const bool quick = now - started_ < kQuickExit;
        error_ = shown_ + " stopped" + (quick ? " right after starting" : "") + ":\n" + tail(log_);
        stop_process(lock);
        if (quick) {
            state_ = AppState::Failed;
            retry_after_ = now + kFailedRetry;
        }
        return;
    }
    const Clock::time_point last{Clock::duration(last_activity_.load())};
    if (state_ == AppState::Running && !app_.keep_running && connections_ == 0 && now - last >= idle_after) {
        stop_process(lock);
        return;
    }
    if (state_ == AppState::Sleeping && app_.keep_running) {
        lock.unlock();
        wake();
    }
}

void AppRunner::close() {
    {
        std::unique_lock lock(mutex_);
        closed_ = true;
        stop_process(lock);  // its connections end with it
    }
    // Connections waiting for a start see closed_ and end; a start under
    // way notices within a moment.
    const auto deadline = Clock::now() + 10s;
    while (threads_ > 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
}

std::string AppRunner::failure_page(const std::string& error) const {
    std::string name;
    {
        const std::lock_guard lock(mutex_);
        name = app_.name;
    }
    const std::string body = "FloServer: " + name + "'s server isn't running.\n\n" + error + "\n\nThe full log: " +
                             path_to_utf8(log_) + "\n";
    return "HTTP/1.1 502 Bad Gateway\r\n"
           "Content-Type: text/plain; charset=utf-8\r\n"
           "Cache-Control: no-store\r\n"
           "Connection: close\r\n"
           "Content-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

json AppRunner::describe() const {
    const std::lock_guard lock(mutex_);
    static constexpr const char* kStates[] = {"sleeping", "starting", "running", "failed"};  // NOLINT
    json j = {{"state", kStates[static_cast<int>(state_)]},
              {"port", listener_->port()},
              {"connections", connections_.load()},
              {"log", path_to_utf8(log_)}};
    if (internal_) {
        j["internal_port"] = *internal_;
    }
    if (process_) {
        j["pid"] = process_->pid();
    }
    if (!shown_.empty()) {
        j["command"] = shown_;
    }
    if (!error_.empty()) {
        j["error"] = error_;
    }
    return j;
}

}  // namespace floserver::core

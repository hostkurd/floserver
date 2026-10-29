#include "floserver/core/app_runner.h"

#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/sha256.h"
#include "floserver/core/encoding.h"

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

fs::path venv_dir(const AppSpec& app) { return app.dir / ".venv"; }

// What the environment of the app's processes adds: the port, loopback
// only, no browser opened by the dev server, plain text in the log, and the
// project's runtime first on PATH (for Python its .venv, then the install).
void set_environment(ChildOptions& o, const AppSpec& app, Port port) {
    std::vector<std::string> first;
    if (app.kind == AppKind::Python) {
        first.push_back(path_to_utf8(venv_python(app.dir).parent_path()));
        if (const auto py = python_program(app.runtime_dir)) {
            first.push_back(path_to_utf8(py->parent_path()));
        }
        if (const auto node = app.node_dir.empty() ? std::nullopt : node_program(app.node_dir)) {
            first.push_back(path_to_utf8(node->parent_path()));
        }
    } else if (const auto node = node_program(app.runtime_dir)) {
        first.push_back(path_to_utf8(node->parent_path()));
    }
    std::string path;
    for (const std::string& p : first) {
        path += (path.empty() ? "" : std::string(1, kPathSeparator)) + p;
    }
    if (const auto current = get_env("PATH")) {
        path += path.empty() ? *current : kPathSeparator + *current;
    }
    o.env = {{"PORT", std::to_string(port)},
             {"HOST", "127.0.0.1"},
             {"BROWSER", "none"},
             {"NO_COLOR", "1"},
             {"FORCE_COLOR", "0"},
             {"PATH", path}};
    if (app.kind == AppKind::Python) {
        o.env.emplace_back("VIRTUAL_ENV", path_to_utf8(venv_dir(app)));
        o.env.emplace_back("PYTHONUNBUFFERED", "1");  // the log shows output as it comes
        // UTF-8 in the log on Windows too, where a file would otherwise get
        // the ANSI code page and Arabic or Kurdish output would fail.
        o.env.emplace_back("PYTHONUTF8", "1");
        o.remove_env = {"PYTHONHOME", "PYTHONPATH", "PIP_REQUIRE_VIRTUALENV"};
    }
}

// "3.13.18" → "3.13".
std::string major_minor(const std::string& version) {
    const auto first = version.find('.');
    const auto second = first == std::string::npos ? std::string::npos : version.find('.', first + 1);
    return version.substr(0, second);
}

// A value from a .venv's pyvenv.cfg: "version" ("3.13.17"; "version_info"
// in some Pythons) or "home" (the folder of the Python it was made with).
std::optional<std::string> venv_setting(const fs::path& venv, std::initializer_list<std::string_view> keys) {
    const fs::path cfg = venv / "pyvenv.cfg";
    const auto text = is_file(cfg) ? read_small_file(cfg, std::size_t{64} * 1024) : std::nullopt;
    if (!text) {
        return std::nullopt;
    }
    std::size_t pos = 0;
    while (pos < text->size()) {
        auto end = text->find('\n', pos);
        if (end == std::string::npos) {
            end = text->size();
        }
        const std::string line = text->substr(pos, end - pos);
        pos = end + 1;
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        const auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) {
                s.pop_back();
            }
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
                s.erase(0, 1);
            }
        };
        trim(key);
        trim(value);
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) {
            return value;
        }
    }
    return std::nullopt;
}

// A file whose text contains `marker`.
bool file_mentions(const fs::path& file, std::string_view marker) {
    const auto text = is_file(file) ? read_small_file(file, std::size_t{1024} * 1024) : std::nullopt;
    return text && text->find(marker) != std::string::npos;
}

std::optional<AppLaunch> python_launch(const AppSpec& app, Port port, std::string& error) {
    AppLaunch launch;
    set_environment(launch.options, app, port);
    launch.options.program = venv_python(app.dir);
    const std::string p = std::to_string(port);
    if (is_file(app.dir / "manage.py")) {
        launch.options.args = {"manage.py", "runserver", "127.0.0.1:" + p};
        launch.shown = "python manage.py runserver 127.0.0.1:" + p;
        return launch;
    }
    for (const auto& [file, module] : {std::pair{"main.py", "main:app"}, std::pair{"app/main.py", "app.main:app"},
                                       std::pair{"app.py", "app:app"}}) {
        if (file_mentions(app.dir / path_from_utf8(file), "FastAPI(")) {
            launch.options.args = {"-m", "uvicorn", module, "--reload", "--host", "127.0.0.1", "--port", p};
            launch.shown = std::string("uvicorn ") + module + " --reload --host 127.0.0.1 --port " + p;
            return launch;
        }
    }
    for (const auto& [file, name] : {std::pair{"app.py", "app"}, std::pair{"wsgi.py", "wsgi"}}) {
        if (file_mentions(app.dir / file, "Flask(")) {
            launch.options.args = {"-m", "flask", "--app", name, "run", "--debug", "--host", "127.0.0.1", "--port", p};
            launch.shown = std::string("flask --app ") + name + " run --debug --host 127.0.0.1 --port " + p;
            return launch;
        }
    }
    error = "FloServer can't tell how to start this Python project (no manage.py, no FastAPI app in main.py, "
            "app/main.py or app.py, no Flask app in app.py or wsgi.py); add a \"command\" to floserver.json";
    return std::nullopt;
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
    // A damaged file is reported by app_launch(); nothing to install here.
    const json doc = json::parse(*text, nullptr, false);
    for (const char* key : {"dependencies", "devDependencies"}) {
        if (doc.is_object() && doc.contains(key) && doc[key].is_object() && !doc[key].empty()) {
            return true;
        }
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
    for (const std::string_view sep : {"&&", "||", ";", "|", "&"}) {
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
        if (s.find('=') != std::string::npos || s == "cross-env" || s == "env" || s == "npx" || s == "pnpm" ||
            s == "pnpx" || s == "yarn" || s == "bunx" || s == "exec" || s == "dlx") {
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

std::optional<fs::path> python_program(const fs::path& python_dir) {
#ifdef _WIN32
    fs::path p = python_dir / "python.exe";
#else
    fs::path p = python_dir / "bin" / "python3";
#endif
    return is_file(p) ? std::optional<fs::path>(std::move(p)) : std::nullopt;
}

fs::path venv_python(const fs::path& project_dir) {
#ifdef _WIN32
    return project_dir / ".venv" / "Scripts" / "python.exe";
#else
    return project_dir / ".venv" / "bin" / "python";
#endif
}

std::optional<std::vector<AppSetupStep>> app_setup(const AppSpec& app, Port port, std::string& error) {
    std::vector<AppSetupStep> steps;
    std::error_code ec;
    if (app.kind == AppKind::Node) {
        if (!needs_npm_install(app.dir)) {
            return steps;
        }
        const auto node = node_program(app.runtime_dir);
        const auto npm = npm_script(app.runtime_dir);
        if (!node || !npm) {
            error = "node or npm is missing from " + path_to_utf8(app.runtime_dir);
            return std::nullopt;
        }
        AppSetupStep step;
        set_environment(step.launch.options, app, port);
        step.launch.options.program = *node;
        step.launch.options.args = {path_to_utf8(*npm), "install"};
        step.launch.shown = "npm install";
        steps.push_back(std::move(step));
        return steps;
    }

    // Python: the project's .venv, then its packages.
    const auto python = python_program(app.runtime_dir);
    if (!python) {
        error = "python is missing from " + path_to_utf8(app.runtime_dir);
        return std::nullopt;
    }
    const fs::path venv = venv_dir(app);
    if (fs::exists(venv, ec)) {
        const auto made_with = venv_setting(venv, {"version", "version_info"});
        if (!made_with || !is_file(venv_python(app.dir))) {
            error = "the project's .venv folder isn't a Python virtual environment FloServer can use; rename or "
                    "delete it, and the next request makes a new one";
            return std::nullopt;
        }
        if (major_minor(*made_with) != major_minor(app.version)) {
            error = "the project's .venv was made with Python " + major_minor(*made_with) + ", and the project now asks "
                    "for Python " + major_minor(app.version) + ". FloServer doesn't replace it on its own: delete the "
                    ".venv folder, and the next request makes a new one with Python " + major_minor(app.version) + ".";
            return std::nullopt;
        }
        // Made with another install of the same version (a patch update
        // replaced the folder): moved over to this one, packages kept.
        const auto home = venv_setting(venv, {"home"});
        const fs::path want = python->parent_path();
        std::error_code e1;
        std::error_code e2;
        if (home && !fs::equivalent(path_from_utf8(*home), want, e1) &&
            fs::weakly_canonical(path_from_utf8(*home), e1) != fs::weakly_canonical(want, e2)) {
            AppSetupStep step;
            set_environment(step.launch.options, app, port);
            step.launch.options.program = *python;
            step.launch.options.args = {"-m", "venv", "--upgrade", ".venv"};
            step.launch.shown = "python -m venv --upgrade .venv";
            steps.push_back(std::move(step));
        }
    } else {
        AppSetupStep step;
        set_environment(step.launch.options, app, port);
        step.launch.options.program = *python;
        step.launch.options.args = {"-m", "venv", ".venv"};
        step.launch.shown = "python -m venv .venv";
        // A .venv cut short (stopped, timed out) isn't left half made: the
        // next start makes it again.
        step.failed = [venv] {
            std::error_code ignored;
            fs::remove_all(venv, ignored);
        };
        steps.push_back(std::move(step));
    }
    // Packages: installed again when the file that lists them changes.
    for (const char* name : {"requirements.txt", "pyproject.toml"}) {
        const fs::path file = app.dir / name;
        if (!is_file(file)) {
            continue;
        }
        const auto text = read_small_file(file, std::size_t{4} * 1024 * 1024);
        if (!text) {
            error = std::string(name) + " can't be read (or is larger than 4 MB)";
            return std::nullopt;
        }
        // pyproject.toml is installed only as a package (a [build-system]):
        // one that only holds tool settings, or an app with no build
        // system, can't be (pip install -e . fails on a flat layout).
        if (std::string_view(name) == "pyproject.toml" && text->find("[build-system]") == std::string::npos) {
            break;
        }
        const std::string stamp = std::string(name) + " " + to_hex(sha256(*text)) + "\n";
        const fs::path marker = venv / ".floserver-installed";
        if (is_file(marker) && read_small_file(marker, 1024) == stamp) {
            break;
        }
        AppSetupStep step;
        set_environment(step.launch.options, app, port);
        step.launch.options.program = venv_python(app.dir);
        const bool requirements = std::string_view(name) == "requirements.txt";
        step.launch.options.args = {"-m", "pip", "install", "--disable-pip-version-check"};
        if (requirements) {
            step.launch.options.args.insert(step.launch.options.args.end(), {"-r", "requirements.txt"});
        } else {
            step.launch.options.args.insert(step.launch.options.args.end(), {"-e", "."});
        }
        step.launch.shown = requirements ? "pip install -r requirements.txt" : "pip install -e .";
        step.done = [marker, stamp] {
            std::string ignored;
            (void)write_file_atomically(marker, stamp, ignored);
        };
        steps.push_back(std::move(step));
        break;
    }
    return steps;
}

std::optional<AppLaunch> app_launch(const AppSpec& app, Port port, std::string& error) {
    AppLaunch launch;
    set_environment(launch.options, app, port);
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

    if (app.kind == AppKind::Python) {
        return python_launch(app, port, error);
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
        error = R"(package.json has no "dev" or "start" script; add one, or a "command" in floserver.json)";
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
        if (state_ == AppState::Starting) {
            stale_ = true;  // the start under way is thrown away when it ends
        } else if (state_ == AppState::Running || state_ == AppState::Failed) {
            stop_process(lock);
        }
    }
}

void AppRunner::stop_process(std::unique_lock<std::mutex>& lock) {
    (void)lock;  // held by the caller
    const std::shared_ptr<ChildProcess> process(std::move(process_));
    const auto port = internal_;
    internal_.reset();
    state_ = AppState::Sleeping;
    changed_.notify_all();
    if (!process && !port) {
        return;
    }
    // The port is given back once the server has let go of it.
    const auto finish = [process, port, ports = config_.internal_ports] {
        if (process) {
            process->kill();
        }
        if (port) {
            ports->give_back(*port);
        }
    };
    ++threads_;
    try {
        std::thread([self = shared_from_this(), finish] {
            finish();
            --self->threads_;
        }).detach();
    } catch (const std::system_error&) {
        --threads_;
        finish();
    }
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
    bool ipv6 = false;
    if (ensure_running(error)) {
        {
            const std::lock_guard lock(mutex_);
            port = internal_;
            ipv6 = internal_ipv6_;
        }
        upstream = port ? connect_loopback(*port, ipv6) : std::nullopt;
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
    stale_ = false;
    const AppSpec app = app_;
    lock.unlock();

    std::string shown;
    std::string why;
    bool ipv6 = false;
    std::unique_ptr<ChildProcess> process;
    const auto port = config_.internal_ports->take(kFirstAppInternalPort, config_.probe);
    if (!port) {
        why = "there's no free port from " + std::to_string(kFirstAppInternalPort) + " upwards";
    } else {
        process = start_server(app, *port, shown, ipv6, why);
    }

    lock.lock();
    if (stale_ && !closed_) {
        // The command or runtime changed meanwhile: this one isn't wanted,
        // and the next connection starts the new one.
        if (process) {
            process->kill();
        }
        if (port) {
            config_.internal_ports->give_back(*port);
        }
        stale_ = false;
        state_ = AppState::Sleeping;
        changed_.notify_all();
        error = "the project's settings changed while it was starting; reload the page";
        return false;
    }
    if (process && !closed_) {
        process_ = std::move(process);
        internal_ = port;
        internal_ipv6_ = ipv6;
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

std::unique_ptr<ChildProcess> AppRunner::start_server(const AppSpec& app, Port port, std::string& shown, bool& ipv6,
                                                      std::string& error) {
    std::error_code ec;
    if (fs::file_size(log_, ec) > kMaxLogBytes && !ec) {
        fs::remove(log_, ec);
    }
    ipv6 = false;
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

    // Setup first: the project's packages (a fresh clone has none), and
    // for Python its .venv.
    auto steps = app_setup(app, port, error);
    if (!steps) {
        append_line(log_, "--- FloServer: can't set up: " + error);
        return nullptr;
    }
    // One budget for all of setup, inside nginx's wait for the first answer.
    const auto setup_deadline = Clock::now() + config_.install_limit;
    for (AppSetupStep& step : *steps) {
        append_line(log_, "--- FloServer: " + step.launch.shown);
        step.launch.options.working_dir = app.dir;
        step.launch.options.output_log = log_;
        auto child = ChildProcess::start(step.launch.options, error);
        if (!child) {
            return nullptr;
        }
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(setup_deadline - Clock::now());
        const auto code = wait_for(*child, std::max(left, std::chrono::seconds(1)));
        if (code != 0) {
            if (step.failed) {
                step.failed();
            }
            error = step.launch.shown + (code ? " failed:\n" + tail(log_) : std::string(" didn't finish"));
            return nullptr;
        }
        if (step.done) {
            step.done();
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
        // 127.0.0.1, or [::1] for a server that listens on "localhost"
        // where that means IPv6 first (Node 17+ on Windows).
        for (const bool v6 : {false, true}) {
            if (const auto s = connect_loopback(port, v6)) {
                close_socket(*s);
                ipv6 = v6;
                return child;
            }
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
    // A server kept running for days: its log is cut short (the server's
    // own handle writes on at the new end).
    std::error_code ec;
    if (state_ == AppState::Running && fs::file_size(log_, ec) > kMaxLogBytes && !ec) {
        fs::resize_file(log_, 0, ec);
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

void AppRunner::close_soon() {
    std::unique_lock lock(mutex_);
    closed_ = true;
    stop_process(lock);  // its connections end with it
}

void AppRunner::close() {
    close_soon();
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
           "X-Content-Type-Options: nosniff\r\n"
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

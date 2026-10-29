// Projects with their own server (D11): how they start, and on POSIX the
// runner with real programs: wake on a request, sleep when idle, a server
// that can't start answering why, Keep running, and nothing left after close.

#include "floserver/core/app_runner.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace floserver::core;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

using Args = std::vector<std::string>;

// A Node install laid out as on this platform, with empty files.
fs::path fake_node(const fs::path& dir) {
#ifdef _WIN32
    test_util::write_file(dir / "node.exe", "");
    test_util::write_file(dir / "node_modules" / "npm" / "bin" / "npm-cli.js", "");
#else
    test_util::write_file(dir / "bin" / "node", "");
    test_util::write_file(dir / "lib" / "node_modules" / "npm" / "bin" / "npm-cli.js", "");
#endif
    return dir;
}

// A Python install laid out as on this platform, with an empty program.
fs::path fake_python(const fs::path& dir) {
#ifdef _WIN32
    test_util::write_file(dir / "python.exe", "");
#else
    test_util::write_file(dir / "bin" / "python3", "");
#endif
    return dir;
}

std::string env_of(const AppLaunch& l, const std::string& name) {
    for (const auto& [k, v] : l.options.env) {
        if (k == name) {
            return v;
        }
    }
    return "<none>";
}

}  // namespace

TEST_CASE("dev servers that ignore PORT are given --port") {
    CHECK(port_arguments("vite", 9601) == Args{"--port", "9601", "--strictPort", "--host", "127.0.0.1"});
    CHECK(port_arguments("tsc -b && vite --mode dev", 9601) ==
          Args{"--port", "9601", "--strictPort", "--host", "127.0.0.1"});
    CHECK(port_arguments("cross-env NODE_ENV=development next dev --turbo", 9601) ==
          Args{"--port", "9601", "--hostname", "127.0.0.1"});
    CHECK(port_arguments("./node_modules/.bin/astro dev", 7) == Args{"--port", "7", "--host", "127.0.0.1"});
    CHECK(port_arguments("nuxt dev", 7) == Args{"--port", "7", "--host", "127.0.0.1"});
    CHECK(port_arguments("ng serve", 7) == Args{"--port", "7", "--host", "127.0.0.1"});
    // The script names a port itself, or reads PORT: nothing added.
    CHECK(port_arguments("vite --port 3000", 7).empty());
    CHECK(port_arguments("vite --port=3000", 7).empty());
    CHECK(port_arguments("node server.js", 7).empty());
    CHECK(port_arguments("nodemon index.js", 7).empty());
    CHECK(port_arguments("vite build && node dist/server.js", 7).empty());  // the last command counts
}

TEST_CASE("an app starts from floserver.json's command or package.json's scripts") {
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "web";
    app.dir = tmp.path() / "web";
    app.runtime_dir = fake_node(tmp.path() / "node");
    fs::create_directories(app.dir);
    std::string error;

    SUBCASE("a command, with {port} filled in, through the shell") {
        app.command = "npm run serve -- --port {port} # {port}";
        const auto l = app_launch(app, 9601, error);
        REQUIRE(l);
        CHECK(l->shown == "npm run serve -- --port 9601 # 9601");
#ifdef _WIN32
        CHECK(l->options.program.filename() == "cmd.exe");
        CHECK(l->options.windows_raw_args == "/d /s /c \"npm run serve -- --port 9601 # 9601\"");
#else
        CHECK(l->options.program == "/bin/sh");
        CHECK(l->options.args == Args{"-c", "npm run serve -- --port 9601 # 9601"});
#endif
        CHECK(env_of(*l, "PORT") == "9601");
        CHECK(env_of(*l, "HOST") == "127.0.0.1");
        CHECK(env_of(*l, "BROWSER") == "none");
        // The project's Node comes first on PATH.
        const auto node = node_program(app.runtime_dir);
        REQUIRE(node);
        CHECK(env_of(*l, "PATH").starts_with(path_to_utf8(node->parent_path())));
    }
    SUBCASE("the dev script, through the project's own npm") {
        test_util::write_file(app.dir / "package.json", R"({"scripts": {"start": "node x.js", "dev": "vite"}})");
        const auto l = app_launch(app, 9602, error);
        REQUIRE(l);
        CHECK(l->options.program == *node_program(app.runtime_dir));
        CHECK(l->options.args == Args{path_to_utf8(*npm_script(app.runtime_dir)), "run", "dev", "--", "--port", "9602",
                                      "--strictPort", "--host", "127.0.0.1"});
        CHECK(l->shown == "npm run dev -- --port 9602 --strictPort --host 127.0.0.1");
    }
    SUBCASE("else the start script") {
        test_util::write_file(app.dir / "package.json", R"({"scripts": {"start": "node server.js"}})");
        const auto l = app_launch(app, 9602, error);
        REQUIRE(l);
        CHECK(l->options.args == Args{path_to_utf8(*npm_script(app.runtime_dir)), "run", "start"});
        CHECK(l->shown == "npm run start");
    }
    SUBCASE("what's missing is said plainly") {
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error.find("no package.json") != std::string::npos);
        test_util::write_file(app.dir / "package.json", R"({"scripts": {"build": "vite build"}})");
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error.find("no \"dev\" or \"start\" script") != std::string::npos);
        test_util::write_file(app.dir / "package.json", "{oops");
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error == "package.json isn't valid JSON");
        test_util::write_file(app.dir / "package.json", R"({"scripts": {"dev": "vite"}})");
        app.runtime_dir = tmp.path() / "no-node";
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error.starts_with("node or npm is missing"));
    }
}


TEST_CASE("a Python app starts as Django, FastAPI or Flask, from its .venv") {
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "api";
    app.kind = AppKind::Python;
    app.version = "3.13.18";
    app.dir = tmp.path() / "api";
    app.runtime_dir = fake_python(tmp.path() / "python");
    fs::create_directories(app.dir);
    std::string error;

    SUBCASE("FastAPI in main.py: uvicorn with --reload") {
        test_util::write_file(app.dir / "main.py", "from fastapi import FastAPI\napp = FastAPI()\n");
        const auto l = app_launch(app, 9601, error);
        REQUIRE(l);
        CHECK(l->options.program == venv_python(app.dir));
        CHECK(l->options.args == Args{"-m", "uvicorn", "main:app", "--reload", "--host", "127.0.0.1", "--port", "9601"});
        CHECK(l->shown == "uvicorn main:app --reload --host 127.0.0.1 --port 9601");
        CHECK(env_of(*l, "VIRTUAL_ENV") == path_to_utf8(app.dir / ".venv"));
        CHECK(env_of(*l, "PATH").starts_with(path_to_utf8(venv_python(app.dir).parent_path())));
        CHECK(env_of(*l, "PYTHONUNBUFFERED") == "1");
        CHECK(std::find(l->options.remove_env.begin(), l->options.remove_env.end(), "PYTHONPATH") !=
              l->options.remove_env.end());
    }
    SUBCASE("FastAPI in app/main.py") {
        test_util::write_file(app.dir / "app" / "main.py", "from fastapi import FastAPI\napp = FastAPI()\n");
        const auto l = app_launch(app, 9601, error);
        REQUIRE(l);
        CHECK(l->options.args[2] == "app.main:app");
    }
    SUBCASE("Django: manage.py runserver") {
        test_util::write_file(app.dir / "manage.py", "");
        test_util::write_file(app.dir / "main.py", "app = FastAPI()");  // manage.py wins
        const auto l = app_launch(app, 9602, error);
        REQUIRE(l);
        CHECK(l->options.args == Args{"manage.py", "runserver", "127.0.0.1:9602"});
    }
    SUBCASE("Flask in app.py: flask run --debug") {
        test_util::write_file(app.dir / "app.py", "from flask import Flask\napp = Flask(__name__)\n");
        const auto l = app_launch(app, 9603, error);
        REQUIRE(l);
        CHECK(l->options.args == Args{"-m", "flask", "--app", "app", "run", "--debug", "--host", "127.0.0.1", "--port", "9603"});
    }
    SUBCASE("FastAPI in app.py; a mention in a comment isn't an app") {
        test_util::write_file(app.dir / "main.py", "# not a FastAPI app\n");
        test_util::write_file(app.dir / "app.py", "from fastapi import FastAPI\napp = FastAPI()\n");
        const auto l = app_launch(app, 9601, error);
        REQUIRE(l);
        CHECK(l->options.args[2] == "app:app");
    }
    SUBCASE("a Python app that names Node has that Node on PATH") {
        test_util::write_file(app.dir / "manage.py", "");
        app.node_dir = fake_node(app.dir.parent_path() / "node");
        const auto l = app_launch(app, 9601, error);
        REQUIRE(l);
        CHECK(env_of(*l, "PATH").find(path_to_utf8(node_program(app.node_dir)->parent_path())) != std::string::npos);
        CHECK(env_of(*l, "PYTHONUTF8") == "1");
    }
    SUBCASE("nothing to go on: a command is asked for") {
        test_util::write_file(app.dir / "script.py", "print(1)\n");
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error.find("add a \"command\"") != std::string::npos);
        app.command = "python script.py --port {port}";
        const auto l = app_launch(app, 9604, error);
        REQUIRE(l);
        CHECK(l->shown == "python script.py --port 9604");
        CHECK(env_of(*l, "VIRTUAL_ENV") == path_to_utf8(app.dir / ".venv"));
    }
}

TEST_CASE("a Python app's setup: .venv, then its packages when the list changes") {
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "api";
    app.kind = AppKind::Python;
    app.version = "3.13.18";
    app.dir = tmp.path() / "api";
    app.runtime_dir = fake_python(tmp.path() / "python");
    fs::create_directories(app.dir);
    std::string error;

    auto steps = app_setup(app, 9601, error);
    REQUIRE(steps);
    REQUIRE(steps->size() == 1);
    CHECK((*steps)[0].launch.shown == "python -m venv .venv");
    CHECK((*steps)[0].launch.options.program == *python_program(app.runtime_dir));
    CHECK((*steps)[0].launch.options.args == Args{"-m", "venv", ".venv"});
    // Cut short: what it made goes, so the next start makes it again.
    test_util::write_file(app.dir / ".venv" / "pyvenv.cfg", "version = 3.13.18\n");
    REQUIRE((*steps)[0].failed);
    (*steps)[0].failed();
    CHECK_FALSE(fs::exists(app.dir / ".venv"));

    // A .venv made with this Python, and a requirements.txt: pip, once.
    const std::string home = "home = " + path_to_utf8(python_program(app.runtime_dir)->parent_path()) + "\n";
    test_util::write_file(app.dir / ".venv" / "pyvenv.cfg", home + "version = 3.13.17\n");
    test_util::write_file(venv_python(app.dir), "");
    test_util::write_file(app.dir / "requirements.txt", "fastapi\nuvicorn\n");
    steps = app_setup(app, 9601, error);
    REQUIRE(steps);
    REQUIRE(steps->size() == 1);
    CHECK((*steps)[0].launch.shown == "pip install -r requirements.txt");
    CHECK((*steps)[0].launch.options.program == venv_python(app.dir));
    REQUIRE((*steps)[0].done);
    (*steps)[0].done();
    CHECK(app_setup(app, 9601, error)->empty());
    test_util::write_file(app.dir / "requirements.txt", "fastapi\nuvicorn\nsqlalchemy\n");
    CHECK(app_setup(app, 9601, error)->size() == 1);  // the list changed

    // pyproject.toml when there's no requirements.txt, and only as a
    // package: tool settings or an app without a build system aren't.
    fs::remove(app.dir / "requirements.txt");
    test_util::write_file(app.dir / "pyproject.toml", "[project]\nname = 'api'\ndependencies = ['django']\n");
    CHECK(app_setup(app, 9601, error)->empty());
    test_util::write_file(app.dir / "pyproject.toml",
                          "[build-system]\nrequires = ['setuptools']\n[project]\nname = 'api'\n");
    steps = app_setup(app, 9601, error);
    REQUIRE(steps);
    REQUIRE(steps->size() == 1);
    CHECK((*steps)[0].launch.shown == "pip install -e .");

    // Made with another install of the same version (a patch update): moved over.
    test_util::write_file(app.dir / ".venv" / "pyvenv.cfg", "home = " + path_to_utf8(tmp.path() / "python-old") +
                                                                "\nversion = 3.13.17\n");
    steps = app_setup(app, 9601, error);
    REQUIRE(steps);
    REQUIRE_FALSE(steps->empty());
    CHECK((*steps)[0].launch.shown == "python -m venv --upgrade .venv");

    // Another Python's .venv is never replaced on its own.
    test_util::write_file(app.dir / ".venv" / "pyvenv.cfg", "version_info = 3.12.9\n");
    CHECK_FALSE(app_setup(app, 9601, error));
    CHECK(error.find("made with Python 3.12") != std::string::npos);
    CHECK(error.find("delete the .venv folder") != std::string::npos);
    fs::remove(app.dir / ".venv" / "pyvenv.cfg");
    CHECK_FALSE(app_setup(app, 9601, error));
    CHECK(error.find("isn't a Python virtual environment") != std::string::npos);
    CHECK(fs::exists(app.dir / ".venv"));  // left as it was
}

#ifndef _WIN32

namespace {

// GET over plain http; "<status> <body>", or "" when it couldn't connect.
std::string get(Port port, const std::string& path = "/") {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeval tv{30, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        close(s);
        return "";
    }
    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: web.localhost\r\n\r\n";
    (void)send(s, req.data(), req.size(), 0);
    std::string resp;
    char buf[4096];  // NOLINT(modernize-avoid-c-arrays)
    for (ssize_t n = 0; (n = recv(s, buf, sizeof buf, 0)) > 0;) {
        resp.append(buf, static_cast<std::size_t>(n));
    }
    close(s);
    const auto sp = resp.find(' ');
    const auto body = resp.find("\r\n\r\n");
    if (sp == std::string::npos || body == std::string::npos) {
        return "";
    }
    return resp.substr(sp + 1, 3) + " " + resp.substr(body + 4);
}

bool alive(std::uint32_t pid) { return ::kill(static_cast<pid_t>(pid), 0) == 0; }

template <class F>
bool eventually(F&& f, std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (f()) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return f();
}

std::optional<fs::path> on_path(const std::string& name) {
    const auto path = get_env("PATH").value_or("");
    std::size_t from = 0;
    while (from <= path.size()) {
        const auto colon = path.find(':', from);
        const std::string dir = path.substr(from, colon == std::string::npos ? std::string::npos : colon - from);
        std::error_code ec;
        if (!dir.empty() && fs::is_regular_file(fs::path(dir) / name, ec)) {
            return fs::canonical(fs::path(dir) / name, ec);
        }
        if (colon == std::string::npos) {
            break;
        }
        from = colon + 1;
    }
    return std::nullopt;
}

// Takes the runner's connections the way the web service's supervisor does.
class Pump {
public:
    explicit Pump(std::shared_ptr<AppRunner> runner) : runner_(std::move(runner)) {
        thread_ = std::thread([this] {
            while (!stop_) {
                const std::vector<const LoopbackListener*> l = {&runner_->listener()};
                if (!waiting_connections(l, 50ms).empty()) {
                    runner_->accept_waiting();
                }
            }
        });
    }
    Pump(const Pump&) = delete;
    Pump& operator=(const Pump&) = delete;
    Pump(Pump&&) = delete;
    Pump& operator=(Pump&&) = delete;
    ~Pump() {
        stop_ = true;
        thread_.join();
    }

private:
    std::shared_ptr<AppRunner> runner_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

Port free_port(Port from) { return pick_port_from(from, {}, can_listen_on_loopback).value_or(0); }

}  // namespace

TEST_CASE("an app's server starts on the first request, sleeps when idle and wakes again") {
    if (!on_path("python3")) {
        MESSAGE("python3 isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "web";
    app.dir = tmp.path() / "web";
    app.runtime_dir = fake_node(tmp.path() / "node");
    app.command = "exec python3 -m http.server {port} --bind 127.0.0.1";
    app.port = free_port(19300);
    test_util::write_file(app.dir / "hello.txt", "hello from web");
    std::string error;
    auto runner = AppRunner::open(app, {tmp.path() / "work", nullptr, nullptr}, error);
    REQUIRE_MESSAGE(runner, error);
    const Pump pump(runner);

    CHECK(runner->state() == AppState::Sleeping);
    CHECK(get(app.port, "/hello.txt") == "200 hello from web");
    CHECK(runner->state() == AppState::Running);
    auto status = runner->describe();
    REQUIRE(status.contains("pid"));
    const auto pid = status["pid"].get<std::uint32_t>();
    CHECK(alive(pid));
    CHECK(status["command"] == "exec python3 -m http.server " + std::to_string(status["internal_port"].get<int>()) +
                                   " --bind 127.0.0.1");

    // Requests while it runs, several at once.
    std::vector<std::thread> burst;
    burst.reserve(10);
    std::atomic<int> ok{0};
    for (int i = 0; i < 10; ++i) {
        burst.emplace_back([&] { ok += get(app.port, "/hello.txt") == "200 hello from web" ? 1 : 0; });
    }
    for (auto& t : burst) {
        t.join();
    }
    CHECK(ok == 10);

    // Idle: no connection, and the idle time passed.
    CHECK(eventually([&] { return runner->describe()["connections"] == 0; }));
    runner->tick(AppRunner::Clock::now(), 3600s);
    CHECK(runner->state() == AppState::Running);
    runner->tick(AppRunner::Clock::now() + 2s, 1s);
    CHECK(runner->state() == AppState::Sleeping);
    CHECK(eventually([&] { return !alive(pid); }));

    // The next request wakes it.
    CHECK(get(app.port, "/hello.txt") == "200 hello from web");
    CHECK(runner->state() == AppState::Running);

    // A server that ends on its own sleeps; the next request starts it.
    const auto pid2 = runner->describe()["pid"].get<std::uint32_t>();
    ::kill(static_cast<pid_t>(pid2), SIGKILL);
    CHECK(eventually([&] {
        runner->tick(AppRunner::Clock::now() + 20s, 3600s);  // not "right after starting"
        return runner->state() == AppState::Sleeping;
    }));
    CHECK(runner->describe()["error"].get<std::string>().find("stopped") != std::string::npos);
    CHECK(get(app.port, "/hello.txt") == "200 hello from web");

    runner->close();
    const auto pid3 = runner->describe().value("pid", 0U);
    CHECK(pid3 == 0);
    CHECK(get(app.port, "/hello.txt").starts_with("502 "));  // closed: says the project is off
}

TEST_CASE("an app that can't start answers why, and Keep running starts it without a request") {
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "web";
    app.dir = tmp.path() / "web";
    app.runtime_dir = fake_node(tmp.path() / "node");
    app.command = "echo cannot find module express; exit 3";
    app.port = free_port(19320);
    fs::create_directories(app.dir);
    std::string error;
    auto runner = AppRunner::open(app, {tmp.path() / "work", nullptr, nullptr}, error);
    REQUIRE(runner);
    const Pump pump(runner);

    const std::string page = get(app.port);
    CHECK(page.starts_with("502 FloServer: web's server isn't running."));
    CHECK(page.find("ended (exit 3)") != std::string::npos);
    CHECK(page.find("cannot find module express") != std::string::npos);  // the log's end
    CHECK(runner->state() == AppState::Failed);
    CHECK(runner->describe()["error"].get<std::string>().find("exit 3") != std::string::npos);
    // Within the retry wait the answer comes at once, without a new try.
    CHECK(get(app.port).find("ended (exit 3)") != std::string::npos);

    // A fixed command takes effect at once; Keep running starts it.
    if (on_path("python3")) {
        app.command = "exec python3 -m http.server {port} --bind 127.0.0.1";
        app.keep_running = true;
        runner->set_app(app);
        CHECK(runner->state() == AppState::Sleeping);
        runner->tick(AppRunner::Clock::now(), 1s);
        CHECK(eventually([&] { return runner->state() == AppState::Running; }));
        runner->tick(AppRunner::Clock::now() + 3600s, 1s);  // never idle
        CHECK(runner->state() == AppState::Running);
    }
    runner->close();
    CHECK(runner->state() == AppState::Sleeping);
}

TEST_CASE("settings changed while starting: the old start is thrown away") {
    if (!on_path("python3")) {
        MESSAGE("python3 isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "web";
    app.dir = tmp.path() / "web";
    app.runtime_dir = fake_node(tmp.path() / "node");
    test_util::write_file(app.dir / "a" / "x.txt", "old");
    test_util::write_file(app.dir / "b" / "x.txt", "new");
    app.command = "sleep 1; exec python3 -m http.server {port} --bind 127.0.0.1 --directory a";
    app.port = free_port(19360);
    std::string error;
    auto runner = AppRunner::open(app, {tmp.path() / "work", nullptr, nullptr}, error);
    REQUIRE(runner);
    const Pump pump(runner);
    std::string first;
    std::thread t([&] { first = get(app.port, "/x.txt"); });
    REQUIRE(eventually([&] { return runner->state() == AppState::Starting; }, 5s));
    app.command = "exec python3 -m http.server {port} --bind 127.0.0.1 --directory b";
    runner->set_app(app);
    t.join();
    CHECK(first.find("settings changed") != std::string::npos);
    CHECK(get(app.port, "/x.txt") == "200 new");
    runner->close();
}

TEST_CASE("a server that listens on [::1] only is found there") {
    if (!on_path("python3") || !loopback_ipv6_available()) {
        MESSAGE("python3 or IPv6 isn't available here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "web";
    app.dir = tmp.path() / "web";
    app.runtime_dir = fake_node(tmp.path() / "node");
    test_util::write_file(app.dir / "x.txt", "six");
    app.command = "exec python3 -m http.server {port} --bind ::1";
    app.port = free_port(19380);
    std::string error;
    auto runner = AppRunner::open(app, {tmp.path() / "work", nullptr, nullptr}, error);
    REQUIRE(runner);
    const Pump pump(runner);
    CHECK(get(app.port, "/x.txt") == "200 six");
    runner->close();
}

TEST_CASE("a Node app runs its dev script through the project's own npm") {
    const auto node = on_path("node");
    if (!node || !npm_script(node->parent_path().parent_path())) {
        MESSAGE("node with npm isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "web";
    app.dir = tmp.path() / "web";
    app.runtime_dir = node->parent_path().parent_path();
    app.port = free_port(19340);
    test_util::write_file(app.dir / "package.json", R"({"name": "web", "private": true,
        "scripts": {"dev": "node server.js"}})");
    test_util::write_file(app.dir / "server.js", R"(
        require("http").createServer((req, res) => {
            res.end("node " + process.version + " " + req.url + " " + req.headers.host);
        }).listen(Number(process.env.PORT), process.env.HOST);
    )");
    std::string error;
    auto runner = AppRunner::open(app, {tmp.path() / "work", nullptr, nullptr}, error);
    REQUIRE(runner);
    const Pump pump(runner);
    const std::string body = get(app.port, "/cart?x=1");
    CHECK_MESSAGE(body.starts_with("200 node v"), body);
    CHECK(body.ends_with(" /cart?x=1 web.localhost"));
    CHECK(runner->describe()["command"] == "npm run dev");
    CHECK_FALSE(fs::exists(app.dir / "node_modules"));  // nothing to install
    runner->close();
}


TEST_CASE("a Python app gets its .venv and packages, then runs from it") {
    const auto python = on_path("python3");
    if (!python) {
        MESSAGE("python3 isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    // This machine's Python as an install.
    const fs::path runtime = tmp.path() / "python";
    fs::create_directories(runtime / "bin");
    fs::create_symlink(*python, runtime / "bin" / "python3");
    AppSpec app;
    app.name = "api";
    app.kind = AppKind::Python;
    // The install's version, as the runtime list would give it.
    std::string version;
    if (FILE* pipe = popen((python->string() + " -c \"import platform; print(platform.python_version())\"").c_str(),
                           "r")) {
        std::array<char, 64> buf{};
        while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
            version += buf.data();
        }
        pclose(pipe);
    }
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) {
        version.pop_back();
    }
    REQUIRE_FALSE(version.empty());
    app.version = version;
    app.dir = tmp.path() / "api";
    app.runtime_dir = runtime;
    app.port = free_port(19400);
    test_util::write_file(app.dir / "requirements.txt", "");
    test_util::write_file(app.dir / "main.py", "from fastapi import FastAPI\napp = FastAPI()\n");
    // Stands in for uvicorn: serves on the port it's given, says what it was asked.
    test_util::write_file(app.dir / "uvicorn" / "__init__.py", "");
    test_util::write_file(app.dir / "uvicorn" / "__main__.py", R"(
import http.server, os, sys
args = sys.argv[1:]
port = int(args[args.index("--port") + 1])
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = ("%s %s %s" % (args[0], "--reload" in args, os.environ.get("VIRTUAL_ENV", "")[-5:])).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
http.server.HTTPServer(("127.0.0.1", port), H).serve_forever()
)");
    std::string error;
    auto runner = AppRunner::open(app, {tmp.path() / "work", nullptr, nullptr}, error);
    REQUIRE(runner);
    const Pump pump(runner);
    const std::string body = get(app.port, "/");
    INFO(read_small_file(runner->log_file(), 1 << 20).value_or(""));
    CHECK(body == "200 main:app True .venv");
    CHECK(fs::exists(venv_python(app.dir)));
    CHECK(fs::exists(app.dir / ".venv" / ".floserver-installed"));

    // Asleep and woken: no new venv, no new pip run.
    runner->tick(AppRunner::Clock::now() + 3600s, 1s);
    CHECK(eventually([&] { return runner->state() == AppState::Sleeping; }));
    CHECK(get(app.port, "/") == "200 main:app True .venv");
    const std::string log = read_small_file(runner->log_file(), 1 << 20).value_or("");
    std::size_t pips = 0;
    for (auto at = log.find("--- FloServer: pip install"); at != std::string::npos;
         at = log.find("--- FloServer: pip install", at + 1)) {
        ++pips;
    }
    CHECK(pips == 1);
    CHECK(log.find("--upgrade") == std::string::npos);  // the same install: nothing to move over
    runner->close();
}

#endif

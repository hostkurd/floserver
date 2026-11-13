// Projects with their own server (D11): how they start, and on POSIX the
// runner with real programs: wake on a request, sleep when idle, a server
// that can't start answering why, Keep running, and nothing left after close.

#include "floserver/core/app_runner.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <atomic>
#include <cstdio>
#include <sstream>
#include <fstream>
#include <cstring>
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

// A Bun or Deno install: one program (D31), empty here.
fs::path fake_single(const fs::path& dir, const std::string& program) {
#ifdef _WIN32
    test_util::write_file(dir / (program + ".exe"), "");
#else
    test_util::write_file(dir / program, "");
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
    // Bun and Deno runners, options and package names (D31).
    const Args vite = {"--port", "7", "--strictPort", "--host", "127.0.0.1"};
    CHECK(port_arguments("bunx --bun vite", 7) == vite);
    CHECK(port_arguments("bun --bun run vite", 7) == vite);
    CHECK(port_arguments("bun x vite", 7) == vite);
    CHECK(port_arguments("bun run vite --mode dev", 7) == vite);
    CHECK(port_arguments("deno run -A npm:vite", 7) == vite);
    CHECK(port_arguments("deno run --allow-net=0.0.0.0 --allow-read npm:vite@5.4.1 --open", 7) == vite);
    CHECK(port_arguments("deno task vite", 7) == vite);
    CHECK(port_arguments("npx -y vite@latest", 7) == vite);
    CHECK(port_arguments("deno run -A jsr:@std/path", 7).empty());  // some other package
    CHECK(port_arguments("deno run -A npm:@sveltejs/kit/bin/vite", 7) == vite);
    CHECK(port_arguments("bun --hot index.ts", 7).empty());
    CHECK(port_arguments("deno run --watch main.ts", 7).empty());
    CHECK(port_arguments("bunx --bun", 7).empty());
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

TEST_CASE("a Bun app starts from its dev script, else bun --hot on the first entry file (D31)") {
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "edge";
    app.kind = AppKind::Bun;
    app.dir = tmp.path() / "edge";
    app.runtime_dir = fake_single(tmp.path() / "bun", "bun");
    fs::create_directories(app.dir);
    std::string error;

    SUBCASE("the dev script, through the project's own bun") {
        test_util::write_file(app.dir / "package.json", R"({"scripts": {"start": "bun x.ts", "dev": "bun --watch x.ts"}})");
        test_util::write_file(app.dir / "index.ts", "");  // the script wins
        const auto l = app_launch(app, 9601, error);
        REQUIRE_MESSAGE(l, error);
        CHECK(l->options.program == *bun_program(app.runtime_dir));
        CHECK(l->options.args == Args{"run", "dev"});
        CHECK(l->shown == "bun run dev");
        CHECK(env_of(*l, "PORT") == "9601");
        CHECK(env_of(*l, "HOST") == "127.0.0.1");
        CHECK(env_of(*l, "BUN_CONFIG_NO_CLEAR_TERMINAL_ON_RELOAD") == "true");
        // The project's Bun comes first on PATH.
        CHECK(env_of(*l, "PATH").starts_with(path_to_utf8(bun_program(app.runtime_dir)->parent_path())));
    }
    SUBCASE("a dev script that ignores PORT is given --port, straight after the script's name") {
        test_util::write_file(app.dir / "package.json", R"({"scripts": {"dev": "vite"}})");
        const auto l = app_launch(app, 9602, error);
        REQUIRE(l);
        CHECK(l->options.args == Args{"run", "dev", "--port", "9602", "--strictPort", "--host", "127.0.0.1"});
        CHECK(l->shown == "bun run dev --port 9602 --strictPort --host 127.0.0.1");
    }
    SUBCASE("no dev script: bun --hot on the first of the entry files, in the documented order") {
        test_util::write_file(app.dir / "package.json", R"({"scripts": {"start": "bun server.ts"}})");  // only dev counts
        for (const char* entry : {"src/index.js", "src/index.ts", "server.js", "server.ts", "index.js", "index.ts"}) {
            test_util::write_file(app.dir / entry, "");
            const auto l = app_launch(app, 9603, error);
            REQUIRE_MESSAGE(l, error);
            CHECK(l->options.args == Args{"--hot", entry});
            CHECK(l->shown == std::string("bun --hot ") + entry);
        }
    }
    SUBCASE("without a package.json at all") {
        test_util::write_file(app.dir / "server.ts", "");
        const auto l = app_launch(app, 9604, error);
        REQUIRE(l);
        CHECK(l->shown == "bun --hot server.ts");
    }
    SUBCASE("a command from floserver.json wins, with {port} filled in, through the shell") {
        app.command = "bun run src/server.ts --port {port}";
        test_util::write_file(app.dir / "index.ts", "");
        const auto l = app_launch(app, 9605, error);
        REQUIRE(l);
        CHECK(l->shown == "bun run src/server.ts --port 9605");
#ifdef _WIN32
        CHECK(l->options.program.filename() == "cmd.exe");
#else
        CHECK(l->options.program == "/bin/sh");
        CHECK(l->options.args == Args{"-c", "bun run src/server.ts --port 9605"});
#endif
        CHECK(env_of(*l, "PATH").starts_with(path_to_utf8(bun_program(app.runtime_dir)->parent_path())));
    }
    SUBCASE("what's missing is said plainly") {
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error.find("can't tell how to start this Bun project") != std::string::npos);
        CHECK(error.find("index.ts, index.js, server.ts, server.js, src/index.ts, src/index.js") != std::string::npos);
        CHECK(error.find("\"command\" in floserver.json") != std::string::npos);
        test_util::write_file(app.dir / "package.json", "{oops");
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error == "package.json can't be read or isn't valid JSON");
        test_util::write_file(app.dir / "package.json", "{}");
        test_util::write_file(app.dir / "index.ts", "");
        app.runtime_dir = tmp.path() / "no-bun";
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error.starts_with("bun is missing from"));
    }
}

TEST_CASE("a Deno app starts from its dev task, else deno run --watch with only a web server's permissions (D31)") {
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "fresh";
    app.kind = AppKind::Deno;
    app.dir = tmp.path() / "fresh";
    app.runtime_dir = fake_single(tmp.path() / "deno", "deno");
    fs::create_directories(app.dir);
    std::string error;

    SUBCASE("deno task dev from deno.json") {
        test_util::write_file(app.dir / "deno.json",
                              R"({"tasks": {"start": "deno run main.ts", "dev": "deno run -A --watch main.ts"}})");
        test_util::write_file(app.dir / "main.ts", "");
        const auto l = app_launch(app, 9601, error);
        REQUIRE_MESSAGE(l, error);
        CHECK(l->options.program == *deno_program(app.runtime_dir));
        CHECK(l->options.args == Args{"task", "dev"});
        CHECK(l->shown == "deno task dev");
        CHECK(env_of(*l, "PORT") == "9601");
        CHECK(env_of(*l, "DENO_NO_UPDATE_CHECK") == "1");
        // Deno.serve ignores PORT: Deno's own address setting moves the first server it starts.
        CHECK(env_of(*l, "DENO_SERVE_ADDRESS") == "tcp:127.0.0.1:9601");
        CHECK(env_of(*l, "HOST") == "127.0.0.1");
        CHECK(env_of(*l, "PATH").starts_with(path_to_utf8(deno_program(app.runtime_dir)->parent_path())));
        // ... and only Deno's: Bun and Node have no such setting.
        AppSpec bun = app;
        bun.kind = AppKind::Bun;
        bun.runtime_dir = fake_single(tmp.path() / "bun", "bun");
        test_util::write_file(bun.dir / "index.ts", "");
        test_util::write_file(bun.dir / "package.json", "{}");
        const auto b = app_launch(bun, 9601, error);
        REQUIRE_MESSAGE(b, error);
        CHECK(std::none_of(b->options.env.begin(), b->options.env.end(),
                           [](const auto& kv) { return kv.first == "DENO_SERVE_ADDRESS"; }));
    }
    SUBCASE("a dev task of any shape is Deno's; package.json's script only when deno.json has none") {
        test_util::write_file(app.dir / "deno.json", R"({"tasks": {"dev": {"dependencies": ["build"]}}})");
        test_util::write_file(app.dir / "main.ts", "");
        test_util::write_file(app.dir / "package.json", R"({"scripts": {"dev": "vite"}})");
        auto l = app_launch(app, 9606, error);
        REQUIRE_MESSAGE(l, error);
        CHECK(l->options.args == Args{"task", "dev"});  // what Deno runs: deno.json's, not package.json's
        CHECK(l->shown == "deno task dev");
        CHECK(env_of(*l, "DENO_SERVE_ADDRESS") == "tcp:127.0.0.1:9606");
        // `deno task` runs package.json's scripts when deno.json has no such task.
        test_util::write_file(app.dir / "deno.json", R"({"tasks": {"start": "deno run main.ts"}})");
        l = app_launch(app, 9606, error);
        REQUIRE_MESSAGE(l, error);
        CHECK(l->options.args == Args{"task", "dev", "--port", "9606", "--strictPort", "--host", "127.0.0.1"});
        // A damaged package.json is not Deno's problem.
        test_util::write_file(app.dir / "package.json", "{oops");
        l = app_launch(app, 9606, error);
        REQUIRE_MESSAGE(l, error);
        CHECK(l->shown.starts_with("deno run --watch"));
    }
    SUBCASE("a framework's dev server gets its port as arguments and no DENO_SERVE_ADDRESS") {
        // That setting would reach the servers its child processes start, which would find the port taken.
        for (const char* script : {"vite", "deno run -A npm:vite@5", "next dev", "nuxt dev", "astro dev", "ng serve"}) {
            test_util::write_file(app.dir / "deno.json",
                                  nlohmann::json{{"tasks", {{"dev", script}}}}.dump());
            const auto l = app_launch(app, 9607, error);
            REQUIRE_MESSAGE(l, error);
            INFO(script);
            CHECK(l->options.args.size() > 2);  // task dev --port ...
            CHECK(env_of(*l, "PORT") == "9607");
            CHECK(std::none_of(l->options.env.begin(), l->options.env.end(),
                               [](const auto& kv) { return kv.first == "DENO_SERVE_ADDRESS"; }));
        }
        // A plain script keeps it.
        test_util::write_file(app.dir / "deno.json", R"({"tasks": {"dev": "deno run --watch main.ts"}})");
        const auto plain = app_launch(app, 9607, error);
        REQUIRE_MESSAGE(plain, error);
        CHECK(env_of(*plain, "DENO_SERVE_ADDRESS") == "tcp:127.0.0.1:9607");
    }
    SUBCASE("deno.json and deno.jsonc both there: Deno reads deno.json only") {
        test_util::write_file(app.dir / "deno.json", R"({"tasks": {"start": "deno run main.ts"}})");
        test_util::write_file(app.dir / "deno.jsonc", R"({"tasks": {"dev": "vite"}})");
        test_util::write_file(app.dir / "main.ts", "");
        const auto l = app_launch(app, 9608, error);
        REQUIRE_MESSAGE(l, error);
        CHECK(l->shown.starts_with("deno run --watch"));  // deno.jsonc's dev task isn't seen
        test_util::write_file(app.dir / "deno.json", "{oops");
        CHECK_FALSE(app_launch(app, 9608, error));
        CHECK(error == "deno.json can't be read or isn't valid JSON");
    }
    SUBCASE("deno.jsonc, with comments and a trailing comma; a task may be an object") {
        test_util::write_file(app.dir / "deno.jsonc", R"({
            // how this app starts
            "tasks": {
                "dev": {"command": "vite", "description": "serve, \"/*\" not a comment"}, /* done */
            },
        })");
        const auto l = app_launch(app, 9602, error);
        REQUIRE_MESSAGE(l, error);
        CHECK(l->options.args == Args{"task", "dev", "--port", "9602", "--strictPort", "--host", "127.0.0.1"});
        CHECK(l->shown == "deno task dev --port 9602 --strictPort --host 127.0.0.1");
    }
    SUBCASE("no dev task: deno run --watch on the first entry file, in order, never -A") {
        test_util::write_file(app.dir / "deno.json", R"({"tasks": {"start": "deno run main.ts"}, "imports": {}})");
        for (const char* entry : {"src/main.ts", "main.js", "server.ts", "main.ts"}) {
            test_util::write_file(app.dir / entry, "");
            const auto l = app_launch(app, 9603, error);
            REQUIRE_MESSAGE(l, error);
            CHECK(l->options.args == Args{"run", "--watch", "--allow-net", "--allow-read=.", "--allow-env", entry});
            CHECK(l->shown == std::string("deno run --watch --allow-net --allow-read=. --allow-env ") + entry);
            CHECK(std::find(l->options.args.begin(), l->options.args.end(), "-A") == l->options.args.end());
            CHECK(std::find(l->options.args.begin(), l->options.args.end(), "--allow-all") == l->options.args.end());
        }
    }
    SUBCASE("a command from floserver.json wins") {
        app.command = "deno run --allow-net=127.0.0.1:{port} main.ts";
        test_util::write_file(app.dir / "main.ts", "");
        const auto l = app_launch(app, 9604, error);
        REQUIRE(l);
        CHECK(l->shown == "deno run --allow-net=127.0.0.1:9604 main.ts");
    }
    SUBCASE("what's missing is said plainly") {
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error.find("can't tell how to start this Deno project") != std::string::npos);
        CHECK(error.find("main.ts, server.ts, main.js, src/main.ts") != std::string::npos);
        test_util::write_file(app.dir / "deno.json", "{oops");
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error == "deno.json can't be read or isn't valid JSON");
        fs::remove(app.dir / "deno.json");
        test_util::write_file(app.dir / "main.ts", "");
        app.runtime_dir = tmp.path() / "no-deno";
        CHECK_FALSE(app_launch(app, 1, error));
        CHECK(error.starts_with("deno is missing from"));
    }
}

TEST_CASE("Bun and Deno install nothing themselves; the packages are the project's to install (D31)") {
    const test_util::TempDir tmp;
    std::string error;
    for (const AppKind kind : {AppKind::Bun, AppKind::Deno}) {
        AppSpec app;
        app.name = "web";
        app.kind = kind;
        app.dir = tmp.path() / "web";
        app.runtime_dir = fake_single(tmp.path() / "rt", kind == AppKind::Bun ? "bun" : "deno");
        test_util::write_file(app.dir / "package.json", R"({"dependencies": {"hono": "^4"}})");
        const auto steps = app_setup(app, 9601, error);
        REQUIRE(steps);
        CHECK(steps->empty());  // no `bun install`, where Node would run npm install
    }
    // What the window says: package.json lists packages and node_modules isn't there.
    const fs::path dir = tmp.path() / "web";
    CHECK(needs_package_install(dir));
    fs::create_directories(dir / "node_modules");
    CHECK_FALSE(needs_package_install(dir));
    fs::remove_all(dir / "node_modules");
    test_util::write_file(dir / "package.json", R"({"scripts": {"dev": "bun index.ts"}})");
    CHECK_FALSE(needs_package_install(dir));  // nothing listed
    test_util::write_file(dir / "package.json", "{oops");
    CHECK_FALSE(needs_package_install(dir));
    CHECK_FALSE(needs_package_install(tmp.path() / "nowhere"));
}

TEST_CASE("a Python app that names Bun or Deno has it on PATH (D31)") {
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "api";
    app.kind = AppKind::Python;
    app.version = "3.13.18";
    app.dir = tmp.path() / "api";
    app.runtime_dir = fake_python(tmp.path() / "python");
    test_util::write_file(app.dir / "manage.py", "");
    app.node_dir = fake_single(tmp.path() / "bun", "bun");
    app.tools_name = "bun";
    std::string error;
    const auto l = app_launch(app, 9601, error);
    REQUIRE_MESSAGE(l, error);
    CHECK(env_of(*l, "PATH").find(path_to_utf8(bun_program(app.node_dir)->parent_path())) != std::string::npos);
}

#ifndef _WIN32

namespace {

// GET over plain http; "<status> <body>", or "(no answer ...)" saying why
// there was none. `wait`: how long to wait for the answer, which for a
// request that wakes a server includes its start (the runner holds the
// connection until the server takes connections, up to its start_limit).
std::string get(Port port, const std::string& path = "/", std::chrono::seconds wait = 30s) {
    const auto started = std::chrono::steady_clock::now();
    const auto no_answer = [&](const std::string& why) {
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        return "(no answer after " + std::to_string(ms) + " ms: " + why + ")";
    };
    const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeval tv{static_cast<time_t>(wait.count()), 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        close(s);
        return no_answer("can't connect");
    }
    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: web.localhost\r\n\r\n";
    (void)send(s, req.data(), req.size(), 0);
    std::string resp;
    char buf[4096];  // NOLINT(modernize-avoid-c-arrays)
    ssize_t n = 0;
    // A signal or a stop of the whole process (EINTR) isn't the server's
    // answer: read on.
    while ((n = recv(s, buf, sizeof buf, 0)) > 0 || (n < 0 && errno == EINTR)) {
        if (n > 0) {
            resp.append(buf, static_cast<std::size_t>(n));
        }
    }
    const int recv_errno = n < 0 ? errno : 0;
    close(s);
    const auto sp = resp.find(' ');
    const auto body = resp.find("\r\n\r\n");
    if (sp == std::string::npos || body == std::string::npos) {
        if (recv_errno == EAGAIN || recv_errno == EWOULDBLOCK) {
            return no_answer("timed out, " + std::to_string(resp.size()) + " bytes");
        }
        return no_answer((recv_errno != 0 ? "connection reset (errno " + std::to_string(recv_errno) + ")" : std::string("connection closed")) + ", " +
                         std::to_string(resp.size()) + " bytes");
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

// A project's own port (AppSpec::port), from this test program's range too.
Port public_port(int slot) { return free_port(static_cast<Port>(test_util::app_port_base() + 100 + slot)); }

AppRunnerConfig runner_config(const fs::path& work) {
    AppRunnerConfig config;
    config.work_dir = work;
    config.first_internal_port = test_util::app_port_base();
    return config;
}

// `n` ports in a row from `from` up that nothing listens on (0 when none).
Port free_run(Port from, int n) {
    for (Port first = from; first < from + 500; ++first) {
        bool all = true;
        for (int i = 0; i < n && all; ++i) {
            all = can_listen_on_loopback(static_cast<Port>(first + i));
        }
        if (all) {
            return first;
        }
    }
    return 0;
}

// What a failed check on a runner should say: its state, its log, and on
// Linux who listens on its internal port and who this is.
std::string runner_diagnostics(const AppRunner& runner, Port internal) {
    std::string text = runner.describe().dump() + "\n--- log:\n" +
                       read_small_file(runner.log_file(), 1 << 20).value_or("(none)");
#ifdef __linux__
    const auto proc_text = [](const char* path) {
        const std::ifstream in(path);
        std::ostringstream all;
        all << in.rdbuf();
        return all.str();
    };
    text += "\n--- this process: euid " + std::to_string(geteuid()) + "; listeners on 127.0.0.1:" +
            std::to_string(internal) + " (inode/uid):";
    for (const ProcListener& l : proc_net_listeners(proc_text("/proc/net/tcp"), proc_text("/proc/net/tcp6"), internal,
                                                    false)) {
        text += " " + std::to_string(l.inode) + "/" + std::to_string(l.uid);
    }
#else
    (void)internal;
#endif
    return text;
}

// Takes every connection waiting on `l` (another program's port); true when
// one of them sent anything, as a relayed request would. FloServer's own
// checks only connect and close.
bool got_a_request(const LoopbackListener& l) {
    bool sent = false;
    const std::vector<const LoopbackListener*> self = {&l};
    while (!waiting_connections(self, 0ms).empty()) {
        const auto c = l.accept_connection();
        if (!c) {
            break;
        }
        const auto data = receive_some(*c, 200ms);
        sent = sent || (data && !data->empty());
        close_socket(*c);
    }
    return sent;
}

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
    app.port = public_port(0);
    test_util::write_file(app.dir / "hello.txt", "hello from web");
    std::string error;
    auto runner = AppRunner::open(app, runner_config(tmp.path() / "work"), error);
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
    app.port = public_port(2);
    fs::create_directories(app.dir);
    std::string error;
    auto runner = AppRunner::open(app, runner_config(tmp.path() / "work"), error);
    REQUIRE(runner);
    const Pump pump(runner);

    const std::string page = get(app.port);
    CHECK_MESSAGE(page.starts_with("502 FloServer: web's server isn't running."), page);
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
    app.port = public_port(6);
    std::string error;
    auto runner = AppRunner::open(app, runner_config(tmp.path() / "work"), error);
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
    app.port = public_port(8);
    std::string error;
    auto runner = AppRunner::open(app, runner_config(tmp.path() / "work"), error);
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
    app.port = public_port(4);
    test_util::write_file(app.dir / "package.json", R"({"name": "web", "private": true,
        "scripts": {"dev": "node server.js"}})");
    test_util::write_file(app.dir / "server.js", R"(
        require("http").createServer((req, res) => {
            res.end("node " + process.version + " " + req.url + " " + req.headers.host);
        }).listen(Number(process.env.PORT), process.env.HOST);
    )");
    std::string error;
    auto runner = AppRunner::open(app, runner_config(tmp.path() / "work"), error);
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
    app.port = public_port(10);
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
    auto runner = AppRunner::open(app, runner_config(tmp.path() / "work"), error);
    REQUIRE(runner);
    const Pump pump(runner);
    const std::string body = get(app.port, "/");
    INFO(read_small_file(runner->log_file(), 1 << 20).value_or(""));
    CHECK(body == "200 main:app True .venv");
    CHECK(fs::exists(venv_python(app.dir)));
    CHECK(fs::exists(app.dir / ".venv" / ".floserver-installed"));

    // Asleep and woken: no new venv, no new pip run. The waking request is
    // held until the new server takes connections (the runner allows
    // start_limit for that), so one request is enough, given that long: on
    // CI's sanitizer machine the restart took longer than the 30 s this
    // client used to wait (main's run 183: the new uvicorn logged nothing).
    // Ticked until it sleeps: the last request's thread may still be ending
    // (connections > 0) at the first tick when the machine is busy.
    CHECK(eventually([&] {
        runner->tick(AppRunner::Clock::now() + 3600s, 1s);
        return runner->state() == AppState::Sleeping;
    }));
    const std::string woken = get(app.port, "/", 150s);
    INFO(runner->describe().dump());
    CHECK(woken == "200 main:app True .venv");
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

namespace {

// A Bun or Deno install to run for real: FLOSERVER_TEST_BUN_DIR or
// FLOSERVER_TEST_DENO_DIR (the folder with the program), else the one on the PATH.
std::optional<fs::path> real_js_install(const std::string& program, const char* env_name) {
    if (const auto dir = get_env(env_name)) {
        std::error_code ec;
        if (fs::is_regular_file(fs::path(*dir) / program, ec)) {
            return fs::path(*dir);
        }
    }
    if (const auto found = on_path(program)) {
        return found->parent_path();
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("a Bun app runs bun --hot on a real Bun, sleeps when idle and wakes again (D31)") {
    const auto install = real_js_install("bun", "FLOSERVER_TEST_BUN_DIR");
    if (!install) {
        MESSAGE("bun isn't installed here; skipped (set FLOSERVER_TEST_BUN_DIR to a folder with it)");
        return;
    }
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "edge";
    app.kind = AppKind::Bun;
    app.dir = tmp.path() / "edge";
    app.runtime_dir = *install;
    app.port = public_port(12);
    test_util::write_file(app.dir / "server.ts", R"(
        const message: string = "bun " + Bun.version;
        Bun.serve({
            port: Number(process.env.PORT),
            hostname: process.env.HOST,
            fetch(req) { return new Response(message + " " + new URL(req.url).pathname + " " + req.headers.get("host")); },
        });
    )");
    std::string error;
    auto runner = AppRunner::open(app, runner_config(tmp.path() / "work"), error);
    REQUIRE_MESSAGE(runner, error);
    const Pump pump(runner);
    const std::string body = get(app.port, "/cart?x=1", 60s);
    INFO(read_small_file(runner->log_file(), 1 << 20).value_or(""));
    CHECK_MESSAGE(body.starts_with("200 bun 1."), body);
    CHECK_MESSAGE(body.ends_with(" /cart web.localhost"), body);
    CHECK(runner->describe()["command"] == "bun --hot server.ts");
    CHECK_FALSE(fs::exists(app.dir / "node_modules"));  // FloServer ran no install

    // Ticked until it sleeps: the last request's thread may still be ending
    // (connections > 0) at the first tick when the machine is busy.
    CHECK(eventually([&] {
        runner->tick(AppRunner::Clock::now() + 3600s, 1s);
        return runner->state() == AppState::Sleeping;
    }));
    CHECK(get(app.port, "/again", 60s).ends_with(" /again web.localhost"));
    runner->close();
}

TEST_CASE("a Bun app runs its dev script, and Keep running starts it without a request (D31)") {
    const auto install = real_js_install("bun", "FLOSERVER_TEST_BUN_DIR");
    if (!install) {
        MESSAGE("bun isn't installed here; skipped (set FLOSERVER_TEST_BUN_DIR to a folder with it)");
        return;
    }
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "edge";
    app.kind = AppKind::Bun;
    app.dir = tmp.path() / "edge";
    app.runtime_dir = *install;
    app.keep_running = true;
    app.port = public_port(14);
    test_util::write_file(app.dir / "package.json", R"({"name": "edge", "private": true, "scripts": {"dev": "bun run app.js"}})");
    test_util::write_file(app.dir / "app.js", R"(
        Bun.serve({port: Number(process.env.PORT), hostname: "127.0.0.1", fetch: () => new Response("dev script")});
    )");
    std::string error;
    auto runner = AppRunner::open(app, runner_config(tmp.path() / "work"), error);
    REQUIRE_MESSAGE(runner, error);
    const Pump pump(runner);
    runner->wake();  // Keep running: started by the service without a request
    CHECK(eventually([&] { return runner->state() == AppState::Running; }, 60s));
    CHECK(runner->describe()["command"] == "bun run dev");
    CHECK(get(app.port, "/", 60s) == "200 dev script");
    runner->close();
}

TEST_CASE("a Deno app runs deno run --watch on a real Deno, with no -A (D31)") {
    const auto install = real_js_install("deno", "FLOSERVER_TEST_DENO_DIR");
    if (!install) {
        MESSAGE("deno isn't installed here; skipped (set FLOSERVER_TEST_DENO_DIR to a folder with it)");
        return;
    }
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "fresh";
    app.kind = AppKind::Deno;
    app.dir = tmp.path() / "fresh";
    app.runtime_dir = *install;
    app.port = public_port(16);
    // A plain Deno.serve(handler): no reading of PORT or HOST, as most apps are written; it
    // must still answer on FloServer's port, on 127.0.0.1. A local import checks that --allow-read=.
    // is enough for the app's own modules.
    test_util::write_file(app.dir / "lib" / "message.ts", R"(export const message: string = "deno " + Deno.version.deno;)");
    test_util::write_file(app.dir / "main.ts", R"(
        import { message } from "./lib/message.ts";
        Deno.serve((req) => new Response(message + " " + new URL(req.url).pathname + " " + req.headers.get("host")));
    )");
    std::string error;
    auto runner = AppRunner::open(app, runner_config(tmp.path() / "work"), error);
    REQUIRE_MESSAGE(runner, error);
    const Pump pump(runner);
    const std::string body = get(app.port, "/cart", 90s);
    INFO(read_small_file(runner->log_file(), 1 << 20).value_or(""));
    CHECK_MESSAGE(body.starts_with("200 deno 2."), body);
    CHECK_MESSAGE(body.ends_with(" /cart web.localhost"), body);
    CHECK(runner->describe()["command"] ==
          "deno run --watch --allow-net --allow-read=. --allow-env main.ts");
    // Loopback only: no socket in the listening table is bound to every address on the app's port.
    {
        const int app_port = runner->describe().at("internal_port").get<int>();  // the server's own, behind the relay
        char hex[16];  // NOLINT(modernize-avoid-c-arrays)
        (void)std::snprintf(hex, sizeof hex, "%04X", app_port);  // NOLINT(cppcoreguidelines-pro-type-vararg)
        std::ifstream table("/proc/net/tcp");
        std::string row;
        bool loopback = false;
        bool everywhere = false;
        while (std::getline(table, row)) {
            if (row.find(std::string(":") + hex + " ") == std::string::npos ||
                row.find(" 0A ") == std::string::npos) {  // 0A: listening
                continue;
            }
            loopback = loopback || row.find(" 0100007F:" + std::string(hex)) != std::string::npos;
            everywhere = everywhere || row.find(" 00000000:" + std::string(hex)) != std::string::npos;
        }
        CHECK(loopback);
        CHECK_FALSE(everywhere);
    }
    // Ticked until it sleeps: the last request's thread may still be ending
    // (connections > 0) at the first tick when the machine is busy.
    CHECK(eventually([&] {
        runner->tick(AppRunner::Clock::now() + 3600s, 1s);
        return runner->state() == AppState::Sleeping;
    }));
    CHECK(get(app.port, "/again", 90s).ends_with(" /again web.localhost"));
    runner->close();
}

TEST_CASE("a Deno app runs its dev task; a server that can't start says why (D31)") {
    const auto install = real_js_install("deno", "FLOSERVER_TEST_DENO_DIR");
    if (!install) {
        MESSAGE("deno isn't installed here; skipped (set FLOSERVER_TEST_DENO_DIR to a folder with it)");
        return;
    }
    const test_util::TempDir tmp;
    AppSpec app;
    app.name = "fresh";
    app.kind = AppKind::Deno;
    app.dir = tmp.path() / "fresh";
    app.runtime_dir = *install;
    app.port = public_port(18);
    test_util::write_file(app.dir / "deno.json", R"({"tasks": {"dev": "deno run --allow-net --allow-env server.ts"}})");
    test_util::write_file(app.dir / "server.ts", R"(
        Deno.serve({port: Number(Deno.env.get("PORT")), hostname: "127.0.0.1"}, () => new Response("dev task"));
    )");
    std::string error;
    auto runner = AppRunner::open(app, runner_config(tmp.path() / "work"), error);
    REQUIRE_MESSAGE(runner, error);
    {
        const Pump pump(runner);
        CHECK(get(app.port, "/", 90s) == "200 dev task");
        CHECK(runner->describe()["command"] == "deno task dev");
    }
    runner->close();

    // A script that throws: the page says so, and the log has Deno's words.
    AppSpec broken = app;
    broken.name = "broken";
    broken.port = public_port(20);
    test_util::write_file(broken.dir / "deno.json", R"({"tasks": {"dev": "deno run server.ts"}})");
    test_util::write_file(broken.dir / "server.ts", "throw new Error(\"boom from deno\");\n");
    auto bad = AppRunner::open(broken, runner_config(tmp.path() / "work"), error);
    REQUIRE_MESSAGE(bad, error);
    {
        const Pump pump(bad);
        const std::string page = get(broken.port, "/", 90s);
        CHECK(page.starts_with("502 "));
        CHECK(page.find("boom from deno") != std::string::npos);
    }
    bad->close();
}

TEST_CASE("another program on the app's internal port: the exit report, never a relay to it") {
    const test_util::TempDir tmp;
    const Port internal = free_run(static_cast<Port>(test_util::app_port_base() + 50), 2);
    REQUIRE(internal != 0);
    std::string error;
    // It started listening after FloServer found the port free.
    const auto other = LoopbackListener::open(internal, error);
    REQUIRE_MESSAGE(other, error);
    AppSpec app;
    app.name = "web";
    app.dir = tmp.path() / "web";
    app.runtime_dir = fake_node(tmp.path() / "node");
    app.command = "echo cannot find module express; exit 3";
    app.port = public_port(22);
    fs::create_directories(app.dir);
    auto config = runner_config(tmp.path() / "work");
    config.first_internal_port = internal;
    config.probe = [internal](Port p) { return p == internal || can_listen_on_loopback(p); };
    auto runner = AppRunner::open(app, config, error);
    REQUIRE(runner);
    const Pump pump(runner);

    const std::string page = get(app.port, "/", 10s);
    INFO(runner_diagnostics(*runner, internal));
    CHECK_MESSAGE(page.starts_with("502 FloServer: web's server isn't running."), page);
    CHECK(page.find("ended (exit 3)") != std::string::npos);
    CHECK(page.find("cannot find module express") != std::string::npos);
    CHECK(runner->state() == AppState::Failed);
    CHECK_FALSE(got_a_request(*other));
    runner->close();
}

#ifdef __linux__

TEST_CASE("a server still starting while another program has its port moves to the next port") {
    const test_util::TempDir tmp;
    const Port internal = free_run(static_cast<Port>(test_util::app_port_base() + 60), 4);
    REQUIRE(internal != 0);
    std::string error;
    const auto other = LoopbackListener::open(internal, error);
    REQUIRE_MESSAGE(other, error);
    AppSpec app;
    app.name = "web";
    app.dir = tmp.path() / "web";
    app.runtime_dir = fake_node(tmp.path() / "node");
    app.port = public_port(24);
    test_util::write_file(app.dir / "x.txt", "mine");
    auto config = runner_config(tmp.path() / "work");
    config.first_internal_port = internal;
    config.probe = [internal](Port p) { return p == internal || can_listen_on_loopback(p); };

    SUBCASE("it ends: the exit report from the next port") {
        // Alive while the other program answers on its port, then it ends.
        app.command = "sleep 1; echo cannot find module express; exit 3";
        auto runner = AppRunner::open(app, config, error);
        REQUIRE(runner);
        const Pump pump(runner);
        const std::string page = get(app.port, "/", 10s);
        INFO(runner_diagnostics(*runner, internal));
        CHECK_MESSAGE(page.starts_with("502 FloServer: web's server isn't running."), page);
        CHECK(page.find("ended (exit 3)") != std::string::npos);
        CHECK(page.find("port " + std::to_string(internal + 1)) != std::string::npos);
        CHECK_FALSE(got_a_request(*other));
        runner->close();
    }
    SUBCASE("it listens: served from the next port") {
        if (!on_path("python3")) {
            MESSAGE("python3 isn't installed here; skipped");
            return;
        }
        // Started by the shell, not exec'd: the listener is the child's child.
        app.command = "python3 -m http.server {port} --bind 127.0.0.1 & wait";
        auto runner = AppRunner::open(app, config, error);
        REQUIRE(runner);
        const Pump pump(runner);
        INFO(runner_diagnostics(*runner, internal + 1));
        CHECK(get(app.port, "/x.txt", 20s) == "200 mine");
        CHECK(runner->state() == AppState::Running);
        CHECK(runner->describe()["internal_port"].get<int>() == internal + 1);
        CHECK_FALSE(got_a_request(*other));
        runner->close();
    }
}

TEST_CASE("a server that never listens while other programs hold its ports says so") {
    const test_util::TempDir tmp;
    const Port internal = free_run(static_cast<Port>(test_util::app_port_base() + 70), 3);
    REQUIRE(internal != 0);
    std::string error;
    std::vector<std::unique_ptr<LoopbackListener>> others;
    for (Port p = internal; p < internal + 3; ++p) {
        others.push_back(LoopbackListener::open(p, error));
        REQUIRE_MESSAGE(others.back(), error);
    }
    AppSpec app;
    app.name = "web";
    app.dir = tmp.path() / "web";
    app.runtime_dir = fake_node(tmp.path() / "node");
    app.command = "exec sleep 60";
    app.port = public_port(26);
    fs::create_directories(app.dir);
    auto config = runner_config(tmp.path() / "work");
    config.first_internal_port = internal;
    config.probe = [internal](Port p) { return (p >= internal && p < internal + 3) || can_listen_on_loopback(p); };
    auto runner = AppRunner::open(app, config, error);
    REQUIRE(runner);
    const Pump pump(runner);

    const std::string page = get(app.port, "/", 20s);
    INFO(runner_diagnostics(*runner, internal + 2));
    CHECK_MESSAGE(page.starts_with("502 FloServer: web's server isn't running."), page);
    CHECK(page.find("port " + std::to_string(internal + 2) + " is used by another program") != std::string::npos);
    CHECK(runner->state() == AppState::Failed);
    CHECK_FALSE(runner->describe().contains("pid"));
    for (const auto& other : others) {
        CHECK_FALSE(got_a_request(*other));
    }
    runner->close();
}

TEST_CASE("who listens on a port: the child, what it started, or another program") {
    if (!on_path("python3")) {
        MESSAGE("python3 isn't installed here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    const Port port = free_run(static_cast<Port>(test_util::app_port_base() + 80), 1);
    REQUIRE(port != 0);
    const auto start = [&](const std::string& command) {
        ChildOptions o;
        o.program = "/bin/sh";
        o.args = {"-c", command};
        o.working_dir = tmp.path();
        std::string error;
        auto child = ChildProcess::start(o, error);
        REQUIRE_MESSAGE(child, error);
        return child;
    };
    const auto listening = [port] {
        return eventually([port] {
            const auto s = connect_loopback(port);
            if (s) {
                close_socket(*s);
            }
            return s.has_value();
        }, 20s);
    };

    auto idle = start("exec sleep 60");
    CHECK(idle->listener_owner(port, false) == ListenerOwner::Unknown);  // nobody listens
    {
        std::string error;
        const auto other = LoopbackListener::open(port, error);
        REQUIRE_MESSAGE(other, error);
        CHECK(idle->listener_owner(port, false) == ListenerOwner::Other);
    }
    idle->kill();

    const std::string server = "python3 -m http.server " + std::to_string(port) + " --bind 127.0.0.1";
    for (const std::string& command : {"exec " + server, server + " & wait"}) {
        CAPTURE(command);
        auto child = start(command);
        REQUIRE(listening());
        CHECK(child->listener_owner(port, false) == ListenerOwner::Child);
        CHECK(idle->listener_owner(port, false) == ListenerOwner::Other);
        child->kill();
    }
    if (on_path("setsid")) {
        // Out of the child's process group, but still its descendant.
        auto child = start("setsid " + server + " & echo $! > pid; wait");
        REQUIRE(listening());
        CHECK(child->listener_owner(port, false) == ListenerOwner::Child);
        const auto pid = std::stoi(test_util::read_file(tmp.path() / "pid"));
        ::kill(pid, SIGKILL);
        child->kill();
    }

    // Another program of this user sharing the port with SO_REUSEPORT: the
    // connection may reach either, so it's not the child's alone.
    // A port of its own: the servers above may still be letting go of theirs.
    const Port reused = free_run(static_cast<Port>(port + 5), 1);
    REQUIRE(reused != 0);
    const int shared = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE(shared >= 0);
    int one = 1;
    setsockopt(shared, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(reused);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(shared, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0);
    REQUIRE(listen(shared, 8) == 0);
    auto child = start("exec python3 -c 'import socket, time; s = socket.socket(); "
                       "s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1); s.bind((\"127.0.0.1\", " +
                       std::to_string(reused) + ")); s.listen(); time.sleep(60)'");
    const auto proc_text = [](const char* path) {
        const std::ifstream in(path);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    };
    CHECK(eventually([&] {
        return proc_net_listeners(proc_text("/proc/net/tcp"), proc_text("/proc/net/tcp6"), reused, false).size() == 2;
    }));
    CHECK(child->listener_owner(reused, false) == ListenerOwner::Other);
    child->kill();
    close(shared);
}

namespace {

// A row of /proc/net/tcp(6) for a socket on `addr` (its bytes as the
// kernel holds them), as the kernel writes it.
std::string proc_row(const std::uint8_t* addr, std::size_t size, Port port, const char* state, unsigned uid,
                     std::uint64_t inode) {
    std::string hex;
    for (std::size_t i = 0; i < size; i += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, addr + i, sizeof word);
        std::array<char, 9> buf{};
        (void)std::snprintf(buf.data(), buf.size(), "%08X", word);
        hex += buf.data();
    }
    std::array<char, 8> port_hex{};
    (void)std::snprintf(port_hex.data(), port_hex.size(), "%04X", static_cast<unsigned>(port));
    return "   0: " + hex + ":" + port_hex.data() + " " + std::string(size == 4 ? 8 : 32, '0') + ":0000 " + state +
           " 00000000:00000000 00:00000000 00000000 " + std::to_string(uid) + " 0 " + std::to_string(inode) +
           " 1 0000000000000000 100 0 0 10 0\n";
}

std::string v4_row(std::uint32_t network_order, Port port, const char* state, unsigned uid, std::uint64_t inode) {
    std::array<std::uint8_t, 4> bytes{};
    std::memcpy(bytes.data(), &network_order, sizeof network_order);
    return proc_row(bytes.data(), bytes.size(), port, state, uid, inode);
}

std::string v6_row(const in6_addr& a, Port port, const char* state, unsigned uid, std::uint64_t inode) {
    return proc_row(a.s6_addr, sizeof a.s6_addr, port, state, uid, inode);
}

const std::string kProcHeader =
    "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";

std::vector<std::uint64_t> inodes(const std::vector<ProcListener>& listeners) {
    std::vector<std::uint64_t> out;
    out.reserve(listeners.size());
    for (const ProcListener& l : listeners) {
        out.push_back(l.inode);
    }
    return out;
}

}  // namespace

TEST_CASE("the listeners a loopback connection reaches, from /proc/net/tcp and tcp6") {
    const std::uint32_t loopback = htonl(INADDR_LOOPBACK);
    const std::uint32_t any = htonl(INADDR_ANY);
    const std::uint32_t elsewhere = htonl(0x7F000002);  // 127.0.0.2
    in6_addr mapped{};
    mapped.s6_addr[10] = 0xff;
    mapped.s6_addr[11] = 0xff;
    std::memcpy(mapped.s6_addr + 12, &loopback, sizeof loopback);
    using Inodes = std::vector<std::uint64_t>;

    // 127.0.0.1 before 0.0.0.0; other ports, states and addresses don't count.
    const std::string tcp = kProcHeader + v4_row(any, 9600, "0A", 1000, 11) + v4_row(loopback, 9600, "0A", 1000, 12) +
                            v4_row(loopback, 9600, "0A", 1000, 13) + v4_row(loopback, 9601, "0A", 1000, 14) +
                            v4_row(loopback, 9600, "01", 1000, 15) + v4_row(elsewhere, 9600, "0A", 1000, 16);
    CHECK(inodes(proc_net_listeners(tcp, kProcHeader, 9600, false)) == Inodes{12, 13});
    CHECK(inodes(proc_net_listeners(kProcHeader + v4_row(any, 9600, "0A", 1000, 11), kProcHeader, 9600, false)) ==
          Inodes{11});
    const auto row = proc_net_listeners(tcp, kProcHeader, 9601, false);
    REQUIRE(row.size() == 1);
    CHECK(row[0].uid == 1000);

    const std::string tcp6 = kProcHeader + v6_row(in6addr_any, 9600, "0A", 0, 21) +
                             v6_row(in6addr_loopback, 9600, "0A", 0, 22) + v6_row(mapped, 9602, "0A", 0, 23);
    CHECK(inodes(proc_net_listeners(tcp, tcp6, 9600, true)) == Inodes{22});
    // IPv4 looks at IPv6 sockets only when no IPv4 one listens.
    CHECK(inodes(proc_net_listeners(tcp, tcp6, 9600, false)) == Inodes{12, 13});
    CHECK(inodes(proc_net_listeners(kProcHeader, tcp6, 9600, false)) == Inodes{21});
    CHECK(inodes(proc_net_listeners(kProcHeader, tcp6, 9602, false)) == Inodes{23});
    CHECK(proc_net_listeners(kProcHeader, tcp6, 9602, true).empty());
    CHECK(proc_net_listeners("", "", 9600, false).empty());
    // A row that can't be read is nobody's.
    std::string bad = v4_row(loopback, 9603, "0A", 1000, 31);
    bad.replace(bad.find(" 1000 "), 6, " x100 ");
    const auto unreadable = proc_net_listeners(kProcHeader + bad, kProcHeader, 9603, false);
    REQUIRE(unreadable.size() == 1);
    CHECK(unreadable[0].uid == 0xFFFFFFFFU);
}

TEST_CASE("listeners in /proc/net/tcp6 as a little-endian kernel prints them") {
    if constexpr (std::endian::native != std::endian::little) {
        MESSAGE("not a little-endian machine; skipped");
        return;
    }
    // Copied from x86-64 kernels: [::1] (python --bind ::1), [::] (Node,
    // Bun and Deno listening on every address, dual-stack), and
    // ::ffff:127.0.0.1; port 0x2580 = 9600.
    const std::string tcp6 =
        kProcHeader +
        "   0: 00000000000000000000000001000000:2580 00000000000000000000000000000000:0000 0A 00000000:00000000 "
        "00:00000000 00000000  1001        0 4001 1 0000000000000000 100 0 0 10 0\n"
        "   1: 00000000000000000000000000000000:2581 00000000000000000000000000000000:0000 0A 00000000:00000000 "
        "00:00000000 00000000  1001        0 4002 1 0000000000000000 100 0 0 10 0\n"
        "   2: 0000000000000000FFFF00000100007F:2582 00000000000000000000000000000000:0000 0A 00000000:00000000 "
        "00:00000000 00000000  1001        0 4003 1 0000000000000000 100 0 0 10 0\n";
    const std::string tcp = kProcHeader +
                            "   0: 0100007F:2583 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1001        "
                            "0 4004 1 0000000000000000 100 0 0 10 0\n"
                            "   1: 00000000:2584 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        "
                            "0 4005 1 0000000000000000 100 0 0 10 0\n";
    using Inodes = std::vector<std::uint64_t>;
    CHECK(inodes(proc_net_listeners(tcp, tcp6, 9600, true)) == Inodes{4001});
    CHECK(proc_net_listeners(tcp, tcp6, 9600, false).empty());  // [::1] doesn't take IPv4
    CHECK(inodes(proc_net_listeners(tcp, tcp6, 9601, false)) == Inodes{4002});  // dual-stack [::]
    CHECK(inodes(proc_net_listeners(tcp, tcp6, 9601, true)) == Inodes{4002});
    CHECK(inodes(proc_net_listeners(tcp, tcp6, 9602, false)) == Inodes{4003});
    CHECK(inodes(proc_net_listeners(tcp, tcp6, 9603, false)) == Inodes{4004});
    const auto any = proc_net_listeners(tcp, tcp6, 9604, false);
    REQUIRE(any.size() == 1);
    CHECK(any[0].uid == 0);
    CHECK(judge_listeners(any, 1001, {4005}, true) == ListenerOwner::Other);  // root's, on 0.0.0.0
    CHECK(judge_listeners(proc_net_listeners(tcp, tcp6, 9601, false), 1001, {4002}, false) == ListenerOwner::Child);
}

TEST_CASE("a listener is the child's only when every one is this user's and the child's") {
    const std::vector<ProcListener> mine = {{12, 1000}, {13, 1000}};
    CHECK(judge_listeners({}, 1000, {}, false) == ListenerOwner::Unknown);
    CHECK(judge_listeners(mine, 1000, {12, 13, 40}, false) == ListenerOwner::Child);
    CHECK(judge_listeners(mine, 1000, {12, 13}, true) == ListenerOwner::Child);
    // SO_REUSEPORT: one of the two is someone else's.
    CHECK(judge_listeners(mine, 1000, {12}, false) == ListenerOwner::Other);
    CHECK(judge_listeners(mine, 1000, {}, false) == ListenerOwner::Other);
    // Not found, but one of the child's processes can't be read (made
    // non-dumpable, a setuid program): this user's listener may be its.
    CHECK(judge_listeners(mine, 1000, {12}, true) == ListenerOwner::Unknown);
    // Another user's listener (root's, say) is never the child's, however
    // unreadable its processes are, even with the inode among them.
    const std::vector<ProcListener> roots = {{12, 0}};
    CHECK(judge_listeners(roots, 65534, {}, true) == ListenerOwner::Other);
    CHECK(judge_listeners(roots, 65534, {12}, true) == ListenerOwner::Other);
    CHECK(judge_listeners({{12, 1000}, {13, 0}}, 1000, {12, 13}, true) == ListenerOwner::Other);
    CHECK(judge_listeners({{12, 0xFFFFFFFFU}}, 1000, {12}, true) == ListenerOwner::Other);
}

#endif

#endif

#ifdef _WIN32

TEST_CASE("who listens on a port: the child's job or another program") {
    const fs::path system = path_from_utf8(get_env("SystemRoot").value_or("C:\\Windows")) / "System32";
    const Port port =
        pick_port_from(static_cast<Port>(test_util::app_port_base() + 80), {}, can_listen_on_loopback).value_or(0);
    REQUIRE(port != 0);
    std::string error;
    ChildOptions idle_options;
    idle_options.program = system / "PING.EXE";
    idle_options.args = {"-n", "60", "127.0.0.1"};
    const auto idle = ChildProcess::start(idle_options, error);
    REQUIRE_MESSAGE(idle, error);
    CHECK(idle->listener_owner(port, false) == ListenerOwner::Unknown);  // nobody listens
    {
        const auto other = LoopbackListener::open(port, error);
        REQUIRE_MESSAGE(other, error);
        CHECK(idle->listener_owner(port, false) == ListenerOwner::Other);
    }

    ChildOptions server;
    server.program = system / "WindowsPowerShell" / "v1.0" / "powershell.exe";
    server.args = {"-NoProfile", "-NonInteractive", "-Command",
                   "$l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, " +
                       std::to_string(port) + "); $l.Start(); Start-Sleep 60"};
    const auto child = ChildProcess::start(server, error);
    REQUIRE_MESSAGE(child, error);
    bool listening = false;
    for (int i = 0; i < 300 && !listening && child->running(); ++i) {
        if (const auto s = connect_loopback(port)) {
            close_socket(*s);
            listening = true;
        } else {
            std::this_thread::sleep_for(100ms);
        }
    }
    REQUIRE(listening);
    CHECK(child->listener_owner(port, false) == ListenerOwner::Child);
    CHECK(idle->listener_owner(port, false) == ListenerOwner::Other);
}

#endif

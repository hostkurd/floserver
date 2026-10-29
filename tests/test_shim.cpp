// The terminal shims (D10, D17): which program each one runs, and the
// user's Path.

#include "floserver/core/app_runner.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/shim.h"
#include "floserver/core/user_path.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <array>
#include <cstdio>
#include <cstdlib>
#endif

using namespace floserver::core;
namespace fs = std::filesystem;

namespace {

using Args = std::vector<std::string>;

void fake_runtime(const fs::path& data, const std::string& name, const std::string& version,
                  const std::string& variant = "") {
    const fs::path dir = runtimes_dir(data) / name / version;
    fs::create_directories(dir);
    test_util::write_file(dir / ".floserver-runtime.json",
                          nlohmann::json{{"schema", 1}, {"name", name}, {"version", version},
                                         {"platform", std::string(current_platform())}, {"variant", variant},
                                         {"license", "x"}, {"sha256", std::string(64, 'a')}}
                              .dump());
}

// Installs laid out as on this platform, with empty programs.
fs::path add_php(const fs::path& data, const std::string& version) {
    fake_runtime(data, "php", version, "nts");
    const fs::path dir = runtimes_dir(data) / "php" / version;
#ifdef _WIN32
    test_util::write_file(dir / "php.exe", "");
#else
    test_util::write_file(dir / "bin" / "php", "");
#endif
    return dir;
}

fs::path add_node(const fs::path& data, const std::string& version) {
    fake_runtime(data, "node", version);
    const fs::path dir = runtimes_dir(data) / "node" / version;
#ifdef _WIN32
    test_util::write_file(dir / "node.exe", "");
    const fs::path npm = dir / "node_modules" / "npm" / "bin";
#else
    test_util::write_file(dir / "bin" / "node", "");
    const fs::path npm = dir / "lib" / "node_modules" / "npm" / "bin";
#endif
    test_util::write_file(npm / "npm-cli.js", "");
    test_util::write_file(npm / "npx-cli.js", "");
    return dir;
}

fs::path add_python(const fs::path& data, const std::string& version) {
    fake_runtime(data, "python", version);
    const fs::path dir = runtimes_dir(data) / "python" / version;
#ifdef _WIN32
    test_util::write_file(dir / "python.exe", "");
#else
    test_util::write_file(dir / "bin" / "python3", "");
#endif
    return dir;
}

}  // namespace

TEST_CASE("each shim runs the version the nearest floserver.json asks for") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    const fs::path php84 = add_php(data, "8.4.27");
    const fs::path php85 = add_php(data, "8.5.12");
    const fs::path node22 = add_node(data, "22.23.4");
    const fs::path node24 = add_node(data, "24.22.1");
    const fs::path py313 = add_python(data, "3.13.18");
    fake_runtime(data, "composer", "2.10.3");
    test_util::write_file(runtimes_dir(data) / "composer" / "2.10.3" / "composer.phar", "");

    const fs::path shop = tmp.path() / "shop";
    test_util::write_file(shop / "floserver.json", R"({"php": "8.4", "node": "22", "extensions": ["intl"]})");
    fs::create_directories(shop / "public" / "css");
    const fs::path here = shop / "public" / "css";  // a folder inside the project
    std::string error;

    const auto php = resolve_shim("php", here, data, error);
    REQUIRE_MESSAGE(php, error);
#ifdef _WIN32
    CHECK(php->program == php84 / "php.exe");
#else
    CHECK(php->program == php84 / "bin" / "php");
#endif
    CHECK(php->version == "8.4.27");
    REQUIRE(php->args.size() == 2);
    CHECK(php->args[0] == "-c");
    CHECK(path_from_utf8(php->args[1]) == data / "cli" / "php" / "8.4.27-nts+intl.ini");
    CHECK(fs::exists(path_from_utf8(php->args[1])));  // written for the shim

    const auto composer = resolve_shim("composer", here, data, error);
    REQUIRE(composer);
    CHECK(composer->program == php->program);
    CHECK(composer->args.back() == path_to_utf8(runtimes_dir(data) / "composer" / "2.10.3" / "composer.phar"));

    const auto node = resolve_shim("node", here, data, error);
    REQUIRE(node);
    CHECK(node->program == *node_program(node22));
    CHECK(node->args.empty());
    const auto npm = resolve_shim("npm", here, data, error);
    REQUIRE(npm);
    CHECK(npm->program == *node_program(node22));
    CHECK(npm->args == Args{path_to_utf8(*npm_script(node22))});
    const auto npx = resolve_shim("npx", here, data, error);
    REQUIRE(npx);
    CHECK(path_from_utf8(npx->args.at(0)).filename() == "npx-cli.js");

    // Outside a project: the newest installed.
    const fs::path elsewhere = tmp.path() / "elsewhere";
    fs::create_directories(elsewhere);
    CHECK(resolve_shim("php", elsewhere, data, error)->version == "8.5.12");
    CHECK(resolve_shim("node", elsewhere, data, error)->program == *node_program(node24));
    (void)php85;

    // Python: the install, then the project's .venv once it has one (D17).
    const fs::path api = tmp.path() / "api";
    test_util::write_file(api / "floserver.json", R"({"python": "3.13"})");
    auto python = resolve_shim("python", api, data, error);
    REQUIRE(python);
    CHECK(python->program == *python_program(py313));
    test_util::write_file(venv_python(api), "");
    python = resolve_shim("python", api / "sub", data, error);
    REQUIRE(python);
    CHECK(python->program == venv_python(api));
    REQUIRE(python->env.size() == 2);
    CHECK(python->env[0] == std::pair<std::string, std::string>{"VIRTUAL_ENV", path_to_utf8(api / ".venv")});
    CHECK(python->env[1].first == "PATH");  // the venv's scripts first
    CHECK(python->env[1].second.starts_with(path_to_utf8(venv_python(api).parent_path())));
    const auto pip = resolve_shim("pip", api, data, error);
    REQUIRE(pip);
    CHECK(pip->program == venv_python(api));
    CHECK(pip->args == Args{"-m", "pip"});
#ifndef _WIN32
    const auto pip3 = resolve_shim("pip3", api, data, error);
    REQUIRE(pip3);
    CHECK(pip3->args == Args{"-m", "pip"});
    CHECK(resolve_shim("python3", api, data, error)->program == venv_python(api));
#endif

    // PHP started again by the program gets the same php.ini.
    REQUIRE(!php->env.empty());
    CHECK(php->env[0] == std::pair<std::string, std::string>{"PHPRC", php->args[1]});
}

#ifndef _WIN32
TEST_CASE("outside a project, with none of FloServer's, a shim runs the one that was there") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    const fs::path bin = tmp.path() / "bin";
    test_util::write_file(bin / "python3", "");
    test_util::write_file(shims_dir(data) / "python3", "");
    const std::string old_path = get_env("PATH").value_or("");
    ::setenv("PATH", (path_to_utf8(shims_dir(data)) + ":" + path_to_utf8(bin)).c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
    std::string error;
    const auto elsewhere = resolve_shim("python3", tmp.path(), data, error);
    test_util::write_file(tmp.path() / "proj" / "floserver.json", R"({"python": "3.13"})");
    const auto in_project = resolve_shim("python3", tmp.path() / "proj", data, error);
    CHECK(error.find("flo install python 3.13") != std::string::npos);
    // A link elsewhere to a shim isn't "the one that was there".
    const fs::path links = tmp.path() / "links";
    fs::create_directories(links);
    fs::create_symlink(shims_dir(data) / "python3", links / "python3");
    fs::create_hard_link(shims_dir(data) / "python3", links / "pip3");
    ::setenv("PATH", (path_to_utf8(links) + ":" + path_to_utf8(bin)).c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
    const auto past_link = resolve_shim("python3", tmp.path(), data, error);
    const auto past_hard_link = resolve_shim("pip3", tmp.path(), data, error);
    ::setenv("PATH", old_path.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
    REQUIRE(past_link);
    CHECK(past_link->program == bin / "python3");
    CHECK_FALSE(past_hard_link);  // nothing else called pip3 here
    REQUIRE(elsewhere);
    CHECK(elsewhere->program == bin / "python3");
    CHECK_FALSE(in_project);  // a project's own version is never swapped for another
}

TEST_CASE("a shim never runs another shim") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    // A broken install whose bin folder is the shims folder.
    fake_runtime(data, "node", "22.0.0");
    fs::create_directories(shims_dir(data));
    test_util::write_file(shims_dir(data) / "node", "");
    fs::create_directory_symlink(shims_dir(data), runtimes_dir(data) / "node" / "22.0.0" / "bin");
    const fs::path project = tmp.path() / "p";
    test_util::write_file(project / "floserver.json", R"({"node": "22"})");
    std::string error;
    CHECK_FALSE(resolve_shim("node", project, data, error));
    CHECK(error.find("is a FloServer shim itself") != std::string::npos);
}
#endif

TEST_CASE("a shim says what to install or fix") {
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    add_php(data, "8.5.12");
    const fs::path old = tmp.path() / "old";
    test_util::write_file(old / "floserver.json", R"({"php": "7.4"})");
    std::string error;
    CHECK_FALSE(resolve_shim("php", old, data, error));
    CHECK(error == "this project asks for PHP 7.4, which isn't installed; install it with: flo install php 7.4");
    CHECK_FALSE(resolve_shim("node", old, data, error));
    CHECK(error == "no Node is installed; install one with: flo install node");
    CHECK_FALSE(resolve_shim("composer", old / "x", data, error));  // PHP 7.4 is checked first
    CHECK(error.find("PHP 7.4") != std::string::npos);
    const fs::path broken = tmp.path() / "broken";
    test_util::write_file(broken / "floserver.json", R"({"php": 8})");
    CHECK_FALSE(resolve_shim("php", broken, data, error));
    CHECK(error.find("floserver.json: php") != std::string::npos);
    CHECK_FALSE(resolve_shim("ruby", old, data, error));
}

TEST_CASE("the shims folder goes first on the Path, once, and comes off again") {
    const std::wstring dir = L"C:\\Users\\Jo\\AppData\\Local\\FloServer\\shims";
    CHECK(path_with(L"", dir) == dir);
    CHECK(path_with(L"C:\\Tools;%USERPROFILE%\\bin", dir) == dir + L";C:\\Tools;%USERPROFILE%\\bin");
    // Already there, written differently: unchanged.
    const std::wstring there = L"C:\\Tools;c:\\users\\jo\\appdata\\local\\floserver\\SHIMS\\";
    CHECK(path_has(there, dir));
    CHECK(path_with(there, dir) == there);
    CHECK(path_without(there, dir) == L"C:\\Tools");
    CHECK(path_without(L"C:\\Tools;;C:\\Other", dir) == L"C:\\Tools;C:\\Other");
    CHECK_FALSE(path_has(L"C:\\Users\\Jo\\AppData\\Local\\FloServer\\shims-old", dir));
}

#ifndef _WIN32

TEST_CASE("the shim program runs the project's node and php, arguments as given") {
    const auto node = fs::path("/opt/node22/bin/node");
    const auto php = fs::path("/usr/bin/php");
    std::error_code ec;
    const fs::path shim = fs::path(FLOSERVER_TEST_BIN_DIR) / "floserver-shim";
    if (!fs::exists(node, ec) || !fs::exists(php, ec) || !fs::exists(shim, ec)) {
        MESSAGE("node, php or the shim isn't here; skipped");
        return;
    }
    const test_util::TempDir tmp;
    const fs::path data = tmp.path() / "data";
    fake_runtime(data, "node", "22.0.0");
    fs::create_directories(runtimes_dir(data) / "node" / "22.0.0" / "bin");
    fs::create_symlink(fs::canonical(node), runtimes_dir(data) / "node" / "22.0.0" / "bin" / "node");
    fake_runtime(data, "php", "8.3.6", "nts");
    fs::create_directories(runtimes_dir(data) / "php" / "8.3.6" / "bin");
    fs::create_symlink(fs::canonical(php), runtimes_dir(data) / "php" / "8.3.6" / "bin" / "php");
    const fs::path shims = tmp.path() / "shims";
    fs::create_directories(shims);
    fs::copy_file(shim, shims / "node");
    fs::copy_file(shim, shims / "php");
    const fs::path project = tmp.path() / "proj";
    test_util::write_file(project / "floserver.json", R"({"node": "22", "php": "8.3"})");

    const auto run = [&](const std::string& command) {
        const std::string full = "cd '" + project.string() + "' && FLOSERVER_DATA_DIR='" + data.string() + "' " +
                                 command + " 2>&1";
        std::string out;
        if (FILE* pipe = popen(full.c_str(), "r")) {
            std::array<char, 256> buf{};
            while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
                out += buf.data();
            }
            CHECK(pclose(pipe) == 0);
        }
        return out;
    };
    CHECK(run("'" + (shims / "node").string() + "' -e 'console.log(process.argv.slice(1).join(\"|\"))' 'a b' \"c'd\"") ==
          "a b|c'd\n");
    CHECK(run("'" + (shims / "php").string() + "' -r 'echo basename(php_ini_loaded_file());'") == "8.3.6-nts.ini");
}

#endif

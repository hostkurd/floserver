#include "floserver/core/shim.h"

#include "floserver/core/app_runner.h"
#include "floserver/core/paths.h"
#include "floserver/core/php_pool.h"
#include "floserver/core/process.h"
#include "floserver/core/project_config.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;

namespace {

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// The newest install of `name` matching `wanted` ("8.2"; empty: any). For
// PHP the usual NTS build is preferred over a thread-safe one.
const InstalledRuntime* newest(const std::vector<InstalledRuntime>& installed, const std::string& name,
                               const std::string& wanted) {
    const auto want = wanted.empty() ? std::nullopt : Version::parse(wanted);
    const InstalledRuntime* best = nullptr;
    std::optional<Version> best_v;
    bool best_nts = false;
    for (const InstalledRuntime& r : installed) {
        const auto v = Version::parse(r.version);
        if (r.name != name || !v || (want && !want->matches(*v))) {
            continue;
        }
        const bool nts = r.variant.empty() || r.variant == "nts";
        if (best == nullptr || !best_v || (nts && !best_nts) || (nts == best_nts && *v > *best_v)) {
            best = &r;
            best_v = v;
            best_nts = nts;
        }
    }
    return best;
}

std::optional<fs::path> php_cli(const fs::path& dir) {
#ifdef _WIN32
    const char* names[] = {"php.exe"};  // NOLINT(modernize-avoid-c-arrays)
#else
    const char* names[] = {"bin/php", "php"};  // NOLINT(modernize-avoid-c-arrays)
#endif
    for (const char* n : names) {
        fs::path p = dir / path_from_utf8(n);
        if (is_file(p)) {
            return p;
        }
    }
    return std::nullopt;
}

struct Project {
    std::optional<fs::path> dir;  // the folder with the nearest floserver.json
    ProjectConfig config;
};

std::optional<Project> project_for(const fs::path& cwd, std::string& error) {
    Project p;
    const auto file = find_project_config(cwd);
    if (!file) {
        return p;
    }
    p.dir = file->parent_path();
    const auto text = read_small_file(*file, std::size_t{64} * 1024);
    const auto parsed = text ? parse_project_config(*text) : ConfigResult{};
    if (!parsed.config) {
        error = path_to_utf8(*file) + (parsed.errors.empty() ? std::string(" can't be read")
                                                             : ": " + parsed.errors.front().key + " " +
                                                                   parsed.errors.front().message);
        return std::nullopt;
    }
    p.config = *parsed.config;
    return p;
}

// "this project asks for PHP 8.2, which isn't installed: flo install php 8.2"
std::string not_installed(const std::string& shown, const std::string& name, const std::optional<std::string>& wanted) {
    if (wanted) {
        return "this project asks for " + shown + " " + *wanted + ", which isn't installed; install it with: flo install " +
               name + " " + *wanted;
    }
    return "no " + shown + " is installed; install one with: flo install " + name;
}

// php with a php.ini for that install and these extensions, written once.
std::optional<ShimTarget> php_target(const InstalledRuntime& php, const std::vector<std::string>& extensions,
                                     const fs::path& data_dir, std::string& error) {
    const auto program = php_cli(php.dir);
    if (!program) {
        error = "PHP " + php.version + " in " + path_to_utf8(php.dir) + " has no php program";
        return std::nullopt;
    }
    std::vector<std::string> ext = extensions;
    std::sort(ext.begin(), ext.end());
    ext.erase(std::unique(ext.begin(), ext.end()), ext.end());
    std::string key = php.version + (php.variant.empty() ? std::string() : "-" + php.variant);
    for (const std::string& e : ext) {
        key += "+" + e;
    }
    const fs::path folder = data_dir / "cli" / "php";  // not in the shims folder: "php" is a shim there
    const fs::path ini = folder / path_from_utf8(key + ".ini");
    // Missing listed extensions are left out here: the web reports them.
    const PhpIniResult made = make_php_ini(php.dir, ext, folder / "php-errors.log");
    if (read_small_file(ini, std::size_t{256} * 1024) != made.text) {
        std::error_code ec;
        fs::create_directories(folder, ec);
        // Another shim may be writing (or php reading) it at the same moment:
        // fine if what's there now is right.
        if (!write_file_atomically(ini, made.text, error) && read_small_file(ini, std::size_t{256} * 1024) != made.text) {
            return std::nullopt;
        }
        error.clear();
    }
    ShimTarget t;
    t.program = *program;
    t.args = {"-c", path_to_utf8(ini)};
    // PHP started again by the program (artisan serve, PHPUnit, Composer's
    // @php) gets the same php.ini through PHPRC. So would another PHP run by
    // its full path from there, with this install's extension_dir: rare, and
    // its own -c or PHPRC wins.
    t.env = {{"PHPRC", path_to_utf8(ini)}};
#ifdef _WIN32
    t.env.emplace_back("PHP_INI_SCAN_DIR", "");  // and no other ini from a system-wide PHP
#endif
    t.version = php.version;
    return t;
}

}  // namespace

const std::vector<std::string>& shim_names() {
#ifdef _WIN32
    static const std::vector<std::string> names = {"php", "node", "npm", "npx", "composer", "python", "pip"};
#else
    // The names people type on macOS and Linux too.
    static const std::vector<std::string> names = {"php",    "node", "npm",     "npx", "composer",
                                                   "python", "pip",  "python3", "pip3"};
#endif
    return names;
}

fs::path shims_dir(const fs::path& data) { return data / "shims"; }

namespace {

// The program `name` further along this process's PATH, past the shims.
std::optional<fs::path> next_on_path(const std::string& name, const fs::path& data_dir) {
    std::error_code ec;
    const fs::path shims = fs::weakly_canonical(shims_dir(data_dir), ec);
    const std::string path = get_env("PATH").value_or("");
#ifdef _WIN32
    const char sep = ';';
    const fs::path file = path_from_utf8(name + ".exe");
#else
    const char sep = ':';
    const fs::path file = path_from_utf8(name);
#endif
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(sep, start);
        const std::string dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty() && fs::weakly_canonical(path_from_utf8(dir), ec) != shims) {
            fs::path p = path_from_utf8(dir) / file;
            if (is_file(p)) {
                return p;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

std::optional<ShimTarget> resolve_installed(const std::string& tool, const fs::path& cwd, const fs::path& data_dir,
                                            std::string& error) {
    const auto& names = shim_names();
    if (std::find(names.begin(), names.end(), tool) == names.end()) {
        error = "\"" + tool + "\" isn't a FloServer shim";
        return std::nullopt;
    }
    const auto project = project_for(cwd, error);
    // A target in the shims folder would start this shim again, without end.
    const auto guard = [](std::optional<ShimTarget> t, const fs::path& data, std::string& why) -> std::optional<ShimTarget> {
        std::error_code ec;
        const fs::path shims = fs::weakly_canonical(shims_dir(data), ec);
        if (t && !ec && fs::weakly_canonical(t->program, ec).parent_path() == shims) {
            why = path_to_utf8(t->program) + " is a FloServer shim itself";
            return std::nullopt;
        }
        return t;
    };
    if (!project) {
        return std::nullopt;
    }
    const ProjectConfig& pc = project->config;
    const auto installed = list_installed(runtimes_dir(data_dir));

    if (tool == "php" || tool == "composer") {
        const InstalledRuntime* php = newest(installed, "php", pc.php.value_or(""));
        if (php == nullptr) {
            error = not_installed("PHP", "php", pc.php);
            return std::nullopt;
        }
        auto t = php_target(*php, pc.extensions, data_dir, error);
        if (!t || tool == "php") {
            return guard(t, data_dir, error);
        }
        const InstalledRuntime* composer = newest(installed, "composer", "");
        const fs::path phar = composer != nullptr ? composer->dir / "composer.phar" : fs::path();
        if (composer == nullptr || !is_file(phar)) {
            error = not_installed("Composer", "composer", std::nullopt);
            return std::nullopt;
        }
        t->args.push_back(path_to_utf8(phar));
        return guard(t, data_dir, error);
    }

    if (tool == "node" || tool == "npm" || tool == "npx") {
        const InstalledRuntime* node = newest(installed, "node", pc.node.value_or(""));
        if (node == nullptr) {
            error = not_installed("Node", "node", pc.node);
            return std::nullopt;
        }
        const auto program = node_program(node->dir);
        const fs::path npm = npm_script(node->dir).value_or(fs::path());
        if (!program || (tool != "node" && npm.empty())) {
            error = "Node " + node->version + " in " + path_to_utf8(node->dir) + " has no node or npm";
            return std::nullopt;
        }
        ShimTarget t;
        t.program = *program;
        t.version = node->version;
        if (tool != "node") {
            const fs::path script = tool == "npm" ? npm : npm.parent_path() / "npx-cli.js";
            if (!is_file(script)) {
                error = "Node " + node->version + " has no " + path_to_utf8(script.filename());
                return std::nullopt;
            }
            t.args = {path_to_utf8(script)};
        }
        return guard(t, data_dir, error);
    }

    // python, pip: the project's .venv first (D17), else its Python.
    ShimTarget t;
    if (project->dir && is_file(venv_python(*project->dir))) {
        t.program = venv_python(*project->dir);
        // As activating the venv does: its scripts (flask, pytest) come first.
        const std::string path = get_env("PATH").value_or("");
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        t.env = {{"VIRTUAL_ENV", path_to_utf8(*project->dir / ".venv")},
                 {"PATH", path_to_utf8(t.program.parent_path()) + (path.empty() ? "" : sep + path)}};
        t.version = ".venv";
    } else {
        const InstalledRuntime* python = newest(installed, "python", pc.python.value_or(""));
        const auto program = python != nullptr ? python_program(python->dir) : std::nullopt;
        if (!program) {
            error = not_installed("Python", "python", pc.python);
            return std::nullopt;
        }
        t.program = *program;
        t.version = python->version;
    }
    if (tool == "pip" || tool == "pip3") {
        t.args = {"-m", "pip"};
    }
    return guard(t, data_dir, error);
}

}  // namespace

std::optional<ShimTarget> resolve_shim(const std::string& tool, const fs::path& cwd, const fs::path& data_dir,
                                       std::string& error) {
    auto t = resolve_installed(tool, cwd, data_dir, error);
    const auto& names = shim_names();
    if (t || std::find(names.begin(), names.end(), tool) == names.end() || find_project_config(cwd)) {
        return t;
    }
    // Outside a project, with none of FloServer's: the one that was there
    // before, so `#!/usr/bin/env python3` scripts and other tools still work.
    if (auto other = next_on_path(tool, data_dir)) {
        error.clear();
        ShimTarget found;
        found.program = std::move(*other);
        return found;
    }
    return t;
}

}  // namespace floserver::core

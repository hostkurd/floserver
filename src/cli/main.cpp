// floserver: the command line (D2). Built as `floserver` and copied as the
// short alias `fls` (Q2). Only the commands that work without the service are here
// so far; start/stop/use/open arrive with floserverd.

#include "floserver/core/build_info.h"
#include "floserver/core/project_config.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using floserver::core::parse_project_config;

// Paths are shown as UTF-8 on every platform. On Windows, path::string() would
// use the ANSI code page and throw on names it can't hold (an Arabic or
// Kurdish folder on an English Windows).
std::string shown(const fs::path& p) {
    const std::u8string u = p.u8string();
    return {u.begin(), u.end()};
}

void print_help(std::ostream& out) {
    out << "FloServer " << floserver::core::kVersion << "\n\n"
        << "Usage: floserver <command>   (or: fls <command>)\n\n"
        << "Commands:\n"
        << "  check [folder]   Check the floserver.json for a project\n"
        << "  version          Show the version\n"
        << "  help             Show this help\n";
}

int check(const fs::path& start) {
    const auto file = floserver::core::find_project_config(start);
    if (!file) {
        std::cerr << "No floserver.json found in " << shown(start) << " or the folders above it.\n";
        return 1;
    }
    const std::ifstream in(*file, std::ios::binary);
    if (!in) {
        std::cerr << "Can't read " << shown(*file) << "\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const auto result = parse_project_config(buffer.str());

    std::cout << shown(*file) << "\n";
    for (const auto& w : result.warnings) {
        std::cout << "  warning: " << (w.key.empty() ? "file" : w.key) << " " << w.message << "\n";
    }
    for (const auto& e : result.errors) {
        std::cout << "  error: " << (e.key.empty() ? "file" : e.key) << " " << e.message << "\n";
    }
    if (!result.config) {
        return 1;
    }
    const auto& c = *result.config;
    std::cout << "  php " << c.php.value_or("default") << ", node " << c.node.value_or("default")
              << ", database "
              << (c.database ? c.database->family + "-" + c.database->version : std::string("none"))
              << ", web " << (c.web == floserver::core::WebServer::Apache ? "apache" : "nginx")
              << ", root " << c.root << "\n";
    return 0;
}

int run(const std::vector<std::string>& args, const std::vector<fs::path>& paths) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print_help(std::cout);
        return 0;
    }
    if (args[0] == "version" || args[0] == "--version") {
        std::cout << "FloServer " << floserver::core::kVersion << "\n";
        return 0;
    }
    if (args[0] == "check") {
        return check(paths.size() > 1 ? paths[1] : fs::current_path());
    }
    std::cerr << "Unknown command: " << args[0] << "\n\n";
    print_help(std::cerr);
    return 2;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    std::vector<fs::path> paths;
    for (int i = 1; i < argc; ++i) {
        paths.emplace_back(argv[i]);
        args.push_back(shown(paths.back()));
    }
    return run(args, paths);
}
#else
int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::vector<fs::path> paths(args.begin(), args.end());
    return run(args, paths);
}
#endif

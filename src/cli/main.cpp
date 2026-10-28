// floserver: the command line (D2). Built as `floserver` and copied as the
// short name `flo` (FloCMS's own CLI is `php flo`, a PHP script, so they don't clash).

#include "floserver/core/build_info.h"
#include "floserver/core/ipc.h"
#include "floserver/core/project_config.h"
#include "floserver/core/service_client.h"

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
        << "Usage: floserver <command>   (or: flo <command>)\n\n"
        << "Commands:\n"
        << "  status           Show whether FloServer is running\n"
        << "  service start    Start FloServer in the background\n"
        << "  service stop     Stop FloServer and everything it runs\n"
        << "  check [folder]   Check the floserver.json for a project\n"
        << "  version          Show the version\n"
        << "  help             Show this help\n";
}

std::string human_bytes(std::uint64_t bytes) {
    std::ostringstream out;
    out.precision(1);
    out << std::fixed;
    if (bytes >= std::uint64_t{1024} * 1024U) {
        out << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
    } else {
        out << static_cast<double>(bytes) / 1024.0 << " KB";
    }
    return out.str();
}

std::string human_duration(std::int64_t seconds) {
    std::ostringstream out;
    if (seconds >= 86400) {
        out << seconds / 86400 << "d " << (seconds % 86400) / 3600 << "h";
    } else if (seconds >= 3600) {
        out << seconds / 3600 << "h " << (seconds % 3600) / 60 << "m";
    } else if (seconds >= 60) {
        out << seconds / 60 << "m " << seconds % 60 << "s";
    } else {
        out << seconds << "s";
    }
    return out.str();
}

int status() {
    std::string error;
    auto client = floserver::core::ServiceClient::connect(floserver::core::ipc::default_instance(), error);
    if (!client) {
        if (!error.empty()) {
            std::cerr << "FloServer: " << error << "\n";
            return 1;
        }
        std::cout << "FloServer isn't running. Start it with: flo service start\n";
        return 3;
    }
    const auto reply = client->call("status");
    if (!reply || !reply->ok || !reply->result.is_object()) {
        std::cerr << "FloServer is running but didn't answer properly.\n";
        return 1;
    }
    const auto& r = reply->result;
    std::cout << "FloServer " << r.value("version", std::string("?")) << " is running (pid "
              << r.value("pid", 0) << ", up " << human_duration(r.value("uptime_s", std::int64_t{0}));
    if (r.contains("memory_bytes") && r["memory_bytes"].is_number_unsigned()) {
        std::cout << ", memory " << human_bytes(r["memory_bytes"].get<std::uint64_t>());
    }
    std::cout << ")\n";
    return 0;
}

int service(const std::vector<std::string>& args) {
    const std::string instance = floserver::core::ipc::default_instance();
    std::string error;
    if (args.size() == 2 && args[1] == "start") {
        switch (floserver::core::start_service(instance, error)) {
            case floserver::core::StartResult::Started:
                std::cout << "FloServer started.\n";
                return 0;
            case floserver::core::StartResult::AlreadyRunning:
                std::cout << "FloServer is already running.\n";
                return 0;
            case floserver::core::StartResult::Failed:
                std::cerr << "FloServer didn't start: " << error << "\n";
                return 1;
        }
    }
    if (args.size() == 2 && args[1] == "stop") {
        if (!floserver::core::stop_service(instance, error)) {
            std::cerr << "FloServer didn't stop: " << error << "\n";
            return 1;
        }
        std::cout << "FloServer is stopped.\n";
        return 0;
    }
    std::cerr << "Usage: flo service start | flo service stop\n";
    return 2;
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
    if (args[0] == "status") {
        return status();
    }
    if (args[0] == "service") {
        return service(args);
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

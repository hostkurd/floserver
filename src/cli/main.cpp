// floserver: the command line (D2). Built as `floserver` and copied as the
// short name `flo` (FloCMS's own CLI is `php flo`, a PHP script, so they don't clash).

#include "floserver/core/build_info.h"
#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_config.h"
#include "floserver/core/version.h"
#include "floserver/core/service_client.h"

#include <chrono>
#include <csignal>
#include <iomanip>
#include <optional>
#include <thread>

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
        << "  status                     Show whether FloServer is running\n"
        << "  service start              Start FloServer in the background\n"
        << "  service stop               Stop FloServer and everything it runs\n"
        << "  runtimes [--refresh]       List installed runtimes and the ones you can install\n"
        << "  install <name> [version]   Install a runtime: flo install php 8.5\n"
        << "  uninstall <name> <version> Remove an installed runtime: flo uninstall php 8.5.12\n"
        << "  projects                   List your projects and what each one needs\n"
        << "  add [folder] [--name <n>]  Add a folder as a project (this folder if none)\n"
        << "  remove <name>              Forget a project (its files are kept)\n"
        << "  use <php|node|mariadb> <v> Set a version in this project's floserver.json\n"
        << "  check [folder]             Check the floserver.json for a project\n"
        << "  version                    Show the version\n"
        << "  help                       Show this help\n";
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

volatile std::sig_atomic_t g_interrupted = 0;

extern "C" void on_interrupt(int /*signal*/) { g_interrupted = 1; }

// Connects to the service, starting it first if it isn't running.
std::optional<floserver::core::ServiceClient> connect_or_start(std::chrono::milliseconds timeout) {
    const std::string instance = floserver::core::ipc::default_instance();
    std::string error;
    if (auto client = floserver::core::ServiceClient::connect(instance, error, timeout)) {
        return client;
    }
    if (!error.empty()) {
        std::cerr << "FloServer: " << error << "\n";
        return std::nullopt;
    }
    if (floserver::core::start_service(instance, error) == floserver::core::StartResult::Failed) {
        std::cerr << "FloServer didn't start: " << error << "\n";
        return std::nullopt;
    }
    auto client = floserver::core::ServiceClient::connect(instance, error, timeout);
    if (!client) {
        std::cerr << "FloServer started but can't be reached" << (error.empty() ? "" : ": " + error) << "\n";
    }
    return client;
}

// Prints a refused request and returns the exit code.
int print_refusal(const std::optional<floserver::core::protocol::Reply>& reply) {
    if (!reply) {
        std::cerr << "FloServer stopped answering.\n";
    } else {
        std::cerr << reply->error_message << "\n";
    }
    return 1;
}

int runtimes(const std::vector<std::string>& args) {
    const bool refresh = args.size() > 1 && args[1] == "--refresh";
    auto client = connect_or_start(std::chrono::seconds(90));
    if (!client) {
        return 1;
    }
    const auto reply = client->call("runtimes.list", {{"refresh", refresh}});
    if (!reply || !reply->ok || !reply->result.is_object()) {
        return print_refusal(reply);
    }
    const auto& r = reply->result;
    const auto text = [](const nlohmann::json& j, const char* key) {
        return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
    };
    std::cout << "Installed:\n";
    if (r["installed"].empty()) {
        std::cout << "  nothing yet\n";
    }
    for (const auto& i : r["installed"]) {
        std::cout << "  " << std::left << std::setw(10) << text(i, "name") << std::setw(10) << text(i, "folder")
                  << text(i, "path") << "\n";
    }
    std::cout << "\n";
    if (!r["list"].is_object()) {
        std::cout << "The runtime list isn't available: " << text(r, "list_error") << "\n";
        return 1;
    }
    std::cout << "Available for " << text(r, "platform") << " (list " << r["list"].value("sequence", 0)
              << ", valid until " << text(r["list"], "expires") << "):\n";
    for (const auto& a : r["available"]) {
        const std::string variant = text(a, "variant");
        std::cout << "  " << std::left << std::setw(10) << text(a, "name") << std::setw(10)
                  << (text(a, "version") + (variant.empty() || variant == "nts" ? "" : "-" + variant))
                  << std::right << std::setw(10) << human_bytes(a.value("size", std::uint64_t{0}))
                  << (a.value("installed", false) ? "   installed" : "") << "\n";
    }
    for (const auto& w : r["list"]["warnings"]) {
        if (w.is_string()) {
            std::cout << "  warning: " << w.get<std::string>() << "\n";
        }
    }
    return 0;
}

int install(const std::vector<std::string>& args) {
    if (args.size() < 2 || args.size() > 3) {
        std::cerr << "Usage: flo install <name> [version]   for example: flo install php 8.5\n";
        return 2;
    }
    auto client = connect_or_start(std::chrono::seconds(15));
    if (!client) {
        return 1;
    }
    nlohmann::json params = {{"name", args[1]}};
    if (args.size() == 3) {
        params["version"] = args[2];
    }
    const auto started = client->call("runtimes.install", params);
    if (!started || !started->ok || !started->result.contains("job")) {
        return print_refusal(started);
    }
    const std::int64_t job = started->result["job"].get<std::int64_t>();

    (void)std::signal(SIGINT, on_interrupt);
    bool cancel_sent = false;
    std::size_t last_width = 0;
    for (;;) {
        if (g_interrupted != 0 && !cancel_sent) {
            (void)client->call("jobs.cancel", {{"id", job}});
            cancel_sent = true;
        }
        const auto reply = client->call("jobs.get", {{"id", job}});
        if (!reply || !reply->ok || !reply->result.is_object()) {
            std::cout << "\n";
            return print_refusal(reply);
        }
        const auto& j = reply->result;
        const std::string state = j.value("state", std::string());
        const std::string what = j.value("name", std::string()) + " " +
                                 (j.value("version", std::string()).empty() ? j.value("requested", std::string())
                                                                            : j.value("version", std::string()));
        std::string line;
        if (state == "queued") {
            line = "Waiting for another install to finish...";
        } else if (state == "checking") {
            line = "Checking the runtime list...";
        } else if (state == "downloading") {
            const auto received = j.value("received", std::uint64_t{0});
            const auto total = j.value("total", std::uint64_t{0});
            line = "Downloading " + what + ": " + human_bytes(received) + " of " + human_bytes(total);
            if (total > 0) {
                line += " (" + std::to_string(received * 100 / total) + "%)";
            }
        } else if (state == "installing") {
            line = "Installing " + what + "...";
        }
        if (!line.empty()) {
            std::cout << "\r" << line << std::string(last_width > line.size() ? last_width - line.size() : 0, ' ')
                      << std::flush;
            last_width = line.size();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        if (last_width > 0) {
            std::cout << "\r" << std::string(last_width, ' ') << "\r";
        }
        const std::string message = j.value("message", std::string());
        if (state == "done") {
            std::cout << message << "\n";
            return 0;
        }
        if (state == "cancelled") {
            std::cout << "Install cancelled.\n";
            return 130;
        }
        std::cerr << "Install failed: " << message << "\n";
        return 1;
    }
}

int uninstall(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        std::cerr << "Usage: flo uninstall <name> <version>   for example: flo uninstall php 8.5.12\n"
                  << "(see the installed versions with: flo runtimes)\n";
        return 2;
    }
    auto client = connect_or_start(std::chrono::seconds(60));
    if (!client) {
        return 1;
    }
    const auto reply = client->call("runtimes.remove", {{"name", args[1]}, {"version", args[2]}});
    if (!reply || !reply->ok) {
        return print_refusal(reply);
    }
    std::cout << args[1] << " " << args[2] << " is removed.\n";
    return 0;
}

std::string text_of(const nlohmann::json& j, const char* key) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

std::string missing_list(const nlohmann::json& p) {
    std::string out;
    if (p.contains("missing") && p["missing"].is_array()) {
        for (const auto& m : p["missing"]) {
            if (m.is_string()) {
                out += (out.empty() ? "" : ", ") + m.get<std::string>();
            }
        }
    }
    return out;
}

std::string state_line(const nlohmann::json& p) {
    const std::string state = text_of(p, "state");
    const std::string missing = missing_list(p);
    if (state == "ready") {
        return "ready";
    }
    if (state == "off") {
        return missing.empty() ? "off" : "off (needs " + missing + ")";
    }
    if (state == "needs_download") {
        return "needs " + missing;
    }
    return "problem: " + text_of(p, "problem");
}

int projects() {
    auto client = connect_or_start(std::chrono::seconds(15));
    if (!client) {
        return 1;
    }
    const auto reply = client->call("projects.list");
    if (!reply || !reply->ok || !reply->result.is_object()) {
        return print_refusal(reply);
    }
    const auto& list = reply->result["projects"];
    if (list.empty()) {
        std::cout << "No projects yet. Add a folder with: flo add <folder>\n";
        return 0;
    }
    for (const auto& p : list) {
        const std::string php = text_of(p, "php");
        std::cout << std::left << std::setw(22) << text_of(p, "name") << std::setw(12)
                  << (php.empty() ? std::string("-") : "php " + php)
                  << (p.value("keep_running", false) ? "keep running  " : "") << state_line(p) << "\n"
                  << "  " << text_of(p, "path") << "\n";
    }
    return 0;
}

int add(const std::vector<std::string>& args, const std::vector<fs::path>& paths) {
    std::optional<fs::path> folder;
    std::string name;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--name" && i + 1 < args.size()) {
            name = args[++i];
        } else if (!folder && !args[i].starts_with("--")) {
            folder = paths[i];
        } else {
            std::cerr << "Usage: flo add [folder] [--name <name>]\n";
            return 2;
        }
    }
    std::error_code ec;
    const fs::path dir = fs::absolute(folder.value_or(fs::current_path()), ec).lexically_normal();
    if (ec) {
        std::cerr << "Can't use that folder: " << floserver::core::error_text(ec) << "\n";
        return 1;
    }
    auto client = connect_or_start(std::chrono::seconds(15));
    if (!client) {
        return 1;
    }
    nlohmann::json params = {{"path", shown(dir)}};
    if (!name.empty()) {
        params["name"] = name;
    }
    const auto reply = client->call("projects.add", params);
    if (!reply || !reply->ok) {
        return print_refusal(reply);
    }
    std::cout << "Added " << text_of(reply->result, "name") << " (" << shown(dir) << ").\n";
    const std::string missing = missing_list(reply->result);
    if (!missing.empty()) {
        std::cout << "It needs " << missing << ". Install with: flo install <name> <version>\n";
    }
    return 0;
}

int remove_cmd(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        std::cerr << "Usage: flo remove <name>   (see your projects with: flo projects)\n";
        return 2;
    }
    auto client = connect_or_start(std::chrono::seconds(15));
    if (!client) {
        return 1;
    }
    const auto reply = client->call("projects.remove", {{"name", args[1]}});
    if (!reply || !reply->ok) {
        return print_refusal(reply);
    }
    std::cout << args[1] << " is no longer a FloServer project. Its files are untouched.\n";
    return 0;
}

// Sets one version in the nearest floserver.json (or a new one in this
// folder), keeping every other key and its order as it was.
int use(const std::vector<std::string>& args) {
    if (args.size() != 3 || (args[1] != "php" && args[1] != "node" && args[1] != "mariadb")) {
        std::cerr << "Usage: flo use <php|node|mariadb> <version>   for example: flo use php 8.4\n";
        return 2;
    }
    const auto version = floserver::core::Version::parse(args[2]);
    if (!version) {
        std::cerr << "\"" << args[2] << "\" is not a version such as 8.4 or 8.4.27\n";
        return 2;
    }
    const fs::path here = fs::current_path();
    const fs::path file = floserver::core::find_project_config(here).value_or(
        here / floserver::core::path_from_utf8(floserver::core::kProjectConfigFileName));

    nlohmann::ordered_json doc = nlohmann::ordered_json::object();
    std::error_code ec;
    if (fs::exists(file, ec)) {
        const auto text = floserver::core::read_small_file(file, std::size_t{64} * 1024);
        if (text) {
            doc = nlohmann::ordered_json::parse(*text, nullptr, false);
        }
        if (!text || doc.is_discarded() || !doc.is_object()) {
            std::cerr << shown(file) << " isn't valid; fix it first (flo check shows what's wrong).\n";
            return 1;
        }
    }
    if (args[1] == "mariadb") {
        doc["database"] = "mariadb-" + version->to_string();
    } else {
        doc[args[1]] = version->to_string();
    }
    const std::string out = doc.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    const auto parsed = parse_project_config(out);
    if (!parsed.config) {
        std::cerr << shown(file) << " has an error; fix it first (flo check shows what's wrong).\n";
        return 1;
    }
    std::string error;
    if (!floserver::core::write_file_atomically(file, out, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    std::cout << shown(file) << ": " << args[1] << " " << version->to_string() << "\n";
    return 0;
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

int run_command(const std::vector<std::string>& args, const std::vector<fs::path>& paths) {
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
    if (args[0] == "runtimes") {
        return runtimes(args);
    }
    if (args[0] == "install") {
        return install(args);
    }
    if (args[0] == "uninstall") {
        return uninstall(args);
    }
    if (args[0] == "projects") {
        return projects();
    }
    if (args[0] == "add") {
        return add(args, paths);
    }
    if (args[0] == "remove") {
        return remove_cmd(args);
    }
    if (args[0] == "use") {
        return use(args);
    }
    if (args[0] == "check") {
        return check(paths.size() > 1 ? paths[1] : fs::current_path());
    }
    std::cerr << "Unknown command: " << args[0] << "\n\n";
    print_help(std::cerr);
    return 2;
}

// Nothing a command does may end the program with an unhandled exception.
int run(const std::vector<std::string>& args, const std::vector<fs::path>& paths) noexcept {
    try {
        return run_command(args, paths);
    } catch (const std::exception& e) {
        std::cerr << "FloServer: unexpected error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "FloServer: unexpected error\n";
    }
    return 1;
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

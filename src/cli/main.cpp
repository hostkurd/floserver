// floserver: the command line (D2). Built as `floserver` and copied as the
// short name `flo` (FloCMS's own CLI is `php flo`, a PHP script, so they don't clash).

#include "floserver/core/build_info.h"
#include "floserver/core/encoding.h"
#include "floserver/core/ipc.h"
#include "floserver/core/mariadb.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_config.h"
#include "floserver/core/version.h"
#include "floserver/core/service_client.h"
#include "floserver/core/trust_store.h"

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
        << "  start [project]            Start FloServer, or turn a project on and show its address\n"
        << "  stop [project]             Turn a project off, or stop FloServer and everything it runs\n"
        << "  runtimes [--refresh]       List installed runtimes and the ones you can install\n"
        << "  install <name> [version]   Install a runtime: flo install php 8.5\n"
        << "  uninstall <name> <version> Remove an installed runtime: flo uninstall php 8.5.12\n"
        << "  projects                   List your projects and what each one needs\n"
        << "  add [folder] [--name <n>]  Add a folder as a project (this folder if none)\n"
        << "  remove <name>              Forget a project (its files are kept)\n"
        << "  use <php|node|python|mariadb> <v> Set a version in this project's floserver.json\n"
        << "  db                         Show the databases and the projects using them\n"
        << "  env [project] [--write]    A project's database settings as .env lines (--show: the password)\n"
        << "  trust                      Let your browsers trust FloServer's HTTPS certificates\n"
        << "  untrust                    Remove FloServer's certificate authority\n"
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
        const std::string run = text_of(p, "run");
        if (run == "failed") {
            // One line of it, printable: the rest is in the app's log.
            std::string why = text_of(p, "error");
            why = why.substr(0, why.find('\n'));
            std::erase_if(why, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
            return text_of(p, "node").empty() && text_of(p, "python").empty() ? "on, PHP can't start"
                                              : "on, its server can't start" + (why.empty() ? "" : ": " + why);
        }
        return run == "running" ? "on, running" : run == "starting" ? "on, starting" : run.empty() ? "on" : "on, asleep";
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
        const std::string node = text_of(p, "node");
        const std::string python = text_of(p, "python");
        std::cout << std::left << std::setw(22) << text_of(p, "name") << std::setw(16)
                  << (!php.empty()      ? "php " + php
                      : !python.empty() ? "python " + python
                      : !node.empty()   ? "node " + node
                                        : std::string("-"))
                  << (p.value("keep_running", false) ? "keep running  " : "") << state_line(p) << "\n";
        if (!text_of(p, "address").empty()) {
            std::cout << "  " << text_of(p, "address") << "\n";
        }
        std::cout << "  " << text_of(p, "path") << "\n";
    }
    return 0;
}

// One line per On project: its address, or what keeps it from running.
void print_served(const nlohmann::json& p) {
    const std::string address = text_of(p, "address");
    if (!address.empty()) {
        std::cout << "  " << std::left << std::setw(20) << text_of(p, "name") << address << "\n";
    } else {
        std::cout << "  " << std::left << std::setw(20) << text_of(p, "name") << state_line(p) << "\n";
    }
}

// What the web server says when something is wrong, and the HTTPS hint.
void print_web_notes(floserver::core::ServiceClient& client) {
    const auto web = client.call("web.status");
    if (!web || !web->ok) {
        return;
    }
    const auto& w = web->result;
    if (w.contains("error") && w["error"].is_string()) {
        std::cout << "Note: " << w["error"].get<std::string>() << "\n";
    }
    if (w["nginx"].contains("error") && w["nginx"]["error"].is_string()) {
        std::cout << "nginx: " << w["nginx"]["error"].get<std::string>() << "\n";
    }
    if (w["nginx"].value("running", false) && !w["nginx"].value("tls", false)) {
        std::cout << "For https addresses, run: flo trust\n";
    }
}

// flo start [name]: FloServer and every project that's On, or turns one on.
int start(const std::vector<std::string>& args) {
    if (args.size() > 2) {
        std::cerr << "Usage: flo start [project]\n";
        return 2;
    }
    auto client = connect_or_start(std::chrono::seconds(60));
    if (!client) {
        return 1;
    }
    if (args.size() == 2) {
        const auto reply = client->call("projects.set", {{"name", args[1]}, {"on", true}});
        if (!reply || !reply->ok) {
            return print_refusal(reply);
        }
        print_served(reply->result);
        print_web_notes(*client);
        return text_of(reply->result, "address").empty() ? 1 : 0;
    }
    (void)client->call("web.reload");
    const auto list = client->call("projects.list");
    if (!list || !list->ok) {
        return print_refusal(list);
    }
    bool any = false;
    for (const auto& p : list->result["projects"]) {
        if (p.value("on", false)) {
            any = true;
            print_served(p);
        }
    }
    if (!any) {
        std::cout << "FloServer is running. No project is on yet: flo start <project>\n";
    }
    print_web_notes(*client);
    return 0;
}

// flo stop [name]: turns one project off, or stops everything.
int stop(const std::vector<std::string>& args) {
    if (args.size() > 2) {
        std::cerr << "Usage: flo stop [project]\n";
        return 2;
    }
    if (args.size() == 1) {
        return service({"service", "stop"});
    }
    auto client = connect_or_start(std::chrono::seconds(60));
    if (!client) {
        return 1;
    }
    const auto reply = client->call("projects.set", {{"name", args[1]}, {"on", false}});
    if (!reply || !reply->ok) {
        return print_refusal(reply);
    }
    std::cout << args[1] << " is off.\n";
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

// The registered project folder that holds `here`, when the service is
// running and knows one. The CLI doesn't start the service just for this.
// `here` is `dir` or inside it. Windows compares paths without regard to
// case (c:\ and C:\ are the same folder).
bool is_inside(const fs::path& here, const fs::path& dir) {
#ifdef _WIN32
    auto lower = [](const fs::path& p) {
        std::wstring w = p.lexically_normal().native();
        if (!w.empty()) {
            CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
        }
        return fs::path(w);
    };
    const fs::path inside = lower(here).lexically_relative(lower(dir));
#else
    const fs::path inside = here.lexically_relative(dir);
#endif
    return !inside.empty() && *inside.begin() != "..";
}

std::optional<fs::path> project_folder_for(const fs::path& here) {
    std::string error;
    auto client = floserver::core::ServiceClient::connect(floserver::core::ipc::default_instance(), error);
    if (!client) {
        return std::nullopt;
    }
    const auto reply = client->call("projects.list");
    if (!reply || !reply->ok || !reply->result.contains("projects")) {
        return std::nullopt;
    }
    std::error_code ec;
    const fs::path real_here = fs::weakly_canonical(here, ec);
    std::optional<fs::path> best;
    for (const auto& p : reply->result["projects"]) {
        const fs::path dir = floserver::core::path_from_utf8(text_of(p, "path"));
        if (is_inside(real_here, dir) && (!best || dir.native().size() > best->native().size())) {
            best = dir;
        }
    }
    return best;
}

// Sets one version in the project's floserver.json, keeping every other key
// and its order as it was. The file is the registered project's own when the
// current folder is inside one; otherwise the nearest one above, or a new one
// here.
int use(const std::vector<std::string>& args) {
    if (args.size() != 3 || (args[1] != "php" && args[1] != "node" && args[1] != "python" && args[1] != "mariadb")) {
        std::cerr << "Usage: flo use <php|node|python|mariadb> <version>   for example: flo use php 8.4\n";
        return 2;
    }
    const auto version = floserver::core::Version::parse(args[2]);
    if (!version) {
        std::cerr << "\"" << args[2] << "\" is not a version such as 8.4 or 8.4.27\n";
        return 2;
    }
    const fs::path here = fs::current_path();
    const fs::path name = floserver::core::path_from_utf8(floserver::core::kProjectConfigFileName);
    fs::path file;
    bool registered = false;
    if (const auto project = project_folder_for(here)) {
        file = *project / name;
        registered = true;
    } else {
        file = floserver::core::find_project_config(here).value_or(here / name);
    }

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
    if (!registered) {
        std::cout << "(This folder isn't a FloServer project yet. Add it with: flo add)\n";
    }
    return 0;
}

std::optional<std::vector<std::uint8_t>> ca_der(const nlohmann::json& ca) {
    return floserver::core::from_base64(text_of(ca, "der"));
}

// Makes the CA (in the service) and adds it to the user's trust store here,
// in the foreground, because Windows asks the user to confirm.
int trust() {
    auto client = connect_or_start(std::chrono::seconds(30));
    if (!client) {
        return 1;
    }
    const auto reply = client->call("certs.ensure_ca");
    if (!reply || !reply->ok || !reply->result.contains("ca")) {
        return print_refusal(reply);
    }
    const auto& ca = reply->result["ca"];
    const auto der = ca_der(ca);
    if (!der) {
        std::cerr << "FloServer sent a certificate that can't be read.\n";
        return 1;
    }
    const std::string name = text_of(ca, "common_name");
    switch (floserver::core::ca_trust_state(*der)) {
        case floserver::core::TrustState::Trusted:
            std::cout << name << " is already trusted.\n";
            return 0;
        case floserver::core::TrustState::Unsupported:
            std::cout << "On this system, add " << text_of(ca, "path")
                      << " to your browser's or system's trusted certificates by hand.\n";
            return 1;
        case floserver::core::TrustState::NotTrusted:
            break;
    }
    std::cout << "Adding " << name << " to your certificates.\n"
              << "Windows will ask you to confirm; the prompt names \"" << name << "\".\n"
              << "It can only sign localhost names, so it can't be used for any real website.\n";
    bool cancelled = false;
    std::string error;
    if (!floserver::core::trust_ca(*der, cancelled, error)) {
        std::cerr << (cancelled ? "Not added: you said no. Run flo trust again when you're ready.\n"
                                : "Not added: " + error + "\n");
        return 1;
    }
    (void)client->call("web.reload");  // projects switch to https now
    std::cout << "Done. Browsers accept https://<project>.localhost now (restart a browser that was open).\n"
              << "Firefox: if it still warns, open about:config and set security.enterprise_roots.enabled to true.\n";
    return 0;
}

int untrust() {
    auto client = connect_or_start(std::chrono::seconds(30));
    if (!client) {
        return 1;
    }
    const auto status = client->call("certs.status");
    if (!status || !status->ok) {
        // A damaged CA can still be removed from disk; its store entry needs
        // the certificate, which we no longer have.
        if (status && status->error_code == "ca_damaged") {
            const auto removed = client->call("certs.remove");
            if (removed && removed->ok) {
                std::cout << "Removed the damaged certificate authority's files. If Windows still lists a "
                             "\"FloServer local CA\", remove it in certmgr.msc under Trusted Root Certification "
                             "Authorities.\n";
                return 0;
            }
        }
        return print_refusal(status);
    }
    const auto& ca = status->result["ca"];
    if (ca.is_object()) {
        const auto der = ca_der(ca);
        if (!der) {
            std::cerr << "FloServer sent a certificate that can't be read, so nothing was removed.\n";
            return 1;
        }
        bool cancelled = false;
        std::string error;
        if (!floserver::core::untrust_ca(*der, cancelled, error)) {
            std::cerr << (cancelled ? "Not removed: you said no.\n" : "Not removed: " + error + "\n");
            return 1;
        }
    }
    const auto removed = client->call("certs.remove");
    if (!removed || !removed->ok) {
        return print_refusal(removed);
    }
    std::cout << "FloServer's certificate authority and project certificates are removed.\n"
              << "Run flo trust to make a new one.\n";
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

// flo db: the MariaDB servers and the projects using each.
int db() {
    auto client = connect_or_start(std::chrono::seconds(60));
    if (!client) {
        return 1;
    }
    const auto reply = client->call("databases.list");
    if (!reply || !reply->ok) {
        return print_refusal(reply);
    }
    const auto& servers = reply->result["servers"];
    if (servers.empty()) {
        std::cout << "No database is in use. A project asks for one in its floserver.json: flo use mariadb 11.4\n";
        return 0;
    }
    for (const auto& s : servers) {
        std::string projects;
        for (const auto& p : s["projects"]) {
            projects += (projects.empty() ? "" : ", ") + p.get<std::string>();
        }
        std::cout << "MariaDB " << text_of(s, "version") << "  127.0.0.1:" << s.value("port", 0) << "  "
                  << text_of(s, "state") << (s.value("keep_running", false) ? " (keep running)" : "") << "  " << projects
                  << "\n";
        if (s.contains("error")) {
            std::cout << "  " << text_of(s, "error") << "\n";
        }
    }
    return 0;
}

// flo env [project] [--show] [--write]: a project's database settings as
// .env lines (the password hidden unless --show), or written into its .env.
int env(const std::vector<std::string>& args) {
    std::string name;
    bool show = false;
    bool write = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--show") {
            show = true;
        } else if (args[i] == "--write") {
            write = true;
        } else if (name.empty() && !args[i].starts_with("-")) {
            name = args[i];
        } else {
            std::cerr << "Usage: flo env [project] [--show] [--write]\n";
            return 2;
        }
    }
    auto client = connect_or_start(std::chrono::seconds(180));  // the first time, MariaDB sets up its data folder
    if (!client) {
        return 1;
    }
    if (name.empty()) {
        const auto list = client->call("projects.list");
        if (!list || !list->ok) {
            return print_refusal(list);
        }
        std::error_code ec;
        const fs::path here = fs::weakly_canonical(fs::current_path(), ec);
        std::size_t best = 0;
        for (const auto& p : list->result["projects"]) {
            const fs::path dir = floserver::core::path_from_utf8(text_of(p, "path"));
            if (is_inside(here, dir) && dir.native().size() > best) {
                best = dir.native().size();
                name = text_of(p, "name");
            }
        }
        if (name.empty()) {
            std::cerr << "This folder isn't in a project. Name one: flo env <project>\n";
            return 2;
        }
    }
    // The first time can take a while: MariaDB sets up its data folder.
    const auto reply = client->call("databases.login", {{"name", name}});
    if (!reply || !reply->ok) {
        return print_refusal(reply);
    }
    const auto& r = reply->result;
    if (!r.contains("env")) {
        std::cerr << "The project's folder can't be found.\n";
        return 1;
    }
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto& kv : r["env"]) {
        values.emplace_back(kv[0].get<std::string>(), kv[1].get<std::string>());
    }
    if (write) {
        const fs::path file = floserver::core::path_from_utf8(text_of(r, "env_file"));
        std::error_code ec;
        std::string text;
        if (fs::exists(file, ec)) {
            const auto old = floserver::core::read_small_file(file, std::size_t{1024} * 1024);
            if (!old) {
                std::cerr << "Can't read " << shown(file) << "\n";
                return 1;
            }
            text = *old;
        }
        std::string error;
        const bool existed = fs::exists(file, ec);
        const auto perms = existed ? fs::status(file, ec).permissions() : fs::perms::none;
        if (!floserver::core::write_file_atomically(file, floserver::core::update_env_text(text, values), error)) {
            std::cerr << error << "\n";
            return 1;
        }
#ifndef _WIN32
        // The file keeps who may read it; a new one is the user's only (it
        // holds a password). The project folder is left as it is.
        fs::permissions(file, existed ? perms : (fs::perms::owner_read | fs::perms::owner_write),
                        fs::perm_options::replace, ec);
#else
        (void)perms;
#endif
        std::cout << "Database settings written to " << shown(file) << "\n";
        return 0;
    }
    for (const auto& [key, value] : values) {
        std::string line = value;
        if (!show && key == "DB_PASSWORD") {
            line = "********   (flo env --show)";
        } else if (!show && key == "DATABASE_URL") {
            // mysql://user:<password>@host...: the password hidden, or all
            // of it when it isn't in that shape.
            const auto scheme = value.find("://");
            const auto colon = scheme == std::string::npos ? scheme : value.find(':', scheme + 3);
            const auto at = value.rfind('@');
            line = colon != std::string::npos && at != std::string::npos && colon < at
                       ? value.substr(0, colon + 1) + "********" + value.substr(at) + "   (flo env --show)"
                       : "********   (flo env --show)";
        }
        std::cout << key << "=" << line << "\n";
    }
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
    if (args[0] == "start") {
        return start(args);
    }
    if (args[0] == "stop") {
        return stop(args);
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
    if (args[0] == "trust" && args.size() == 1) {
        return trust();
    }
    if (args[0] == "untrust" && args.size() == 1) {
        return untrust();
    }
    if (args[0] == "db" && args.size() == 1) {
        return db();
    }
    if (args[0] == "env") {
        return env(args);
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

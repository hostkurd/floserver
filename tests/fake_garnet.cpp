// A stand-in for Garnet in the service's tests (D30), where the real one
// can't be had: started as "<dotnet>/dotnet GarnetServer.dll <args>", it
// takes Garnet's arguments, reads the password from its config file (and
// refuses to start when one is in an argument), listens on the Unix socket,
// asks for AUTH first, and keeps its keys across SAVE and --recover. Every
// byte it's sent is appended to <checkpointdir>/../fake-received.log, so a
// test can see what reached it.

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
std::map<std::string, std::string> g_store;
std::string g_password;
fs::path g_dump;
fs::path g_received;

std::string bulk(const std::string& s) { return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n"; }

void save() {
    nlohmann::json doc = nlohmann::json::object();
    for (const auto& [k, v] : g_store) {
        doc[k] = v;
    }
    std::ofstream(g_dump, std::ios::binary | std::ios::trunc) << doc.dump();
}

// One command from `buffer` (taken out), or nothing while it's incomplete.
std::optional<std::vector<std::string>> take_command(std::string& buffer) {
    if (buffer.empty()) {
        return std::nullopt;
    }
    if (buffer[0] != '*') {
        const auto end = buffer.find('\n');
        if (end == std::string::npos) {
            return std::nullopt;
        }
        std::string line = buffer.substr(0, end);
        buffer.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::vector<std::string> out;
        std::istringstream in(line);
        for (std::string word; in >> word;) {
            out.push_back(word);
        }
        return out;
    }
    std::size_t pos = 0;
    const auto read_line = [&](std::string& line) {
        const auto end = buffer.find("\r\n", pos);
        if (end == std::string::npos) {
            return false;
        }
        line = buffer.substr(pos, end - pos);
        pos = end + 2;
        return true;
    };
    std::string line;
    if (!read_line(line)) {
        return std::nullopt;
    }
    const long n = std::strtol(line.c_str() + 1, nullptr, 10);
    std::vector<std::string> out;
    for (long i = 0; i < n; ++i) {
        if (!read_line(line) || line.empty() || line[0] != '$') {
            return std::nullopt;
        }
        const auto size = static_cast<std::size_t>(std::strtol(line.c_str() + 1, nullptr, 10));
        if (buffer.size() < pos + size + 2) {
            return std::nullopt;
        }
        out.push_back(buffer.substr(pos, size));
        pos += size + 2;
    }
    buffer.erase(0, pos);
    return out;
}

void serve(int fd) {
    bool authed = false;
    std::string buffer;
    char chunk[4096];  // NOLINT(modernize-avoid-c-arrays)
    for (;;) {
        const ssize_t got = recv(fd, chunk, sizeof chunk, 0);
        if (got <= 0) {
            break;
        }
        {
            const std::lock_guard lock(g_mutex);
            std::ofstream(g_received, std::ios::binary | std::ios::app).write(chunk, got);
        }
        buffer.append(chunk, static_cast<std::size_t>(got));
        while (auto cmd = take_command(buffer)) {
            if (cmd->empty()) {
                continue;
            }
            std::string name = (*cmd)[0];
            for (char& c : name) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            std::string reply;
            const std::lock_guard lock(g_mutex);
            if (name == "AUTH") {
                authed = cmd->size() == 2 && (*cmd)[1] == g_password;
                reply = authed ? "+OK\r\n" : "-WRONGPASS invalid password\r\n";
            } else if (!authed) {
                reply = "-NOAUTH Authentication required.\r\n";
            } else if (name == "PING") {
                reply = "+PONG\r\n";
            } else if (name == "SET" && cmd->size() == 3) {
                g_store[(*cmd)[1]] = (*cmd)[2];
                reply = "+OK\r\n";
            } else if (name == "GET" && cmd->size() == 2) {
                const auto it = g_store.find((*cmd)[1]);
                reply = it == g_store.end() ? "$-1\r\n" : bulk(it->second);
            } else if (name == "DBSIZE") {
                reply = ":" + std::to_string(g_store.size()) + "\r\n";
            } else if (name == "FLUSHALL") {
                g_store.clear();
                reply = "+OK\r\n";
            } else if (name == "SAVE") {
                // Test switches next to the checkpoints: a slow SAVE, a failing one.
                std::error_code ec;
                if (fs::exists(g_dump.parent_path() / "slow-save", ec)) {
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                }
                // busy-saves holds N: the next N SAVEs meet Garnet's own checkpoint.
                const fs::path busy = g_dump.parent_path() / "busy-saves";
                int left = 0;
                if (fs::exists(busy, ec)) {
                    std::ifstream(busy) >> left;
                }
                if (left > 0) {
                    std::ofstream(busy, std::ios::trunc) << (left - 1);
                    reply = "-ERR checkpoint already in progress\r\n";
                } else if (fs::exists(g_dump.parent_path() / "fail-save", ec)) {
                    reply = "-ERR checkpoint failed\r\n";
                } else {
                    save();
                    reply = "+OK\r\n";
                }
            } else if (name == "QUIT") {
                (void)send(fd, "+OK\r\n", 5, MSG_NOSIGNAL);
                close(fd);
                return;
            } else {
                reply = "-ERR unknown command\r\n";
            }
            (void)send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
        }
    }
    close(fd);
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> opts;
    bool recover = false;
    for (int i = 2; i < argc; ++i) {  // argv[1]: GarnetServer.dll
        const std::string a = argv[i];
        if (a == "--recover") {
            recover = true;
        } else if (a == "--lua") {
            continue;
        } else if (a == "--password" || a == "--auth") {
            std::fprintf(stderr, "fake garnet: a secret in the arguments\n");
            return 3;
        } else if (a.rfind("--", 0) == 0 && i + 1 < argc) {
            opts[a] = argv[++i];
        }
    }
    if (std::getenv("DOTNET_ROOT") == nullptr || std::getenv("DOTNET_STARTUP_HOOKS") != nullptr) {
        std::fprintf(stderr, "fake garnet: DOTNET_ROOT missing or a startup hook set\n");
        return 5;
    }
    std::ifstream conf(opts["--config-import-path"]);
    const nlohmann::json config = nlohmann::json::parse(conf, nullptr, false);
    if (!config.is_object() || config.value("AuthenticationMode", "") != "Password" ||
        config.value("Password", "").empty()) {
        std::fprintf(stderr, "fake garnet: no password in the config file\n");
        return 4;
    }
    g_password = config.value("Password", "");
    const fs::path checkpoints = opts["--checkpointdir"];
    fs::create_directories(checkpoints);
    g_dump = checkpoints / "fake.dump";
    g_received = checkpoints.parent_path() / "fake-received.log";
    if (recover) {
        std::ifstream in(g_dump);
        const nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
        if (doc.is_object()) {
            for (const auto& [k, v] : doc.items()) {
                g_store[k] = v.get<std::string>();
            }
        }
    }
    const std::string path = opts["--unixsocket"];
    ::unlink(path.c_str());
    const int s = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || listen(s, 64) != 0) {
        std::perror("fake garnet: listen");
        return 6;
    }
    ::chmod(path.c_str(), 0600);
    std::printf("fake garnet listening on %s\n", path.c_str());
    std::fflush(stdout);
    for (;;) {
        const int c = accept(s, nullptr, nullptr);
        if (c >= 0) {
            std::thread(serve, c).detach();
        }
    }
}

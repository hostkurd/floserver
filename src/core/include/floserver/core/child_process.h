#pragma once

// Programs the service runs and watches (D2, D11): nginx and php-cgi.
//
// Each child and everything it starts (nginx's worker, php-cgi's children)
// is one unit: a job object on Windows (closed with the child, so nothing is
// left running if the service itself is killed), a process group elsewhere.
// Programs are always started by full path, never looked up in PATH.
//
// A LoopbackListener is a listening TCP socket on 127.0.0.1 that the service
// holds for good (Q1): php-cgi is started with it as standard input when the
// first connection waits, and stopped again when idle, while the socket and
// any waiting connections stay.

#include "floserver/core/ports.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace floserver::core {

class LoopbackListener {
public:
    // Listens on 127.0.0.1:port. Nothing (with `error`) when the port is taken.
    [[nodiscard]] static std::unique_ptr<LoopbackListener> open(Port port, std::string& error);
    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;
    LoopbackListener(LoopbackListener&&) = delete;
    LoopbackListener& operator=(LoopbackListener&&) = delete;
    ~LoopbackListener();

    [[nodiscard]] Port port() const { return port_; }
    // The native socket (SOCKET on Windows, a file descriptor elsewhere).
    [[nodiscard]] std::uintptr_t native() const { return socket_; }
    // Accepts one waiting connection and closes it at once, so the web
    // server answers "bad gateway" straight away instead of waiting for a
    // pool that can't run. Only call when a connection is waiting.
    void drop_waiting_connection() const;

private:
    LoopbackListener(std::uintptr_t socket, Port port) : socket_(socket), port_(port) {}
    std::uintptr_t socket_;
    Port port_;
};

// GET http://127.0.0.1:port<path> with that Host header; the body when the
// status is 200, else nothing. For the service's own checks on nginx.
[[nodiscard]] std::optional<std::string> loopback_http_get(Port port, const std::string& host, const std::string& path,
                                                           std::chrono::milliseconds limit);

// Which of `listeners` have a connection waiting, waiting at most `limit`.
// Indexes into `listeners`; empty when none did in time.
[[nodiscard]] std::vector<std::size_t> waiting_connections(const std::vector<const LoopbackListener*>& listeners,
                                                           std::chrono::milliseconds limit);

struct ChildOptions {
    std::filesystem::path program;  // absolute
    std::vector<std::string> args;
    std::filesystem::path working_dir;
    // Added to (or replacing) this process's environment.
    std::vector<std::pair<std::string, std::string>> env;
    // Taken out of it (names, case-insensitive on Windows).
    std::vector<std::string> remove_env;
    // Standard input is this socket (php-cgi's FastCGI listener).
    const LoopbackListener* stdin_socket = nullptr;
    // Standard output and error are appended here. Empty: none at all (on
    // Windows the handles are INVALID_HANDLE_VALUE, which php-cgi needs to
    // see before it runs as a FastCGI server).
    std::filesystem::path output_log;
};

// True when process `pid` exists and runs `program` (compared by the file
// system's own idea of the same file). Used before signalling a pid read
// from a file, which may by now belong to someone else.
[[nodiscard]] bool process_is_program(std::uint32_t pid, const std::filesystem::path& program);

// Waits until process `pid` is gone; true when it is.
bool wait_for_process_exit(std::uint32_t pid, std::chrono::milliseconds limit);

class ChildProcess {
public:
    [[nodiscard]] static std::unique_ptr<ChildProcess> start(const ChildOptions& options, std::string& error);
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;
    // Stops it and everything it started.
    ~ChildProcess();

    [[nodiscard]] std::uint32_t pid() const { return pid_; }
    // False once the program itself has ended.
    [[nodiscard]] bool running();
    // Its exit code, waiting at most `limit`; nothing while it still runs.
    [[nodiscard]] std::optional<int> wait(std::chrono::milliseconds limit);
    // Ends it and everything it started, and waits for that.
    void kill();

private:
    struct Native;
    explicit ChildProcess(std::unique_ptr<Native> native, std::uint32_t pid);
    std::unique_ptr<Native> native_;
    std::uint32_t pid_ = 0;
    std::optional<int> exit_code_;
};

}  // namespace floserver::core

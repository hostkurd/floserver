#pragma once

// The private channel between floserverd and the window and CLI (D2).
//
// Windows: a named pipe, \\.\pipe\floserver-<user SID>-<instance>
//   - its access list lets only the current user in; remote clients are refused
//   - the service claims the name with FILE_FLAG_FIRST_PIPE_INSTANCE, so only
//     one service per user and instance can run
//   - a client checks that the program serving the pipe runs as the same user,
//     so another account can't pose as the service by taking the name first
//
// macOS and Linux: a Unix socket in a folder only the user can open
//   ($XDG_RUNTIME_DIR/floserver, else $TMPDIR/floserver-<uid>, else
//   /tmp/floserver-<uid>), with a lock file for the single instance; both sides
//   check the other end's user id.
//
// Messages are a 4-byte little-endian length, then that many bytes of UTF-8
// JSON, at most kMaxMessage bytes.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace floserver::core::ipc {

inline constexpr std::uint32_t kMaxMessage = 1U << 20U;  // 1 MiB

// "floserverd" normally. FLOSERVER_INSTANCE in the environment adds a suffix,
// so tests and a second development copy don't meet the real service.
[[nodiscard]] std::string default_instance();

// One end of a connection. Move-only; closes on destruction.
class Connection {
public:
    Connection() = default;
    ~Connection();
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] bool valid() const;
    // Both return false / nothing when the other end is gone, a message is
    // too long, or the connection breaks. A failed connection stays failed.
    [[nodiscard]] bool send(std::string_view message);
    [[nodiscard]] std::optional<std::string> receive();
    void close();

    struct Impl;
    explicit Connection(std::unique_ptr<Impl> impl);

private:
    std::unique_ptr<Impl> impl_;
};

enum class ListenStatus : std::uint8_t {
    Listening,
    AlreadyRunning,  // this user's service for this instance is already up
    Failed,          // see the error text
};

class Server {
public:
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // Claims the channel for `instance`. On Listening, `server` is set.
    [[nodiscard]] static ListenStatus listen(const std::string& instance,
                                             std::unique_ptr<Server>& server, std::string& error);

    // Waits for the next client. Nothing once stop() was called.
    [[nodiscard]] std::optional<Connection> accept();

    // Makes accept() return nothing; safe to call from any thread, more than once.
    void stop();

    struct Impl;

private:
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Connects to this user's service for `instance`. Nothing when it isn't
// running; `error` says why when something is wrong beyond that (for example
// the channel is held by another user's program).
[[nodiscard]] std::optional<Connection> connect(const std::string& instance, std::string& error);

// Where the channel lives, for messages and tests.
[[nodiscard]] std::string endpoint_path(const std::string& instance);

}  // namespace floserver::core::ipc

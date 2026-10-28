// macOS and Linux half of the private channel: a Unix socket in a folder only
// the user can open, a lock file for the single instance, and a user-id check
// on both ends.

#ifndef _WIN32

#include "ipc_impl.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace floserver::core::ipc {

namespace {

int set_cloexec(int fd) {
    if (fd >= 0) {
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
}

void no_sigpipe([[maybe_unused]] int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

constexpr int kSendFlags =
#ifdef MSG_NOSIGNAL
    MSG_NOSIGNAL;
#else
    0;
#endif

// The user id at the other end of a connected Unix socket.
bool peer_uid(int fd, uid_t& uid) {
#if defined(SO_PEERCRED) && defined(__linux__)
    struct ucred cred{};
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        return false;
    }
    uid = cred.uid;
    return true;
#else
    gid_t gid = 0;
    return getpeereid(fd, &uid, &gid) == 0;
#endif
}

std::string run_dir() {
    // NOLINTBEGIN(concurrency-mt-unsafe): environment read, not written
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg != nullptr && xdg[0] == '/') {
        return std::string(xdg) + "/floserver";
    }
    const std::string uid = std::to_string(getuid());
#ifdef __APPLE__
    // macOS gives each user a private TMPDIR under /var/folders.
    const char* tmp = std::getenv("TMPDIR");
    if (tmp != nullptr && tmp[0] == '/') {
        std::string dir(tmp);
        while (dir.size() > 1 && dir.back() == '/') {
            dir.pop_back();
        }
        return dir + "/floserver-" + uid;
    }
#endif
    // NOLINTEND(concurrency-mt-unsafe)
    return "/tmp/floserver-" + uid;
}

// Creates `dir` if needed and makes sure it is a real folder owned by this
// user that nobody else can open.
bool ensure_private_dir(const std::string& dir, std::string& error) {
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        error = "can't create " + dir + ": " + std::strerror(errno);  // NOLINT(concurrency-mt-unsafe)
        return false;
    }
    struct stat st{};
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        error = dir + " isn't a folder";
        return false;
    }
    if (st.st_uid != getuid()) {
        error = dir + " belongs to another user; FloServer won't use it";
        return false;
    }
    if ((st.st_mode & 077U) != 0 && chmod(dir.c_str(), 0700) != 0) {
        error = "can't make " + dir + " private";
        return false;
    }
    return true;
}

bool make_address(const std::string& path, sockaddr_un& addr, std::string& error) {
    addr = {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        error = "the channel path is too long: " + path;
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

}  // namespace

// ---- Connection ------------------------------------------------------------

bool Connection::Impl::is_open() const { return fd >= 0; }

bool Connection::Impl::write_all(const std::uint8_t* data, std::size_t size) const {
    while (size > 0) {
        const ssize_t n = ::send(fd, data, size, kSendFlags);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool Connection::Impl::read_all(std::uint8_t* data, std::size_t size) const {
    while (size > 0) {
        const ssize_t n = ::recv(fd, data, size, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

void Connection::Impl::close() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// ---- Server ----------------------------------------------------------------

struct Server::Impl {
    std::string socket_path;
    int listen_fd = -1;
    int lock_fd = -1;
    int wake_read = -1;
    int wake_write = -1;
    std::atomic<bool> stopped{false};

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() {
        if (listen_fd >= 0) {
            ::close(listen_fd);
            // Only the lock holder may remove the socket.
            (void)unlink(socket_path.c_str());
        }
        for (const int fd : {wake_read, wake_write, lock_fd}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }
};

Server::Server(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Server::~Server() = default;

std::string endpoint_path(const std::string& instance) { return run_dir() + "/" + instance + ".sock"; }

ListenStatus Server::listen(const std::string& instance, std::unique_ptr<Server>& server,
                            std::string& error) {
    if (!is_instance_name(instance)) {
        error = "invalid instance name";
        return ListenStatus::Failed;
    }
    const std::string dir = run_dir();
    if (!ensure_private_dir(dir, error)) {
        return ListenStatus::Failed;
    }
    auto impl = std::make_unique<Impl>();
    impl->socket_path = dir + "/" + instance + ".sock";
    const std::string lock_path = dir + "/" + instance + ".lock";

    // One service per user and instance: whoever holds the lock owns the socket.
    impl->lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl->lock_fd < 0) {
        error = "can't open " + lock_path;
        return ListenStatus::Failed;
    }
    if (flock(impl->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            return ListenStatus::AlreadyRunning;
        }
        error = "can't lock " + lock_path;
        return ListenStatus::Failed;
    }

    sockaddr_un addr{};
    if (!make_address(impl->socket_path, addr, error)) {
        return ListenStatus::Failed;
    }
    (void)unlink(impl->socket_path.c_str());  // left over from a crash
    impl->listen_fd = set_cloexec(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (impl->listen_fd < 0 ||
        ::bind(impl->listen_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 ||
        chmod(impl->socket_path.c_str(), 0600) != 0 || ::listen(impl->listen_fd, 16) != 0) {
        error = "can't open the channel at " + impl->socket_path + ": " +
                std::strerror(errno);  // NOLINT(concurrency-mt-unsafe)
        return ListenStatus::Failed;
    }

    std::array<int, 2> wake{-1, -1};
    if (pipe(wake.data()) != 0) {
        error = "can't create the stop signal";
        return ListenStatus::Failed;
    }
    impl->wake_read = set_cloexec(wake[0]);
    impl->wake_write = set_cloexec(wake[1]);

    // The constructor is private, so make_unique can't reach it.
    server.reset(new Server(std::move(impl)));  // NOLINT(cppcoreguidelines-owning-memory)
    return ListenStatus::Listening;
}

std::optional<Connection> Server::accept() {
    while (!impl_->stopped.load()) {
        std::array<pollfd, 2> fds = {{{impl_->listen_fd, POLLIN, 0}, {impl_->wake_read, POLLIN, 0}}};
        const int n = poll(fds.data(), fds.size(), -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        if (impl_->stopped.load() || (fds[1].revents & POLLIN) != 0) {
            return std::nullopt;
        }
        if ((fds[0].revents & POLLIN) == 0) {
            continue;
        }
        const int fd = set_cloexec(::accept(impl_->listen_fd, nullptr, nullptr));
        if (fd < 0) {
            continue;
        }
        uid_t uid = 0;
        if (!peer_uid(fd, uid) || uid != getuid()) {
            ::close(fd);  // not this user: say nothing
            continue;
        }
        no_sigpipe(fd);
        auto impl = std::make_unique<Connection::Impl>();
        impl->fd = fd;
        return Connection(std::move(impl));
    }
    return std::nullopt;
}

void Server::stop() {
    if (!impl_->stopped.exchange(true) && impl_->wake_write >= 0) {
        const char byte = 1;
        // If this fails the pipe is full, so accept() is being woken anyway.
        [[maybe_unused]] const ssize_t written = ::write(impl_->wake_write, &byte, 1);
    }
}

std::optional<Connection> connect(const std::string& instance, std::string& error) {
    error.clear();
    if (!is_instance_name(instance)) {
        error = "invalid instance name";
        return std::nullopt;
    }
    sockaddr_un addr{};
    if (!make_address(endpoint_path(instance), addr, error)) {
        return std::nullopt;
    }
    const int fd = set_cloexec(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (fd < 0) {
        error = "can't create a socket";
        return std::nullopt;
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        ::close(fd);
        return std::nullopt;  // not running (no socket, or nobody listening)
    }
    uid_t uid = 0;
    if (!peer_uid(fd, uid) || uid != getuid()) {
        ::close(fd);
        error = "the FloServer channel is held by another user's program; not using it";
        return std::nullopt;
    }
    no_sigpipe(fd);
    auto impl = std::make_unique<Connection::Impl>();
    impl->fd = fd;
    return Connection(std::move(impl));
}

}  // namespace floserver::core::ipc

#endif  // !_WIN32

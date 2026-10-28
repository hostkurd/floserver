// Windows half of the private channel: a named pipe whose access list lets
// only the current user in, claimed with FILE_FLAG_FIRST_PIPE_INSTANCE, and a
// check of the user that runs the program at the other end.

#ifdef _WIN32

#include "ipc_impl.h"

#include <sddl.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace floserver::core::ipc {

namespace {

constexpr DWORD kBufferSize = 64 * 1024;
constexpr DWORD kBusyWaitMs = 2000;

class Handle {
public:
    explicit Handle(HANDLE h = nullptr) : h_(h) {}
    ~Handle() {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) = delete;
    Handle& operator=(Handle&&) = delete;
    [[nodiscard]] HANDLE get() const { return h_; }
    HANDLE* out() { return &h_; }

private:
    HANDLE h_;
};

// The SID of the user a process runs as, as text ("S-1-5-21-..."), or empty.
std::wstring token_user_sid(HANDLE process) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, token.out())) {
        return {};
    }
    DWORD size = 0;
    (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (size == 0) {
        return {};
    }
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) {
        return {};
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &text)) {
        return {};
    }
    std::wstring sid(text);
    LocalFree(text);
    return sid;
}

const std::wstring& current_user_sid() {
    static const std::wstring sid = token_user_sid(GetCurrentProcess());
    return sid;
}

std::wstring process_user_sid(ULONG pid) {
    const Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (process.get() == nullptr) {
        return {};
    }
    return token_user_sid(process.get());
}

std::wstring pipe_name(const std::string& instance) {
    return L"\\\\.\\pipe\\floserver-" + current_user_sid() + L"-" +
           std::wstring(instance.begin(), instance.end());
}

std::string narrow(const std::wstring& w) {
    std::string out;
    for (const wchar_t c : w) {
        out += (c < 0x80) ? static_cast<char>(c) : '?';
    }
    return out;
}

std::string error_text(DWORD code) { return "Windows error " + std::to_string(code); }

}  // namespace

// ---- Connection ------------------------------------------------------------

bool Connection::Impl::is_open() const { return handle != INVALID_HANDLE_VALUE; }

bool Connection::Impl::write_all(const std::uint8_t* data, std::size_t size) const {
    while (size > 0) {
        const DWORD chunk = size > kBufferSize ? kBufferSize : static_cast<DWORD>(size);
        DWORD written = 0;
        if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

bool Connection::Impl::read_all(std::uint8_t* data, std::size_t size) const {
    while (size > 0) {
        const DWORD chunk = size > kBufferSize ? kBufferSize : static_cast<DWORD>(size);
        DWORD got = 0;
        if (!ReadFile(handle, data, chunk, &got, nullptr) || got == 0) {
            return false;
        }
        data += got;
        size -= got;
    }
    return true;
}

void Connection::Impl::close() {
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
}

// ---- Server ----------------------------------------------------------------

struct Server::Impl {
    std::wstring name;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    HANDLE pending = INVALID_HANDLE_VALUE;  // the instance waiting for the next client
    std::atomic<bool> stopped{false};
    std::mutex mutex;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() {
        if (pending != INVALID_HANDLE_VALUE) {
            CloseHandle(pending);
        }
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
    }

    HANDLE create_instance(bool first) {
        return CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0U),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, kBufferSize, kBufferSize, 0, &attributes);
    }
};

Server::Server(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Server::~Server() = default;

std::string endpoint_path(const std::string& instance) { return narrow(pipe_name(instance)); }

ListenStatus Server::listen(const std::string& instance, std::unique_ptr<Server>& server,
                            std::string& error) {
    if (!is_instance_name(instance)) {
        error = "invalid instance name";
        return ListenStatus::Failed;
    }
    const std::wstring& sid = current_user_sid();
    if (sid.empty()) {
        error = "can't find the current user's identity";
        return ListenStatus::Failed;
    }
    auto impl = std::make_unique<Impl>();
    impl->name = pipe_name(instance);

    // Protected access list: full access for this user, nobody else.
    const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &impl->descriptor, nullptr)) {
        error = "can't build the channel's access list: " + error_text(GetLastError());
        return ListenStatus::Failed;
    }
    impl->attributes = {sizeof(SECURITY_ATTRIBUTES), impl->descriptor, FALSE};

    impl->pending = impl->create_instance(true);
    if (impl->pending == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_ACCESS_DENIED) {
            // The name is taken. If it's our own service, that's "already
            // running"; otherwise someone else holds it.
            std::string connect_error;
            if (connect(instance, connect_error)) {
                return ListenStatus::AlreadyRunning;
            }
            error = connect_error.empty() ? "the FloServer channel is in use by another program"
                                          : connect_error;
            return ListenStatus::Failed;
        }
        error = "can't open the channel: " + error_text(code);
        return ListenStatus::Failed;
    }
    // The constructor is private, so make_unique can't reach it.
    server.reset(new Server(std::move(impl)));  // NOLINT(cppcoreguidelines-owning-memory)
    return ListenStatus::Listening;
}

std::optional<Connection> Server::accept() {
    while (!impl_->stopped.load()) {
        HANDLE h = INVALID_HANDLE_VALUE;
        {
            const std::lock_guard lock(impl_->mutex);
            h = impl_->pending;
        }
        if (h == INVALID_HANDLE_VALUE) {
            return std::nullopt;
        }
        const BOOL connected = ConnectNamedPipe(h, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (impl_->stopped.load()) {
            return std::nullopt;
        }
        if (!connected) {
            DisconnectNamedPipe(h);  // the client left before we got to it
            continue;
        }

        // Keep the name claimed: the next instance waits while this one serves.
        {
            const std::lock_guard lock(impl_->mutex);
            impl_->pending = impl_->create_instance(false);
        }

        // Only this user gets here: the pipe's access list admits nobody else.
        // (Checking the client's process here would turn away `fls` run from
        // an administrator terminal, whose token a normal program can't read.)
        auto impl = std::make_unique<Connection::Impl>();
        impl->handle = h;
        return Connection(std::move(impl));
    }
    return std::nullopt;
}

void Server::stop() {
    if (impl_->stopped.exchange(true)) {
        return;
    }
    // Wake a thread blocked in ConnectNamedPipe by connecting to it.
    const HANDLE h = CreateFileW(impl_->name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                 nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
}

std::optional<Connection> connect(const std::string& instance, std::string& error) {
    error.clear();
    if (!is_instance_name(instance)) {
        error = "invalid instance name";
        return std::nullopt;
    }
    if (current_user_sid().empty()) {
        error = "can't find the current user's identity";
        return std::nullopt;
    }
    const std::wstring name = pipe_name(instance);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        // SECURITY_IDENTIFICATION: the service may learn who we are, but can't
        // act as us.
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) {
            return std::nullopt;  // not running
        }
        if (code != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), kBusyWaitMs)) {
            if (code != ERROR_PIPE_BUSY) {
                error = "can't open the FloServer channel: " + error_text(code);
            }
            return std::nullopt;
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    // The program serving the pipe must run as this user.
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(h, &pid) || process_user_sid(pid) != current_user_sid()) {
        CloseHandle(h);
        error = "the FloServer channel is held by another user's program; not using it";
        return std::nullopt;
    }
    auto impl = std::make_unique<Connection::Impl>();
    impl->handle = h;
    return Connection(std::move(impl));
}

}  // namespace floserver::core::ipc

#endif  // _WIN32

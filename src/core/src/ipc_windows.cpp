// Windows half of the private channel.
//
// The service creates the pipe with an explicit security descriptor:
//   owner      the current user
//   access     full for the current user; network logons denied
//   label      Medium integrity, no write-up and no read-up, so sandboxed
//              (Low integrity, AppContainer) programs of the same user can't
//              open it at all
// It claims the name with FILE_FLAG_FIRST_PIPE_INSTANCE and refuses remote
// clients.
//
// A client checks the pipe itself, not the process behind it: the owner must
// be the current user and the integrity label at least Medium. Another account
// can't make the user the owner, and a sandboxed program of the same user
// can't give its pipe a label above its own (Low), so neither can pose as the
// service by creating the pipe first. Clients connect at identification level,
// so the service can learn who they are but never act as them.

#ifdef _WIN32

#include "ipc_impl.h"

#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace floserver::core::ipc {

namespace {

constexpr DWORD kBufferSize = 64 * 1024;
constexpr DWORD kBusyWaitMs = 2000;
constexpr auto kBackoff = std::chrono::milliseconds(200);

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

std::wstring sid_text(PSID sid) {
    LPWSTR text = nullptr;
    if (sid == nullptr || !ConvertSidToStringSidW(sid, &text)) {
        return {};
    }
    std::wstring out(text);
    LocalFree(text);
    return out;
}

// The SID of the user this process runs as, as text ("S-1-5-21-..."), or empty.
const std::wstring& current_user_sid() {
    static const std::wstring sid = [] {
        Handle token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.out())) {
            return std::wstring();
        }
        DWORD size = 0;
        (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> buffer(size);
        if (size == 0 || !GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) {
            return std::wstring();
        }
        return sid_text(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid);
    }();
    return sid;
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

// True when the pipe was made by FloServer running as this user (see the top
// of this file).
bool pipe_is_ours(HANDLE h) {
    PSID owner = nullptr;
    PACL sacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
                        &owner, nullptr, nullptr, &sacl, &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    bool ours = !current_user_sid().empty() && sid_text(owner) == current_user_sid();
    if (ours && sacl != nullptr) {
        for (DWORD i = 0; i < sacl->AceCount; ++i) {
            void* ace = nullptr;
            if (!GetAce(sacl, i, &ace) ||
                static_cast<const ACE_HEADER*>(ace)->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
                continue;
            }
            auto* label = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace);
            PSID label_sid = &label->SidStart;
            const UCHAR parts = *GetSidSubAuthorityCount(label_sid);
            if (parts == 0 || *GetSidSubAuthority(label_sid, parts - 1U) < SECURITY_MANDATORY_MEDIUM_RID) {
                ours = false;  // made by a sandboxed program
            }
        }
    }
    // No label at all means the implicit Medium label: fine.
    LocalFree(descriptor);
    return ours;
}

// Service-side handles are synchronous. Client handles are opened overlapped
// so a request can time out when the service doesn't answer.
bool transfer(HANDLE h, bool overlapped, DWORD timeout_ms, bool write, std::uint8_t* data,
              std::size_t size) {
    const Handle event(overlapped ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr);
    if (overlapped && event.get() == nullptr) {
        return false;
    }
    while (size > 0) {
        const DWORD chunk = size > kBufferSize ? kBufferSize : static_cast<DWORD>(size);
        DWORD done = 0;
        if (!overlapped) {
            const BOOL ok = write ? WriteFile(h, data, chunk, &done, nullptr) : ReadFile(h, data, chunk, &done, nullptr);
            if (!ok || done == 0) {
                return false;
            }
        } else {
            OVERLAPPED ov{};
            ov.hEvent = event.get();
            ResetEvent(event.get());
            const BOOL ok = write ? WriteFile(h, data, chunk, nullptr, &ov) : ReadFile(h, data, chunk, nullptr, &ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING) {
                return false;
            }
            if (WaitForSingleObject(event.get(), timeout_ms == 0 ? INFINITE : timeout_ms) != WAIT_OBJECT_0) {
                CancelIoEx(h, &ov);
                (void)GetOverlappedResult(h, &ov, &done, TRUE);
                return false;
            }
            if (!GetOverlappedResult(h, &ov, &done, FALSE) || done == 0) {
                return false;
            }
        }
        data += done;
        size -= done;
    }
    return true;
}

}  // namespace

// ---- Connection ------------------------------------------------------------

bool Connection::Impl::is_open() const { return handle != INVALID_HANDLE_VALUE; }

bool Connection::Impl::write_all(const std::uint8_t* data, std::size_t size) const {
    // WriteFile takes a const buffer; transfer() shares one signature for both.
    return transfer(handle, overlapped, timeout_ms, true, const_cast<std::uint8_t*>(data), size);
}

bool Connection::Impl::read_all(std::uint8_t* data, std::size_t size) const {
    return transfer(handle, overlapped, timeout_ms, false, data, size);
}

void Connection::Impl::close() {
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
}

bool Connection::Impl::set_timeout(std::chrono::milliseconds limit) {
    if (!overlapped) {
        return false;  // service-side handles don't time out
    }
    timeout_ms = static_cast<DWORD>(limit.count());
    return true;
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

    // See the top of this file.
    const std::wstring sddl = L"O:" + sid + L"D:P(D;;GA;;;NU)(A;;GA;;;" + sid + L")S:(ML;;NWNR;;;ME)";
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
            if (impl_->pending == INVALID_HANDLE_VALUE) {
                // Creating the next instance failed last time (out of
                // resources?): try again rather than stop serving.
                impl_->pending = impl_->create_instance(false);
            }
            h = impl_->pending;
        }
        if (h == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(kBackoff);
            continue;
        }
        const BOOL connected = ConnectNamedPipe(h, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (impl_->stopped.load()) {
            return std::nullopt;
        }
        if (!connected) {
            const DWORD code = GetLastError();
            DisconnectNamedPipe(h);
            if (code != ERROR_NO_DATA) {
                std::this_thread::sleep_for(kBackoff);  // not just a client that left: don't spin
            }
            continue;
        }

        // Keep the name claimed: the next instance waits while this one serves.
        {
            const std::lock_guard lock(impl_->mutex);
            impl_->pending = impl_->create_instance(false);
        }
        // Only this user gets here: the pipe's access list and label admit
        // nobody else.
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
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
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
    if (!pipe_is_ours(h)) {
        CloseHandle(h);
        error = "the FloServer channel was created by another program, not FloServer running as you; "
                "not using it";
        return std::nullopt;
    }
    auto impl = std::make_unique<Connection::Impl>();
    impl->handle = h;
    impl->overlapped = true;
    return Connection(std::move(impl));
}

}  // namespace floserver::core::ipc

#endif  // _WIN32

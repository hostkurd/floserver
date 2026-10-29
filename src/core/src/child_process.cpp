#include "floserver/core/child_process.h"

#include "floserver/core/paths.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <array>
#include <thread>

#ifdef _WIN32
#include "win_text.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>

#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;  // NOLINT(readability-redundant-declaration)
#endif

namespace floserver::core {

// ---------------------------------------------------------------- listener

namespace {

// The body of an HTTP/1.x response with status 200, or nothing.
std::optional<std::string> http_body_if_ok(const std::string& response) {
    const auto body = response.find("\r\n\r\n");
    if (body == std::string::npos || !response.starts_with("HTTP/1.") || response.compare(8, 5, " 200 ") != 0) {
        return std::nullopt;
    }
    return response.substr(body + 4);
}

}  // namespace


#ifdef _WIN32

namespace {

bool winsock_started() {
    static const bool ok = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}

}  // namespace

std::unique_ptr<LoopbackListener> LoopbackListener::open(Port port, std::string& error) {
    if (!winsock_started()) {
        error = "Windows networking (Winsock) didn't start";
        return nullptr;
    }
    // Inheritable (no WSA_FLAG_NO_HANDLE_INHERIT): php-cgi gets it as stdin.
    // Only children given it in their handle list ever see it.
    const SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
    if (s == INVALID_SOCKET) {
        error = "can't make a socket (" + std::to_string(WSAGetLastError()) + ")";
        return nullptr;
    }
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               static_cast<int>(sizeof exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), static_cast<int>(sizeof addr)) != 0 ||
        listen(s, SOMAXCONN) != 0) {
        error = "port " + std::to_string(port) + " is taken";
        closesocket(s);
        return nullptr;
    }
    return std::unique_ptr<LoopbackListener>(new LoopbackListener(static_cast<std::uintptr_t>(s), port));
}

std::optional<std::string> loopback_http_get(Port port, const std::string& host, const std::string& path,
                                             std::chrono::milliseconds limit) {
    if (!winsock_started()) {
        return std::nullopt;
    }
    const SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) {
        return std::nullopt;
    }
    const DWORD ms = static_cast<DWORD>(limit.count());
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), static_cast<int>(sizeof ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), static_cast<int>(sizeof ms));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string response;
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), static_cast<int>(sizeof addr)) == 0) {
        const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
        send(s, req.data(), static_cast<int>(req.size()), 0);
        std::array<char, 2048> buf{};
        for (int n = 0; (n = recv(s, buf.data(), static_cast<int>(buf.size()), 0)) > 0 && response.size() < 65536;) {
            response.append(buf.data(), static_cast<std::size_t>(n));
        }
    }
    closesocket(s);
    return http_body_if_ok(response);
}

bool has_open_connections(Port port) {
    ULONG size = 0;
    if (GetTcpTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }
    std::vector<std::uint8_t> buf(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE*>(buf.data());
    if (GetTcpTable(table, &size, FALSE) != NO_ERROR) {
        return false;
    }
    const DWORD wanted = htons(port);
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW& row = table->table[i];
        if (static_cast<WORD>(row.dwLocalPort) == wanted && row.dwState != MIB_TCP_STATE_LISTEN &&
            row.dwState != MIB_TCP_STATE_TIME_WAIT && row.dwState != MIB_TCP_STATE_CLOSED) {
            return true;
        }
    }
    return false;
}

std::optional<std::uintptr_t> LoopbackListener::accept_connection() const {
    const SOCKET c = WSAAccept(static_cast<SOCKET>(socket_), nullptr, nullptr, nullptr, 0);
    if (c == INVALID_SOCKET) {
        return std::nullopt;
    }
    // Never inherited by programs FloServer starts.
    SetHandleInformation(reinterpret_cast<HANDLE>(c), HANDLE_FLAG_INHERIT, 0);  // NOLINT(performance-no-int-to-ptr)
    return static_cast<std::uintptr_t>(c);
}

std::optional<std::uintptr_t> connect_loopback(Port port) {
    if (!winsock_started()) {
        return std::nullopt;
    }
    const SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) {
        return std::nullopt;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), static_cast<int>(sizeof addr)) != 0) {
        closesocket(s);
        return std::nullopt;
    }
    return static_cast<std::uintptr_t>(s);
}

void close_socket(std::uintptr_t socket) { closesocket(static_cast<SOCKET>(socket)); }

void relay_sockets(std::uintptr_t a, std::uintptr_t b, const std::function<void()>& activity) {
    const SOCKET sa = static_cast<SOCKET>(a);
    const SOCKET sb = static_cast<SOCKET>(b);
    std::array<char, 65536> buf{};
    bool open = true;
    while (open) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(sa, &set);
        FD_SET(sb, &set);
        if (select(0, &set, nullptr, nullptr, nullptr) <= 0) {
            break;
        }
        for (const auto& [from, to] : {std::pair{sa, sb}, std::pair{sb, sa}}) {
            if (!open || !FD_ISSET(from, &set)) {
                continue;
            }
            const int n = recv(from, buf.data(), static_cast<int>(buf.size()), 0);
            if (n <= 0) {
                open = false;
                break;
            }
            for (int sent = 0; sent < n;) {
                const int w = send(to, buf.data() + sent, n - sent, 0);
                if (w <= 0) {
                    open = false;
                    break;
                }
                sent += w;
            }
            activity();
        }
    }
    shutdown(sa, SD_BOTH);
    shutdown(sb, SD_BOTH);
    closesocket(sa);
    closesocket(sb);
}

void answer_and_close(std::uintptr_t socket, std::string_view data) {
    const SOCKET s = static_cast<SOCKET>(socket);
    for (std::size_t sent = 0; sent < data.size();) {
        const int chunk = static_cast<int>(std::min<std::size_t>(data.size() - sent, 65536));
        const int w = send(s, data.data() + sent, chunk, 0);
        if (w <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(w);
    }
    shutdown(s, SD_SEND);
    std::array<char, 4096> buf{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval wait{0, 200000};
        if (select(0, &set, nullptr, nullptr, &wait) < 0 ||
            (FD_ISSET(s, &set) && recv(s, buf.data(), static_cast<int>(buf.size()), 0) <= 0)) {
            break;
        }
    }
    closesocket(s);
}

LoopbackListener::~LoopbackListener() { closesocket(static_cast<SOCKET>(socket_)); }

void LoopbackListener::drop_waiting_connection() const {
    // Still waiting? A client that gave up meanwhile would leave accept()
    // blocking the caller.
    const std::vector<const LoopbackListener*> self = {this};
    if (waiting_connections(self, std::chrono::milliseconds(0)).empty()) {
        return;
    }
    const SOCKET c = accept(static_cast<SOCKET>(socket_), nullptr, nullptr);
    if (c != INVALID_SOCKET) {
        closesocket(c);
    }
}

std::vector<std::size_t> waiting_connections(const std::vector<const LoopbackListener*>& listeners,
                                             std::chrono::milliseconds limit) {
    std::vector<std::size_t> ready;
    if (listeners.empty()) {
        std::this_thread::sleep_for(limit);
        return ready;
    }
    // select() takes at most FD_SETSIZE (64) sockets; more pools than that
    // are checked in turns.
    for (std::size_t first = 0; first < listeners.size(); first += FD_SETSIZE) {
        fd_set set;
        FD_ZERO(&set);
        const std::size_t last = std::min(listeners.size(), first + FD_SETSIZE);
        for (std::size_t i = first; i < last; ++i) {
            FD_SET(static_cast<SOCKET>(listeners[i]->native()), &set);
        }
        timeval tv{};
        const auto wait_ms = first == 0 ? limit.count() : 0;
        tv.tv_sec = static_cast<long>(wait_ms / 1000);
        tv.tv_usec = static_cast<long>((wait_ms % 1000) * 1000);
        if (select(0, &set, nullptr, nullptr, &tv) > 0) {
            for (std::size_t i = first; i < last; ++i) {
                if (FD_ISSET(static_cast<SOCKET>(listeners[i]->native()), &set)) {
                    ready.push_back(i);
                }
            }
        }
    }
    return ready;
}

#else

std::unique_ptr<LoopbackListener> LoopbackListener::open(Port port, std::string& error) {
    // Close-on-exec: only the php-cgi given it as stdin (dup2 clears the
    // flag on the copy) ever has it.
    const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (s < 0) {
        error = "can't make a socket";
        return nullptr;
    }
    // Lets the service listen again right after a restart; it can't take a
    // port another program is listening on.
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 || listen(s, SOMAXCONN) != 0) {
        error = "port " + std::to_string(port) + " is taken";
        close(s);
        return nullptr;
    }
    return std::unique_ptr<LoopbackListener>(new LoopbackListener(static_cast<std::uintptr_t>(s), port));
}

std::optional<std::string> loopback_http_get(Port port, const std::string& host, const std::string& path,
                                             std::chrono::milliseconds limit) {
    const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (s < 0) {
        return std::nullopt;
    }
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(limit.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((limit.count() % 1000) * 1000);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string response;
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0) {
        const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
        (void)send(s, req.data(), req.size(), MSG_NOSIGNAL);
        std::array<char, 2048> buf{};
        for (ssize_t n = 0; (n = recv(s, buf.data(), buf.size(), 0)) > 0 && response.size() < 65536;) {
            response.append(buf.data(), static_cast<std::size_t>(n));
        }
    }
    close(s);
    return http_body_if_ok(response);
}

bool has_open_connections(Port port) {
#ifdef __linux__
    // /proc/net/tcp, one connection per line after the header:
    //   sl  local_address rem_address   st ...
    //   0:  0100007F:2390 0100007F:A1B2 01 ...
    // Ports are hex; st 01 established, 03 waiting to be accepted
    // (syn-recv), 08 close-wait.
    std::ifstream in("/proc/net/tcp");
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string slot;
        std::string local;
        std::string remote;
        std::string state;
        if (!(fields >> slot >> local >> remote >> state)) {
            continue;
        }
        const auto colon = local.rfind(':');
        if (colon == std::string::npos) {
            continue;
        }
        unsigned long local_port = 0;
        try {
            local_port = std::stoul(local.substr(colon + 1), nullptr, 16);
        } catch (const std::exception&) {
            continue;
        }
        if (local_port == port && (state == "01" || state == "03" || state == "08")) {
            return true;
        }
    }
    return false;
#else
    (void)port;  // macOS comes with its own stage (D3)
    return false;
#endif
}

std::optional<std::uintptr_t> LoopbackListener::accept_connection() const {
#ifdef __linux__
    const int c = accept4(static_cast<int>(socket_), nullptr, nullptr, SOCK_CLOEXEC);
#else
    const int c = accept(static_cast<int>(socket_), nullptr, nullptr);
    if (c >= 0) {
        fcntl(c, F_SETFD, FD_CLOEXEC);
    }
#endif
    if (c < 0) {
        return std::nullopt;
    }
    return static_cast<std::uintptr_t>(c);
}

std::optional<std::uintptr_t> connect_loopback(Port port) {
    const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (s < 0) {
        return std::nullopt;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        close(s);
        return std::nullopt;
    }
    return static_cast<std::uintptr_t>(s);
}

void close_socket(std::uintptr_t socket) { close(static_cast<int>(socket)); }

void relay_sockets(std::uintptr_t a, std::uintptr_t b, const std::function<void()>& activity) {
    const int sa = static_cast<int>(a);
    const int sb = static_cast<int>(b);
    std::array<char, 65536> buf{};
    bool open = true;
    while (open) {
        std::array<pollfd, 2> fds{{{sa, POLLIN, 0}, {sb, POLLIN, 0}}};
        if (poll(fds.data(), fds.size(), -1) <= 0) {
            break;
        }
        for (std::size_t i = 0; i < 2 && open; ++i) {
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            const int from = i == 0 ? sa : sb;
            const int to = i == 0 ? sb : sa;
            const ssize_t n = recv(from, buf.data(), buf.size(), 0);
            if (n <= 0) {
                open = false;
                break;
            }
            for (ssize_t sent = 0; sent < n;) {
                const ssize_t w = send(to, buf.data() + sent, static_cast<std::size_t>(n - sent), MSG_NOSIGNAL);
                if (w <= 0) {
                    open = false;
                    break;
                }
                sent += w;
            }
            activity();
        }
    }
    shutdown(sa, SHUT_RDWR);
    shutdown(sb, SHUT_RDWR);
    close(sa);
    close(sb);
}

void answer_and_close(std::uintptr_t socket, std::string_view data) {
    const int s = static_cast<int>(socket);
    for (std::size_t sent = 0; sent < data.size();) {
        const ssize_t w = send(s, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (w <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(w);
    }
    shutdown(s, SHUT_WR);
    std::array<char, 4096> buf{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd fd{s, POLLIN, 0};
        const int ready = poll(&fd, 1, 200);
        if (ready < 0 || (ready > 0 && recv(s, buf.data(), buf.size(), 0) <= 0)) {
            break;
        }
    }
    close(s);
}

LoopbackListener::~LoopbackListener() { close(static_cast<int>(socket_)); }

void LoopbackListener::drop_waiting_connection() const {
    const std::vector<const LoopbackListener*> self = {this};
    if (waiting_connections(self, std::chrono::milliseconds(0)).empty()) {
        return;
    }
#ifdef __linux__
    const int c = accept4(static_cast<int>(socket_), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    const int c = accept(static_cast<int>(socket_), nullptr, nullptr);
#endif
    if (c >= 0) {
        close(c);
    }
}

std::vector<std::size_t> waiting_connections(const std::vector<const LoopbackListener*>& listeners,
                                             std::chrono::milliseconds limit) {
    std::vector<pollfd> fds;
    fds.reserve(listeners.size());
    for (const LoopbackListener* l : listeners) {
        fds.push_back({static_cast<int>(l->native()), POLLIN, 0});
    }
    std::vector<std::size_t> ready;
    if (fds.empty()) {
        std::this_thread::sleep_for(limit);
        return ready;
    }
    if (poll(fds.data(), static_cast<nfds_t>(fds.size()), static_cast<int>(limit.count())) > 0) {
        for (std::size_t i = 0; i < fds.size(); ++i) {
            if ((fds[i].revents & POLLIN) != 0) {
                ready.push_back(i);
            }
        }
    }
    return ready;
}

#endif

// ------------------------------------------------------------------- child

#ifdef _WIN32

struct ChildProcess::Native {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    Native() = default;
    Native(const Native&) = delete;
    Native& operator=(const Native&) = delete;
    Native(Native&&) = delete;
    Native& operator=(Native&&) = delete;
    ~Native() {
        if (process != nullptr) {
            CloseHandle(process);
        }
        if (job != nullptr) {
            CloseHandle(job);  // KILL_ON_JOB_CLOSE ends anything still in it
        }
    }
};

namespace {

bool same_name(const std::wstring& entry, const std::wstring& name) {
    if (entry.size() <= name.size() || entry[name.size()] != L'=') {
        return false;
    }
    return CompareStringOrdinal(entry.c_str(), static_cast<int>(name.size()), name.c_str(),
                                static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
}

// This process's environment without `removed` and with `extra` set, as a
// CreateProcessW block, sorted by name ignoring case as Windows documents.
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& extra,
                               const std::vector<std::string>& removed) {
    std::vector<std::wstring> entries;
    if (wchar_t* env = GetEnvironmentStringsW(); env != nullptr) {
        for (const wchar_t* p = env; *p != L'\0'; p += wcslen(p) + 1) {
            entries.emplace_back(p);
        }
        FreeEnvironmentStringsW(env);
    }
    for (const std::string& name : removed) {
        const std::wstring wname = win::widen(name);
        std::erase_if(entries, [&wname](const std::wstring& e) { return same_name(e, wname); });
    }
    for (const auto& [name, value] : extra) {
        const std::wstring wname = win::widen(name);
        std::erase_if(entries, [&wname](const std::wstring& e) { return same_name(e, wname); });
        entries.push_back(wname + L"=" + win::widen(value));
    }
    // Entries starting with '=' (the per-drive folders, "=C:=...") stay first.
    std::stable_sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        const std::size_t an = a.find(L'=', 1);
        const std::size_t bn = b.find(L'=', 1);
        return CompareStringOrdinal(a.c_str(), static_cast<int>(an), b.c_str(), static_cast<int>(bn), TRUE) ==
               CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const std::wstring& e : entries) {
        block += e;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

}  // namespace

std::unique_ptr<ChildProcess> ChildProcess::start(const ChildOptions& o, std::string& error) {
    auto native = std::make_unique<Native>();
    native->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (native->job == nullptr ||
        SetInformationJobObject(native->job, JobObjectExtendedLimitInformation, &limits, sizeof limits) == FALSE) {
        error = "can't make a job object (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    }

    HANDLE log = INVALID_HANDLE_VALUE;
    if (!o.output_log.empty()) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        log = CreateFileW(o.output_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE) {
            error = "can't open " + path_to_utf8(o.output_log);
            return nullptr;
        }
    }
    struct CloseLog {
        HANDLE h;
        CloseLog(const CloseLog&) = delete;
        CloseLog& operator=(const CloseLog&) = delete;
        CloseLog(CloseLog&&) = delete;
        CloseLog& operator=(CloseLog&&) = delete;
        explicit CloseLog(HANDLE handle) : h(handle) {}
        ~CloseLog() {
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
    } const close_log(log);
    HANDLE input = INVALID_HANDLE_VALUE;
    if (!o.stdin_file.empty()) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        input = CreateFileW(o.stdin_file.c_str(), GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input == INVALID_HANDLE_VALUE) {
            error = "can't open " + path_to_utf8(o.stdin_file);
            return nullptr;
        }
    }
    const CloseLog close_input(input);

    // Only the handles named here are inherited, never the service's own
    // (its pipe, other pools' sockets).
    std::vector<HANDLE> inherit;
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    if (o.stdin_socket != nullptr) {
        si.StartupInfo.hStdInput = reinterpret_cast<HANDLE>(o.stdin_socket->native());  // NOLINT(performance-no-int-to-ptr)
        inherit.push_back(si.StartupInfo.hStdInput);
    } else if (input != INVALID_HANDLE_VALUE) {
        si.StartupInfo.hStdInput = input;
        inherit.push_back(input);
    }
    if (log != INVALID_HANDLE_VALUE) {
        inherit.push_back(log);
    }
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::uint8_t> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) == FALSE) {
        error = "can't prepare the process attributes";
        return nullptr;
    }
    si.lpAttributeList = attrs;
    if (!inherit.empty() &&
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                  inherit.size() * sizeof(HANDLE), nullptr, nullptr) == FALSE) {
        DeleteProcThreadAttributeList(attrs);
        error = "can't limit the inherited handles";
        return nullptr;
    }

    std::wstring command = win::quote(o.program.wstring());
    for (const std::string& a : o.args) {
        command += L" " + win::quote(win::widen(a));
    }
    if (!o.windows_raw_args.empty()) {
        command += L" " + win::widen(o.windows_raw_args);
    }
    std::wstring env = environment_block(o.env, o.remove_env);
    const std::wstring dir = o.working_dir.empty() ? o.program.parent_path().wstring() : o.working_dir.wstring();
    PROCESS_INFORMATION pi{};
    // Suspended until it's in the job, so nothing it starts escapes it.
    const BOOL created = CreateProcessW(
        o.program.c_str(), command.data(), nullptr, nullptr, inherit.empty() ? FALSE : TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        env.data(), dir.c_str(), &si.StartupInfo, &pi);
    const DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    if (created == FALSE) {
        error = "can't start " + path_to_utf8(o.program.filename()) + " (Windows error " + std::to_string(create_error) + ")";
        return nullptr;
    }
    if (AssignProcessToJobObject(native->job, pi.hProcess) == FALSE) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        error = "can't put " + path_to_utf8(o.program.filename()) + " in a job object";
        return nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    native->process = pi.hProcess;
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(native), pi.dwProcessId));
}

bool ChildProcess::running() {
    if (exit_code_) {
        return false;
    }
    DWORD code = 0;
    if (WaitForSingleObject(native_->process, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(native_->process, &code)) {
        exit_code_ = static_cast<int>(code);
        return false;
    }
    return true;
}

std::optional<int> ChildProcess::wait(std::chrono::milliseconds limit) {
    if (exit_code_) {
        return exit_code_;
    }
    DWORD code = 0;
    if (WaitForSingleObject(native_->process, static_cast<DWORD>(limit.count())) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(native_->process, &code)) {
        exit_code_ = static_cast<int>(code);
    }
    return exit_code_;
}

void ChildProcess::kill() {
    TerminateJobObject(native_->job, 1);
    (void)wait(std::chrono::seconds(10));
}

#else

struct ChildProcess::Native {
    pid_t pid = -1;
    bool reaped = false;
};

namespace {

bool has_name(const std::string& entry, const std::string& name) {
    return entry.size() > name.size() && entry.compare(0, name.size(), name) == 0 && entry[name.size()] == '=';
}

std::vector<std::string> environment_with(const std::vector<std::pair<std::string, std::string>>& extra,
                                          const std::vector<std::string>& removed) {
    std::vector<std::string> out;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string entry(*e);
        const bool replaced =
            std::any_of(extra.begin(), extra.end(), [&entry](const auto& kv) { return has_name(entry, kv.first); }) ||
            std::any_of(removed.begin(), removed.end(), [&entry](const std::string& n) { return has_name(entry, n); });
        if (!replaced) {
            out.push_back(entry);
        }
    }
    for (const auto& [name, value] : extra) {
        std::string entry = name;
        entry += '=';
        entry += value;
        out.push_back(std::move(entry));
    }
    return out;
}

std::vector<char*> pointers(std::vector<std::string>& strings) {
    std::vector<char*> p;
    p.reserve(strings.size() + 1);
    for (std::string& s : strings) {
        p.push_back(s.data());
    }
    p.push_back(nullptr);
    return p;
}

}  // namespace

std::unique_ptr<ChildProcess> ChildProcess::start(const ChildOptions& o, std::string& error) {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attr{};
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    const std::string input_path = o.stdin_file.string();
    if (o.stdin_socket != nullptr) {
        posix_spawn_file_actions_adddup2(&actions, static_cast<int>(o.stdin_socket->native()), 0);
    } else if (!o.stdin_file.empty()) {
        posix_spawn_file_actions_addopen(&actions, 0, input_path.c_str(), O_RDONLY, 0);
    } else {
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    }
    const std::string log = o.output_log.empty() ? std::string("/dev/null") : o.output_log.string();
    posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    // Its own group, so kill() reaches its children; macOS: nothing else inherited.
    const short flags = static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    const short flags = POSIX_SPAWN_SETPGROUP;  // its own group, so kill() reaches its children
#endif
#if !defined(POSIX_SPAWN_CLOEXEC_DEFAULT) && defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);  // nothing beyond 0-2 inherited
#endif
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0);
    if (!o.working_dir.empty()) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
        posix_spawn_file_actions_addchdir_np(&actions, o.working_dir.c_str());
#endif
    }

    std::vector<std::string> argv_s;
    argv_s.push_back(o.program.string());
    argv_s.insert(argv_s.end(), o.args.begin(), o.args.end());
    std::vector<std::string> env_s = environment_with(o.env, o.remove_env);
    std::vector<char*> argv = pointers(argv_s);
    std::vector<char*> envp = pointers(env_s);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, o.program.c_str(), &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        error = "can't start " + path_to_utf8(o.program.filename()) + ": " + error_text(std::error_code(rc, std::generic_category()));
        return nullptr;
    }
    auto native = std::make_unique<Native>();
    native->pid = pid;
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(native), static_cast<std::uint32_t>(pid)));
}

bool ChildProcess::running() {
    if (exit_code_) {
        return false;
    }
    int status = 0;
    const pid_t done = waitpid(native_->pid, &status, WNOHANG);
    if (done == native_->pid) {
        native_->reaped = true;
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        return false;
    }
    if (done < 0 && errno == ECHILD) {
        native_->reaped = true;
        exit_code_ = -1;
        return false;
    }
    return true;
}

std::optional<int> ChildProcess::wait(std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (running()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return exit_code_;
}

void ChildProcess::kill() {
    // The whole group: nginx's workers, php-cgi's children. TERM first so
    // they can finish cleanly, then KILL; done only when no member is left,
    // so the ports and files they held are free when this returns.
    const pid_t group = native_->pid;
    const auto group_alive = [group] { return ::kill(-group, 0) == 0 || errno == EPERM; };
    ::kill(-group, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((running() || group_alive()) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (running() || group_alive()) {
        ::kill(-group, SIGKILL);
        (void)wait(std::chrono::seconds(5));
        for (int i = 0; i < 500 && group_alive(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

#endif

#ifdef _WIN32

bool process_is_program(std::uint32_t pid, const std::filesystem::path& program) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) {
        return false;
    }
    std::wstring name(32768, L'\0');
    DWORD size = static_cast<DWORD>(name.size());
    const BOOL ok = QueryFullProcessImageNameW(h, 0, name.data(), &size);
    CloseHandle(h);
    if (ok == FALSE) {
        return false;
    }
    name.resize(size);
    std::error_code ec;
    return std::filesystem::equivalent(std::filesystem::path(name), program, ec);
}

bool wait_for_process_exit(std::uint32_t pid, std::chrono::milliseconds limit) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h == nullptr) {
        return true;  // already gone
    }
    const bool gone = WaitForSingleObject(h, static_cast<DWORD>(limit.count())) == WAIT_OBJECT_0;
    CloseHandle(h);
    return gone;
}

#else

bool process_is_program(std::uint32_t pid, const std::filesystem::path& program) {
#ifdef __linux__
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec);
    if (ec) {
        return false;
    }
    std::error_code ec2;
    return std::filesystem::equivalent(exe, program, ec2) ||
           std::filesystem::equivalent(exe, std::filesystem::canonical(program, ec2), ec2);
#else
    // macOS comes with its own stage (D3); until then never signal a pid we
    // can't identify.
    (void)pid;
    (void)program;
    return false;
#endif
}

bool wait_for_process_exit(std::uint32_t pid, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (::kill(static_cast<pid_t>(pid), 0) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

#endif

ChildProcess::ChildProcess(std::unique_ptr<Native> native, std::uint32_t pid) : native_(std::move(native)), pid_(pid) {}

ChildProcess::~ChildProcess() {
    if (native_) {
        kill();
    }
}

}  // namespace floserver::core

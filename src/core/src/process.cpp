#include "floserver/core/process.h"

#include <array>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach.h>
#endif
extern char** environ;  // NOLINT(readability-redundant-declaration)
#endif

namespace floserver::core {

namespace fs = std::filesystem;

std::uint32_t current_pid() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

std::optional<std::uint64_t> memory_in_use() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(counters.PrivateUsage);
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.resident_size);
#else
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmRSS:")) {
            std::istringstream fields(line.substr(6));
            std::uint64_t kib = 0;
            if (fields >> kib) {
                return kib * 1024U;
            }
        }
    }
    return std::nullopt;
#endif
}

std::optional<fs::path> executable_path() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 6; ++attempt) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return std::nullopt;
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return fs::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::nullopt;
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
    }
    std::error_code ec;
    auto p = fs::canonical(fs::path(buffer.c_str()), ec);
    return ec ? std::nullopt : std::optional<fs::path>(p);
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::nullopt : std::optional<fs::path>(p);
#endif
}

std::optional<fs::path> sibling_program(const std::string& name) {
    auto self = executable_path();
    if (!self) {
        return std::nullopt;
    }
#ifdef _WIN32
    return self->parent_path() / (name + ".exe");
#else
    return self->parent_path() / name;
#endif
}

#ifdef _WIN32

namespace {

// Quotes one argument the way CommandLineToArgvW reads it back.
std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

bool spawn_detached(const fs::path& program, const std::vector<std::string>& args, std::string& error) {
    std::wstring command = quote(program.wstring());
    for (const std::string& a : args) {
        command += L" " + quote(widen(a));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info{};
    // No console window, its own process group (Ctrl+C in the caller's
    // console doesn't reach it), no inherited handles.
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &info)) {
        error = "can't start " + program.filename().string() + " (Windows error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

#else

bool spawn_detached(const fs::path& program, const std::vector<std::string>& args, std::string& error) {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attr{};
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attr);
    for (const int fd : {0, 1, 2}) {
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0);
    }
#ifdef POSIX_SPAWN_SETSID
    // A session of its own: a closed terminal doesn't take the service with it.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
    std::vector<std::string> owned;
    owned.reserve(args.size() + 1);
    owned.push_back(program.string());
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (std::string& s : owned) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, program.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        error = "can't start " + program.filename().string();
        return false;
    }
    return true;
}

#endif

}  // namespace floserver::core

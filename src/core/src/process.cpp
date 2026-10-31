#include "floserver/core/process.h"

#ifdef _WIN32
#include "win_text.h"
#endif

#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
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

std::optional<std::string> get_env(const char* name) {
#ifdef _WIN32
    // The Windows API rather than std::getenv (deprecated under MSVC, C4996),
    // read as UTF-16 so non-English values come through intact.
    const std::wstring wide_name(name, name + std::char_traits<char>::length(name));
    const DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    const DWORD got = GetEnvironmentVariableW(wide_name.c_str(), value.data(), size);
    if (got >= size) {
        return std::nullopt;  // changed in between
    }
    value.resize(got);
    if (value.empty()) {
        return std::string();
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), bytes, nullptr,
                        nullptr);
    return out;
#else
    const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read only
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
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
    // Up to the 32767-character limit of long paths.
    std::wstring buffer(MAX_PATH, L'\0');
    while (buffer.size() <= 32768) {
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


bool spawn_detached(const fs::path& program, const std::vector<std::string>& args, std::string& error) {
    std::wstring command = win::quote(program.wstring());
    for (const std::string& a : args) {
        command += L" " + win::quote(win::widen(a));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info{};
    // No console window, its own process group (Ctrl+C in the caller's
    // console doesn't reach it), no inherited handles.
    // Run in the program's own folder, so the caller's current folder isn't
    // kept in use for as long as the service runs.
    const std::wstring folder = program.parent_path().wstring();
    // Out of the caller's job when it allows that (a shim's does), so the
    // service outlives a terminal that's closed; else in it, as before.
    const auto create = [&](DWORD flags) {
        return CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | flags, nullptr,
                              folder.empty() ? nullptr : folder.c_str(), &startup, &info) != 0;
    };
    if (!create(CREATE_BREAKAWAY_FROM_JOB) && !create(0)) {
        error = "can't start " + program.filename().string() + " (Windows error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

std::optional<int> run_and_wait(const fs::path& program, const std::vector<std::string>& args,
                                std::chrono::milliseconds limit, std::string& error, bool* timed_out) {
    if (timed_out != nullptr) {
        *timed_out = false;
    }
    std::wstring command = win::quote(program.wstring());
    for (const std::string& a : args) {
        command += L" " + win::quote(win::widen(a));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &info)) {
        error = "can't start " + program.filename().string();
        return std::nullopt;
    }
    CloseHandle(info.hThread);
    const DWORD waited = WaitForSingleObject(info.hProcess, static_cast<DWORD>(limit.count()));
    DWORD code = 0;
    const bool done = waited == WAIT_OBJECT_0 && GetExitCodeProcess(info.hProcess, &code);
    if (!done) {
        // Stuck (a hidden error box, say): it would hold its files open for good.
        TerminateProcess(info.hProcess, 1);
        WaitForSingleObject(info.hProcess, 2000);
        CloseHandle(info.hProcess);
        error = program.filename().string() + " didn't finish in time";
        if (timed_out != nullptr) {
            *timed_out = true;
        }
        return std::nullopt;
    }
    CloseHandle(info.hProcess);
    return static_cast<int>(code);
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

std::optional<int> run_and_wait(const fs::path& program, const std::vector<std::string>& args,
                                std::chrono::milliseconds limit, std::string& error, bool* timed_out) {
    if (timed_out != nullptr) {
        *timed_out = false;
    }
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    for (const int fd : {0, 1, 2}) {
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0);
    }
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
    const int rc = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        error = "can't start " + program.filename().string();
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (true) {
        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (done < 0) {
            error = "lost " + program.filename().string();
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);  // stuck: it would hold its files open for good
            int ignored = 0;
            (void)waitpid(pid, &ignored, 0);
            error = program.filename().string() + " didn't finish in time";
            if (timed_out != nullptr) {
                *timed_out = true;
            }
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

#endif

}  // namespace floserver::core

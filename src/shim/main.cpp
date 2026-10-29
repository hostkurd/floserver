// The terminal shim (D10): one small program, copied as php, node, npm, npx,
// composer, python and pip into the shims folder on the user's PATH. It runs
// the version the nearest floserver.json asks for (shim.h).
//
// The tool is the program's own file name. On Windows the rest of the
// command line is passed on exactly as typed (the target reads its own
// quoting), the shim waits and returns the target's exit code, Ctrl+C is
// left to the target, and a killed shim takes the target with it. Elsewhere
// the shim replaces itself with the target (exec).

#include "floserver/core/paths.h"
#include "floserver/core/shim.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace core = floserver::core;

namespace {

// "C:\...\shims\npm.exe" -> "npm".
std::string tool_name(const fs::path& self) {
    std::string name = core::path_to_utf8(self.stem());
    std::transform(name.begin(), name.end(), name.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return name;
}

int fail(const std::string& tool, const std::string& message) {
    std::cerr << tool << " (FloServer): " << message << "\n";
    return 127;
}

}  // namespace

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// An argument as the C runtime reads it back (backslashes before quotes doubled).
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
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

// The command line after the program's own name, exactly as typed. The
// name is read as the C runtime reads argv[0]: quotes toggle, no escapes,
// it ends at the first space or tab outside quotes.
std::wstring rest_of_command_line() {
    const wchar_t* p = GetCommandLineW();
    bool quoted = false;
    for (; *p != L'\0'; ++p) {
        if (*p == L'"') {
            quoted = !quoted;
        } else if (!quoted && (*p == L' ' || *p == L'\t')) {
            break;
        }
    }
    while (*p == L' ' || *p == L'\t') {
        ++p;
    }
    return p;
}

// The shim itself doesn't stop on Ctrl+C; the target decides. A handler,
// not SetConsoleCtrlHandler(nullptr, TRUE): that flag is inherited and would
// make the target ignore Ctrl+C too.
BOOL WINAPI ignore_ctrl(DWORD /*event*/) { return TRUE; }

}  // namespace

int wmain(int /*argc*/, wchar_t** /*argv*/) {
    // The real file, not argv[0], which whoever starts us chooses.
    std::wstring self(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        if (n < self.size()) {
            self.resize(n);
            break;
        }
        self.resize(self.size() * 2);
    }
    const std::string tool = tool_name(self);
    const auto data = core::data_dir();
    if (!data) {
        return fail(tool, "FloServer can't find its folder (LOCALAPPDATA)");
    }
    std::error_code ec;
    std::string error;
    const auto target = core::resolve_shim(tool, fs::current_path(ec), *data, error);
    if (!target) {
        return fail(tool, error);
    }
    std::wstring command = quote(target->program.wstring());
    for (const std::string& a : target->args) {
        command += L" " + quote(widen(a));
    }
    if (const std::wstring rest = rest_of_command_line(); !rest.empty()) {
        command += L" " + rest;
    }
    for (const auto& [name, value] : target->env) {
        SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str());
    }
    SetConsoleCtrlHandler(ignore_ctrl, TRUE);

    // If the shim is killed (a tool stopping "npm run dev" kills only the
    // process it started), the target goes with it. On a normal exit that
    // is lifted first, so daemons the target started on purpose live on.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (job != nullptr && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) == FALSE) {
        CloseHandle(job);
        job = nullptr;
    }

    // As we were started: window settings and the C runtime's extra handles
    // (Node passes its IPC channel that way).
    STARTUPINFOW si{};
    GetStartupInfoW(&si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(target->program.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr,
                       nullptr, &si, &pi) == FALSE) {
        return fail(tool, "can't start " + core::path_to_utf8(target->program) + " (" +
                              std::to_string(GetLastError()) + ")");
    }
    if (job != nullptr) {
        (void)AssignProcessToJobObject(job, pi.hProcess);  // fails only inside a job that forbids it
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (job != nullptr) {
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        CloseHandle(job);
    }
    return static_cast<int>(code);
}

#else

int main(int argc, char** argv) {
    const fs::path self = argc > 0 ? fs::path(argv[0]) : fs::path("shim");
    const std::string tool = tool_name(self);
    const auto data = core::data_dir();
    if (!data) {
        return fail(tool, "FloServer can't find its folder (HOME)");
    }
    std::error_code ec;
    std::string error;
    const auto target = core::resolve_shim(tool, fs::current_path(ec), *data, error);
    if (!target) {
        return fail(tool, error);
    }
    std::vector<std::string> args = {target->program.string()};
    args.insert(args.end(), target->args.begin(), target->args.end());
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    std::vector<char*> c_args;
    c_args.reserve(args.size() + 1);
    for (std::string& a : args) {
        c_args.push_back(a.data());
    }
    c_args.push_back(nullptr);
    for (const auto& [name, value] : target->env) {
        setenv(name.c_str(), value.c_str(), 1);  // NOLINT(concurrency-mt-unsafe): one thread, just before exec
    }
    execv(target->program.c_str(), c_args.data());
    return fail(tool, "can't start " + target->program.string());
}

#endif

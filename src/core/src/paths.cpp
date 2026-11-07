#include "floserver/core/paths.h"

#include "floserver/core/process.h"

#ifdef _WIN32
#include <windows.h>

#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <array>
#include <cstdint>
#include <fstream>
#include <random>
#include <system_error>
#include <vector>

namespace floserver::core {

namespace fs = std::filesystem;

fs::path path_from_utf8(std::string_view text) {
    const std::u8string u(text.begin(), text.end());
    return {u};
}

std::string path_to_utf8(const fs::path& path) {
    const std::u8string u = path.u8string();
    return {u.begin(), u.end()};
}

namespace {

// An absolute folder from an environment variable, or nothing.
std::optional<fs::path> env_folder(const char* name) {
    const auto value = get_env(name);
    if (!value || value->empty()) {
        return std::nullopt;
    }
    fs::path p = path_from_utf8(*value);
    if (!p.is_absolute()) {
        return std::nullopt;
    }
    return p;
}

}  // namespace

std::optional<fs::path> data_dir() {
    if (auto custom = env_folder("FLOSERVER_DATA_DIR")) {
        return custom;
    }
#if defined(_WIN32)
    if (auto local = env_folder("LOCALAPPDATA")) {
        return *local / "FloServer";
    }
    return std::nullopt;
#elif defined(__APPLE__)
    if (auto home = env_folder("HOME")) {
        return *home / "Library" / "Application Support" / "FloServer";
    }
    return std::nullopt;
#else
    if (auto xdg = env_folder("XDG_DATA_HOME")) {
        return *xdg / "floserver";
    }
    if (auto home = env_folder("HOME")) {
        return *home / ".local" / "share" / "floserver";
    }
    return std::nullopt;
#endif
}

fs::path runtimes_dir(const fs::path& data) { return data / "runtimes"; }
fs::path downloads_dir(const fs::path& data) { return data / "downloads"; }
fs::path updates_dir(const fs::path& data) { return data / "updates"; }
fs::path manifest_state_file(const fs::path& data) { return data / "state" / "manifest-state.json"; }
fs::path kept_manifest_file(const fs::path& data) { return data / "state" / "manifest.json"; }
fs::path kept_signature_file(const fs::path& data) { return data / "state" / "manifest.json.sig"; }

std::string error_text(const std::error_code& ec) {
#ifdef _WIN32
    if (ec.category() == std::system_category()) {
        std::array<wchar_t, 512> text{};
        DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                 static_cast<DWORD>(ec.value()), 0, text.data(), static_cast<DWORD>(text.size()),
                                 nullptr);
        while (n > 0 && (text[n - 1] == L'\n' || text[n - 1] == L'\r' || text[n - 1] == L' ' || text[n - 1] == L'.')) {
            --n;
        }
        if (n > 0) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(n), nullptr, 0, nullptr,
                                                  nullptr);
            std::string out(static_cast<std::size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(n), out.data(), bytes, nullptr, nullptr);
            return out;
        }
        return "system error " + std::to_string(ec.value());
    }
#endif
    return ec.message();
}

std::string_view current_platform() {
#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
    return "windows-arm64";
#elif defined(_WIN32)
    return "windows-x64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
}

namespace {

bool write_atomically(const fs::path& file, std::string_view bytes, bool owner_only, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "can't create " + path_to_utf8(file.parent_path()) + ": " + error_text(ec);
        return false;
    }
    std::random_device rd;
    fs::path temp = file;
    temp += ".tmp-" + std::to_string(rd());
#ifndef _WIN32
    if (owner_only) {
        // Owner-only from the first moment: created 0600 with O_EXCL, so no
        // other user can open it while it's being written; the folder too.
        fs::permissions(file.parent_path(), fs::perms::owner_all, fs::perm_options::replace, ec);
        const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            error = "can't create " + path_to_utf8(temp);
            return false;
        }
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
            if (n <= 0) {
                break;
            }
            done += static_cast<std::size_t>(n);
        }
        const bool synced = done == bytes.size() && ::fsync(fd) == 0;
        if (::close(fd) != 0 || !synced) {
            error = "can't write " + path_to_utf8(temp);
            fs::remove(temp, ec);
            return false;
        }
    } else
#else
    (void)owner_only;
#endif
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            error = "can't write " + path_to_utf8(temp);
            out.close();
            fs::remove(temp, ec);
            return false;
        }
    }
    // Replaces an existing file on every platform (MoveFileExW with
    // MOVEFILE_REPLACE_EXISTING on Windows).
    fs::rename(temp, file, ec);
    if (ec) {
        error = "can't replace " + path_to_utf8(file) + ": " + error_text(ec);
        std::error_code ignored;
        fs::remove(temp, ignored);
        return false;
    }
    return true;
}

}  // namespace

bool write_file_atomically(const fs::path& file, std::string_view bytes, std::string& error) {
    return write_atomically(file, bytes, false, error);
}

bool write_private_file_atomically(const fs::path& file, std::string_view bytes, std::string& error) {
    return write_atomically(file, bytes, true, error);
}

#ifdef _WIN32
namespace {

std::string last_error_text() {
    return error_text(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
}

// This user and SYSTEM, full control; `inherit`: passed on to what's inside.
bool set_owner_only_acl(const fs::path& target, bool inherit, std::string& error) {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        error = "can't read this user's identity: " + last_error_text();
        return false;
    }
    DWORD size = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> user(size == 0 ? 1 : size);
    const BOOL got = GetTokenInformation(token, TokenUser, user.data(), size, &size);
    CloseHandle(token);
    if (got == FALSE) {
        error = "can't read this user's identity: " + last_error_text();
        return false;
    }
    // Its owner may always change its ACL back: it must be this user (or
    // the owner this user's files get, Administrators when elevated).
    {
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        std::wstring path_name = target.native();
        const DWORD read = GetNamedSecurityInfoW(path_name.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                                 nullptr, nullptr, nullptr, &descriptor);
        if (read != ERROR_SUCCESS || owner == nullptr) {
            if (descriptor != nullptr) {
                LocalFree(descriptor);
            }
            error = "can't read who owns " + path_to_utf8(target) + ": " +
                    error_text(std::error_code(static_cast<int>(read), std::system_category()));
            return false;
        }
        bool ours = EqualSid(owner, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid) != FALSE;
        if (!ours) {
            HANDLE again = nullptr;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &again) != FALSE) {
                DWORD owner_size = 0;
                (void)GetTokenInformation(again, TokenOwner, nullptr, 0, &owner_size);
                std::vector<std::uint8_t> default_owner(owner_size == 0 ? 1 : owner_size);
                if (GetTokenInformation(again, TokenOwner, default_owner.data(), owner_size, &owner_size) != FALSE) {
                    ours = EqualSid(owner, reinterpret_cast<TOKEN_OWNER*>(default_owner.data())->Owner) != FALSE;
                }
                CloseHandle(again);
            }
        }
        LocalFree(descriptor);
        if (!ours) {
            error = path_to_utf8(target) + " belongs to another user, so it can't be kept private; remove it (or "
                                           "choose another FLOSERVER_DATA_DIR) and try again";
            return false;
        }
    }
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system{};
    DWORD system_size = static_cast<DWORD>(system.size());
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &system_size) == FALSE) {
        error = "can't name SYSTEM: " + last_error_text();
        return false;
    }
    std::array<EXPLICIT_ACCESS_W, 2> access{};
    const DWORD inheritance = inherit ? static_cast<DWORD>(SUB_CONTAINERS_AND_OBJECTS_INHERIT) : NO_INHERITANCE;
    const std::array<PSID, 2> sids = {reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, system.data()};
    for (std::size_t i = 0; i < access.size(); ++i) {
        access[i].grfAccessPermissions = FILE_ALL_ACCESS;
        access[i].grfAccessMode = SET_ACCESS;
        access[i].grfInheritance = inheritance;
        access[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access[i].Trustee.TrusteeType = i == 0 ? TRUSTEE_IS_USER : TRUSTEE_IS_WELL_KNOWN_GROUP;
        access[i].Trustee.ptstrName = static_cast<LPWSTR>(sids[i]);
    }
    PACL acl = nullptr;
    if (SetEntriesInAclW(static_cast<ULONG>(access.size()), access.data(), nullptr, &acl) != ERROR_SUCCESS) {
        error = "can't make an access list";
        return false;
    }
    std::wstring name = target.native();
    const DWORD set = SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
                                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                            nullptr, acl, nullptr);
    LocalFree(acl);
    if (set != ERROR_SUCCESS) {
        error = "can't limit who may open " + path_to_utf8(target) + ": " +
                error_text(std::error_code(static_cast<int>(set), std::system_category()));
        return false;
    }
    return true;
}

}  // namespace
#endif

bool make_owner_only_folder(const fs::path& dir, std::string& error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(fs::symlink_status(dir, ec))) {
        error = "can't make the folder " + path_to_utf8(dir) + (ec ? ": " + error_text(ec) : std::string(" (it's a link)"));
        return false;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(dir.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = "can't keep " + path_to_utf8(dir) + " private: it's a link or can't be read";
        return false;
    }
    return set_owner_only_acl(dir, true, error);
#else
    struct stat st {};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        error = "can't keep " + path_to_utf8(dir) + " private: it's a link or can't be read";
        return false;
    }
    if (st.st_uid != ::geteuid()) {
        error = path_to_utf8(dir) + " belongs to another user, so it can't be kept private; remove it (or choose "
                                    "another FLOSERVER_DATA_DIR) and try again";
        return false;
    }
    if (::chmod(dir.c_str(), 0700) != 0) {
        error = "can't limit who may open " + path_to_utf8(dir) + ": " +
                error_text(std::error_code(errno, std::generic_category()));
        return false;
    }
    return true;
#endif
}

bool make_owner_only_files(const fs::path& dir, std::string& error) {
#ifdef _WIN32
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        // Asked of the name itself: a reparse point (an AF_UNIX socket, a
        // link) is never opened, followed or changed.
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
            continue;
        }
        if (!set_owner_only_acl(it->path(), false, error)) {
            return false;
        }
    }
    if (ec) {
        error = "can't read " + path_to_utf8(dir) + ": " + error_text(ec);
        return false;
    }
#else
    (void)dir;
    (void)error;
#endif
    return true;
}

bool rename_no_replace(const fs::path& from, const fs::path& to, std::string& error) {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
        error = "can't move " + path_to_utf8(from) + " to " + path_to_utf8(to) + ": " +
                error_text(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        return false;
    }
#else
    // link() fails when `to` exists; then the old name goes.
    if (::link(from.c_str(), to.c_str()) != 0) {
        error = "can't move " + path_to_utf8(from) + " to " + path_to_utf8(to) + ": " +
                error_text(std::error_code(errno, std::generic_category()));
        return false;
    }
    if (::unlink(from.c_str()) != 0) {
        error = "can't move " + path_to_utf8(from) + ": " + error_text(std::error_code(errno, std::generic_category()));
        (void)::unlink(to.c_str());
        return false;
    }
#endif
    return true;
}

std::optional<std::string> read_small_file(const fs::path& file, std::size_t max_bytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string out;
    std::array<char, 4096> chunk{};
    while (in.read(chunk.data(), chunk.size()) || in.gcount() > 0) {
        const auto got = static_cast<std::size_t>(in.gcount());
        if (out.size() + got > max_bytes) {
            return std::nullopt;
        }
        out.append(chunk.data(), got);
    }
    if (in.bad()) {
        return std::nullopt;
    }
    return out;
}

}  // namespace floserver::core

#include "floserver/core/paths.h"

#include "floserver/core/process.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <array>
#include <fstream>
#include <random>
#include <system_error>

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
fs::path manifest_state_file(const fs::path& data) { return data / "state" / "manifest-state.json"; }

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

bool write_file_atomically(const fs::path& file, std::string_view bytes, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "can't create " + path_to_utf8(file.parent_path()) + ": " + error_text(ec);
        return false;
    }
    std::random_device rd;
    fs::path temp = file;
    temp += ".tmp-" + std::to_string(rd());
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

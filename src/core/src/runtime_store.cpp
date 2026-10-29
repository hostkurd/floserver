#include "floserver/core/runtime_store.h"

#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/version.h"
#include "floserver/core/tar_extract.h"
#include "floserver/core/zip_extract.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <random>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::size_t kMaxMarkerBytes = std::size_t{16} * 1024;
constexpr std::string_view kStagingPrefix = ".staging-";
constexpr std::string_view kRemovingPrefix = ".removing-";

bool is_plain(std::string_view s) {
    return !s.empty() && s.size() <= 64 && s.front() != '.' && s.front() != '-' &&
           std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
           }) &&
           s.find("..") == std::string_view::npos;
}

std::string random_suffix() {
    std::random_device rd;
    return std::to_string(rd()) + std::to_string(rd());
}

std::string now_utc() {
    const auto t = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto days = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day ymd{days};
    const std::chrono::hh_mm_ss hms{t - days};
    std::array<char, 32> text{};
    const int n = std::snprintf(text.data(), text.size(), "%04d-%02u-%02uT%02d:%02d:%02dZ",
                                static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                                static_cast<unsigned>(ymd.day()), static_cast<int>(hms.hours().count()),
                                static_cast<int>(hms.minutes().count()),
                                static_cast<int>(hms.seconds().count()));
    return {text.data(), n > 0 ? static_cast<std::size_t>(n) : 0};
}

// The file name for a runtime used as is ("file" archives: composer.phar).
std::string plain_file_name(const RuntimeFile& entry) {
    if (!entry.urls.empty()) {
        std::string_view url = entry.urls.front();
        url = url.substr(0, url.find_first_of("?#"));
        const std::string_view last = url.substr(url.rfind('/') + 1);
        if (!safe_entry_path(last).empty() && !last.starts_with('.')) {
            return std::string(last);
        }
    }
    return entry.name + ".bin";
}

bool has_program(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        std::string ext = path_to_utf8(p.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (ext == ".exe" && it->is_regular_file(ec)) {
            return true;
        }
    }
    return false;
}

// Copies the VC++ runtime DLLs next to the runtime's programs. The folders
// are the ones the 1.0 runtimes keep their programs in: the top folder (PHP,
// nginx, Node.js), bin (MariaDB) and Apache24/bin (Apache Lounge).
bool add_vc_runtime(const fs::path& install, const fs::path& vc_dir, std::string& error) {
    std::error_code ec;
    std::vector<fs::path> dlls;
    for (fs::directory_iterator it(vc_dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string ext = path_to_utf8(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (ext == ".dll" && it->is_regular_file(ec)) {
            dlls.push_back(it->path());
        }
    }
    if (ec) {
        error = "can't read the Visual C++ runtime folder " + path_to_utf8(vc_dir) + ": " + error_text(ec);
        return false;
    }
    for (const fs::path& sub : {fs::path("."), fs::path("bin"), fs::path("Apache24") / "bin"}) {
        const fs::path dir = (install / sub).lexically_normal();
        if (!fs::is_directory(dir, ec) || !has_program(dir)) {
            continue;
        }
        for (const fs::path& dll : dlls) {
            const fs::path to = dir / dll.filename();
            if (fs::exists(to, ec)) {
                continue;  // the runtime brings its own copy
            }
            fs::copy_file(dll, to, ec);
            if (ec) {
                error = "can't copy " + path_to_utf8(dll.filename()) + ": " + error_text(ec);
                return false;
            }
        }
    }
    return true;
}

std::optional<InstalledRuntime> read_marker(const fs::path& dir, const std::string& name) {
    const auto text = read_small_file(dir / path_from_utf8(kRuntimeMarker), kMaxMarkerBytes);
    if (!text) {
        return std::nullopt;
    }
    const json doc = json::parse(*text, nullptr, false);
    if (!doc.is_object() || doc.value("schema", 0) != 1) {
        return std::nullopt;
    }
    InstalledRuntime r;
    try {
        r.name = doc.at("name").get<std::string>();
        r.version = doc.at("version").get<std::string>();
        r.platform = doc.at("platform").get<std::string>();
        r.variant = doc.value("variant", std::string());
        r.license = doc.value("license", std::string());
        r.sha256 = doc.value("sha256", std::string());
    } catch (const json::exception&) {
        return std::nullopt;
    }
    if (r.name != name || !Version::parse(r.version)) {
        return std::nullopt;
    }
    r.dir = dir;
    return r;
}

}  // namespace

std::string runtime_folder(const RuntimeFile& entry) {
    if (!is_plain(entry.name) || !Version::parse(entry.version)) {
        return {};
    }
    std::string folder = entry.version;
    if (!entry.variant.empty() && entry.variant != "nts") {
        if (!is_plain(entry.variant)) {
            return {};
        }
        folder += "-" + entry.variant;
    }
    return is_plain(folder) ? folder : std::string();
}

std::vector<InstalledRuntime> list_installed(const fs::path& runtimes_root) {
    std::vector<InstalledRuntime> out;
    std::error_code ec;
    for (fs::directory_iterator names(runtimes_root, ec), end; !ec && names != end; names.increment(ec)) {
        const std::string name = path_to_utf8(names->path().filename());
        if (!is_plain(name) || !names->is_directory(ec)) {
            continue;
        }
        std::error_code inner;
        for (fs::directory_iterator v(names->path(), inner); !inner && v != end; v.increment(inner)) {
            const std::string folder = path_to_utf8(v->path().filename());
            if (!is_plain(folder) || !v->is_directory(inner)) {
                continue;
            }
            if (auto r = read_marker(v->path(), name)) {
                out.push_back(std::move(*r));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const InstalledRuntime& a, const InstalledRuntime& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        const auto va = Version::parse(a.version).value_or(Version{});
        const auto vb = Version::parse(b.version).value_or(Version{});
        if (va != vb) {
            return va > vb;
        }
        return a.variant < b.variant;
    });
    return out;
}

bool is_installed(const fs::path& runtimes_root, const RuntimeFile& entry) {
    const std::string folder = runtime_folder(entry);
    if (folder.empty()) {
        return false;
    }
    const fs::path dir = runtimes_root / path_from_utf8(entry.name) / path_from_utf8(folder);
    const auto r = read_marker(dir, entry.name);
    return r && r->version == entry.version && r->platform == entry.platform;
}

InstallResult install_runtime(const RuntimeFile& entry, const fs::path& archive, const fs::path& runtimes_root,
                              const fs::path& vc_runtime_dir) {
    InstallResult result;
    const std::string folder = runtime_folder(entry);
    if (folder.empty()) {
        result.message = "the runtime's name or version can't be used as a folder name";
        return result;
    }
    const fs::path parent = runtimes_root / path_from_utf8(entry.name);
    result.dir = parent / path_from_utf8(folder);
    if (is_installed(runtimes_root, entry)) {
        result.ok = true;
        result.already_installed = true;
        return result;
    }

    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        result.message = "can't create " + path_to_utf8(parent) + ": " + error_text(ec);
        return result;
    }
    const fs::path staging = parent / path_from_utf8(std::string(kStagingPrefix) + folder + "-" + random_suffix());
    const auto fail = [&](std::string message) {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        result.message = std::move(message);
        return result;
    };

    if (entry.archive == "zip" || entry.archive == "tar.gz") {
        const ZipResult z = entry.archive == "zip" ? extract_zip(archive, staging) : extract_tar_gz(archive, staging);
        if (!z.ok) {
            return fail("couldn't unpack " + entry.name + " " + entry.version + ": " + z.message);
        }
    } else if (entry.archive == "file") {
        fs::create_directories(staging, ec);
        if (!ec) {
            fs::copy_file(archive, staging / path_from_utf8(plain_file_name(entry)), ec);
        }
        if (ec) {
            return fail("couldn't copy " + entry.name + " " + entry.version + ": " + error_text(ec));
        }
    } else {
        return fail("FloServer can't unpack " + entry.archive + " archives on this platform yet");
    }

    if (!vc_runtime_dir.empty()) {
        std::string error;
        if (!add_vc_runtime(staging, vc_runtime_dir, error)) {
            return fail(error);
        }
    }

    const json marker = {{"schema", 1},
                         {"name", entry.name},
                         {"version", entry.version},
                         {"platform", entry.platform},
                         {"variant", entry.variant},
                         {"license", entry.license},
                         {"sha256", to_hex(entry.sha256)},
                         {"size", entry.size},
                         {"installed", now_utc()}};
    std::string error;
    if (!write_file_atomically(staging / path_from_utf8(kRuntimeMarker), marker.dump(2) + "\n", error)) {
        return fail(error);
    }

    if (fs::exists(result.dir, ec)) {
        // A folder without a valid marker: a broken earlier install. Move it
        // aside rather than guess what's in it.
        const fs::path aside = parent / path_from_utf8(std::string(kRemovingPrefix) + folder + "-" + random_suffix());
        fs::rename(result.dir, aside, ec);
        if (ec) {
            return fail("can't replace the broken install in " + path_to_utf8(result.dir) + ": " + error_text(ec));
        }
        fs::remove_all(aside, ec);
    }
    fs::rename(staging, result.dir, ec);
    if (ec) {
        return fail("can't move the install into " + path_to_utf8(result.dir) + ": " + error_text(ec));
    }
    result.ok = true;
    return result;
}

bool remove_runtime(const fs::path& runtimes_root, const std::string& name, const std::string& folder,
                    std::string& error) {
    if (!is_plain(name) || !is_plain(folder)) {
        error = "that isn't a runtime FloServer installed";
        return false;
    }
    const fs::path parent = runtimes_root / path_from_utf8(name);
    const fs::path dir = parent / path_from_utf8(folder);
    if (!read_marker(dir, name)) {
        error = name + " " + folder + " isn't installed";
        return false;
    }
    // Rename first: a runtime that's in use fails here, before anything is
    // deleted, instead of being left half removed.
    std::error_code ec;
    const fs::path aside = parent / path_from_utf8(std::string(kRemovingPrefix) + folder + "-" + random_suffix());
    fs::rename(dir, aside, ec);
    if (ec) {
        error = "can't remove " + name + " " + folder + " (is it running?): " + error_text(ec);
        return false;
    }
    fs::remove_all(aside, ec);
    return true;
}

void remove_leftovers(const fs::path& runtimes_root) {
    std::error_code ec;
    for (fs::directory_iterator names(runtimes_root, ec), end; !ec && names != end; names.increment(ec)) {
        if (!names->is_directory(ec)) {
            continue;
        }
        std::error_code inner;
        std::vector<fs::path> doomed;
        for (fs::directory_iterator v(names->path(), inner); !inner && v != end; v.increment(inner)) {
            const std::string folder = path_to_utf8(v->path().filename());
            if (folder.starts_with(kStagingPrefix) || folder.starts_with(kRemovingPrefix)) {
                doomed.push_back(v->path());
            }
        }
        for (const auto& d : doomed) {
            fs::remove_all(d, inner);
        }
    }
}

}  // namespace floserver::core

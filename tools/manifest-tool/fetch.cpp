#include "fetch.h"

#include "build.h"

#include "floserver/core/encoding.h"
#include "floserver/core/garnet.h"
#include "floserver/core/paths.h"
#include "floserver/core/sha256.h"
#include "floserver/core/version.h"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>

namespace floserver::manifest_tool {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using core::Version;

constexpr std::uint64_t kMaxFile = std::uint64_t{2} * 1024 * 1024 * 1024;  // as Publish's limit
constexpr std::size_t kMaxText = std::size_t{8} * 1024 * 1024;
constexpr std::uint64_t kWranglerMax = std::uint64_t{300} * 1024 * 1024;  // "files up to 300 MiB"

bool stopped(const std::atomic<bool>* cancel) { return cancel != nullptr && cancel->load(); }

std::optional<std::string> get_text(core::HttpTransport& http, const std::string& url, const std::atomic<bool>* cancel,
                                    std::string& error) {
    std::string body;
    core::HttpLimits limits;
    limits.max_time = std::chrono::seconds{60};
    limits.cancel = cancel;
    bool too_big = false;
    const core::HttpReply reply = http.get(url, limits, [&](std::span<const std::uint8_t> chunk) {
        if (body.size() + chunk.size() > kMaxText) {
            too_big = true;
            return false;
        }
        body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        return true;
    });
    if (reply.status != 200 || reply.stopped || too_big) {
        error = url + ": " + (too_big ? std::string("answer too large")
                                      : reply.status == 0 ? (reply.error.empty() ? "no answer" : reply.error)
                                                          : "HTTP " + std::to_string(reply.status));
        return std::nullopt;
    }
    return body;
}

// Whether a zip is there (its first bytes only).
bool zip_there(core::HttpTransport& http, const std::string& url, const std::atomic<bool>* cancel) {
    core::HttpLimits limits;
    limits.max_time = std::chrono::seconds{30};
    limits.cancel = cancel;
    bool zip = false;
    const core::HttpReply reply = http.get(url, limits, [&zip](std::span<const std::uint8_t> chunk) {
        zip = chunk.size() >= 4 && chunk[0] == 'P' && chunk[1] == 'K' && chunk[2] == 3 && chunk[3] == 4;
        return false;
    });
    return reply.status == 200 && zip;
}

std::string regex_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (std::string_view(R"(\^$.|?*+()[]{})").find(c) != std::string_view::npos) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

// The first match of `re` in `text`, group `g`.
std::optional<std::string> find(std::string_view text, const std::regex& re, std::size_t g = 0) {
    const std::string s(text);
    std::smatch m;
    if (std::regex_search(s, m, re)) {
        return m[g].str();
    }
    return std::nullopt;
}

std::string base_name(std::string_view url) {
    const auto slash = url.rfind('/');
    return std::string(slash == std::string_view::npos ? url : url.substr(slash + 1));
}

// ifstream/fstream readers and writers for miniz: paths stay wide on Windows.
std::size_t zip_read(void* opaque, mz_uint64 offset, void* buffer, std::size_t n) {
    auto* in = static_cast<std::ifstream*>(opaque);
    in->clear();
    in->seekg(static_cast<std::streamoff>(offset));
    in->read(static_cast<char*>(buffer), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(in->gcount());
}

std::size_t zip_write(void* opaque, mz_uint64 offset, const void* buffer, std::size_t n) {
    auto* out = static_cast<std::fstream*>(opaque);
    out->seekp(static_cast<std::streamoff>(offset));
    out->write(static_cast<const char*>(buffer), static_cast<std::streamsize>(n));
    return *out ? n : 0;
}

// Downloads `url` to `file` (a .part first), with its SHA-256.
bool download(core::HttpTransport& http, const std::string& url, const fs::path& file, const std::atomic<bool>* cancel,
              const std::function<void(std::uint64_t)>& bytes, std::string& sha256, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path part = file;
    part += ".part";
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "can't write " + core::path_to_utf8(part);
        return false;
    }
    core::Sha256 hash;
    std::uint64_t size = 0;
    bool too_big = false;
    core::HttpLimits limits;
    limits.max_time = std::chrono::hours{2};
    limits.cancel = cancel;
    const core::HttpReply reply = http.get(url, limits, [&](std::span<const std::uint8_t> chunk) {
        if (size + chunk.size() > kMaxFile) {
            too_big = true;
            return false;
        }
        hash.update(chunk);
        out.write(reinterpret_cast<const char*>(chunk.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                  static_cast<std::streamsize>(chunk.size()));
        size += chunk.size();
        if (bytes) {
            bytes(size);
        }
        return static_cast<bool>(out);
    });
    out.close();
    if (reply.status != 200 || reply.stopped || too_big || !out) {
        fs::remove(part, ec);
        error = too_big ? url + ": larger than 2 GB"
                : !out  ? "can't write " + core::path_to_utf8(part) + " (is the disk full?)"
                        : url + ": " + (reply.status == 0 ? (reply.error.empty() ? std::string("no answer") : reply.error)
                                                          : "HTTP " + std::to_string(reply.status));
        return false;
    }
    fs::rename(part, file, ec);
    if (ec) {
        error = "can't write " + core::path_to_utf8(file) + ": " + ec.message();
        fs::remove(part, ec);
        return false;
    }
    sha256 = core::to_hex(hash.finish());
    return true;
}

}  // namespace

bool make_postgresql_server_zip(const fs::path& edb_zip, const fs::path& out, std::string& error,
                                const std::atomic<bool>* cancel) {
    // The same list Publish checks against (OfficialSource::kept, required).
    const OfficialSource rules = official_source("postgresql/18.0/postgresql-18.0-1-windows-x64-server.zip");
    std::ifstream in(edb_zip, std::ios::binary);
    std::error_code ec;
    const auto size = fs::file_size(edb_zip, ec);
    if (!in || ec) {
        error = "can't read " + core::path_to_utf8(edb_zip);
        return false;
    }
    mz_zip_archive reader{};
    reader.m_pRead = zip_read;
    reader.m_pIO_opaque = &in;
    if (mz_zip_reader_init(&reader, size, 0) == MZ_FALSE) {
        error = core::path_to_utf8(edb_zip) + " isn't a zip EDB made";
        return false;
    }
    fs::create_directories(out.parent_path(), ec);
    fs::path part = out;
    part += ".part";
    std::fstream file(part, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    mz_zip_archive writer{};
    writer.m_pWrite = zip_write;
    writer.m_pIO_opaque = &file;
    // A zip64 source copies only into a zip64 archive (miniz refuses otherwise).
    const mz_uint flags = mz_zip_is_zip64(&reader) != MZ_FALSE ? MZ_ZIP_FLAG_WRITE_ZIP64 : 0;
    bool ok = file.is_open();
    if (!ok) {
        error = "can't write " + core::path_to_utf8(part);
    } else if (mz_zip_writer_init_v2(&writer, 0, flags) == MZ_FALSE) {
        error = "can't start the server zip";
        ok = false;
    }
    std::set<std::string> names;
    const mz_uint count = mz_zip_reader_get_num_files(&reader);
    for (mz_uint i = 0; ok && i < count; ++i) {
        if (stopped(cancel)) {
            error = "stopped";
            ok = false;
            break;
        }
        mz_zip_archive_file_stat st{};
        if (mz_zip_reader_file_stat(&reader, i, &st) == MZ_FALSE || st.m_is_directory != 0) {
            continue;  // folders come with their files
        }
        std::string name(st.m_filename);
        std::replace(name.begin(), name.end(), '\\', '/');
        const bool wanted = std::any_of(rules.kept.begin(), rules.kept.end(),
                                        [&name](const std::string& k) { return name.starts_with(k); });
        if (!wanted) {
            continue;
        }
        // Copied as it is: the same compressed bytes, name and date.
        if (mz_zip_writer_add_from_zip_reader(&writer, &reader, i) == MZ_FALSE) {
            error = "can't copy " + name + " into the server zip";
            ok = false;
            break;
        }
        names.insert(name);
    }
    if (ok) {
        for (const std::string& need : rules.required) {
            if (!names.contains(need)) {
                error = "EDB's zip has no " + need + ": is it the Windows x64 binaries zip?";
                ok = false;
                break;
            }
        }
    }
    if (ok && (mz_zip_writer_finalize_archive(&writer) == MZ_FALSE || !file)) {
        error = "can't finish the server zip (is the disk full?)";
        ok = false;
    }
    mz_zip_writer_end(&writer);
    mz_zip_reader_end(&reader);
    file.close();
    if (ok && file.fail()) {
        error = "can't finish the server zip (is the disk full?)";
        ok = false;
    }
    if (ok) {
        fs::rename(part, out, ec);
        if (ec) {
            error = "can't write " + core::path_to_utf8(out) + ": " + ec.message();
            ok = false;
        }
    }
    if (!ok) {
        fs::remove(part, ec);
    }
    return ok;
}

FetchPlan plan_fetch(core::HttpTransport& http, const UpdateRow& row,
                     const std::map<std::string, std::vector<std::string>>& listed, const std::atomic<bool>* cancel) {
    FetchPlan plan;
    const std::string& v = row.latest;
    const std::string& r = row.runtime;
    plan.title = runtime_title(r, v, "");
    const auto add = [&](const std::string& url, const std::string& file, FetchFile::Prepare prepare = {}) {
        plan.files.push_back({url, r + "/" + v + "/" + file, prepare});
    };
    const auto simple = [&](const std::string& base, const std::string& file) { add(base + file, file); };
    std::string error;
    if (r == "php") {
        // Its name: with a rebuild number or without, vs16 or vs17. The
        // current releases first, then the archives (where it goes once a
        // newer patch is out).
        const std::regex re("php-" + regex_escape(v) + R"((-\d+)?-nts-Win32-vs1[67]-x64\.zip)");
        std::string read_errors;  // a list that couldn't be read, said with the answer
        for (const std::string base : {"https://downloads.php.net/~windows/releases/",
                                       "https://downloads.php.net/~windows/releases/archives/"}) {
            if (const auto sums = get_text(http, base + "sha256sum.txt", cancel, error)) {
                if (const auto file = find(*sums, re)) {
                    plan.files.clear();
                    simple(base, *file);
                    plan.problem.clear();
                    read_errors.clear();
                    break;
                }
                plan.problem = "php.net's checksum lists don't name PHP " + v + "'s Windows NTS x64 zip";
            } else {
                read_errors += (read_errors.empty() ? "" : "; ") + error;
            }
        }
        if (!read_errors.empty()) {
            plan.problem = plan.problem.empty() ? read_errors : plan.problem + " (" + read_errors + ")";
        }
    } else if (r == "mariadb") {
        simple("https://archive.mariadb.org/mariadb-" + v + "/winx64-packages/", "mariadb-" + v + "-winx64.zip");
    } else if (r == "node") {
        simple("https://nodejs.org/dist/v" + v + "/", "node-v" + v + "-win-x64.zip");
    } else if (r == "composer") {
        simple("https://getcomposer.org/download/" + v + "/", "composer.phar");
    } else if (r == "mysql") {
        simple("https://cdn.mysql.com/Downloads/MySQL-8.4/", "mysql-" + v + "-winx64.zip");
    } else if (r == "nginx") {
        simple("https://nginx.org/download/", "nginx-" + v + ".zip");
    } else if (r == "wordpress") {
        simple("https://wordpress.org/", "wordpress-" + v + ".zip");
    } else if (r == "phpmyadmin") {
        simple("https://files.phpmyadmin.net/phpMyAdmin/" + v + "/", "phpMyAdmin-" + v + "-all-languages.zip");
    } else if (r == "mailpit") {
        simple("https://github.com/axllent/mailpit/releases/download/v" + v + "/", "mailpit-windows-amd64.zip");
    } else if (r == "garnet") {
        simple("https://github.com/microsoft/garnet/releases/download/v" + v + "/", "win-x64-based-readytorun.zip");
    } else if (r == "dotnet") {
        simple("https://builds.dotnet.microsoft.com/dotnet/Runtime/" + v + "/", "dotnet-runtime-" + v + "-win-x64.zip");
    } else if (r == "deno") {
        simple("https://github.com/denoland/deno/releases/download/v" + v + "/", "deno-x86_64-pc-windows-msvc.zip");
    } else if (r == "bun") {
        // The regular build and the "baseline" one for CPUs without AVX2.
        simple("https://github.com/oven-sh/bun/releases/download/bun-v" + v + "/", "bun-windows-x64.zip");
        simple("https://github.com/oven-sh/bun/releases/download/bun-v" + v + "/", "bun-windows-x64-baseline.zip");
    } else if (r == "apache") {
        // Apache Lounge's name carries its build stamp and compiler.
        const std::string page = "https://www.apachelounge.com/download/";
        if (const auto html = get_text(http, page, cancel, error)) {
            const std::regex re("href=\"([^\"]*/(httpd-" + regex_escape(v) + R"(-\d+-Win64-VS1[78]\.zip))\")");
            const auto href = find(*html, re, 1);
            const auto file = find(*html, re, 2);
            if (href && file) {
                const std::string url = href->starts_with("http") ? *href
                                        : href->starts_with('/')  ? "https://www.apachelounge.com" + *href
                                                                  : page + *href;
                add(url, *file);
            } else {
                plan.problem = page + " has no Win64 zip of httpd " + v;
            }
        } else {
            plan.problem = error;
        }
    } else if (r == "python") {
        // python-build-standalone's name has "+<date>"; the CDN's has "-<date>".
        const std::string url = "https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest";
        if (const auto text = get_text(http, url, cancel, error)) {
            const std::regex re("^cpython-" + regex_escape(v) +
                                R"(\+(\d{8})-x86_64-pc-windows-msvc-install_only_stripped\.tar\.gz$)");
            try {
                const json doc = json::parse(*text);
                for (const json& a : doc.at("assets")) {
                    const std::string name = a.at("name").get<std::string>();
                    std::smatch m;
                    if (std::regex_match(name, m, re)) {
                        std::string cdn = name;
                        cdn[cdn.find('+')] = '-';
                        add(a.at("browser_download_url").get<std::string>(), cdn);
                        break;
                    }
                }
                if (plan.files.empty()) {
                    plan.problem = "python-build-standalone's latest release has no Windows x64 build of " + v;
                }
            } catch (const json::exception& e) {
                plan.problem = "python-build-standalone's latest release couldn't be read: " + std::string(e.what());
            }
        } else {
            plan.problem = error;
        }
    } else if (r == "postgresql") {
        // EDB's build number: the highest of 1..4 that's there.
        int build = 0;
        for (int n = 1; n <= 4; ++n) {
            if (zip_there(http, "https://get.enterprisedb.com/postgresql/postgresql-" + v + "-" + std::to_string(n) +
                                    "-windows-x64-binaries.zip",
                          cancel)) {
                build = n;
            }
        }
        if (build == 0) {
            plan.problem = "EDB has no Windows x64 binaries zip of PostgreSQL " + v + " yet";
        } else {
            const std::string n = std::to_string(build);
            add("https://get.enterprisedb.com/postgresql/postgresql-" + v + "-" + n + "-windows-x64-binaries.zip",
                "postgresql-" + v + "-" + n + "-windows-x64-server.zip", FetchFile::Prepare::PostgresServerZip);
        }
    } else if (r == "php-redis") {
        // One DLL zip per PHP series in the list: vs17 from 8.4, vs16 before.
        std::set<std::string> series;
        if (const auto php = listed.find("php"); php != listed.end()) {
            for (const std::string& p : php->second) {
                series.insert(update_series("php", p));
            }
        }
        std::vector<std::string> missing;
        for (const std::string& s : series) {
            const auto sv = Version::parse(s);
            const auto floor = Version::parse("8.4");
            const std::string vs = sv && floor && !(*sv < *floor) ? "vs17" : "vs16";
            std::string file = "php_redis-";
            file.append(v).append("-").append(s).append("-nts-").append(vs).append("-x64.zip");
            const std::string url = core::php_redis_upstream(v, file);
            if (zip_there(http, url, cancel)) {
                add(url, file);
            } else {
                missing.push_back(s);
            }
        }
        if (series.empty()) {
            plan.problem = "there's no PHP in the next list to build phpredis for";
        } else if (plan.files.empty()) {
            plan.problem = "PECL has no Windows builds of phpredis " + v + " yet";
        } else if (!missing.empty()) {
            plan.note = "no Windows build yet for PHP";
            for (const std::string& m : missing) {
                plan.note += " " + m;
            }
        }
    } else {
        plan.problem = "the Publisher doesn't know how to get " + r;
    }
    return plan;
}

FetchResult fetch_files(core::HttpTransport& http, const FetchPlan& plan, const fs::path& folder,
                        const std::atomic<bool>* cancel, const std::function<void(const FetchProgress&)>& progress) {
    FetchResult result;
    CheckContext ctx{http, std::string(kCdn)};
    ctx.cancel = cancel;
    const auto tell = [&](const std::string& step, std::uint64_t bytes) {
        if (progress) {
            progress({plan.title, step, bytes});
        }
    };
    for (const FetchFile& f : plan.files) {
        if (stopped(cancel)) {
            result.problem = "stopped";
            break;
        }
        const fs::path target = folder / core::path_from_utf8(f.cdn_path);
        std::string sha;
        std::string error;
        std::error_code ec;
        if (f.prepare == FetchFile::Prepare::PostgresServerZip) {
            // EDB's whole zip first, then the server part of it beside the target.
            const fs::path edb = target.parent_path() / core::path_from_utf8(base_name(f.url));
            if (!download(http, f.url, edb, cancel, [&](std::uint64_t b) { tell("Downloading EDB's zip", b); }, sha, error)) {
                result.problem = error;
                break;
            }
            tell("Making the server zip", 0);
            const bool made = make_postgresql_server_zip(edb, target, error, cancel);
            fs::remove(edb, ec);
            if (!made) {
                result.problem = error.empty() ? "the server zip couldn't be made" : error;
                break;
            }
        } else {
            if (!download(http, f.url, target, cancel, [&](std::uint64_t b) { tell("Downloading", b); }, sha, error)) {
                result.problem = error;
                break;
            }
            // Its official checksum, where its project publishes one.
            tell("Checking", 0);
            std::string why_not;
            const auto official = official_checksum(ctx, f.cdn_path, why_not);
            if (official && *official != sha) {
                fs::remove(target, ec);
                result.problem = f.cdn_path + " is DIFFERENT from its official checksum: deleted, not to be uploaded";
                break;
            }
            if (!official && !why_not.empty()) {
                fs::remove(target, ec);
                result.problem = f.cdn_path + ": " + why_not;
                break;
            }
        }
        result.cdn_paths.push_back(f.cdn_path);
    }
    if (!result.problem.empty() || result.cdn_paths.size() != plan.files.size()) {
        // All of a version or none: half of Bun's builds mustn't replace the old ones.
        if (result.problem.empty()) {
            result.problem = "not every file was written";
        }
        for (const FetchFile& f : plan.files) {
            const fs::path target = folder / core::path_from_utf8(f.cdn_path);
            std::error_code ec;
            fs::remove(target, ec);
            // And the folders made for it, while empty, up to `folder`.
            for (fs::path d = target.parent_path(); d != folder && d.has_relative_path(); d = d.parent_path()) {
                if (!fs::is_empty(d, ec) || ec || !fs::remove(d, ec)) {
                    break;
                }
            }
        }
        result.cdn_paths.clear();
    }
    return result;
}

std::string upload_steps(const std::vector<std::string>& cdn_paths, const fs::path& folder) {
    std::string out =
        "Upload these files to the floserver-cdn bucket, each at the path shown (the same as its place in this "
        "folder),\r\nthen press Publish in FloServer Publisher: it checks every file from the CDN before signing.\r\n\r\n"
        "With Wrangler, from this folder (npx wrangler login once first):\r\n\r\n";
    std::vector<std::string> big;
    for (const std::string& p : cdn_paths) {
        std::error_code ec;
        if (!folder.empty() && fs::file_size(folder / core::path_from_utf8(p), ec) > kWranglerMax && !ec) {
            big.push_back(p);
            continue;
        }
        std::string local = p;
        std::replace(local.begin(), local.end(), '/', '\\');
        out.append("npx wrangler r2 object put floserver-cdn/").append(p).append(" --file \"").append(local).append(
            "\" --remote\r\n");
    }
    if (!big.empty()) {
        out += "\r\nOver 300 MiB, which Wrangler and the dashboard can't upload: use an S3 client with an R2 key "
               "(rclone, or aws s3 cp --endpoint-url https://<account id>.r2.cloudflarestorage.com):\r\n\r\n";
        for (const std::string& p : big) {
            out.append("  ").append(p).append("\r\n");
        }
    }
    return out;
}

ListChange add_to_list(std::vector<PublishFile> files, const std::vector<std::string>& cdn_paths) {
    ListChange change;
    for (const std::string& path : cdn_paths) {
        const CdnPath n = describe_cdn_path(path);
        const auto nv = Version::parse(n.version);
        if (!n.listable || !nv) {
            change.problems.push_back(n.problem.empty() ? path + ": not a runtime version" : n.problem);
            continue;
        }
        // Checked before anything older is taken out for it.
        if (const AddResult alone = new_file(path, kCdn, {}); !alone.file) {
            change.problems.push_back(alone.error);
            continue;
        }
        const std::string series = update_series(n.runtime, n.version);
        std::erase_if(files, [&](const PublishFile& f) {
            const CdnPath o = describe_cdn_path(f.path);
            const auto ov = Version::parse(o.version);
            const bool older = o.listable && ov && o.runtime == n.runtime && o.variant == n.variant &&
                               update_series(o.runtime, o.version) == series && *ov < *nv;
            if (older && f.published) {
                change.left_out.push_back(f.path);
            }
            return older;
        });
        if (std::any_of(files.begin(), files.end(), [&](const PublishFile& f) { return f.path == path; })) {
            continue;  // already there
        }
        const AddResult added = new_file(path, kCdn, files);
        if (!added.file) {
            change.problems.push_back(added.error);
            continue;
        }
        files.push_back(*added.file);
        change.added.push_back(path);
    }
    change.files = std::move(files);
    return change;
}

}  // namespace floserver::manifest_tool

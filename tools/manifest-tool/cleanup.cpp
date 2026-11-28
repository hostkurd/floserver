#include "cleanup.h"

#include "build.h"
#include "publish.h"

#include "floserver/core/paths.h"
#include "floserver/core/manifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace floserver::manifest_tool {

namespace {

using nlohmann::json;

// "https://cdn.floserver.org/php/8.5.12/x.zip" -> "php/8.5.12/x.zip".
std::optional<std::string> path_under(std::string_view url, std::string_view cdn) {
    std::string base(cdn);
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    base += '/';
    if (!url.starts_with(base) || url.size() == base.size()) {
        return std::nullopt;
    }
    return std::string(url.substr(base.size()));
}

void add_first_url(const json& entry, std::string_view cdn, std::vector<std::string>& out) {
    if (!entry.is_object()) {
        return;
    }
    const auto urls = entry.find("urls");
    if (urls == entry.end() || !urls->is_array()) {
        return;
    }
    for (const json& u : *urls) {
        if (u.is_string()) {
            if (auto p = path_under(u.get<std::string>(), cdn)) {
                out.push_back(std::move(*p));
                return;
            }
        }
    }
}

// "php/8.5.12" for "php/8.5.12/<file>".
std::string folder_of(std::string_view path) {
    const auto last = path.rfind('/');
    return last == std::string_view::npos ? std::string() : std::string(path.substr(0, last));
}

int order_of(CleanupKind k) {
    switch (k) {
        case CleanupKind::Ready:
            return 0;
        case CleanupKind::Wait:
            return 1;
        case CleanupKind::Unknown:
            return 2;
        case CleanupKind::Kept:
            break;
    }
    return 3;
}

}  // namespace

std::optional<std::vector<std::string>> list_paths(std::string_view manifest_json, std::string_view cdn) {
    const json doc = json::parse(manifest_json, nullptr, false);
    if (!doc.is_object()) {
        return std::nullopt;
    }
    const auto runtimes = doc.find("runtimes");
    if (runtimes == doc.end() || !runtimes->is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    for (const json& r : *runtimes) {
        add_first_url(r, cdn, out);
    }
    if (const auto app = doc.find("app"); app != doc.end()) {
        add_first_url(*app, cdn, out);
    }
    return out;
}

std::vector<SavedList> read_saved_lists(const std::filesystem::path& folder, std::string_view cdn) {
    namespace fs = std::filesystem;
    std::vector<SavedList> out;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = core::path_to_utf8(it->path().filename());
        // "2026-11-25 sequence 12" or "... (2)"
        if (name.size() < 21 || name.substr(10, 10) != " sequence " || !it->is_directory(ec)) {
            continue;
        }
        const auto read = [](const fs::path& p, std::size_t max) -> std::optional<std::string> {
            std::ifstream f(p, std::ios::binary);
            if (!f) {
                return std::nullopt;
            }
            std::string text(max + 1, '\0');
            f.read(text.data(), static_cast<std::streamsize>(text.size()));
            text.resize(static_cast<std::size_t>(f.gcount()));
            if (text.size() > max) {
                return std::nullopt;
            }
            return text;
        };
        const auto manifest = read(it->path() / "manifest.json", std::size_t{1} << 20);
        if (!manifest) {
            continue;
        }
        const json doc = json::parse(*manifest, nullptr, false);
        auto paths = list_paths(*manifest, cdn);
        const auto sequence = doc.is_object() ? doc.find("sequence") : doc.end();
        if (!paths || sequence == doc.end() || !sequence->is_number_integer()) {
            continue;
        }
        SavedList s;
        s.sequence = sequence->get<std::int64_t>();
        s.paths = std::move(*paths);
        std::optional<std::chrono::sys_seconds> made;
        if (const auto steps = read(it->path() / "upload-steps.txt", std::size_t{64} * 1024)) {
            // "FloServer runtime list, sequence 12, made 2026-11-25T10:00:00Z"
            if (const auto at = steps->find(", made "); at != std::string::npos) {
                made = core::parse_utc_time(steps->substr(at + 7, 20));
            }
        }
        if (!made) {
            made = core::parse_utc_time(name.substr(0, 10) + "T00:00:00Z");
        }
        if (!made) {
            continue;
        }
        s.made = *made;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<CleanupRow> plan_cleanup(const CleanupInput& input) {
    std::set<std::string> in_use(input.live_paths.begin(), input.live_paths.end());
    in_use.insert(input.next_paths.begin(), input.next_paths.end());
    // A source archive stays while a binary of its runtime and version is
    // listed (GPL: the source goes with the binary), on any platform.
    std::set<std::string> listed_folders;
    for (const std::string& p : in_use) {
        listed_folders.insert(folder_of(p));
    }
    std::vector<const SavedList*> saved;
    saved.reserve(input.saved.size());
    for (const SavedList& s : input.saved) {
        saved.push_back(&s);
    }
    std::sort(saved.begin(), saved.end(), [](const SavedList* a, const SavedList* b) { return a->sequence < b->sequence; });
    std::vector<std::set<std::string>> saved_paths;
    std::vector<std::set<std::string>> saved_folders;
    for (const SavedList* s : saved) {
        saved_paths.emplace_back(s->paths.begin(), s->paths.end());
        std::set<std::string> folders;
        for (const std::string& p : s->paths) {
            folders.insert(folder_of(p));
        }
        saved_folders.push_back(std::move(folders));
    }

    std::vector<CleanupRow> rows;
    for (const R2Object& o : input.objects) {
        if (in_use.contains(o.key)) {
            continue;
        }
        const CdnPath p = describe_cdn_path(o.key);
        if (!p.listable && !p.source) {
            continue;  // manifest.json, files put there by hand, unknown folders: never offered
        }
        CleanupRow row;
        row.path = o.key;
        row.size = o.size;
        if (p.runtime == kAppFolder) {
            row.title = "FloServer " + p.version;
        } else if (p.source) {
            row.title = runtime_title(p.runtime, p.version, "") + " source";
        } else {
            row.title = file_title(p.runtime, p.version, p.variant, p.platform);
        }
        const std::string folder = folder_of(o.key);
        if (p.source && listed_folders.contains(folder)) {
            row.kind = CleanupKind::Kept;
            row.note = "kept: " + runtime_title(p.runtime, p.version, "") + " is listed (its source goes with it)";
            rows.push_back(std::move(row));
            continue;
        }
        // The last saved list that had it (a source: a binary of its version).
        std::optional<std::size_t> last;
        for (std::size_t i = 0; i < saved.size(); ++i) {
            if (p.source ? saved_folders[i].contains(folder) : saved_paths[i].contains(o.key)) {
                last = i;
            }
        }
        if (!last) {
            row.kind = CleanupKind::Unknown;
            row.note = "in no list saved on this PC";
        } else if (*last + 1 == saved.size()) {
            // In the newest list signed here, though not live nor next.
            row.kind = CleanupKind::Wait;
            row.note = "wait: in the newest list signed on this PC";
        } else {
            row.left = saved[*last + 1]->made;
            const bool long_ago = input.now - *row.left >= input.wait;
            row.kind = long_ago ? CleanupKind::Ready : CleanupKind::Wait;
            if (!long_ago) {
                row.note = "wait: left less than " + std::to_string(input.wait.count()) + " days ago";
            }
        }
        row.ticked = row.kind == CleanupKind::Ready;
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const CleanupRow& a, const CleanupRow& b) {
        if (order_of(a.kind) != order_of(b.kind)) {
            return order_of(a.kind) < order_of(b.kind);
        }
        return a.path < b.path;
    });
    return rows;
}

std::string left_text(const CleanupRow& row, std::chrono::sys_seconds now) {
    if (row.kind == CleanupKind::Kept) {
        return "—";
    }
    if (!row.left) {
        return "unknown";
    }
    const auto days = std::chrono::floor<std::chrono::days>(now - *row.left).count();
    if (days <= 0) {
        return "today";
    }
    return days == 1 ? "1 day ago" : std::to_string(days) + " days ago";
}

std::string purge_deleted(core::HttpTransport& http, const PurgeToken& token, const std::vector<std::string>& paths,
                          std::string_view cdn, const std::atomic<bool>* cancel) {
    constexpr std::size_t kBatch = 30;  // Cloudflare's free plan: 30 files a purge
    std::string base(cdn);
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    std::string problems;
    for (std::size_t i = 0; i < paths.size(); i += kBatch) {
        std::vector<std::string> urls;
        for (std::size_t j = i; j < std::min(paths.size(), i + kBatch); ++j) {
            urls.push_back(base + "/" + paths[j]);
        }
        if (const std::string p = purge_cache(http, token, urls, cancel); !p.empty()) {
            problems += (problems.empty() ? "" : "; ") + p;
        }
    }
    return problems;
}

std::string cleanup_record(const std::vector<CleanupDone>& done, std::chrono::sys_seconds when,
                           std::string_view purge_note) {
    std::uint64_t freed = 0;
    std::size_t deleted = 0;
    std::string lines;
    for (const CleanupDone& d : done) {
        if (d.problem.empty()) {
            ++deleted;
            freed += d.size;
            lines += "deleted  " + d.path + "  (" + std::to_string(d.size) + " bytes)\r\n";
        } else {
            lines += "FAILED   " + d.path + ": " + d.problem + "\r\n";
        }
    }
    std::string text = "FloServer Publisher clean-up of the R2 bucket, " + format_utc_time(when) + "\r\n";
    text += std::to_string(deleted) + " of " + std::to_string(done.size()) + " files deleted, " +
            std::to_string(freed) + " bytes freed.\r\n";
    text += purge_note.empty() ? std::string("Cloudflare's cache: purged (or no purge token saved).\r\n")
                               : "Cloudflare's cache: " + std::string(purge_note) + "\r\n";
    text += "\r\n" + lines;
    return text;
}

}  // namespace floserver::manifest_tool

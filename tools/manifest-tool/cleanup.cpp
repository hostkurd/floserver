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

// Every address of `entry` under `cdn` into `out`; false when it has none.
bool add_cdn_urls(const json& entry, std::string_view cdn, std::vector<std::string>& out) {
    if (!entry.is_object()) {
        return false;
    }
    const auto urls = entry.find("urls");
    if (urls == entry.end() || !urls->is_array()) {
        return false;
    }
    bool any = false;
    for (const json& u : *urls) {
        if (u.is_string()) {
            if (auto p = path_under(u.get<std::string>(), cdn)) {
                out.push_back(std::move(*p));
                any = true;
            }
        }
    }
    return any;
}

bool add_cdn_urls(const std::vector<std::string>& urls, std::string_view cdn, std::vector<std::string>& out) {
    bool any = false;
    for (const std::string& u : urls) {
        if (auto p = path_under(u, cdn)) {
            out.push_back(std::move(*p));
            any = true;
        }
    }
    return any;
}

// "2026-11-01T10:00:00.000Z" (an S3 LastModified) -> its time.
std::optional<std::chrono::sys_seconds> s3_time(std::string_view text) {
    if (text.size() < 19) {
        return std::nullopt;
    }
    return core::parse_utc_time(std::string(text.substr(0, 19)) + "Z");
}

std::optional<std::string> read_small(const std::filesystem::path& p, std::size_t max) {
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
}

struct SavedFolder {
    std::filesystem::path path;
    SavedList list;
};

// The signed lists saved under `folder`, with where each is.
std::vector<SavedFolder> saved_folders(const std::filesystem::path& folder, std::string_view cdn) {
    namespace fs = std::filesystem;
    std::vector<SavedFolder> out;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = core::path_to_utf8(it->path().filename());
        // "2026-11-25 sequence 12" or "... (2)"
        std::error_code dir_ec;
        if (name.size() < 21 || name.substr(10, 10) != " sequence " || !it->is_directory(dir_ec)) {
            continue;
        }
        // Only a list that was signed could have gone live.
        std::error_code sig_ec;
        if (!fs::is_regular_file(it->path() / "manifest.json.sig", sig_ec)) {
            continue;
        }
        const auto manifest = read_small(it->path() / "manifest.json", std::size_t{1} << 20);
        if (!manifest) {
            continue;
        }
        const json doc = json::parse(*manifest, nullptr, false);
        auto paths = list_paths(*manifest, cdn);
        const auto sequence = doc.is_object() ? doc.find("sequence") : doc.end();
        if (!paths || sequence == doc.end() || !sequence->is_number_integer()) {
            continue;
        }
        SavedFolder f;
        f.path = it->path();
        f.list.sequence = sequence->get<std::int64_t>();
        f.list.paths = std::move(*paths);
        std::optional<std::chrono::sys_seconds> made;
        if (const auto steps = read_small(it->path() / "upload-steps.txt", std::size_t{64} * 1024)) {
            // "FloServer runtime list, sequence 12, made 2026-11-25T10:00:00Z"
            if (const auto at = steps->find(", made "); at != std::string::npos) {
                made = core::parse_utc_time(steps->substr(at + 7, 20));
            }
        }
        if (!made) {
            // The folder's date: the end of that day, so never too early.
            made = core::parse_utc_time(name.substr(0, 10) + "T23:59:59Z");
        }
        if (!made) {
            continue;
        }
        f.list.made = *made;
        if (const auto live = read_small(it->path() / "live-at.txt", 1024); live && live->starts_with("live at ")) {
            f.list.live_at = core::parse_utc_time(live->substr(8, 20));
        }
        out.push_back(std::move(f));
    }
    return out;
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
        if (!add_cdn_urls(r, cdn, out)) {
            return std::nullopt;
        }
    }
    if (const auto app = doc.find("app"); app != doc.end() && !app->is_null() && !add_cdn_urls(*app, cdn, out)) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::vector<std::string>> list_paths(const LiveList& live, std::string_view cdn) {
    std::vector<std::string> out;
    for (const core::RuntimeFile& r : live.runtimes) {
        if (!add_cdn_urls(r.urls, cdn, out)) {
            return std::nullopt;
        }
    }
    if (live.app && !add_cdn_urls(live.app->urls, cdn, out)) {
        return std::nullopt;
    }
    return out;
}

std::vector<SavedList> read_saved_lists(const std::filesystem::path& folder, std::string_view cdn) {
    std::vector<SavedList> out;
    for (SavedFolder& f : saved_folders(folder, cdn)) {
        out.push_back(std::move(f.list));
    }
    return out;
}

bool mark_live(const std::filesystem::path& list_folder, std::chrono::sys_seconds when) {
    std::error_code ec;
    if (std::filesystem::exists(list_folder / "live-at.txt", ec)) {
        return true;
    }
    std::ofstream f(list_folder / "live-at.txt", std::ios::binary | std::ios::trunc);
    f << "live at " << format_utc_time(when) << "\r\n";
    f.close();
    return static_cast<bool>(f);
}

void mark_live_lists(const std::filesystem::path& folder, const LiveList& live, std::string_view cdn,
                     std::chrono::sys_seconds when) {
    const auto live_paths = list_paths(live, cdn);
    if (!live_paths) {
        return;
    }
    const std::set<std::string> want(live_paths->begin(), live_paths->end());
    for (const SavedFolder& f : saved_folders(folder, cdn)) {
        if (f.list.sequence == live.sequence && std::set<std::string>(f.list.paths.begin(), f.list.paths.end()) == want) {
            (void)mark_live(f.path, when);
        }
    }
}

std::vector<CleanupRow> plan_cleanup(const CleanupInput& input) {
    using std::chrono::sys_seconds;
    // Without the live list, what is in use can't be told.
    if (input.live_sequence <= 0 || input.live_paths.empty()) {
        return {};
    }
    std::set<std::string> in_use(input.live_paths.begin(), input.live_paths.end());
    in_use.insert(input.next_paths.begin(), input.next_paths.end());

    // GPL: a source archive stays while a binary of its runtime and version
    // stays on the CDN: one listed, or one Clean up never offers (another
    // build, a file put there by hand).
    std::set<std::string> staying_folders;
    for (const std::string& p : in_use) {
        staying_folders.insert(folder_of(p));
    }
    // When the live list went up: the later of the pair.
    std::optional<sys_seconds> live_since;
    for (const R2Object& o : input.objects) {
        if (o.key == "manifest.json" || o.key == "manifest.json.sig") {
            if (const auto t = s3_time(o.last_modified); t && (!live_since || *t > *live_since)) {
                live_since = t;
            }
            continue;
        }
        const CdnPath p = describe_cdn_path(o.key);
        if (!p.source && (in_use.contains(o.key) || !p.listable)) {
            staying_folders.insert(folder_of(o.key));
        }
    }

    // The saved lists that could have been live, one per sequence (an
    // abandoned list shares its sequence with the one that went up).
    struct Group {
        std::int64_t sequence = 0;
        sys_seconds made;
        std::optional<sys_seconds> known_live;  // no earlier than it went live
        std::set<std::string> paths;
        std::set<std::string> folders;
    };
    std::map<std::int64_t, Group> by_sequence;
    for (const SavedList& s : input.saved) {
        if (s.sequence > input.live_sequence) {
            continue;  // never went live
        }
        Group& g = by_sequence[s.sequence];
        g.sequence = s.sequence;
        g.made = std::max(g.made, s.made);
        if (s.live_at && (!g.known_live || *s.live_at > *g.known_live)) {
            g.known_live = s.live_at;
        }
        for (const std::string& p : s.paths) {
            g.paths.insert(p);
            g.folders.insert(folder_of(p));
        }
    }
    std::vector<Group> groups;
    groups.reserve(by_sequence.size());
    for (auto& [seq, g] : by_sequence) {
        groups.push_back(std::move(g));
    }
    // Lists go live in sequence order, so the time a later list was seen
    // live bounds when an earlier one went live: walk back taking the least.
    std::vector<std::optional<sys_seconds>> went_live_by(groups.size());
    std::optional<sys_seconds> bound = live_since;  // the live list, saved here or not
    for (std::size_t i = groups.size(); i-- > 0;) {
        if (groups[i].known_live && (!bound || *groups[i].known_live < *bound)) {
            bound = groups[i].known_live;
        }
        went_live_by[i] = bound;
    }
    // Whether every sequence from groups[i] to the live one is saved here:
    // a list made elsewhere in between may have put a file back.
    std::vector<bool> no_gap_after(groups.size(), false);
    for (std::size_t i = groups.size(); i-- > 0;) {
        no_gap_after[i] = i + 1 == groups.size() ? groups[i].sequence == input.live_sequence
                                                  : no_gap_after[i + 1] && groups[i + 1].sequence == groups[i].sequence + 1;
    }

    std::vector<CleanupRow> rows;
    for (const R2Object& o : input.objects) {
        if (in_use.contains(o.key)) {
            continue;
        }
        const CdnPath p = describe_cdn_path(o.key);
        if (!may_delete(o.key)) {
            continue;  // manifest.json, files put there by hand, unknown folders: never offered
        }
        CleanupRow row;
        row.path = o.key;
        row.size = o.size;
        row.source = p.source;
        if (p.runtime == kAppFolder) {
            row.title = "FloServer " + p.version;
        } else if (p.source) {
            row.title = runtime_title(p.runtime, p.version, "") + " source";
        } else {
            row.title = file_title(p.runtime, p.version, p.variant, p.platform);
        }
        const std::string folder = folder_of(o.key);
        if (p.source && staying_folders.contains(folder)) {
            row.kind = CleanupKind::Kept;
            row.note = "kept: a " + runtime_title(p.runtime, p.version, "") + " binary stays (its source goes with it)";
            rows.push_back(std::move(row));
            continue;
        }
        // The last saved list that had it (a source: a binary of its version).
        std::optional<std::size_t> last;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            if (p.source ? groups[i].folders.contains(folder) : groups[i].paths.contains(o.key)) {
                last = i;
            }
        }
        if (!last) {
            row.kind = CleanupKind::Unknown;
            row.note = "in no list saved on this PC";
        } else if (*last + 1 == groups.size() && groups[*last].sequence == input.live_sequence) {
            // In the live sequence's list saved here (an abandoned twin), though not live nor next.
            row.kind = CleanupKind::Wait;
            row.note = "wait: in the newest list signed on this PC";
        } else if (*last + 1 == groups.size() || !no_gap_after[*last + 1] ? !live_since : !went_live_by[*last + 1]) {
            row.kind = CleanupKind::Unknown;
            row.note = "when the list without it went live isn't known";
        } else {
            // The next list saved here, or else the live one (made elsewhere).
            // With a list missing after that one, only the live list's
            // upload is sure to be after it left (the live list lacks it).
            sys_seconds left = *last + 1 == groups.size() || !no_gap_after[*last + 1]
                                   ? *live_since
                                   : std::max(groups[*last + 1].made, *went_live_by[*last + 1]);
            if (*last + 1 < groups.size()) {
                left = std::max(left, groups[*last + 1].made);
            }
            // Uploaded again since: count from then.
            if (const auto up = s3_time(o.last_modified); up && *up > left) {
                left = *up;
            }
            row.left = left;
            const bool long_ago = input.now - left >= input.wait;
            row.kind = long_ago ? CleanupKind::Ready : CleanupKind::Wait;
            if (!long_ago) {
                row.note = "wait: left less than " + std::to_string(input.wait.count()) + " days ago";
            }
        }
        row.ticked = row.kind == CleanupKind::Ready;
        rows.push_back(std::move(row));
    }
    tie_sources(rows);
    std::stable_sort(rows.begin(), rows.end(), [](const CleanupRow& a, const CleanupRow& b) {
        if (order_of(a.kind) != order_of(b.kind)) {
            return order_of(a.kind) < order_of(b.kind);
        }
        return a.path < b.path;
    });
    return rows;
}

void tie_sources(std::vector<CleanupRow>& rows) {
    std::set<std::string> binary_stays;
    for (const CleanupRow& r : rows) {
        if (!r.source && !r.ticked) {
            binary_stays.insert(folder_of(r.path));
        }
    }
    for (CleanupRow& r : rows) {
        if (r.source && r.ticked && binary_stays.contains(folder_of(r.path))) {
            r.ticked = false;
            r.note = "goes only with every binary in its folder";
        }
    }
}

bool may_delete(std::string_view key) {
    if (key.empty() || key.starts_with('/') || key.starts_with("manifest.json")) {
        return false;
    }
    const CdnPath p = describe_cdn_path(key);
    return p.listable || p.source;
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

#pragma once

// D34: what's new upstream for the runtimes FloServer offers. Each runtime's
// official site is asked for the versions it offers now (a "finder"), and the
// answer is compared with the list in the Publisher: a newer patch of a
// series it lists is an Update, a series it doesn't list is a New series.
// The parsers are pure functions, tested with copies of each site's answer;
// find_updates() does the asking.

#include "floserver/core/http.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace floserver::manifest_tool {

// One series as a site offers it now: its newest version.
struct UpstreamVersion {
    std::string series;   // "8.5", "24", "18", Bun's "1.3"; empty for a runtime with one series (Mailpit...)
    std::string version;  // "8.5.13"
    int build = 0;        // FloServer's own builds (D36.3): the release's build number
    bool operator==(const UpstreamVersion&) const = default;
};

// A runtime's site's answer: its series, or why it couldn't be read.
struct FinderAnswer {
    std::vector<UpstreamVersion> versions;
    std::string problem;  // empty when read
    std::string source;   // the address asked (shown when it fails)
};

// The series a runtime's version belongs to, as D34 compares them:
// Node and PostgreSQL "24", "18"; Deno, Mailpit, Garnet, phpMyAdmin and
// phpredis "" (one series); the others "8.5", "11.4", "1.30", Bun "1.3".
[[nodiscard]] std::string update_series(std::string_view runtime, std::string_view version);

// --- parsers: a site's answer -> the newest version of each series --------

// PHP for Windows' sha256sum.txt: its current builds ("php-8.5.13-nts-Win32-vs17-x64.zip").
[[nodiscard]] std::vector<UpstreamVersion> parse_php_sums(std::string_view text);
// nodejs.org/dist/index.json: LTS releases only (an even major once it's LTS).
[[nodiscard]] std::vector<UpstreamVersion> parse_node_index(std::string_view json);
// MariaDB's REST API (rest-api/mariadb/): its Stable, Long Term Support series.
[[nodiscard]] std::vector<std::string> parse_mariadb_majors(std::string_view json);
// rest-api/mariadb/<series>/latest/: the newest release of that series.
[[nodiscard]] std::optional<std::string> parse_mariadb_latest(std::string_view json);
// getcomposer.org/versions: the stable 2.x releases (its "stable" entries).
[[nodiscard]] std::vector<UpstreamVersion> parse_composer_versions(std::string_view json);
// nginx.org/en/download.html: Windows zips of the stable branches (even minor).
[[nodiscard]] std::vector<UpstreamVersion> parse_nginx_page(std::string_view html);
// postgresql.org/versions.json: the supported majors' latest versions.
[[nodiscard]] std::vector<UpstreamVersion> parse_postgres_versions(std::string_view json);
// api.wordpress.org/core/stable-check/1.0/: "latest" and "outdated" versions (never "insecure").
[[nodiscard]] std::vector<UpstreamVersion> parse_wordpress_stable_check(std::string_view json);
// phpmyadmin.net/home_page/version.json: the newest version.
[[nodiscard]] std::optional<std::string> parse_phpmyadmin_version(std::string_view json);
// A GitHub "latest release" (api.github.com/repos/<o>/<r>/releases/latest):
// its tag without `tag_prefix` ("v", "bun-v"); nothing for a pre-release or draft.
[[nodiscard]] std::optional<std::string> parse_github_latest(std::string_view json, std::string_view tag_prefix);
// python-build-standalone's latest release: the CPython versions of its
// Windows x64 install_only_stripped builds, by series.
[[nodiscard]] std::vector<UpstreamVersion> parse_python_release(std::string_view json);
// apachelounge.com/download/: the Win64 httpd zips ("httpd-2.4.69-261002-Win64-VS18.zip").
[[nodiscard]] std::vector<UpstreamVersion> parse_apachelounge_page(std::string_view html);
// .NET's release-metadata/<channel>/releases.json: its "latest-runtime".
[[nodiscard]] std::optional<std::string> parse_dotnet_releases(std::string_view json);
// dev.mysql.com's 8.4 download page: the newest 8.4 version it names.
[[nodiscard]] std::optional<std::string> parse_mysql_page(std::string_view html);
// pecl.php.net/rest/r/redis/stable.txt: the version alone.
[[nodiscard]] std::optional<std::string> parse_pecl_stable(std::string_view text);

// hostkurd/floserver-runtimes' releases (api.github.com/.../releases), D36.3:
// the versions of `runtime` with a build for `platform` (a release
// "<runtime>-<v>-<build>" holding "<runtime>-<v>-<platform>.tar.gz"), by series.
[[nodiscard]] std::vector<UpstreamVersion> parse_our_releases(std::string_view json, std::string_view runtime,
                                                              std::string_view platform);
struct OurRelease {
    std::string tag;   // "php-8.5.12-2"
    std::string url;   // the asset's download address
    std::string file;  // its CDN name: "php-8.5.12-2-linux-x64.tar.gz"
    int build = 0;
};
// The highest build of `runtime` `version` for `platform` in those releases.
[[nodiscard]] std::optional<OurRelease> find_our_release(std::string_view json, std::string_view runtime,
                                                         std::string_view version, std::string_view platform);

// --- the list, per runtime and platform (D36.3) -------------------------------

// A Windows or platform-free file's key is its runtime ("php"); another
// platform's is "<runtime>@<platform>" ("php@linux-x64"): its own rows,
// finder and files, so Linux's PHP can trail Windows' and the reverse.
[[nodiscard]] std::string list_key(std::string_view runtime, std::string_view platform);
struct ListKey {
    std::string runtime;   // "php"
    std::string platform;  // "" for Windows and platform-free files
};
[[nodiscard]] ListKey split_list_key(std::string_view key);
// The platforms other than Windows FloServer lists runtimes for, and which
// runtimes (FloServer's own builds so far).
inline constexpr std::array<std::string_view, 1> kOtherPlatforms = {"linux-x64"};
[[nodiscard]] bool listed_on(std::string_view runtime, std::string_view platform);
// key -> versions for the list's CDN paths (<runtime>/<version>/<file>). A
// runtime listed for Windows that FloServer also offers on another platform
// gets that key too, empty until its first version, so it shows as a new
// series there.
[[nodiscard]] std::map<std::string, std::vector<std::string>> listed_from_paths(const std::vector<std::string>& paths);
// key -> version -> build, for FloServer's own builds in the list.
using ListedBuilds = std::map<std::string, std::map<std::string, int>>;
[[nodiscard]] ListedBuilds listed_builds_from_paths(const std::vector<std::string>& paths);

// --- what to show -------------------------------------------------------------

enum class UpdateKind : std::uint8_t {
    UpToDate,   // the list has the newest
    Update,     // a newer patch of a series in the list (ticked)
    NewSeries,  // a series the list doesn't have (not ticked: your choice)
    Failed,     // the site couldn't be read
};

struct UpdateRow {
    std::string runtime;  // "postgresql"
    std::string series;   // "17"
    std::string title;    // "PostgreSQL 17"
    std::string listed;   // the newest version of it in the list ("" for a new series)
    std::string latest;   // the site's newest ("" when Failed)
    UpdateKind kind = UpdateKind::UpToDate;
    std::string detail;   // why it Failed
    bool ticked = false;
};

// The rows for `listed` (runtime -> versions in the Publisher's list) and
// the sites' answers (runtime -> answer). A new series is shown only when
// it's one FloServer would offer (update_series_wanted()). Sorted as the
// Publisher lists runtimes, then newest series first.
// A newer build of a listed version of FloServer's own builds (`builds`) is
// an Update too.
[[nodiscard]] std::vector<UpdateRow> compare_updates(const std::map<std::string, std::vector<std::string>>& listed,
                                                     const std::map<std::string, FinderAnswer>& found,
                                                     const ListedBuilds& builds = {});

// Whether FloServer would offer `series` of `runtime` when the list doesn't
// have it yet: PHP from 8.2, MariaDB's long-term series from 11.4, MySQL
// 8.4 only, PostgreSQL from 17, Node from 22 (LTS), Python from 3.12, and
// for the others a series newer than the newest listed.
[[nodiscard]] bool update_series_wanted(std::string_view runtime, std::string_view series,
                                        const std::vector<std::string>& listed_series);

// How many runtimes find_updates() asks about for `listed` (for a progress bar).
[[nodiscard]] std::size_t runtimes_to_ask(const std::map<std::string, std::vector<std::string>>& listed);

// Asks every runtime's site (the runtimes in `listed`). `progress` is told
// each runtime as it starts. `save_dir`, when set, keeps each site's answer
// there (to send when a parser needs fixing).
[[nodiscard]] std::map<std::string, FinderAnswer> find_updates(
    core::HttpTransport& http, const std::map<std::string, std::vector<std::string>>& listed,
    const std::atomic<bool>* cancel = nullptr, const std::function<void(std::string_view runtime)>& progress = {},
    const std::string& save_dir = {});

}  // namespace floserver::manifest_tool

// D34: the Publisher's finders read each runtime's site; copies of each
// site's answer (as of Nov 2026) stand in for the sites here.

#include "test_util.h"
#include "updates.h"

#include "floserver/core/paths.h"

#include <doctest/doctest.h>

using namespace floserver::manifest_tool;
using test_util::FakeTransport;

namespace {

using V = std::vector<UpstreamVersion>;

}  // namespace

TEST_CASE("updates: each site's answer read as the newest version of each series") {
    CHECK(update_series("php", "8.5.13") == "8.5");
    CHECK(update_series("node", "24.22.1") == "24");
    CHECK(update_series("postgresql", "17.12") == "17");
    CHECK(update_series("bun", "1.3.14").empty());
    CHECK(update_series("dotnet", "10.0.14") == "10.0");

    CHECK(parse_php_sums("aa *php-8.5.13-1-nts-Win32-vs17-x64.zip\n"
                         "bb *php-8.5.13-Win32-vs17-x64.zip\n"
                         "cc *php-8.4.16-nts-Win32-vs17-x64.zip\n"
                         "dd *php-8.2.30-nts-Win32-vs16-x64.zip\n"
                         "ee *php-8.5.13-nts-Win32-vs17-x86.zip\n"
                         "ff *php-debug-pack-8.5.13-nts-Win32-vs17-x64.zip\n") ==
          V{{"8.2", "8.2.30"}, {"8.4", "8.4.16"}, {"8.5", "8.5.13"}});

    CHECK(parse_node_index(R"([{"version":"v25.2.1","files":["win-x64-zip"],"lts":false},
                               {"version":"v24.23.0","files":["linux-x64","win-x64-zip"],"lts":"Krypton"},
                               {"version":"v24.22.1","files":["win-x64-zip"],"lts":"Krypton"},
                               {"version":"v22.23.5","files":["win-x64-zip"],"lts":"Jod"}])") ==
          V{{"22", "22.23.5"}, {"24", "24.23.0"}});

    CHECK(parse_mariadb_majors(R"({"major_releases":[
        {"release_id":"12.1","release_status":"Stable","release_support_type":"Short Term Support"},
        {"release_id":"11.8","release_status":"Stable","release_support_type":"Long Term Support"},
        {"release_id":"12.2","release_status":"RC","release_support_type":"Long Term Support"},
        {"release_id":"11.4","release_status":"Stable","release_support_type":"Long Term Support"}]})") ==
          std::vector<std::string>{"11.8", "11.4"});
    CHECK(parse_mariadb_latest(R"({"releases":{"11.4.13":{"release_id":"11.4.13"},"11.4.9":{}}})") == "11.4.13");

    CHECK(parse_composer_versions(R"({"stable":[{"path":"/download/2.11.1/composer.phar","version":"2.11.1"}],
                                      "preview":[{"version":"2.12.0-RC1"}],
                                      "2.2":[{"version":"2.2.26"}],"1":[{"version":"1.10.27"}]})") ==
          V{{"2.11", "2.11.1"}, {"2.2", "2.2.26"}});

    CHECK(parse_nginx_page(R"(<a href="/download/nginx-1.31.2.zip">nginx/Windows-1.31.2</a>
                              <a href="/download/nginx-1.30.7.zip">nginx/Windows-1.30.7</a>
                              <a href="/download/nginx-1.28.3.zip">nginx/Windows-1.28.3</a>)") ==
          V{{"1.28", "1.28.3"}, {"1.30", "1.30.7"}});

    CHECK(parse_postgres_versions(R"([{"major":"19","latestMinor":"0","supported":false},
                                      {"major":"18","latestMinor":"7","supported":true},
                                      {"major":"17","latestMinor":12,"supported":true},
                                      {"major":"13","latestMinor":"23","supported":false}])") ==
          V{{"18", "18.7"}, {"17", "17.12"}});

    CHECK(parse_wordpress_stable_check(R"({"6.9.4":"insecure","7.0.8":"outdated","7.0.9":"outdated","7.1.5":"latest"})") ==
          V{{"7.0", "7.0.9"}, {"7.1", "7.1.5"}});

    CHECK(parse_phpmyadmin_version(R"({"version":"5.2.3","date":"2025-10-08","releases":[]})") == "5.2.3");

    CHECK(parse_github_latest(R"({"tag_name":"bun-v1.3.15","prerelease":false,"draft":false})", "bun-v") == "1.3.15");
    CHECK_FALSE(parse_github_latest(R"({"tag_name":"v2.10.0-rc.1","prerelease":true})", "v"));
    CHECK_FALSE(parse_github_latest(R"({"tag_name":"canary"})", "v"));

    CHECK(parse_python_release(R"({"tag_name":"20261118","assets":[
        {"name":"cpython-3.13.19+20261118-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"},
        {"name":"cpython-3.13.19+20261118-x86_64-pc-windows-msvc-install_only.tar.gz"},
        {"name":"cpython-3.12.17+20261118-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"},
        {"name":"cpython-3.14.2+20261118-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"},
        {"name":"cpython-3.14.2+20261118-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"}]})") ==
          V{{"3.12", "3.12.17"}, {"3.13", "3.13.19"}, {"3.14", "3.14.2"}});

    CHECK(parse_apachelounge_page(R"(<a href="/download/VS18/binaries/httpd-2.4.70-261115-Win64-VS18.zip">
                                      httpd-2.4.63-250207-win64-VS17.zip)") == V{{"2.4", "2.4.70"}});
    CHECK(parse_apachelounge_page("httpd-2.4.63-250207-win64-VS17.zip") == V{{"2.4", "2.4.63"}});
    CHECK(parse_dotnet_releases(R"({"channel-version":"10.0","latest-release":"10.0.15","latest-runtime":"10.0.15"})") ==
          "10.0.15");
    CHECK(parse_mysql_page(R"(<a>mysql-8.4.14-winx64.zip</a> 10.18.4.200 <a>mysql-8.4.13-winx64.zip</a> 9.7.4)") ==
          "8.4.14");
    CHECK_FALSE(parse_mysql_page("MySQL 8.4.99 is coming"));
    CHECK(parse_pecl_stable("6.3.1\n") == "6.3.1");

    // Something that isn't the expected answer: nothing, never a guess.
    CHECK(parse_node_index("<html>").empty());
    CHECK(parse_postgres_versions("{}").empty());
    CHECK_FALSE(parse_phpmyadmin_version("not json"));
}

TEST_CASE("updates: compared with the list, as Update, New series, Up to date or Failed") {
    const std::map<std::string, std::vector<std::string>> listed = {
        {"postgresql", {"18.7"}},
        {"php", {"8.5.12", "8.4.27"}},
        {"node", {"24.22.1"}},
        {"bun", {"1.3.14", "1.3.14"}},  // the regular and baseline builds
        {"nginx", {"1.30.6"}},
        {"app", {"0.1.0"}},             // FloServer's own installer: never checked
    };
    std::map<std::string, FinderAnswer> found;
    found["postgresql"].versions = {{"18", "18.7"}, {"17", "17.12"}, {"16", "16.16"}};
    found["php"].versions = {{"8.5", "8.5.13"}, {"8.4", "8.4.27"}, {"8.3", "8.3.30"}, {"8.1", "8.1.34"}};
    found["node"].problem = "https://nodejs.org/dist/index.json: HTTP 503";
    found["bun"].versions = {{"", "1.3.15"}};
    found["nginx"].versions = {{"1.30", "1.30.6"}, {"1.28", "1.28.3"}, {"1.32", "1.32.0"}};

    const auto rows = compare_updates(listed, found);
    const auto row = [&rows](const std::string& title) {
        for (const UpdateRow& r : rows) {
            if (r.title == title) {
                return r;
            }
        }
        FAIL("no row " << title);
        return UpdateRow{};
    };
    // PHP first (the Publisher's order), newest series first.
    REQUIRE(rows.size() == 9);  // PHP 3, PostgreSQL 2, nginx 2, Bun, Node.js
    CHECK(rows[0].title == "PHP 8.5");
    CHECK(rows[0].kind == UpdateKind::Update);
    CHECK(rows[0].listed == "8.5.12");
    CHECK(rows[0].latest == "8.5.13");
    CHECK(rows[0].ticked);
    CHECK(row("PHP 8.4").kind == UpdateKind::UpToDate);
    CHECK_FALSE(row("PHP 8.4").ticked);
    CHECK(row("PHP 8.3").kind == UpdateKind::NewSeries);
    CHECK_FALSE(row("PHP 8.3").ticked);  // a new series is the user's choice
    // PHP 8.1 is older than FloServer offers; PostgreSQL 16 too.
    CHECK(row("PostgreSQL 17").kind == UpdateKind::NewSeries);
    CHECK(row("PostgreSQL 17").latest == "17.12");
    CHECK(row("PostgreSQL 18").kind == UpdateKind::UpToDate);
    // nginx: a newer stable branch is offered, an older one isn't.
    CHECK(row("nginx 1.32").kind == UpdateKind::NewSeries);
    CHECK(row("Bun").kind == UpdateKind::Update);
    CHECK(row("Bun").listed == "1.3.14");
    CHECK(row("Node.js").kind == UpdateKind::Failed);
    CHECK(row("Node.js").detail.find("HTTP 503") != std::string::npos);

    // A site whose answer names none of the listed series: Failed, never up to date.
    {
        std::map<std::string, FinderAnswer> changed;
        changed["php"].versions = {{"8.1", "8.1.34"}};
        changed["php"].source = "https://downloads.php.net/~windows/releases/sha256sum.txt";
        const auto r = compare_updates({{"php", {"8.5.12", "8.4.27"}}}, changed);
        REQUIRE(r.size() == 1);
        CHECK(r[0].kind == UpdateKind::Failed);
        CHECK(r[0].detail.find("named none of the listed series") != std::string::npos);
    }
    // Composer names only its newest minor (and 2.2): a new minor is a new series, not a broken site.
    {
        std::map<std::string, FinderAnswer> composer;
        composer["composer"].versions = {{"2.12", "2.12.0"}, {"2.2", "2.2.26"}};
        const auto r = compare_updates({{"composer", {"2.11.0", "2.10.3"}}}, composer);
        REQUIRE_FALSE(r.empty());
        CHECK(r[0].title == "Composer 2.12");
        CHECK(r[0].kind == UpdateKind::NewSeries);
    }
    // phpredis: an update isn't ticked (its Windows builds can come later).
    {
        std::map<std::string, FinderAnswer> redis;
        redis["php-redis"].versions = {{"", "6.3.1"}};
        const auto r = compare_updates({{"php-redis", {"6.3.0", "6.3.0"}}}, redis);
        REQUIRE(r.size() == 1);
        CHECK(r[0].kind == UpdateKind::Update);
        CHECK_FALSE(r[0].ticked);
    }

    CHECK(update_series_wanted("mysql", "8.4", {}));
    CHECK_FALSE(update_series_wanted("mysql", "9.5", {}));
    CHECK(update_series_wanted("composer", "2.12", {"2.10", "2.11"}));
    CHECK_FALSE(update_series_wanted("composer", "2.2", {"2.10", "2.11"}));
}

TEST_CASE("updates: every site asked once, a failed one doesn't stop the others") {
    FakeTransport http;
    http.serve("https://www.postgresql.org/versions.json",
               {200, R"([{"major":"18","latestMinor":"7","supported":true},{"major":"17","latestMinor":"12","supported":true}])"});
    http.serve("https://downloads.php.net/~windows/releases/sha256sum.txt", {503, ""});
    http.serve("https://downloads.mariadb.org/rest-api/mariadb/",
               {200, R"({"major_releases":[{"release_id":"11.8","release_status":"Stable","release_support_type":"Long Term Support"},
                                            {"release_id":"11.4","release_status":"Stable","release_support_type":"Long Term Support"}]})"});
    http.serve("https://downloads.mariadb.org/rest-api/mariadb/11.8/latest/", {200, R"({"releases":{"11.8.10":{}}})"});
    http.serve("https://downloads.mariadb.org/rest-api/mariadb/11.4/latest/", {200, R"({"releases":{"11.4.14":{}}})"});
    http.serve("https://api.github.com/repos/axllent/mailpit/releases/latest", {200, "<html>not json</html>"});
    http.serve("https://api.github.com/repos/oven-sh/bun/releases/latest", {403, "{}"});
    http.serve("https://builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json",
               {200, R"({"latest-runtime":"10.0.15"})"});

    const std::map<std::string, std::vector<std::string>> listed = {
        {"postgresql", {"18.7"}}, {"php", {"8.5.12"}}, {"mariadb", {"11.4.13", "11.8.9"}}, {"mailpit", {"1.33.0"}},
        {"bun", {"1.3.14"}}, {"dotnet", {"10.0.14"}}};
    std::vector<std::string> asked;
    const test_util::TempDir saved;
    const auto found = find_updates(http, listed, nullptr, [&asked](std::string_view r) { asked.emplace_back(r); },
                                    floserver::core::path_to_utf8(saved.path()));
    CHECK(asked.size() == 6);
    CHECK(found.at("bun").problem.find("hourly limit") != std::string::npos);
    CHECK(found.at("dotnet").versions == V{{"10.0", "10.0.15"}});  // the listed channel's address
    CHECK(found.at("postgresql").problem.empty());
    CHECK(found.at("postgresql").versions == V{{"18", "18.7"}, {"17", "17.12"}});
    CHECK(found.at("php").problem.find("HTTP 503") != std::string::npos);
    CHECK(found.at("mariadb").versions == V{{"11.8", "11.8.10"}, {"11.4", "11.4.14"}});
    CHECK(found.at("mailpit").problem.find("no version could be read") != std::string::npos);
    // What each site answered is kept, to send when a reader needs fixing.
    CHECK(std::filesystem::exists(saved.path() / "www.postgresql.org_versions.json"));

    const auto rows = compare_updates(listed, found);
    std::size_t updates = 0;
    for (const UpdateRow& r : rows) {
        updates += r.kind == UpdateKind::Update ? 1U : 0U;
    }
    CHECK(updates == 3);  // MariaDB 11.8 and 11.4, .NET
}

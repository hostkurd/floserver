// Settings > Folders' and Settings > Shared sites' model
// (src/ui-win32/model/folders_page): the answers read, sizes, the words.

#include "model/folders_page.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

using namespace floserver::ui;
using nlohmann::json;

TEST_CASE("folders.get's answer, then a change and a clean-up merged into it") {
    CHECK_FALSE(folders_from_reply(json::object()));
    CHECK_FALSE(folders_from_reply(json::array()));
    auto info = folders_from_reply(json::parse(R"({
        "projects_dir": "C:/Users/me/FloServer/projects", "default_projects_dir": "C:/Users/me/FloServer/projects",
        "custom": false, "data_dir": "C:/Users/me/AppData/Local/FloServer",
        "data_bytes": 1610612736, "data_complete": true,
        "logs_bytes": 5242880, "leftovers_bytes": 0, "installing": true})"));
    REQUIRE(info);
    CHECK(info->projects_dir == "C:/Users/me/FloServer/projects");
    CHECK_FALSE(info->custom);
    CHECK(info->data_bytes == 1610612736U);
    CHECK(info->installing);
    CHECK(data_usage_text(*info, Lang::En) == "Uses 1.5 GB");

    // folders.set: the projects folder changes, the sizes stay.
    merge_folders_reply(*info, json::parse(R"({"projects_dir": "D:/work", "default_projects_dir": "C:/Users/me/FloServer/projects",
        "custom": true})"));
    CHECK(info->projects_dir == "D:/work");
    CHECK(info->custom);
    CHECK(info->data_bytes == 1610612736U);
    CHECK(info->logs_bytes == 5242880U);

    // folders.clean: what was freed comes off the data folder; the sizes are fresh.
    merge_folders_reply(*info, json::parse(R"({"what": "logs", "freed": 5242880, "done": 3, "skipped": 0,
        "logs_bytes": 0, "leftovers_bytes": 1024, "installing": false})"));
    CHECK(info->data_bytes == 1610612736U - 5242880U);
    CHECK(info->logs_bytes == 0);
    CHECK(info->leftovers_bytes == 1024);
    CHECK_FALSE(info->installing);
    CHECK(info->projects_dir == "D:/work");  // a clean-up doesn't say: kept
    merge_folders_reply(*info, json{{"freed", std::uint64_t{1} << 40U}});
    CHECK(info->data_bytes == 0);  // never below nothing

    info->data_complete = false;
    info->data_bytes = std::uint64_t{3} * 1024 * 1024;
    CHECK(data_usage_text(*info, Lang::En) == "Uses more than 3 MB (counting took too long to finish)");
}

TEST_CASE("sizes and what a clean-up button says") {
    CHECK(disk_size_text(0) == "0 KB");
    CHECK(disk_size_text(std::uint64_t{512} * 1024) == "512 KB");
    CHECK(disk_size_text(std::uint64_t{1024} * 1024 + std::uint64_t{512} * 1024) == "1.5 MB");
    CHECK(disk_size_text(std::uint64_t{1024} * 1024 * 1024) == "1 GB");
    CHECK(disk_size_text(std::uint64_t{23} * 1024 * 1024 * 1024) == "23 GB");

    CHECK(clean_size_text(2048, false, Lang::En) == "Frees 2 KB");
    CHECK(clean_size_text(2048, true, Lang::En) == "Frees 2 KB");
    CHECK(clean_size_text(0, false, Lang::En) == "Nothing to free");
    CHECK(clean_size_text(0, true, Lang::En) == "A download or new project is running: try again when it's done.");
    CHECK(clean_done_text(json{{"freed", std::uint64_t{3} * 1024 * 1024}, {"skipped", 0}}, Lang::En) == "Freed 3 MB.");
    CHECK(clean_done_text(json{{"freed", 1024}, {"skipped", 2}}, Lang::En) == "Freed 1 KB. 2 in use were left as they are.");
    // Values are isolated in right-to-left languages.
    const std::string lri{'\xE2', '\x80', '\xAA'};
    CHECK(clean_done_text(json{{"freed", 1024}, {"skipped", 0}}, Lang::Ar).find(lri + "1 KB") != std::string::npos);
}

TEST_CASE("Settings > Shared sites: hint, pickers and the www login") {
    const Installed installed = {{"php", {"8.5.12", "8.4.27"}}, {"mariadb", {"11.4.13"}}, {"apache", {"2.4.65"}}};
    const json servers = {{"servers", json::array({{{"series", "11.4"}, {"version", "11.4.13"}, {"port", 3307}}})}};

    // Off: what they do, the pickers, no login (it exists only while on).
    SharedSites s = shared_from_reply({{"on", false}, {"dir", "C:/www"}, {"web", "apache"}, {"database", "mariadb-11.4"},
                                       {"status", nullptr}});
    SharedPage page = shared_page(s, installed, servers, Lang::En);
    CHECK(page.hint == "Every folder in C:/www opens at localhost/<folder>/");
    CHECK_FALSE(page.problem);
    REQUIRE(page.fields.size() == 3);
    CHECK(page.fields[0].key == "php");
    CHECK(page.fields[1].key == "database");
    CHECK(page.fields[2].key == "web");
    CHECK_FALSE(page.login);

    // On and served: the address in the hint, the login with its port.
    s = shared_from_reply({{"on", true}, {"dir", "C:/www"}, {"web", "apache"}, {"database", "mariadb-11.4"},
                           {"status", {{"address", "http://localhost:8080"}, {"run", "running"}}}});
    page = shared_page(s, installed, servers, Lang::En);
    CHECK(page.hint == "Every folder in C:/www opens at localhost:8080/<folder>/");
    REQUIRE(page.login);
    CHECK(page.login_title == "Database www");
    CHECK(page.login_detail.find("127.0.0.1:3307") != std::string::npos);
    // No database server answering yet: the host alone.
    CHECK(shared_page(s, installed, json(), Lang::En).login_detail.starts_with("127.0.0.1 "));
    // On without a database: no login.
    s.database.clear();
    CHECK_FALSE(shared_page(s, installed, servers, Lang::En).login);

    // A problem: the service's words, marked as one.
    s = shared_from_reply({{"on", true}, {"status", {{"state", "problem"}, {"problem", "the www folder is gone"}}}});
    page = shared_page(s, installed, servers, Lang::En);
    CHECK(page.problem);
    CHECK(page.hint == "the www folder is gone");
    // Needs a download: what's missing.
    s = shared_from_reply({{"on", true}, {"status", {{"state", "needs_download"}, {"missing", {"nginx"}}}}});
    CHECK(shared_page(s, installed, servers, Lang::En).hint == "Needs nginx");
}

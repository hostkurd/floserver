// The first-run page's model (src/ui-win32/model/first_run.h): when it
// shows, what's offered and ticked, the rows while they download, the
// summary's words, and the done mark in window.json.

#include "model/first_run.h"
#include "model/general_page.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace floserver::ui;
using nlohmann::json;

namespace {

const json kReply = json::parse(R"({
    "installed": [],
    "list": {"sequence": 12, "expires": "2027-01-28T00:00:00Z"},
    "available": [
        {"name": "php", "version": "8.5.9", "variant": "nts", "size": 30000000},
        {"name": "php", "version": "8.5.12", "variant": "nts", "size": 31000000},
        {"name": "php", "version": "8.5.13", "variant": "ts", "size": 32000000},
        {"name": "php", "version": "8.4.30", "variant": "nts", "size": 29000000},
        {"name": "php", "version": "8.50.1", "variant": "", "size": 1},
        {"name": "mariadb", "version": "11.4.13", "variant": "", "size": 90000000},
        {"name": "mariadb", "version": "11.8.3", "variant": "", "size": 95000000},
        {"name": "node", "version": "24.22.1", "variant": "", "size": 30000000},
        {"name": "node", "version": "22.20.0", "variant": "", "size": 29000000},
        {"name": "composer", "version": "2.10.3", "variant": "", "size": 3000000},
        {"name": "composer", "version": "2.9.1", "variant": "", "size": 3000000},
        {"name": "python", "version": "3.13.9", "variant": "", "size": 25000000},
        {"name": "apache", "version": "2.4.65", "variant": "", "size": 12000000},
        {"name": "nginx", "version": "1.30.6", "variant": "", "size": 2000000},
        {"name": "nginx", "version": "1.28.0", "variant": "", "size": 2000000}
    ]})");

}  // namespace

TEST_CASE("first run: when the page shows") {
    const Installed none;
    CHECK(first_run_wanted(false, false, none, 0));
    CHECK(first_run_wanted(false, false, Installed{{"php", {}}}, 0));
    CHECK_FALSE(first_run_wanted(false, true, none, 0));                     // done in window.json
    CHECK_FALSE(first_run_wanted(false, false, none, 1));                    // a project
    CHECK_FALSE(first_run_wanted(false, false, Installed{{"nginx", {"1.30.6"}}}, 0));  // a runtime
    CHECK(first_run_wanted(true, true, Installed{{"nginx", {"1.30.6"}}}, 3));          // FLOSERVER_FIRST_RUN=1

    CHECK(first_run_forced("1"));
    CHECK_FALSE(first_run_forced(nullptr));
    CHECK_FALSE(first_run_forced(""));
    CHECK_FALSE(first_run_forced("0"));
    CHECK_FALSE(first_run_forced("yes"));
}

TEST_CASE("first run: the step strip") {
    CHECK(first_run_step_label(FirstRunStep::Language, FirstRunStep::Language, Lang::En) == "1 Language");
    CHECK(first_run_step_label(FirstRunStep::Runtimes, FirstRunStep::Language, Lang::En) == "2 Runtimes");
    CHECK(first_run_step_label(FirstRunStep::Ready, FirstRunStep::Language, Lang::En) == "3 Ready");
    CHECK(first_run_step_label(FirstRunStep::Language, FirstRunStep::Runtimes, Lang::En) == "\xE2\x9C\x93 Language");
    CHECK(first_run_step_label(FirstRunStep::Runtimes, FirstRunStep::Ready, Lang::En) == "\xE2\x9C\x93 Runtimes");
    CHECK(first_run_step_label(FirstRunStep::Ready, FirstRunStep::Ready, Lang::Ar) == "3 جاهز");
}

TEST_CASE("first run: the newest of each series is offered, in the approved order") {
    const std::vector<FirstRunOffer> offers = first_run_offers(kReply);
    // phpMyAdmin isn't in this list: its row is left out.
    REQUIRE(offers.size() == 7);
    CHECK(offers[0].name == "php");
    CHECK(offers[0].version == "8.5.12");  // not the ts build, not 8.50, not 8.4
    CHECK(offers[0].label == "PHP 8.5");
    CHECK(offers[0].size == 31000000U);
    CHECK(offers[0].ticked);
    CHECK(offers[1].version == "11.4.13");
    CHECK(offers[1].label == "MariaDB 11.4");
    CHECK(offers[2].version == "24.22.1");
    CHECK(offers[2].label == "Node.js 24");
    CHECK(offers[3].version == "2.10.3");
    CHECK(offers[3].label == "Composer 2");
    CHECK(offers[3].ticked);
    CHECK(offers[4].label == "Python 3.13");
    CHECK_FALSE(offers[4].ticked);
    CHECK(offers[5].label == "Apache 2.4");
    CHECK_FALSE(offers[5].ticked);
    CHECK(offers[6].name == "nginx");
    CHECK(offers[6].version == "1.30.6");
    CHECK(offers[6].label == "nginx 1.30");
    CHECK(offers[6].always);
    CHECK(offers[1].what == Str::FirstRunWhatMariadb);

    // No list (none fetched yet): nothing is offered.
    CHECK(first_run_offers(json::parse(R"({"list": null, "available": []})")).empty());
    CHECK(first_run_offers(json::array()).empty());
}

TEST_CASE("first run: the ticks, what's installed and the total") {
    const std::vector<FirstRunOffer> offers = first_run_offers(kReply);
    std::vector<bool> ticks = first_run_default_ticks(offers);
    CHECK(ticks == std::vector<bool>{true, true, true, true, false, false, true});
    CHECK(first_run_total(offers, ticks) == 31000000U + 90000000U + 30000000U + 3000000U + 2000000U);

    const std::vector<Download> wanted = first_run_downloads(offers, ticks);
    REQUIRE(wanted.size() == 5);
    CHECK(wanted[0] == Download{"nginx", "1.30.6"});  // first, always
    CHECK(wanted[1] == Download{"php", "8.5.12"});
    CHECK(wanted[4] == Download{"composer", "2.10.3"});

    // Nothing but nginx wanted: nginx is still installed; its box can't be unticked.
    const std::vector<bool> nothing(offers.size(), false);
    CHECK(first_run_downloads(offers, nothing) == std::vector<Download>{{"nginx", "1.30.6"}});
    CHECK(first_run_total(offers, nothing) == 2000000U);

    ticks[4] = true;  // Python
    CHECK(first_run_downloads(offers, ticks).size() == 6);
    CHECK(first_run_downloads(offers, ticks)[5] == Download{"python", "3.13.9"});
}

TEST_CASE("first run: the list's answers") {
    const auto offers = first_run_list_answer(kReply, {}, false);
    CHECK(offers.kind == FirstRunListAnswer::Kind::Offers);
    CHECK(offers.offers.size() == 7);

    // None on this computer yet: fetched once, then the fetch's reason is shown.
    const json none = json::parse(R"({"list": null, "list_error": "there's no runtime list on this computer yet"})");
    CHECK(first_run_list_answer(none, {}, false).kind == FirstRunListAnswer::Kind::Refresh);
    const json offline = json::parse(R"({"list": null, "list_error": "couldn't reach the mirror"})");
    const auto failed = first_run_list_answer(offline, {}, true);
    CHECK(failed.kind == FirstRunListAnswer::Kind::Failed);
    CHECK(failed.error == "couldn't reach the mirror");
    CHECK(first_run_list_error(failed.error, Lang::En) == "Couldn't get the list of runtimes: couldn't reach the mirror");

    const auto lost = first_run_list_answer(std::nullopt, "FloServer stopped answering", false);
    CHECK(lost.kind == FirstRunListAnswer::Kind::Failed);
    CHECK(first_run_list_error(lost.error, Lang::En) == "Couldn't get the list of runtimes: FloServer stopped answering");
    // Worded when shown, so a new language rewords it; the service's English keeps its direction.
    CHECK(first_run_list_error("gone", Lang::Ar).find("\xE2\x80\xAAgone\xE2\x80\xAC") != std::string::npos);

    const json empty = json::parse(R"({"list": {"sequence": 1}, "available": []})");
    const auto nothing = first_run_list_answer(empty, {}, false);
    CHECK(nothing.kind == FirstRunListAnswer::Kind::Failed);
    CHECK(nothing.error.empty());
    CHECK(first_run_list_error(nothing.error, Lang::En) == "The list has nothing for this computer.");
}

TEST_CASE("first run: the rows while they download") {
    const std::vector<Download> wanted = {{"nginx", "1.30.6"}, {"php", "8.5.12"}, {"mariadb", "11.4.13"},
                                          {"node", "24.22.1"}, {"composer", "2.10.3"}, {"python", "3.13.9"}};
    const Installed installed = {{"nginx", {"1.30.6"}}, {"php", {"8.4.1"}}};
    const std::vector<RuntimeJob> jobs = {{7, "php", "8.5.12", "downloading", 19000000, 31000000},
                                          {8, "mariadb", "11.4.13", "queued", 0, 0}};
    const std::map<std::string, FirstRunEnd> ends = {{"node", {"failed", "the file's checksum didn't match"}},
                                                    {"composer", {"cancelled", "cancelled"}}};
    const std::vector<FirstRunRow> rows = first_run_rows(wanted, installed, jobs, ends, Lang::En);
    REQUIRE(rows.size() == 6);
    CHECK(rows[0].label == "nginx 1.30.6");
    CHECK(rows[0].state == FirstRunRowState::Ready);
    CHECK(rows[0].line == "Ready");
    CHECK(rows[1].state == FirstRunRowState::Running);  // another PHP being installed isn't this one
    CHECK(rows[1].line == "Downloading… 18 MB of 30 MB");
    REQUIRE(rows[1].fraction);
    CHECK(*rows[1].fraction == doctest::Approx(19.0 / 31.0));
    CHECK(rows[1].job == 7);
    CHECK(rows[2].state == FirstRunRowState::Waiting);  // queued behind PHP: no Cancel
    CHECK(rows[2].job == 0);
    CHECK(rows[3].state == FirstRunRowState::Failed);
    CHECK(rows[3].line == "Failed: the file's checksum didn't match");
    CHECK(rows[4].state == FirstRunRowState::Cancelled);
    CHECK(rows[4].line == "Cancelled");
    CHECK(rows[5].state == FirstRunRowState::Waiting);  // its install not answered yet
    CHECK(rows[5].line == "Waiting");

    const FirstRunProgress p = first_run_progress(rows);
    CHECK(p.ready == 1);
    CHECK(p.total == 6);
    CHECK(p.failed == 2);
    CHECK(p.running());

    CHECK(first_run_runtimes_line(rows, Lang::En) ==
          "nginx 1.30.6 · still downloading: 1 of 6 ready · Node.js 24.22.1, Composer 2.10.3 didn't download; try "
          "again in Settings → Runtimes");
}

TEST_CASE("first run: the summary's words") {
    CHECK(first_run_runtimes_line({}, Lang::En) == "Nothing downloaded yet. Settings → Runtimes has every version.");
    const std::vector<Download> wanted = {{"nginx", "1.30.6"}, {"php", "8.5.12"}};
    const Installed all = {{"nginx", {"1.30.6"}}, {"php", {"8.5.12"}}};
    const std::vector<FirstRunRow> done = first_run_rows(wanted, all, {}, {}, Lang::En);
    CHECK_FALSE(first_run_progress(done).running());
    CHECK(first_run_runtimes_line(done, Lang::En) == "nginx 1.30.6, PHP 8.5.12");
    // Latin names isolated in Arabic.
    CHECK(first_run_runtimes_line(done, Lang::Ar) == "\xE2\x80\xAAnginx 1.30.6, PHP 8.5.12\xE2\x80\xAC");

    CHECK(first_run_https_line(FirstRunTrust::Trusted, {}, Lang::En) ==
          "Windows trusts FloServer's certificate. Projects open at https://name.localhost.");
    CHECK(first_run_https_line(FirstRunTrust::NotAsked, {}, Lang::En).starts_with("Not trusted"));
    CHECK(first_run_https_line(FirstRunTrust::Declined, {}, Lang::En).starts_with("You chose No"));
    CHECK(first_run_https_line(FirstRunTrust::Failed, "access denied", Lang::En) ==
          "The certificate wasn't added: access denied. Try again in Settings → Ports & HTTPS.");
    CHECK(first_run_https_line(FirstRunTrust::Asking, {}, Lang::En).starts_with("Windows is asking"));
    CHECK(first_run_https_line(FirstRunTrust::Failed, {}, Lang::En).starts_with("The certificate wasn't added: "));
    CHECK(first_run_https_line(FirstRunTrust::Failed, {}, Lang::En) != first_run_https_line(FirstRunTrust::Failed, {}, Lang::Ar));
}

TEST_CASE("first run: the worker's key") {
    CHECK(is_first_run_key(kFirstRunKey));
    CHECK_FALSE(is_first_run_key("firstrun"));
    CHECK_FALSE(is_first_run_key("_settings/php/8.5.12"));
}

TEST_CASE("first run: done is kept in window.json with the other keys") {
    WindowPrefs prefs;
    CHECK_FALSE(prefs.first_run_done);
    CHECK_FALSE(parse_window_prefs(R"({"language": "ar"})").first_run_done);
    CHECK(parse_window_prefs(R"({"first_run_done": true})").first_run_done);
    CHECK_FALSE(parse_window_prefs(R"({"first_run_done": "yes"})").first_run_done);
    prefs.first_run_done = true;
    prefs.lang = LangChoice::Ku;
    const json written = json::parse(window_prefs_json(prefs, json{{"newer_key", 5}}));
    CHECK(written["first_run_done"] == true);
    CHECK(written["language"] == "ku");
    CHECK(written["newer_key"] == 5);
    CHECK(parse_window_prefs(window_prefs_json(prefs)) == prefs);
    prefs.first_run_done = false;
    CHECK_FALSE(json::parse(window_prefs_json(prefs)).contains("first_run_done"));
}

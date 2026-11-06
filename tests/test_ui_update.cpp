// App updates in the window (Q5, mockups approved 5 Nov): the banner's
// states and words, the time left, Settings' Updates group, and the
// updating dialog's steps and arguments.

#include "model/app_update.h"
#include "model/ui_strings.h"

#include <doctest/doctest.h>

#include <chrono>

using namespace floserver::ui;
using namespace std::chrono_literals;

namespace {

nlohmann::json downloading() {
    return {{"state", "downloading"},
            {"available", true},
            {"current", "1.1.0"},
            {"version", "1.2.0"},
            {"size", 9 * 1024 * 1024 + 400 * 1024},
            {"done", 4 * 1024 * 1024 + 200 * 1024},
            {"sha256", std::string(64, 'a')},
            {"notes_url", "https://floserver.org/news/1.2.0"},
            {"last_checked", "2026-11-05T09:12:00Z"}};
}

nlohmann::json ready() {
    nlohmann::json j = downloading();
    j["state"] = "ready";
    j["done"] = j["size"];
    j["file"] = "C:\\Users\\me\\AppData\\Local\\FloServer\\updates\\FloServer-1.2.0-x64.msi";
    return j;
}

}  // namespace

TEST_CASE("updates.status is read, odd answers give nothing to show") {
    UpdateStatus s = update_status_from(ready());
    CHECK(s.state == UpdateState::Ready);
    CHECK(s.available);
    CHECK(s.current == "1.1.0");
    CHECK(s.version == "1.2.0");
    CHECK(s.done == s.size);
    CHECK(s.notes_url == "https://floserver.org/news/1.2.0");
    CHECK(s.last_checked == "2026-11-05T09:12:00Z");

    CHECK(update_status_from(nlohmann::json()).state == UpdateState::Unknown);  // an older service
    CHECK(update_status_from({{"state", "dancing"}}).state == UpdateState::Unknown);
    // Ready without its file, or downloading without a version: nothing to offer.
    nlohmann::json j = ready();
    j.erase("file");
    CHECK(update_status_from(j).state == UpdateState::Idle);
    j = downloading();
    j.erase("version");
    CHECK(update_status_from(j).state == UpdateState::Idle);
    // Only a plain https What's new is opened.
    j = ready();
    j["notes_url"] = "file:///C:/Windows/System32/calc.exe";
    CHECK(update_status_from(j).notes_url.empty());
    j["notes_url"] = "http://floserver.org/news";
    CHECK(update_status_from(j).notes_url.empty());
    // More done than the size is shown as the size.
    j = downloading();
    j["done"] = 1ULL << 40U;
    const UpdateStatus over = update_status_from(j);
    CHECK(over.done == over.size);
    // No updates here: said by the service.
    const UpdateStatus none = update_status_from({{"state", "failed"}, {"available", false}, {"error", "no"}});
    CHECK_FALSE(none.available);
}

TEST_CASE("the banner: downloading, ready, and Later until the next day or start") {
    const UpdateStatus d = update_status_from(downloading());
    const UpdateStatus r = update_status_from(ready());
    CHECK(update_banner(d, std::nullopt, "2026-11-05") == UpdateBanner::Downloading);
    CHECK(update_banner(r, std::nullopt, "2026-11-05") == UpdateBanner::Ready);

    const UpdateLater later{"1.2.0", "2026-11-05"};
    CHECK(update_banner(r, later, "2026-11-05") == UpdateBanner::None);
    CHECK(update_banner(r, later, "2026-11-06") == UpdateBanner::Ready);  // the next day
    CHECK(update_banner(r, UpdateLater{"1.1.5", "2026-11-05"}, "2026-11-05") == UpdateBanner::Ready);  // another version
    // Later is for the ready banner; a download always shows.
    CHECK(update_banner(d, later, "2026-11-05") == UpdateBanner::Downloading);

    for (const char* state : {"idle", "checking", "failed"}) {
        nlohmann::json j = ready();
        j["state"] = state;
        CHECK(update_banner(update_status_from(j), std::nullopt, "2026-11-05") == UpdateBanner::None);
    }
    CHECK(update_banner(UpdateStatus{}, std::nullopt, "2026-11-05") == UpdateBanner::None);
}

TEST_CASE("the banner's words, as in the mockups") {
    const UpdateStatus d = update_status_from(downloading());
    UpdateBannerText t = update_banner_text(UpdateBanner::Downloading, d, 12.0, Lang::En);
    CHECK(t.title == "Downloading FloServer 1.2.0");
    CHECK(t.line == "4.2 of 9.4 MB · about 15 seconds left. Your projects keep running.");
    t = update_banner_text(UpdateBanner::Downloading, d, std::nullopt, Lang::En);
    CHECK(t.line == "4.2 of 9.4 MB. Your projects keep running.");
    CHECK(update_fraction(d) == doctest::Approx(4.2 / 9.4).epsilon(0.01));

    const UpdateStatus r = update_status_from(ready());
    t = update_banner_text(UpdateBanner::Ready, r, std::nullopt, Lang::En);
    CHECK(t.title == "FloServer 1.2.0 is ready");
    CHECK(t.line == "9.4 MB · downloaded and checked. Your projects stop for a moment while it updates.");
    CHECK(update_fraction(r) == 1.0);
    CHECK(update_fraction(UpdateStatus{}) == 0.0);

    // In Arabic the version and the numbers read left to right.
    t = update_banner_text(UpdateBanner::Downloading, d, 12.0, Lang::Ar);
    CHECK(t.title.find(isolate_ltr("1.2.0")) != std::string::npos);
    CHECK(t.line.find(isolate_ltr("4.2")) != std::string::npos);
    CHECK(t.line.find(time_left_text(12.0, Lang::Ar)) != std::string::npos);  // the phrase itself isn't isolated
    CHECK(update_banner_text(UpdateBanner::None, d, 1.0, Lang::En).title.empty());
}

TEST_CASE("the time left: seconds, then a minute, then minutes; from the last few seconds' speed") {
    CHECK(time_left_text(0.2, Lang::En) == "about 1 seconds left");
    CHECK(time_left_text(7.4, Lang::En) == "about 8 seconds left");
    CHECK(time_left_text(12, Lang::En) == "about 15 seconds left");
    CHECK(time_left_text(40, Lang::En) == "about 40 seconds left");
    CHECK(time_left_text(70, Lang::En) == "about a minute left");
    CHECK(time_left_text(150, Lang::En) == "about 3 minutes left");
    CHECK(time_left_text(600, Lang::En) == "about 10 minutes left");
    CHECK_FALSE(time_left_text(150, Lang::Ku).empty());

    CHECK(mb_number(0) == "0.0");
    CHECK(mb_number(1024 * 1024) == "1.0");
    CHECK(mb_number(9 * 1024 * 1024 + 400 * 1024) == "9.4");

    UpdateRate rate;
    const auto t0 = std::chrono::steady_clock::time_point{} + 100s;
    CHECK_FALSE(rate.seconds_left(1000).has_value());
    rate.add(t0, 0);
    CHECK_FALSE(rate.seconds_left(1000).has_value());  // one sample
    rate.add(t0 + 500ms, 50);
    CHECK_FALSE(rate.seconds_left(1000).has_value());  // under a second
    rate.add(t0 + 2s, 200);
    REQUIRE(rate.seconds_left(1000).has_value());
    CHECK(*rate.seconds_left(1000) == doctest::Approx(8.0));  // 100 bytes a second, 800 to go
    // The speed of the last ten seconds, not since the start.
    for (int i = 3; i <= 20; ++i) {
        rate.add(t0 + std::chrono::seconds(i), 200 + static_cast<std::uint64_t>(i - 2) * 10);
    }
    CHECK(*rate.seconds_left(1000) == doctest::Approx((1000.0 - 380.0) / 10.0).epsilon(0.05));
    // Stalled: no estimate rather than a wrong one.
    UpdateRate stalled;
    stalled.add(t0, 300);
    stalled.add(t0 + 5s, 300);
    CHECK_FALSE(stalled.seconds_left(1000).has_value());
    // A new download (less done) starts over.
    rate.add(t0 + 30s, 10);
    CHECK_FALSE(rate.seconds_left(1000).has_value());
}

TEST_CASE("Settings > General: Updates says when it last looked, and what's ready") {
    const LocalTime now{2026, 11, 5, 14, 30};
    UpdateStatus s = update_status_from(ready());
    CHECK(updates_summary(s, "1.1.0", LocalTime{2026, 11, 5, 9, 2}, now, Lang::En) ==
          "FloServer 1.1.0 · checked today at 09:02.");
    CHECK(updates_summary(s, "1.1.0", LocalTime{2026, 11, 4, 18, 40}, now, Lang::En) ==
          "FloServer 1.1.0 · checked yesterday at 18:40.");
    CHECK(updates_summary(s, "1.1.0", LocalTime{2026, 10, 31, 8, 0}, now, Lang::En) ==
          "FloServer 1.1.0 · checked on 2026-10-31.");
    // Across a month's end, yesterday is still yesterday.
    CHECK(updates_summary(s, "1.1.0", LocalTime{2026, 10, 31, 23, 50}, LocalTime{2026, 11, 1, 0, 5}, Lang::En) ==
          "FloServer 1.1.0 · checked yesterday at 23:50.");
    CHECK(updates_summary(s, "1.1.0", std::nullopt, now, Lang::En) == "FloServer 1.1.0 · not checked yet.");
    s.state = UpdateState::Checking;
    CHECK(updates_summary(s, "1.1.0", LocalTime{2026, 11, 5, 9, 2}, now, Lang::En) ==
          "FloServer 1.1.0 · checking now.");
    CHECK(updates_summary(s, "1.1.0", std::nullopt, now, Lang::Ar).find(isolate_ltr("1.1.0")) != std::string::npos);

    CHECK(parse_checked("2026-11-05T09:12:00Z").has_value());
    CHECK_FALSE(parse_checked("today").has_value());
    CHECK(day_text(LocalTime{2026, 3, 7, 0, 0}) == "2026-03-07");

    UpdatesRow row = updates_row(update_status_from(ready()), Lang::En);
    CHECK(row.ready);
    CHECK(row.title == "FloServer 1.2.0 is ready");
    CHECK(row.line == "9.4 MB · downloaded and checked against the signed list");
    row = updates_row(update_status_from(downloading()), Lang::En);
    CHECK_FALSE(row.ready);
    CHECK(row.title == "Downloading FloServer 1.2.0");
    CHECK(row.line == "4.2 of 9.4 MB");
    nlohmann::json idle = ready();
    idle["state"] = "idle";
    CHECK(updates_row(update_status_from(idle), Lang::En).title.empty());

    CHECK(updates_problem(update_status_from(ready()), Lang::En).empty());
    CHECK(updates_problem(update_status_from({{"state", "failed"}, {"error", "no network"}}), Lang::En) ==
          "The last check didn't work: no network");
    CHECK(updates_problem(update_status_from({{"state", "failed"}, {"available", false}}), Lang::En) ==
          "Updates aren't available on this computer yet.");
}

TEST_CASE("the updating dialog: four steps with ticks, and where it stopped") {
    CHECK(apply_marks(ApplyStep::Checked, false) ==
          std::vector<StepMark>{StepMark::Now, StepMark::Waiting, StepMark::Waiting, StepMark::Waiting});
    CHECK(apply_marks(ApplyStep::Installing, false) ==
          std::vector<StepMark>{StepMark::Done, StepMark::Done, StepMark::Now, StepMark::Waiting});
    CHECK(apply_marks(ApplyStep::Installing, true) ==
          std::vector<StepMark>{StepMark::Done, StepMark::Done, StepMark::Failed, StepMark::Waiting});
    CHECK(apply_marks(ApplyStep::Opening, false).back() == StepMark::Now);
    CHECK(text(Lang::En, apply_step_label(ApplyStep::Checked)) == "Downloaded and checked against the signed list");
    CHECK(text(Lang::En, apply_step_label(ApplyStep::Stopped)) == "Stopped your projects and databases");
    CHECK(text(Lang::En, apply_step_label(ApplyStep::Installing)) == "Installing the new version…");
    CHECK(text(Lang::En, apply_step_label(ApplyStep::Opening)) == "Open FloServer and start your projects again");
    CHECK(text(Lang::En, step_mark_label(StepMark::Done)) == "Done");
    CHECK(fill(Lang::En, Str::UpdatingTitle, {"1.2.0"}) == "Updating FloServer to 1.2.0");
}

TEST_CASE("Windows Installer's progress messages move the bar") {
    MsiProgress p;
    CHECK(p.fraction() == 0.0);
    p.on_record(0, 1000, 0, 1);  // writing the script: the bar waits
    p.on_record(2, 500, 0, 0);
    CHECK(p.fraction() == 0.0);
    p.on_record(0, 1000, 0, 0);  // the real work
    p.on_record(2, 250, 0, 0);
    CHECK(p.fraction() == doctest::Approx(0.25));
    p.on_record(1, 50, 1, 0);  // action data moves it 50 at a time
    p.on_action_data();
    p.on_action_data();
    CHECK(p.fraction() == doctest::Approx(0.35));
    p.on_record(1, 50, 0, 0);  // ...and now it doesn't
    p.on_action_data();
    CHECK(p.fraction() == doctest::Approx(0.35));
    p.on_record(3, 1000, 0, 0);  // more to do
    CHECK(p.fraction() == doctest::Approx(0.175));
    p.on_record(2, 5000, 0, 0);
    CHECK(p.fraction() == 1.0);  // never past the end
    p.on_record(0, 100, 1, 0);  // backwards (a rollback)
    CHECK(p.fraction() == 1.0);
    p.on_record(2, 40, 0, 0);
    CHECK(p.fraction() == doctest::Approx(0.6));
}

TEST_CASE("FloServer.exe --apply-update: its arguments and the copy's command line") {
    auto a = parse_apply_update_args({"--apply-update", "C:\\Users\\me\\FloServer-1.2.0-x64.msi", "--instance", "dev",
                                      "--lang", "ar"});
    REQUIRE(a.has_value());
    CHECK(a->msi == "C:\\Users\\me\\FloServer-1.2.0-x64.msi");
    CHECK(a->instance == "dev");
    CHECK(a->lang == Lang::Ar);
    CHECK_FALSE(parse_apply_update_args({"--tray"}).has_value());
    CHECK_FALSE(parse_apply_update_args({"--apply-update"}).has_value());
    CHECK_FALSE(parse_apply_update_args({"--apply-update", "C:\\setup.exe"}).has_value());  // an .msi only
    a = parse_apply_update_args({"--apply-update", "C:\\u.msi", "--lang", "fr"});
    REQUIRE(a.has_value());
    CHECK_FALSE(a->lang.has_value());
    CHECK(a->instance.empty());

    ApplyUpdateArgs args{"C:\\Users\\me\\up date.msi", "dev", Lang::Ku};
    CHECK(apply_update_command("C:\\Temp\\FloServer-update\\FloServer.exe", args) ==
          "\"C:\\Temp\\FloServer-update\\FloServer.exe\" --apply-update \"C:\\Users\\me\\up date.msi\" --instance dev "
          "--lang ku");
    args = {"C:\\u.msi", "", std::nullopt};
    CHECK(apply_update_command("C:\\FloServer.exe", args) == "\"C:\\FloServer.exe\" --apply-update \"C:\\u.msi\"");
    args.msi = "C:\\a\"b.msi";
    CHECK(apply_update_command("C:\\FloServer.exe", args).empty());
}

TEST_CASE("the update words: in every language, with the same values in each") {
    const auto values = [](std::string_view s) {
        std::string found;
        for (const char* v : {"{0}", "{1}", "{2}"}) {
            if (s.find(v) != std::string_view::npos) {
                found += v;
            }
        }
        return found;
    };
    for (auto i = static_cast<int>(Str::UpdateDownloading); i <= static_cast<int>(Str::UpdateInProgress); ++i) {
        const auto key = static_cast<Str>(i);
        CAPTURE(i);
        CHECK_FALSE(text(Lang::Ar, key).empty());
        CHECK_FALSE(text(Lang::Ku, key).empty());
        CHECK(values(text(Lang::Ar, key)) == values(text(Lang::En, key)));
        CHECK(values(text(Lang::Ku, key)) == values(text(Lang::En, key)));
        // FloServer is a name: kept left to right inside Arabic and Kurdish.
        if (text(Lang::En, key).find("FloServer") != std::string_view::npos) {
            CHECK(text(Lang::Ar, key).find(isolate_ltr("FloServer")) != std::string_view::npos);
            CHECK(text(Lang::Ku, key).find(isolate_ltr("FloServer")) != std::string_view::npos);
        }
    }
}

TEST_CASE("Open the installer: only for an installer that passed its checks, still the same") {
    // Before the checks passed (the service's answer, stopping, the hash itself): never.
    for (const ApplyStep at : {ApplyStep::Checked, ApplyStep::Stopped, ApplyStep::Installing}) {
        CHECK_FALSE(offers_installer(at, false));
    }
    CHECK_FALSE(offers_installer(ApplyStep::Checked, true));
    CHECK_FALSE(offers_installer(ApplyStep::Stopped, true));
    // Windows Installer failed, or the new FloServer didn't open: it may be tried the usual way.
    CHECK(offers_installer(ApplyStep::Installing, true));
    CHECK(offers_installer(ApplyStep::Opening, true));

    const UpdateStatus checked = update_status_from(ready());
    const std::string sha(64, 'a');
    CHECK(installer_matches(checked, checked.size, sha));
    CHECK(installer_matches(checked, checked.size, std::string(64, 'A')));
    CHECK_FALSE(installer_matches(checked, checked.size + 1, sha));
    CHECK_FALSE(installer_matches(checked, checked.size, std::string(63, 'a') + "b"));
    CHECK_FALSE(installer_matches(checked, checked.size, "abc"));
    CHECK_FALSE(installer_matches(UpdateStatus{}, 0, sha));  // nothing was checked
}

TEST_CASE("installed, Windows must restart: three ticks, FloServer opens after the restart") {
    CHECK(apply_marks_restart() ==
          std::vector<StepMark>{StepMark::Done, StepMark::Done, StepMark::Done, StepMark::Waiting});
    CHECK(fill(Lang::En, Str::UpdateRestartNeeded, {"1.2.0"}) ==
          "FloServer 1.2.0 is installed. Restart Windows to finish the update, then open FloServer.");
}

TEST_CASE("the updater's own folder under %TEMP%, and earlier ones known again") {
    CHECK(updater_folder_name(0x1234abcdULL) == "FloServer-update-000000001234abcd");
    CHECK(updater_folder_name(~0ULL) == "FloServer-update-ffffffffffffffff");
    CHECK(is_updater_folder(updater_folder_name(42)));
    CHECK_FALSE(is_updater_folder("FloServer-update"));
    CHECK_FALSE(is_updater_folder("FloServer-update-1234"));
    CHECK_FALSE(is_updater_folder("FloServer-update-000000001234ABCD"));
    CHECK_FALSE(is_updater_folder("FloServer-update-00000000..\\x1234"));
    CHECK_FALSE(is_updater_folder("Other-000000001234abcd"));
}

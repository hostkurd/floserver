#pragma once

// App updates in the window (Q5, mockups approved 5 Nov): what the service's
// updates.status says, the banner on the main screen above the project list
// (downloading; ready, with Later and Update now), Settings > General's
// Updates group, the time left of a download, and the updating dialog's
// steps. Plain C++ so it's tested on every platform; the window draws it,
// and FloServer.exe --apply-update runs the steps (main_window_update.cpp,
// apply_update.cpp).

#include "model/ui_strings.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

enum class UpdateState : std::uint8_t { Unknown, Idle, Checking, Downloading, Ready, Failed };

// updates.status's answer (update_service.h).
struct UpdateStatus {
    UpdateState state = UpdateState::Unknown;  // Unknown: not asked yet, or an older service
    bool available = true;   // false: no updates on this platform
    std::string current;     // this FloServer's version, as the service has it
    std::string version;     // the newer one the list offers; empty for none
    std::uint64_t size = 0;
    std::uint64_t done = 0;
    std::string sha256;
    std::string notes_url;   // What's new (https), or empty
    std::string file;        // ready: the checked MSI
    std::string last_checked;  // "2026-11-05T09:12:00Z", or empty
    std::string error;
    bool operator==(const UpdateStatus&) const = default;
};

[[nodiscard]] UpdateStatus update_status_from(const nlohmann::json& reply);

// A What's new address the window may open: https and nothing odd in it.
[[nodiscard]] bool is_openable_notes(std::string_view url);

// --- the time left -------------------------------------------------------------

// The download's speed over its last few seconds, from the window's polls.
class UpdateRate {
public:
    // `done` bytes at `at`. A smaller `done` (a new download) starts over.
    void add(std::chrono::steady_clock::time_point at, std::uint64_t done);
    void reset() { samples_.clear(); }
    // Seconds until `total`; nothing until two samples a second apart show it moving.
    [[nodiscard]] std::optional<double> seconds_left(std::uint64_t total) const;

private:
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>> samples_;
};

// "about 40 seconds left", "about a minute left", "about 3 minutes left".
[[nodiscard]] std::string time_left_text(double seconds, Lang lang);

// "4.2": megabytes (MiB) with one decimal, as "4.2 of 9.4 MB" shows them.
[[nodiscard]] std::string mb_number(std::uint64_t bytes);

// --- the banner above the project list ------------------------------------------

enum class UpdateBanner : std::uint8_t { None, Downloading, Ready };

// "Later" hides the ready banner until FloServer starts again or the next
// day: the version it was chosen for, on which local day ("2026-11-05").
struct UpdateLater {
    std::string version;
    std::string day;
};

[[nodiscard]] UpdateBanner update_banner(const UpdateStatus& status, const std::optional<UpdateLater>& later,
                                         std::string_view today);

struct UpdateBannerText {
    std::string title;  // "Downloading FloServer 1.2.0", "FloServer 1.2.0 is ready"
    std::string line;   // the progress, or what Update now does
};
[[nodiscard]] UpdateBannerText update_banner_text(UpdateBanner banner, const UpdateStatus& status,
                                                  std::optional<double> seconds_left, Lang lang);

// The progress bar: 0 to 1.
[[nodiscard]] double update_fraction(const UpdateStatus& status);

// --- Settings > General: Updates ------------------------------------------------

// A local date and time (the window turns the UTC ones into this).
struct LocalTime {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
};
// "2026-11-05T09:12:00Z" read; nothing for anything else.
[[nodiscard]] std::optional<std::chrono::sys_seconds> parse_checked(std::string_view utc);
// "2026-11-05", for UpdateLater::day.
[[nodiscard]] std::string day_text(const LocalTime& t);

// "FloServer 0.9.0 · checked today at 09:12." / "… · checked yesterday at 18:40."
// / "… · checked on 2026-11-02." / "… · not checked yet." / "… · checking now."
[[nodiscard]] std::string updates_summary(const UpdateStatus& status, std::string_view current,
                                          const std::optional<LocalTime>& checked, const LocalTime& now, Lang lang);

// The second row's lines while a newer version is on its way or ready;
// empty title when there's no second row.
struct UpdatesRow {
    std::string title;  // "FloServer 1.2.0 is ready"
    std::string line;   // "9.4 MB · downloaded and checked against the signed list"
    bool ready = false; // Update now is offered
};
[[nodiscard]] UpdatesRow updates_row(const UpdateStatus& status, Lang lang);

// Why the last check didn't work, said under the summary; empty when it did.
[[nodiscard]] std::string updates_problem(const UpdateStatus& status, Lang lang);

// --- Update now: FloServer.exe --apply-update -----------------------------------

// The four steps of the updating dialog, in order.
enum class ApplyStep : std::uint8_t { Checked, Stopped, Installing, Opening };
inline constexpr std::size_t kApplySteps = 4;

// How each step shows: a tick, the one going now, one to come, or the one that failed.
enum class StepMark : std::uint8_t { Done, Now, Waiting, Failed };

// The marks when `at` is the step going now (and `failed`: it went wrong there).
[[nodiscard]] std::vector<StepMark> apply_marks(ApplyStep at, bool failed);
[[nodiscard]] Str apply_step_label(ApplyStep step);
// A mark's words for screen readers ("Done", "Now"...).
[[nodiscard]] Str step_mark_label(StepMark mark);

// Windows Installer's progress messages (INSTALLMESSAGE_PROGRESS, fields 1
// to 4), turned into how far the bar goes, as MSDN's "Parsing Windows
// Installer Messages" reads them: a reset gives the total, action data
// moves by its step, a report moves by its ticks, "add" makes the total larger.
class MsiProgress {
public:
    void on_record(int field1, int field2, int field3, int field4);
    // INSTALLMESSAGE_ACTIONDATA: one step, when the action said its data moves the bar.
    void on_action_data();
    // 0 to 1; 0 before the first reset.
    [[nodiscard]] double fraction() const;

private:
    long long total_ = 0;
    long long done_ = 0;
    bool backward_ = false;
    bool script_ = false;  // writing the script, not doing it: the bar waits
    bool explicit_ = false;  // action data moves the bar
    long long step_ = 0;
};

// FloServer.exe --apply-update <msi> [--instance <suffix>] [--lang en|ar|ku]:
// the copy in %TEMP% that installs the update. Nothing for other arguments.
struct ApplyUpdateArgs {
    std::string msi;
    std::string instance;
    std::optional<Lang> lang;
};
inline constexpr std::string_view kApplyUpdateFlag = "--apply-update";
inline constexpr std::string_view kLangFlag = "--lang";
[[nodiscard]] std::optional<ApplyUpdateArgs> parse_apply_update_args(const std::vector<std::string>& args);
// The same as a command line, each argument quoted (the copy's command).
[[nodiscard]] std::string apply_update_command(std::string_view exe, const ApplyUpdateArgs& args);

}  // namespace floserver::ui

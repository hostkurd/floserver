#include "model/app_update.h"

#include "model/general_page.h"
#include "model/runtimes_page.h"

#include "floserver/core/manifest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace floserver::ui {

namespace core = floserver::core;

namespace {

std::string string_field(const nlohmann::json& j, const char* key) {
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

std::uint64_t number_field(const nlohmann::json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_number_integer()) {
        return 0;
    }
    return j[key].is_number_unsigned() ? j[key].get<std::uint64_t>()
                                       : static_cast<std::uint64_t>(std::max<std::int64_t>(0, j[key].get<std::int64_t>()));
}

// Versions, sizes and times are code words: left to right inside Arabic and Kurdish.
std::string code(std::string_view s, Lang lang) { return is_rtl(lang) ? isolate_ltr(s) : std::string(s); }

std::string two(int n) {
    std::array<char, 8> buf{};
    const int len = std::snprintf(buf.data(), buf.size(), "%02d", n);
    return {buf.data(), len > 0 ? static_cast<std::size_t>(len) : 0};
}

std::chrono::sys_days civil(const LocalTime& t) {
    return std::chrono::sys_days{std::chrono::year{t.year} / std::chrono::month{static_cast<unsigned>(t.month)} /
                                 std::chrono::day{static_cast<unsigned>(t.day)}};
}

// Quote-free arguments only: each is put in quotes as it is.
bool quotable(std::string_view s) { return s.find_first_of("\"\r\n") == std::string_view::npos; }

}  // namespace

UpdateStatus update_status_from(const nlohmann::json& reply) {
    UpdateStatus s;
    if (!reply.is_object()) {
        return s;
    }
    const std::string state = string_field(reply, "state");
    s.state = state == "idle"          ? UpdateState::Idle
              : state == "checking"    ? UpdateState::Checking
              : state == "downloading" ? UpdateState::Downloading
              : state == "ready"       ? UpdateState::Ready
              : state == "failed"      ? UpdateState::Failed
                                       : UpdateState::Unknown;
    s.available = !reply.contains("available") || !reply["available"].is_boolean() || reply["available"].get<bool>();
    s.current = string_field(reply, "current");
    s.version = string_field(reply, "version");
    s.size = number_field(reply, "size");
    s.done = std::min(number_field(reply, "done"), s.size);
    s.sha256 = string_field(reply, "sha256");
    s.notes_url = string_field(reply, "notes_url");
    s.file = string_field(reply, "file");
    s.last_checked = string_field(reply, "last_checked");
    s.error = string_field(reply, "error");
    // Downloading or ready without a version is nothing the window can show.
    if ((s.state == UpdateState::Downloading || s.state == UpdateState::Ready) && s.version.empty()) {
        s.state = UpdateState::Idle;
    }
    if (s.state == UpdateState::Ready && s.file.empty()) {
        s.state = UpdateState::Idle;
    }
    if (!is_openable_notes(s.notes_url)) {
        s.notes_url.clear();
    }
    return s;
}

bool is_openable_notes(std::string_view url) { return !url.empty() && core::is_plain_https_url(url); }

// --- the time left -------------------------------------------------------------

void UpdateRate::add(std::chrono::steady_clock::time_point at, std::uint64_t done) {
    if (!samples_.empty() && done < samples_.back().second) {
        samples_.clear();  // a new download
    }
    samples_.emplace_back(at, done);
    // The last few seconds: the speed now, not the average since it began.
    constexpr auto kWindow = std::chrono::seconds(10);
    while (samples_.size() > 2 && at - samples_.front().first > kWindow) {
        samples_.pop_front();
    }
}

std::optional<double> UpdateRate::seconds_left(std::uint64_t total) const {
    if (samples_.size() < 2) {
        return std::nullopt;
    }
    const auto& [t0, d0] = samples_.front();
    const auto& [t1, d1] = samples_.back();
    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    if (seconds < 1.0 || d1 <= d0) {
        return std::nullopt;
    }
    const double rate = static_cast<double>(d1 - d0) / seconds;
    return total > d1 ? static_cast<double>(total - d1) / rate : 0.0;
}

std::string time_left_text(double seconds, Lang lang) {
    const double s = std::max(1.0, seconds);
    if (s < 55) {
        // Whole seconds up to 10, then in fives: it doesn't flicker at each poll.
        const auto n = s <= 10 ? static_cast<long>(std::ceil(s)) : static_cast<long>(std::ceil(s / 5) * 5);
        return fill_pattern(text(lang, Str::UpdateSecondsLeft), false, {std::to_string(n)});
    }
    if (s < 90) {
        return std::string(text(lang, Str::UpdateMinuteLeft));
    }
    const long minutes = std::lround(s / 60);
    return fill_pattern(text(lang, Str::UpdateMinutesLeft), false, {std::to_string(minutes)});
}

std::string mb_number(std::uint64_t bytes) {
    constexpr std::uint64_t kMb = std::uint64_t{1024} * 1024;
    const std::uint64_t tenths = (bytes * 10 + kMb / 2) / kMb;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

// --- the banner -------------------------------------------------------------------

UpdateBanner update_banner(const UpdateStatus& status, const std::optional<UpdateLater>& later,
                           std::string_view today) {
    if (status.version.empty()) {
        return UpdateBanner::None;
    }
    if (status.state == UpdateState::Downloading) {
        return UpdateBanner::Downloading;
    }
    if (status.state == UpdateState::Ready) {
        const bool hidden = later && later->version == status.version && later->day == today;
        return hidden ? UpdateBanner::None : UpdateBanner::Ready;
    }
    return UpdateBanner::None;
}

UpdateBannerText update_banner_text(UpdateBanner banner, const UpdateStatus& status,
                                    std::optional<double> seconds_left, Lang lang) {
    UpdateBannerText out;
    const std::string version = code(status.version, lang);
    switch (banner) {
        case UpdateBanner::Downloading: {
            out.title = fill_pattern(text(lang, Str::UpdateDownloading), false, {version});
            const std::string done = code(mb_number(status.done), lang);
            const std::string total = code(mb_number(status.size), lang);
            out.line = seconds_left ? fill_pattern(text(lang, Str::UpdateProgressLine), false,
                                                   {done, total, time_left_text(*seconds_left, lang)})
                                    : fill_pattern(text(lang, Str::UpdateProgressLineNoTime), false, {done, total});
            break;
        }
        case UpdateBanner::Ready:
            out.title = fill_pattern(text(lang, Str::UpdateReadyTitle), false, {version});
            out.line = fill_pattern(text(lang, Str::UpdateReadyLine), false, {code(size_text(status.size), lang)});
            break;
        case UpdateBanner::None:
            break;
    }
    return out;
}

double update_fraction(const UpdateStatus& status) {
    if (status.size == 0) {
        return 0;
    }
    return std::clamp(static_cast<double>(status.done) / static_cast<double>(status.size), 0.0, 1.0);
}

// --- Settings > General ----------------------------------------------------------

std::optional<std::chrono::sys_seconds> parse_checked(std::string_view utc) { return core::parse_utc_time(utc); }

std::string day_text(const LocalTime& t) {
    return std::to_string(t.year) + "-" + two(t.month) + "-" + two(t.day);
}

std::string updates_summary(const UpdateStatus& status, std::string_view current,
                            const std::optional<LocalTime>& checked, const LocalTime& now, Lang lang) {
    std::string when;
    if (status.state == UpdateState::Checking) {
        when = text(lang, Str::UpdatesCheckingNow);
    } else if (checked) {
        const std::string clock = code(two(checked->hour) + ":" + two(checked->minute), lang);
        const auto days = (civil(now) - civil(*checked)).count();
        if (days == 0) {
            when = fill_pattern(text(lang, Str::UpdatesCheckedToday), false, {clock});
        } else if (days == 1) {
            when = fill_pattern(text(lang, Str::UpdatesCheckedYesterday), false, {clock});
        } else {
            when = fill_pattern(text(lang, Str::UpdatesCheckedOn), false, {code(day_text(*checked), lang)});
        }
    } else {
        when = text(lang, Str::UpdatesNotChecked);
    }
    return fill_pattern(text(lang, Str::UpdatesSummary), false, {code(current, lang), when});
}

UpdatesRow updates_row(const UpdateStatus& status, Lang lang) {
    UpdatesRow row;
    if (status.version.empty()) {
        return row;
    }
    const std::string version = code(status.version, lang);
    if (status.state == UpdateState::Downloading) {
        row.title = fill_pattern(text(lang, Str::UpdateDownloading), false, {version});
        row.line = fill_pattern(text(lang, Str::UpdatesRowDownloading), false,
                                {code(mb_number(status.done), lang), code(mb_number(status.size), lang)});
    } else if (status.state == UpdateState::Ready) {
        row.title = fill_pattern(text(lang, Str::UpdateReadyTitle), false, {version});
        row.line = fill_pattern(text(lang, Str::UpdatesReadyLine), false, {code(size_text(status.size), lang)});
        row.ready = true;
    }
    return row;
}

std::string updates_problem(const UpdateStatus& status, Lang lang) {
    if (!status.available) {
        return std::string(text(lang, Str::UpdatesUnavailable));
    }
    if (status.state == UpdateState::Failed) {
        return fill(lang, Str::UpdatesFailed, {status.error});
    }
    return {};
}

// --- Update now -------------------------------------------------------------------

std::vector<StepMark> apply_marks(ApplyStep at, bool failed) {
    std::vector<StepMark> marks;
    for (std::size_t i = 0; i < kApplySteps; ++i) {
        const auto now = static_cast<std::size_t>(at);
        marks.push_back(i < now    ? StepMark::Done
                        : i == now ? (failed ? StepMark::Failed : StepMark::Now)
                                   : StepMark::Waiting);
    }
    return marks;
}

Str apply_step_label(ApplyStep step) {
    switch (step) {
        case ApplyStep::Checked:
            return Str::UpdateStepChecked;
        case ApplyStep::Stopped:
            return Str::UpdateStepStopped;
        case ApplyStep::Installing:
            return Str::UpdateStepInstalling;
        case ApplyStep::Opening:
            break;
    }
    return Str::UpdateStepOpen;
}

Str step_mark_label(StepMark mark) {
    switch (mark) {
        case StepMark::Done:
            return Str::UpdateMarkDone;
        case StepMark::Now:
            return Str::UpdateMarkNow;
        case StepMark::Failed:
            return Str::UpdateMarkFailed;
        case StepMark::Waiting:
            break;
    }
    return Str::UpdateMarkWaiting;
}

std::vector<StepMark> apply_marks_restart() {
    std::vector<StepMark> marks = apply_marks(ApplyStep::Opening, false);
    marks.back() = StepMark::Waiting;
    return marks;
}

bool offers_installer(ApplyStep failed_at, bool verified) {
    return verified && (failed_at == ApplyStep::Installing || failed_at == ApplyStep::Opening);
}

bool installer_matches(const UpdateStatus& checked, std::uint64_t size, std::string_view sha256) {
    if (checked.size == 0 || size != checked.size || sha256.size() != 64 || checked.sha256.size() != 64) {
        return false;
    }
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return std::equal(sha256.begin(), sha256.end(), checked.sha256.begin(),
                      [&](char a, char b) { return lower(a) == lower(b); });
}

std::string updater_folder_name(std::uint64_t random) {
    std::array<char, 17> hex{};
    (void)std::snprintf(hex.data(), hex.size(), "%016llx", static_cast<unsigned long long>(random));
    return std::string(kUpdaterFolderPrefix) + hex.data();
}

bool is_updater_folder(std::string_view name) {
    if (!name.starts_with(kUpdaterFolderPrefix) || name.size() != kUpdaterFolderPrefix.size() + 16) {
        return false;
    }
    const std::string_view digits = name.substr(kUpdaterFolderPrefix.size());
    return std::all_of(digits.begin(), digits.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::optional<ApplyUpdateArgs> parse_apply_update_args(const std::vector<std::string>& args) {
    ApplyUpdateArgs out;
    bool found = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == kApplyUpdateFlag && i + 1 < args.size() && !args[i + 1].empty()) {
            out.msi = args[++i];
            found = true;
        } else if (args[i] == kLangFlag && i + 1 < args.size()) {
            out.lang = parse_lang(args[++i]);
        }
    }
    if (!found || !out.msi.ends_with(".msi")) {
        return std::nullopt;
    }
    out.instance = parse_start_args(args).instance;
    return out;
}

std::string apply_update_command(std::string_view exe, const ApplyUpdateArgs& args) {
    if (exe.empty() || args.msi.empty() || !quotable(exe) || !quotable(args.msi) || !quotable(args.instance)) {
        return {};
    }
    std::string out = "\"" + std::string(exe) + "\" " + std::string(kApplyUpdateFlag) + " \"" + args.msi + "\"";
    if (!args.instance.empty()) {
        out += " " + std::string(kInstanceFlag) + " " + args.instance;
    }
    if (args.lang) {
        out += " " + std::string(kLangFlag) + " " + std::string(lang_code(*args.lang));
    }
    return out;
}

}  // namespace floserver::ui

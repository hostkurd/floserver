#pragma once

// The first-run page (approved 2 Nov): three steps inside the main window,
// Language, Runtimes (then their downloads) and Ready. Which runtimes are
// offered and ticked, when the page shows, the rows while they download and
// the summary's words are here, in plain C++, so they're tested on every
// platform; the window only draws them and sends what the user picks.

#include "model/runtimes_page.h"
#include "model/ui_strings.h"
#include "model/view_model.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

enum class FirstRunStep : std::uint8_t { Language, Runtimes, Ready };

// FLOSERVER_FIRST_RUN's value (nullptr when unset): "1" shows the page
// whatever window.json and the service say (for trying it out).
[[nodiscard]] bool first_run_forced(const char* value);

// Whether the page shows: forced, or not marked done in window.json, with
// no runtime installed and no project. Asked once, when the service has
// answered projects.list and runtimes.list.
[[nodiscard]] bool first_run_wanted(bool forced, bool done, const Installed& installed, std::size_t projects);

// "1 Language", "✓ Language": a step in the strip, from `current`.
[[nodiscard]] std::string first_run_step_label(FirstRunStep step, FirstRunStep current, Lang lang);

// A runtime offered on step 2: the newest version of its series in the list.
struct FirstRunOffer {
    std::string name;     // "php"
    std::string version;  // "8.5.12"
    std::string label;    // "PHP 8.5"
    Str what = Str::FirstRunWhatPhp;
    std::uint64_t size = 0;
    bool ticked = false;  // ticked when the page opens
    bool always = false;  // nginx: always included, its box can't be changed
};

// runtimes.list's full answer: PHP 8.5, MariaDB 11.4, Node.js 24 and
// Composer ticked, Python 3.13, phpMyAdmin 5.2 and Apache 2.4 offered, nginx
// always; each the newest of its series in the signed list ("available"),
// in that order. A series the list doesn't have is left out.
[[nodiscard]] std::vector<FirstRunOffer> first_run_offers(const nlohmann::json& runtimes_reply);

// The boxes as the page opens: one per offer.
[[nodiscard]] std::vector<bool> first_run_default_ticks(const std::vector<FirstRunOffer>& offers);

// What "Download and continue" installs: nginx first (always), then each
// ticked offer in the page's order. `ticks` has one per offer.
[[nodiscard]] std::vector<Download> first_run_downloads(const std::vector<FirstRunOffer>& offers,
                                                        const std::vector<bool>& ticks);

// Their sizes added up, for the footer.
[[nodiscard]] std::uint64_t first_run_total(const std::vector<FirstRunOffer>& offers, const std::vector<bool>& ticks);

// What a runtimes.list answer means for step 2: the offers, ask again with a
// fresh list (none on this computer yet; asked once), or why it can't be had
// (a whole sentence, in `lang`).
struct FirstRunListAnswer {
    enum class Kind : std::uint8_t { Offers, Refresh, Failed } kind = Kind::Failed;
    std::vector<FirstRunOffer> offers;
    std::string error;
};
// `reply`: the answer, or nothing with the connection's or service's `error`.
// `refreshed`: it's the answer to a fresh fetch.
[[nodiscard]] FirstRunListAnswer first_run_list_answer(const std::optional<nlohmann::json>& reply,
                                                       const std::string& error, bool refreshed, Lang lang);

// The worker's key for the page's installs; project names can't have '_'.
inline constexpr std::string_view kFirstRunKey = "_firstrun";
[[nodiscard]] bool is_first_run_key(std::string_view key);

// How an install the page started ended without being installed (jobs.get's
// "failed" or "cancelled", or runtimes.install refusing it).
struct FirstRunEnd {
    std::string state;    // "failed" or "cancelled"
    std::string message;  // the service's words
};

enum class FirstRunRowState : std::uint8_t { Ready, Running, Waiting, Failed, Cancelled };

// One download's row while they run.
struct FirstRunRow {
    std::string name;   // "php"
    std::string label;  // "PHP 8.5.12"
    FirstRunRowState state = FirstRunRowState::Waiting;
    std::string line;   // "Ready", "Downloading… 19 MB of 31 MB", "Waiting", "Failed: ..."
    std::optional<double> fraction;  // Running, once the size is known
    std::int64_t job = 0;            // Running: what Cancel cancels
};

// `wanted`: what was asked for. `installed` is the 2 s poll's, `jobs` the
// installs running (jobs.get), `ends` how the others ended, by name.
[[nodiscard]] std::vector<FirstRunRow> first_run_rows(const std::vector<Download>& wanted, const Installed& installed,
                                                      const std::vector<RuntimeJob>& jobs,
                                                      const std::map<std::string, FirstRunEnd>& ends, Lang lang);

struct FirstRunProgress {
    std::size_t ready = 0;
    std::size_t total = 0;
    std::size_t failed = 0;  // failed or cancelled
    [[nodiscard]] bool running() const { return ready + failed < total; }
};
[[nodiscard]] FirstRunProgress first_run_progress(const std::vector<FirstRunRow>& rows);

// Step 3's Runtimes line: what's ready, "still downloading: 3 of 5 ready"
// while they run, and what didn't download; or that nothing was.
[[nodiscard]] std::string first_run_runtimes_line(const std::vector<FirstRunRow>& rows, Lang lang);

// Asking Windows to trust FloServer's certificate, as Settings → Ports &
// HTTPS does.
enum class FirstRunTrust : std::uint8_t {
    NotAsked,  // no downloads started (skipped)
    Checking,  // certs.ensure_ca on its way
    Asking,    // Windows' own question is open
    Trusted,
    Declined,  // the user said No
    Failed,
};

// Step 3's HTTPS line. `error`: why it failed (the service's or Windows' words).
[[nodiscard]] std::string first_run_https_line(FirstRunTrust trust, const std::string& error, Lang lang);

}  // namespace floserver::ui

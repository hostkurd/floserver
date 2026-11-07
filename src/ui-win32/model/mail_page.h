#pragma once

// Mail (D27, approved 7 Nov): Mailpit catches the emails the projects send.
// mail.status's answer, what Settings > Mail shows from it, the changes not
// saved yet and what Save sends, the main window's "Mail · 3 new" button,
// and when a new email earns a toast. Plain C++ so it's tested on every
// platform; the windows only draw it.

#include "model/ui_strings.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace floserver::ui {

enum class MailState : std::uint8_t { NotInstalled, Asleep, Starting, Running, Failed };

struct MailSettings {
    bool php = true;      // PHP's mail goes to Mailpit
    bool notices = true;  // a toast for new mail
    bool keep_running = false;
    int max = 500;        // the emails kept, oldest go first
};

// The newest email, and how many came in the batch the service last announced.
struct MailLatest {
    std::string subject;
    std::string to;
    int new_in_batch = 1;
};

struct MailStatus {
    bool installed = false;
    std::string version;
    MailState state = MailState::NotInstalled;
    std::string error;
    int smtp_port = 1025;
    std::string inbox_url = "https://mail.localhost";
    bool inbox_served = true;
    std::string inbox_blocked_by;  // the project that holds the address
    std::uint64_t unread = 0;
    std::uint64_t total = 0;
    std::uint64_t size_bytes = 0;
    std::uint64_t notice_seq = 0;  // grows with each batch of new mail
    std::optional<MailLatest> latest;
    MailSettings settings;
};

// mail.status's result; missing fields take the defaults above. Nothing when
// `reply` isn't an object.
[[nodiscard]] std::optional<MailStatus> mail_status_from_reply(const nlohmann::json& reply);
// What an older service (one that doesn't know mail.status) means.
[[nodiscard]] MailStatus mail_not_installed();
// The service's error code for a method it doesn't have.
inline constexpr std::string_view kUnknownMethod = "unknown_method";

// The address the browser opens: the service's inbox_url when it's one the
// window opens (a .localhost address), else https://mail.localhost.
[[nodiscard]] std::string mail_inbox_address(const MailStatus& status);

// --- Settings > Mail -----------------------------------------------------------

[[nodiscard]] std::string mail_state_text(MailState state, Lang lang);
enum class MailPill : std::uint8_t { Good, Waiting, Bad, Neutral };
[[nodiscard]] MailPill mail_pill(MailState state);

// "127.0.0.1 : 1025"
[[nodiscard]] std::string mail_smtp_text(const MailStatus& status);
// "12 emails · 3.4 MB on disk"
[[nodiscard]] std::string mail_count_text(const MailStatus& status, Lang lang);
// Why the inbox address isn't served; empty when it is.
[[nodiscard]] std::string mail_not_served_text(const MailStatus& status, Lang lang);
// The .env-free line SMTP's "Copy" puts on the clipboard.
[[nodiscard]] std::string mail_smtp_copy(const MailStatus& status);

// The changes until Save.
struct MailChanges {
    std::optional<bool> php;
    std::optional<bool> notices;
    std::optional<bool> keep_running;
    std::optional<std::string> max;  // as typed
    [[nodiscard]] bool empty() const { return !php && !notices && !keep_running && !max; }
};

[[nodiscard]] bool mail_php(const MailStatus& status, const MailChanges& changes);
[[nodiscard]] bool mail_notices(const MailStatus& status, const MailChanges& changes);
[[nodiscard]] bool mail_keep_running(const MailStatus& status, const MailChanges& changes);
void mail_toggle_php(const MailStatus& status, MailChanges& changes);
void mail_toggle_notices(const MailStatus& status, MailChanges& changes);
void mail_toggle_keep_running(const MailStatus& status, MailChanges& changes);

inline constexpr int kMailMaxLeast = 1;
inline constexpr int kMailMaxMost = 100000;
[[nodiscard]] std::string mail_max_text(const MailStatus& status, const MailChanges& changes);
// A typed number goes back to "what's saved" when it equals it.
void mail_set_max_text(const MailStatus& status, MailChanges& changes, const std::string& text);
// The typed number can't be sent: not a whole number from 1 to 100000.
[[nodiscard]] bool mail_max_bad(const MailChanges& changes);

// mail.set's parameters: only what changed (and a number that's valid).
[[nodiscard]] nlohmann::json mail_set_params(const MailChanges& changes);
// The status as it is once `changes` were saved (until the next answer).
void mail_apply(MailStatus& status, const MailChanges& changes);

// The footer: "Applies when saved. …"; `php_projects` is how many projects
// run PHP (the first switch restarts it for them).
[[nodiscard]] std::string mail_footer_text(std::size_t php_projects, Lang lang);

// --- the main window --------------------------------------------------------------

// The status line's button: only once Mailpit is installed.
[[nodiscard]] bool mail_link_shown(const std::optional<MailStatus>& status);
// "Mail · 3 new", or "Mail" when none are.
[[nodiscard]] std::string mail_link_text(const MailStatus& status, Lang lang);

// --- the toast ----------------------------------------------------------------------

// At most one toast in this time; what comes meanwhile is merged into the next.
inline constexpr std::int64_t kMailToastGapMs = 5000;
inline constexpr std::size_t kMailSubjectMaxChars = 70;

// What the window remembers between polls.
struct MailNotice {
    std::optional<std::uint64_t> seen;  // notice_seq at the last poll; none before the first
    std::uint64_t pending = 0;          // emails announced and not shown yet
    std::string subject;                // the newest of them
    std::string to;
    std::optional<std::int64_t> last_toast_ms;
};

struct MailToastText {
    std::string title;   // "New email: Reset your password", or "3 new emails"
    std::string detail;  // "to customer@example.com", or "Latest: …"
};

// A subject as plain text for one line: controls and direction marks gone,
// spaces collapsed, cut with an ellipsis after `max_chars` characters.
[[nodiscard]] std::string mail_plain_subject(std::string_view subject, std::size_t max_chars = kMailSubjectMaxChars);

// One poll's answer (nothing: no answer, or Mailpit isn't there). The first
// answer after the program starts only sets what's seen. Returns the toast to
// show now, if any, at `now_ms` (any steady clock, in milliseconds).
[[nodiscard]] std::optional<MailToastText> mail_notice_step(MailNotice& notice, const std::optional<MailStatus>& status,
                                                            std::int64_t now_ms, Lang lang);

}  // namespace floserver::ui

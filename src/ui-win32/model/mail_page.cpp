#include "model/mail_page.h"

#include "model/view_model.h"

#include <algorithm>
#include <cctype>

namespace floserver::ui {

namespace {

using nlohmann::json;

std::string text_of(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return {};
}

bool flag_of(const json& j, const char* key, bool fallback) {
    if (j.is_object() && j.contains(key) && j[key].is_boolean()) {
        return j[key].get<bool>();
    }
    return fallback;
}

std::uint64_t count_of(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number_integer()) {
        const auto n = j[key].get<std::int64_t>();
        return n > 0 ? static_cast<std::uint64_t>(n) : 0;
    }
    return 0;
}

MailState state_of(std::string_view s) {
    if (s == "asleep") {
        return MailState::Asleep;
    }
    if (s == "starting") {
        return MailState::Starting;
    }
    if (s == "running") {
        return MailState::Running;
    }
    if (s == "failed") {
        return MailState::Failed;
    }
    return MailState::NotInstalled;
}

// The number typed, when it is a whole number in range.
std::optional<int> parse_max(std::string_view s) {
    if (s.empty() || s.size() > 7) {
        return std::nullopt;
    }
    int n = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        n = n * 10 + (c - '0');
    }
    if (n < kMailMaxLeast || n > kMailMaxMost) {
        return std::nullopt;
    }
    return n;
}

}  // namespace

std::optional<MailStatus> mail_status_from_reply(const json& reply) {
    if (!reply.is_object()) {
        return std::nullopt;
    }
    MailStatus s;
    s.installed = flag_of(reply, "installed", false);
    s.version = mail_plain_subject(text_of(reply, "version"), 40);
    s.state = s.installed ? state_of(text_of(reply, "state")) : MailState::NotInstalled;
    s.error = mail_plain_subject(text_of(reply, "error"), kMailErrorMaxChars);
    if (reply.contains("smtp_port") && reply["smtp_port"].is_number_integer()) {
        const auto port = reply["smtp_port"].get<std::int64_t>();
        if (port > 0 && port < 65536) {
            s.smtp_port = static_cast<int>(port);
        }
    }
    if (const std::string url = text_of(reply, "inbox_url"); !url.empty()) {
        s.inbox_url = url;
    }
    s.inbox_served = flag_of(reply, "inbox_served", true);
    s.inbox_blocked_by = mail_plain_subject(text_of(reply, "inbox_blocked_by"), 80);
    s.unread = count_of(reply, "unread");
    s.total = count_of(reply, "total");
    s.size_bytes = count_of(reply, "size_bytes");
    s.notice_seq = count_of(reply, "notice_seq");
    if (reply.contains("latest") && reply["latest"].is_object()) {
        const json& l = reply["latest"];
        MailLatest latest;
        latest.subject = text_of(l, "subject");
        latest.to = text_of(l, "to");
        latest.new_in_batch = std::max(1, static_cast<int>(std::min<std::uint64_t>(count_of(l, "new_in_batch"), 100000)));
        s.latest = std::move(latest);
    }
    if (reply.contains("settings") && reply["settings"].is_object()) {
        const json& o = reply["settings"];
        s.settings.php = flag_of(o, "php", s.settings.php);
        s.settings.notices = flag_of(o, "notices", s.settings.notices);
        s.settings.keep_running = flag_of(o, "keep_running", s.settings.keep_running);
        if (const std::uint64_t max = count_of(o, "max"); max > 0) {
            s.settings.max = static_cast<int>(std::min<std::uint64_t>(max, 1000000));
        }
    }
    return s;
}

MailStatus mail_not_installed() { return MailStatus{}; }

std::string mail_inbox_address(const MailStatus& status) {
    std::string url = status.inbox_url;
    // is_openable_address wants a host ending in .localhost with an optional port and one "/".
    if (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return is_openable_address(url) ? url : std::string("https://mail.localhost");
}

std::string mail_state_text(MailState state, Lang lang) {
    switch (state) {
        case MailState::NotInstalled:
            return std::string(text(lang, Str::AccessNotInstalled));
        case MailState::Asleep:
            return std::string(text(lang, Str::MailStateAsleep));
        case MailState::Starting:
            return std::string(text(lang, Str::MailStateStarting));
        case MailState::Running:
            return std::string(text(lang, Str::MailStateRunning));
        case MailState::Failed:
            return std::string(text(lang, Str::MailStateFailed));
    }
    return {};
}

MailPill mail_pill(MailState state) {
    switch (state) {
        case MailState::Running:
            return MailPill::Good;
        case MailState::Asleep:
        case MailState::Starting:
            return MailPill::Waiting;
        case MailState::Failed:
            return MailPill::Bad;
        case MailState::NotInstalled:
            break;
    }
    return MailPill::Neutral;
}

std::string mail_smtp_text(const MailStatus& status) { return "127.0.0.1 : " + std::to_string(status.smtp_port); }

std::string mail_smtp_copy(const MailStatus& status) { return "127.0.0.1:" + std::to_string(status.smtp_port); }

std::string mail_count_text(const MailStatus& status, Lang lang) {
    if (status.stale) {
        return {};
    }
    const std::string size = human_bytes(status.size_bytes);
    if (status.total == 1) {
        return fill(lang, Str::MailCountOne, {size});
    }
    return fill(lang, Str::MailCount, {std::to_string(status.total), size});
}

std::string mail_remove_emails_error(const json& result, Lang lang) {
    if (!result.is_object()) {
        return {};
    }
    const bool said_no = result.contains("emails_deleted") && result["emails_deleted"].is_boolean() &&
                         !result["emails_deleted"].get<bool>();
    const std::string error = mail_plain_subject(text_of(result, "emails_error"), kMailErrorMaxChars);
    if (!said_no && error.empty()) {
        return {};
    }
    // Kept on purpose: another Mailpit version is still installed and uses them.
    if (error.empty() && result.contains("emails_kept")) {
        return {};
    }
    if (error.empty()) {
        return std::string(text(lang, Str::MailRemoveEmailsFailedNoWhy));
    }
    return fill(lang, Str::MailRemoveEmailsFailed, {error});
}

std::string mail_not_served_text(const MailStatus& status, Lang lang) {
    if (status.inbox_served || !status.installed) {
        return {};
    }
    const std::string address = mail_inbox_address(status);
    if (status.inbox_blocked_by.empty()) {
        return fill(lang, Str::MailNotServedNoWhy, {address});
    }
    // A project's name is typed text: it follows its own first letter.
    const std::string who = is_rtl(lang) ? isolate_auto(status.inbox_blocked_by) : status.inbox_blocked_by;
    return fill_pattern(text(lang, Str::MailNotServed), false, {is_rtl(lang) ? isolate_ltr(address) : address, who});
}

bool mail_php(const MailStatus& status, const MailChanges& changes) {
    return changes.php.value_or(status.settings.php);
}
bool mail_notices(const MailStatus& status, const MailChanges& changes) {
    return changes.notices.value_or(status.settings.notices);
}
bool mail_keep_running(const MailStatus& status, const MailChanges& changes) {
    return changes.keep_running.value_or(status.settings.keep_running);
}

namespace {

void toggle(std::optional<bool>& change, bool saved) {
    const bool wanted = !change.value_or(saved);
    if (wanted != saved) {
        change = wanted;
    } else {
        change.reset();  // back to what's saved
    }
}

}  // namespace

void mail_toggle_php(const MailStatus& status, MailChanges& changes) { toggle(changes.php, status.settings.php); }
void mail_toggle_notices(const MailStatus& status, MailChanges& changes) {
    toggle(changes.notices, status.settings.notices);
}
void mail_toggle_keep_running(const MailStatus& status, MailChanges& changes) {
    toggle(changes.keep_running, status.settings.keep_running);
}

std::string mail_max_text(const MailStatus& status, const MailChanges& changes) {
    return changes.max ? *changes.max : std::to_string(status.settings.max);
}

void mail_set_max_text(const MailStatus& status, MailChanges& changes, const std::string& typed) {
    if (typed == std::to_string(status.settings.max)) {
        changes.max.reset();
    } else {
        changes.max = typed;
    }
}

bool mail_max_bad(const MailChanges& changes) { return changes.max && !parse_max(*changes.max); }

json mail_set_params(const MailChanges& changes) {
    json params = json::object();
    if (changes.php) {
        params["php"] = *changes.php;
    }
    if (changes.notices) {
        params["notices"] = *changes.notices;
    }
    if (changes.keep_running) {
        params["keep_running"] = *changes.keep_running;
    }
    if (changes.max) {
        if (const auto n = parse_max(*changes.max)) {
            params["max"] = *n;
        }
    }
    return params;
}

void mail_apply(MailStatus& status, const MailChanges& changes) {
    status.settings.php = mail_php(status, changes);
    status.settings.notices = mail_notices(status, changes);
    status.settings.keep_running = mail_keep_running(status, changes);
    if (changes.max) {
        if (const auto n = parse_max(*changes.max)) {
            status.settings.max = *n;
        }
    }
}

std::string mail_footer_text(std::size_t php_projects, Lang lang) {
    if (php_projects == 0) {
        return std::string(text(lang, Str::MailSaveApplies));
    }
    const std::string who = php_projects == 1 ? std::string(text(lang, Str::SettingsOneProject))
                                              : fill(lang, Str::SettingsProjects, {std::to_string(php_projects)});
    return fill_pattern(text(lang, Str::MailSaveAppliesPhp), false, {who});
}

bool mail_link_shown(const std::optional<MailStatus>& status) { return status && status->installed; }

std::string mail_link_text(const MailStatus& status, Lang lang) {
    if (status.unread == 0 || status.stale) {
        return std::string(text(lang, Str::MailLink));
    }
    return fill_pattern(text(lang, Str::MailLinkNew), false, {std::to_string(status.unread)});
}

// --- the toast ----------------------------------------------------------------------

namespace {

// One code point read from UTF-8 at `i`: length 0 when it isn't a whole, valid one
// (stray or missing continuation bytes, overlong forms, surrogates, above U+10FFFF).
struct Decoded {
    std::size_t len = 0;
    char32_t cp = 0;
};

Decoded decode_at(std::string_view s, std::size_t i) {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
        return {1, b};
    }
    const std::size_t len = (b & 0xE0U) == 0xC0U ? 2 : (b & 0xF0U) == 0xE0U ? 3 : (b & 0xF8U) == 0xF0U ? 4 : 0;
    if (len == 0 || i + len > s.size()) {
        return {};
    }
    char32_t cp = len == 2 ? (b & 0x1FU) : len == 3 ? (b & 0x0FU) : (b & 0x07U);
    for (std::size_t k = 1; k < len; ++k) {
        const auto c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0U) != 0x80U) {
            return {};
        }
        cp = (cp << 6U) | (c & 0x3FU);
    }
    const char32_t least = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
    if (cp < least || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        return {};
    }
    return {len, cp};
}

// Characters that change how a line reads or hide in it: direction marks and
// embeddings, Arabic letter mark, C1 controls, line/paragraph separators,
// zero-width characters, word joiner and the byte-order mark.
bool is_invisible(char32_t cp) {
    return cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) ||
           cp == 0x061C || (cp >= 0x80 && cp <= 0x9F) || cp == 0x2028 || cp == 0x2029 || (cp >= 0x200B && cp <= 0x200D) ||
           cp == 0x2060 || cp == 0xFEFF;
}

}  // namespace

std::string mail_plain_subject(std::string_view subject, std::size_t max_chars) {
    std::string out;
    std::size_t chars = 0;
    bool space = false;
    bool cut = false;
    for (std::size_t i = 0; i < subject.size();) {
        const Decoded d = decode_at(subject, i);
        if (d.len == 0) {
            ++i;  // broken UTF-8 is left out
            continue;
        }
        const std::string_view seq = subject.substr(i, d.len);
        i += d.len;
        if (is_invisible(d.cp)) {
            continue;
        }
        if (d.cp <= 0x20 || d.cp == 0x7F || d.cp == 0xA0) {
            space = !out.empty();  // controls and spaces (also no-break): one space, never first
            continue;
        }
        if (chars >= max_chars) {
            cut = true;
            break;
        }
        if (space) {
            out += ' ';
            ++chars;
            space = false;
            if (chars >= max_chars) {
                cut = true;
                break;
            }
        }
        out += seq;
        ++chars;
    }
    if (cut) {
        out += "\xE2\x80\xA6";  // …
    }
    return out;
}

std::optional<MailToastText> mail_notice_step(MailNotice& notice, const std::optional<MailStatus>& status,
                                              std::int64_t now_ms, Lang lang, bool blocked) {
    if (status && status->stale) {
        return std::nullopt;  // no real answer this time: nothing changes
    }
    if (!status || !status->installed) {
        // Not connected or not installed: forget, so a restart isn't missed and
        // the first answer after this only records notice_seq.
        notice = MailNotice{};
        return std::nullopt;
    }
    if (!notice.seen || status->notice_seq < *notice.seen) {
        // The first answer, or the service started over: where it is now is the start.
        notice.seen = status->notice_seq;
        notice.pending = 0;
        notice.unread = status->unread;
        return std::nullopt;
    }
    if (status->notice_seq > *notice.seen) {
        notice.seen = status->notice_seq;
        if (status->settings.notices) {
            std::uint64_t batch = 1;
            if (status->latest) {
                batch = static_cast<std::uint64_t>(std::max(1, status->latest->new_in_batch));
            }
            if (status->unread > notice.unread) {
                batch = std::max(batch, status->unread - notice.unread);  // several batches in one poll
            }
            notice.pending += batch;
            // The newest email's own words; none given: not the previous email's.
            notice.subject = status->latest ? status->latest->subject : std::string();
            notice.to = status->latest ? status->latest->to : std::string();
        }
    }
    notice.unread = status->unread;
    if (!status->settings.notices) {
        notice.pending = 0;  // switched off: nothing is held back
        return std::nullopt;
    }
    if (notice.pending == 0 || blocked) {
        return std::nullopt;
    }
    if (notice.last_toast_ms && now_ms - *notice.last_toast_ms < kMailToastGapMs) {
        return std::nullopt;  // too soon: merged into the next
    }
    // Typed by a stranger: it follows its own first letter, and never joins the sentence's direction.
    std::string subject = mail_plain_subject(notice.subject);
    if (subject.empty()) {
        subject = std::string(text(lang, Str::MailNoSubject));
    }
    const std::string shown_subject = isolate_auto(subject);
    MailToastText out;
    if (notice.pending > 1) {
        out.title = fill_pattern(text(lang, Str::MailToastMany), false, {std::to_string(notice.pending)});
        out.detail = fill_pattern(text(lang, Str::MailToastLatest), false, {shown_subject});
    } else {
        out.title = fill_pattern(text(lang, Str::MailToastOne), false, {shown_subject});
        const std::string to = mail_plain_subject(notice.to, 120);
        if (!to.empty()) {
            out.detail = fill_pattern(text(lang, Str::MailToastTo), false, {isolate_ltr(to)});
        }
    }
    notice.pending = 0;
    notice.last_toast_ms = now_ms;
    return out;
}

}  // namespace floserver::ui

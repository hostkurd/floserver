// Mail's model (src/ui-win32/model/mail_page.h, D27): mail.status read, the
// Settings page's words, changes until Save, the status line's button and
// when a new email earns a toast.

#include "model/first_run.h"
#include "model/main_access.h"
#include "model/mail_page.h"
#include "model/runtimes_page.h"
#include "model/settings_access.h"
#include "model/toast.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

using namespace floserver::ui;
using nlohmann::json;

namespace {

json status_reply() {
    return json::parse(R"({
        "installed": true, "version": "1.27.3", "state": "asleep", "smtp_port": 1025,
        "inbox_url": "https://mail.localhost", "inbox_served": true,
        "unread": 3, "total": 12, "size_bytes": 3565158, "notice_seq": 7,
        "latest": {"subject": "Reset your password", "to": "customer@example.com", "new_in_batch": 1},
        "settings": {"php": true, "notices": true, "keep_running": false, "max": 500}})");
}

MailStatus status_with_seq(std::uint64_t seq, int batch = 1) {
    json j = status_reply();
    j["notice_seq"] = seq;
    j["latest"]["new_in_batch"] = batch;
    return *mail_status_from_reply(j);
}

}  // namespace

TEST_CASE("mail.status is read, with the defaults for what's missing") {
    const auto s = mail_status_from_reply(status_reply());
    REQUIRE(s);
    CHECK(s->installed);
    CHECK(s->version == "1.27.3");
    CHECK(s->state == MailState::Asleep);
    CHECK(s->smtp_port == 1025);
    CHECK(s->unread == 3);
    CHECK(s->total == 12);
    CHECK(s->notice_seq == 7);
    REQUIRE(s->latest);
    CHECK(s->latest->subject == "Reset your password");
    CHECK(s->latest->new_in_batch == 1);
    CHECK(s->settings.php);
    CHECK_FALSE(s->settings.keep_running);
    CHECK(s->settings.max == 500);

    // Not installed, whatever else it says.
    const auto none = mail_status_from_reply(json::parse(R"({"installed": false, "state": "running"})"));
    REQUIRE(none);
    CHECK_FALSE(none->installed);
    CHECK(none->state == MailState::NotInstalled);
    CHECK(none->inbox_url == "https://mail.localhost");
    CHECK(none->inbox_served);
    CHECK_FALSE(none->latest);

    // Odd values don't break it.
    const auto odd = mail_status_from_reply(json::parse(
        R"({"installed": true, "state": "bogus", "smtp_port": -4, "unread": "x", "latest": 5, "settings": {"max": 0}})"));
    REQUIRE(odd);
    CHECK(odd->state == MailState::NotInstalled);
    CHECK(odd->smtp_port == 1025);
    CHECK(odd->unread == 0);
    CHECK_FALSE(odd->latest);
    CHECK(odd->settings.max == 500);
    CHECK_FALSE(mail_status_from_reply(json::array()));
    CHECK_FALSE(mail_not_installed().installed);
}

TEST_CASE("mail: the inbox address that's opened is a .localhost one") {
    MailStatus s = *mail_status_from_reply(status_reply());
    CHECK(mail_inbox_address(s) == "https://mail.localhost");
    s.inbox_url = "http://mail.localhost:8080/";
    CHECK(mail_inbox_address(s) == "http://mail.localhost:8080");
    s.inbox_url = "https://evil.example.com";
    CHECK(mail_inbox_address(s) == "https://mail.localhost");
    s.inbox_url = "javascript:alert(1)";
    CHECK(mail_inbox_address(s) == "https://mail.localhost");
}

TEST_CASE("mail: the state's words, the lines of the page") {
    CHECK(mail_state_text(MailState::Asleep, Lang::En) == "Asleep");
    CHECK(mail_state_text(MailState::Running, Lang::En) == "Running");
    CHECK(mail_state_text(MailState::Starting, Lang::En) == "Starting…");
    CHECK(mail_state_text(MailState::NotInstalled, Lang::En) == "Not installed");
    CHECK(mail_state_text(MailState::Failed, Lang::En) == "Failed");
    CHECK(mail_pill(MailState::Running) == MailPill::Good);
    CHECK(mail_pill(MailState::Asleep) == MailPill::Waiting);
    CHECK(mail_pill(MailState::Failed) == MailPill::Bad);
    CHECK(mail_pill(MailState::NotInstalled) == MailPill::Neutral);

    MailStatus s = *mail_status_from_reply(status_reply());
    CHECK(mail_smtp_text(s) == "127.0.0.1 : 1025");
    CHECK(mail_smtp_copy(s) == "127.0.0.1:1025");
    CHECK(mail_count_text(s, Lang::En) == "12 emails · 3.4 MB on disk");
    s.total = 1;
    CHECK(mail_count_text(s, Lang::En).starts_with("1 email · "));
    s.total = 0;
    s.size_bytes = 0;
    CHECK(mail_count_text(s, Lang::En).starts_with("0 emails · "));

    // The address is held by a project called mail.
    CHECK(mail_not_served_text(s, Lang::En).empty());
    s.inbox_served = false;
    s.inbox_blocked_by = "mail";
    CHECK(mail_not_served_text(s, Lang::En) ==
          "https://mail.localhost isn't served: the project “mail” has that address. Rename the project to use it.");
    s.inbox_blocked_by.clear();
    CHECK(mail_not_served_text(s, Lang::En) == "https://mail.localhost isn't served right now.");
    s.installed = false;
    CHECK(mail_not_served_text(s, Lang::En).empty());
}

TEST_CASE("mail: changes until Save, and what Save sends") {
    const MailStatus s = *mail_status_from_reply(status_reply());
    MailChanges c;
    CHECK(c.empty());
    CHECK(mail_php(s, c));
    CHECK_FALSE(mail_keep_running(s, c));
    CHECK(mail_max_text(s, c) == "500");

    mail_toggle_php(s, c);
    CHECK_FALSE(mail_php(s, c));
    CHECK_FALSE(c.empty());
    mail_toggle_php(s, c);  // back to what's saved: no change
    CHECK(c.empty());
    mail_toggle_keep_running(s, c);
    mail_toggle_notices(s, c);
    CHECK(mail_keep_running(s, c));
    CHECK_FALSE(mail_notices(s, c));

    mail_set_max_text(s, c, "250");
    CHECK(mail_max_text(s, c) == "250");
    CHECK_FALSE(mail_max_bad(c));
    CHECK(mail_set_params(c) == json({{"notices", false}, {"keep_running", true}, {"max", 250}}));
    mail_set_max_text(s, c, "500");  // as saved
    CHECK_FALSE(c.max);

    for (const char* bad : {"", "0", "abc", "-5", "12.5", "100001", " 5", "99999999"}) {
        CAPTURE(bad);
        MailChanges b;
        mail_set_max_text(s, b, bad);
        CHECK(mail_max_bad(b));
        CHECK_FALSE(mail_set_params(b).contains("max"));  // never sent
    }
    for (const char* good : {"1", "100000", "42"}) {
        MailChanges b;
        mail_set_max_text(s, b, good);
        CHECK_FALSE(mail_max_bad(b));
    }

    // Once saved the status shows what was saved.
    MailStatus saved = s;
    mail_set_max_text(s, c, "250");
    mail_apply(saved, c);
    CHECK_FALSE(saved.settings.notices);
    CHECK(saved.settings.keep_running);
    CHECK(saved.settings.max == 250);
    CHECK(saved.settings.php);
}

TEST_CASE("mail: the footer says how many projects restart") {
    CHECK(mail_footer_text(0, Lang::En) == "Applies when saved.");
    CHECK(mail_footer_text(1, Lang::En) ==
          "Applies when saved. Changing the first switch restarts PHP for 1 project (under a second).");
    CHECK(mail_footer_text(3, Lang::En) ==
          "Applies when saved. Changing the first switch restarts PHP for 3 projects (under a second).");
    CHECK(mail_footer_text(3, Lang::Ar).find("3") != std::string::npos);
}

TEST_CASE("mail: the status line's button") {
    CHECK_FALSE(mail_link_shown(std::nullopt));
    CHECK_FALSE(mail_link_shown(mail_not_installed()));
    MailStatus s = *mail_status_from_reply(status_reply());
    CHECK(mail_link_shown(s));
    CHECK(mail_link_text(s, Lang::En) == "Mail · 3 new");
    CHECK(mail_link_text(s, Lang::Ar) == "البريد · 3 جديد");
    CHECK(mail_link_text(s, Lang::Ku) == "ئیمەیل · 3 نوێ");
    s.unread = 0;
    CHECK(mail_link_text(s, Lang::En) == "Mail");
    CHECK(mail_link_text(s, Lang::Ar) == "البريد");

    // For screen readers: a button that says what it does.
    for (const Lang lang : {Lang::En, Lang::Ar, Lang::Ku}) {
        const AccessNode n = mail_link_node(mail_link_text(*mail_status_from_reply(status_reply()), lang), lang);
        CHECK(n.id == "mail/link");
        CHECK(n.role == AccessRole::Button);
        CHECK(n.focusable);
        CHECK(n.name == mail_link_text(*mail_status_from_reply(status_reply()), lang));
        CHECK(n.help == std::string(text(lang, Str::MailLinkHelp)));
    }
}

TEST_CASE("mail: a subject as plain text for one line") {
    CHECK(mail_plain_subject("Reset your password") == "Reset your password");
    CHECK(mail_plain_subject("  a\r\n\tb   c \x01 d  ") == "a b c d");
    CHECK(mail_plain_subject("").empty());
    CHECK(mail_plain_subject("\n\n").empty());
    // Direction marks can't reorder the line.
    CHECK(mail_plain_subject("ab\xE2\x80\xAE" "cd\xE2\x80\xAC") == "abcd");
    // Cut with an ellipsis, never in the middle of a character.
    CHECK(mail_plain_subject("abcdefghij", 5) == "abcde\xE2\x80\xA6");
    CHECK(mail_plain_subject("abcde", 5) == "abcde");
    CHECK(mail_plain_subject("\xD8\xA3\xD8\xA8\xD8\xAC\xD8\xAF", 2) == "\xD8\xA3\xD8\xA8\xE2\x80\xA6");
    const std::string long_subject(300, 'x');
    CHECK(mail_plain_subject(long_subject).size() == kMailSubjectMaxChars + 3);
    // Broken UTF-8 is left out.
    CHECK(mail_plain_subject("a\xFF" "b") == "ab");
    // HTML stays as the plain text it is.
    CHECK(mail_plain_subject("<b>Hi</b> & bye") == "<b>Hi</b> & bye");
}

TEST_CASE("mail toast: never on the first answer; then when notice_seq grows") {
    MailNotice n;
    // Nothing yet, or not installed: nothing seen.
    CHECK_FALSE(mail_notice_step(n, std::nullopt, 0, Lang::En));
    CHECK_FALSE(n.seen);
    CHECK_FALSE(mail_notice_step(n, mail_not_installed(), 0, Lang::En));
    CHECK_FALSE(n.seen);

    // The first answer after the program starts only sets the start, even with mail waiting.
    CHECK_FALSE(mail_notice_step(n, status_with_seq(7), 1000, Lang::En));
    REQUIRE(n.seen);
    CHECK(*n.seen == 7);
    // The same again: nothing.
    CHECK_FALSE(mail_notice_step(n, status_with_seq(7), 3000, Lang::En));

    // A new batch: one email says its subject and where it went.
    const auto one = mail_notice_step(n, status_with_seq(8), 5000, Lang::En);
    REQUIRE(one);
    CHECK(one->title == "New email: Reset your password");
    CHECK(one->detail == "to customer@example.com");
    CHECK_FALSE(mail_notice_step(n, status_with_seq(8), 7000, Lang::En));  // said once
    // The same words in Arabic and Kurdish (the subject isolated, in right-to-left).
    MailNotice ar;
    (void)mail_notice_step(ar, status_with_seq(1), 0, Lang::Ar);
    const auto in_ar = mail_notice_step(ar, status_with_seq(2), 100, Lang::Ar);
    REQUIRE(in_ar);
    CHECK(in_ar->title.find("Reset your password") != std::string::npos);
    CHECK(in_ar->title.starts_with("رسالة جديدة: "));
    CHECK(in_ar->detail.starts_with("إلى "));
    MailNotice ku;
    (void)mail_notice_step(ku, status_with_seq(1), 0, Lang::Ku);
    const auto in_ku = mail_notice_step(ku, status_with_seq(2), 100, Lang::Ku);
    REQUIRE(in_ku);
    CHECK(in_ku->title.starts_with("ئیمەیلی نوێ: "));
    CHECK(in_ku->detail.starts_with("بۆ "));
}

TEST_CASE("mail toast: a batch says how many; bursts are merged, one toast in 5 seconds") {
    MailNotice n;
    (void)mail_notice_step(n, status_with_seq(1), 0, Lang::En);
    const auto many = mail_notice_step(n, status_with_seq(2, 4), 10000, Lang::En);
    REQUIRE(many);
    CHECK(many->title == "4 new emails");
    CHECK(many->detail == "Latest: Reset your password");

    // Within 5 seconds of the last one: held, and merged with what comes next.
    CHECK_FALSE(mail_notice_step(n, status_with_seq(3), 12000, Lang::En));
    CHECK_FALSE(mail_notice_step(n, status_with_seq(4), 14000, Lang::En));
    CHECK(n.pending == 2);
    CHECK_FALSE(mail_notice_step(n, status_with_seq(4), 14999, Lang::En));
    const auto merged = mail_notice_step(n, status_with_seq(4), 15000, Lang::En);
    REQUIRE(merged);
    CHECK(merged->title == "2 new emails");
    CHECK(n.pending == 0);
    CHECK_FALSE(mail_notice_step(n, status_with_seq(4), 30000, Lang::En));

    // After a quiet spell one comes at once; a held one on its own says its subject again.
    CHECK(mail_notice_step(n, status_with_seq(5), 31000, Lang::En));
    CHECK_FALSE(mail_notice_step(n, status_with_seq(6), 32000, Lang::En));
    const auto single = mail_notice_step(n, status_with_seq(6), 36000, Lang::En);
    REQUIRE(single);
    CHECK(single->title == "New email: Reset your password");
}

TEST_CASE("mail toast: switched off, empty subjects, and a service that started over") {
    MailNotice n;
    (void)mail_notice_step(n, status_with_seq(1), 0, Lang::En);
    // Notices off in Settings: nothing, and nothing held back for later.
    MailStatus off = status_with_seq(2);
    off.settings.notices = false;
    CHECK_FALSE(mail_notice_step(n, off, 10000, Lang::En));
    CHECK(n.pending == 0);
    CHECK(*n.seen == 2);
    // Switched back on: only what comes after.
    CHECK_FALSE(mail_notice_step(n, status_with_seq(2), 20000, Lang::En));

    // An email with no subject, and a long one.
    MailStatus empty = status_with_seq(3);
    empty.latest->subject = "  ";
    const auto none = mail_notice_step(n, empty, 30000, Lang::En);
    REQUIRE(none);
    CHECK(none->title == "New email: (no subject)");
    MailStatus longer = status_with_seq(4);
    longer.latest->subject = std::string(500, 'a');
    const auto cut = mail_notice_step(n, longer, 40000, Lang::En);
    REQUIRE(cut);
    CHECK(cut->title.ends_with("\xE2\x80\xA6"));
    CHECK(cut->title.size() < 120);

    // A seq that went back: the service started over; that's the new start.
    CHECK_FALSE(mail_notice_step(n, status_with_seq(0), 50000, Lang::En));
    CHECK(*n.seen == 0);
    CHECK(mail_notice_step(n, status_with_seq(1), 60000, Lang::En));
}

TEST_CASE("mail toast: the card's look") {
    Toast t = toast_mail({}, "New email: Hi", "to a@b.c", "Open");
    CHECK(t.mail);
    CHECK(t.text == "New email: Hi");
    CHECK(t.detail == "to a@b.c");
    CHECK(t.kind == ToastKind::Info);
    // It has a button, so it stays until it's closed (like a backup's).
    CHECK_FALSE(toast_timer_runs(t));
    CHECK(toast_after(t, ToastEvent::TimeUp) == t);
    CHECK(toast_after(t, ToastEvent::Close) == Toast{});
    // A new one takes its place; nothing to say keeps what's shown.
    const Toast next = toast_mail(t, "3 new emails", "Latest: Hi", "Open");
    CHECK(next.text == "3 new emails");
    CHECK(toast_mail(t, "", "x", "Open") == t);
    // Any other message drops the mail look.
    CHECK_FALSE(toast_with(t, "Saved.", ToastKind::Info).mail);
    CHECK(toast_with(t, "Saved.", ToastKind::Info).detail.empty());

    // For screen readers: both lines, the button and the close mark.
    for (const Lang lang : {Lang::En, Lang::Ar, Lang::Ku}) {
        const ToastNodes nodes = toast_nodes(access_join(lang, {t.text, t.detail}), lang, std::string(text(lang, Str::MailToastOpen)));
        CHECK(nodes.text.name.find("New email: Hi") != std::string::npos);
        CHECK(nodes.text.name.find("to a@b.c") != std::string::npos);
        CHECK(nodes.action.id == "toast/action");
        CHECK(nodes.action.name == std::string(text(lang, Str::MailToastOpen)));
        CHECK(nodes.close.id == "toast/close");
    }
}

TEST_CASE("mail: Mailpit in Runtimes' Tools card, and what Remove sends") {
    const json reply = json::parse(R"({
        "installed": [{"name": "mailpit", "version": "1.27.3", "folder": "1.27.3", "used_by": []},
                      {"name": "phpmyadmin", "version": "5.2.3", "folder": "5.2.3", "used_by": []}],
        "list": {"sequence": 1}, "available": [], "defaults": {}})");
    const Installed installed{{"mailpit", {"1.27.3"}}, {"phpmyadmin", {"5.2.3"}}};
    const RuntimesPage page = runtimes_page(reply, &installed, json(), Lang::En);
    const auto tools = std::find_if(page.groups.begin(), page.groups.end(),
                                    [](const RuntimeGroup& g) { return g.title == "Tools"; });
    REQUIRE(tools != page.groups.end());
    REQUIRE(tools->rows.size() == 2);
    CHECK(tools->rows[0].label == "phpMyAdmin 5.2.3");
    CHECK(tools->rows[1].label == "Mailpit 1.27.3");
    CHECK(tools->rows[1].name == "mailpit");
    CHECK(tools->rows[1].action == RowAction::Remove);
    CHECK(tools->rows[1].folder == "1.27.3");
    // Mailpit's row isn't given phpMyAdmin's served line.
    CHECK(tools->rows[1].usage.empty());
    CHECK(runtime_label("mailpit") == "Mailpit");

    // Offered to download beside it.
    const json offer = json::parse(R"({
        "installed": [], "list": {"sequence": 1}, "defaults": {},
        "available": [{"name": "mailpit", "version": "1.27.3", "variant": "", "size": 15000000}]})");
    const RuntimesPage page2 = runtimes_page(offer, nullptr, json(), Lang::En);
    REQUIRE(page2.groups.size() == 1);
    CHECK(page2.groups[0].title == "Tools");
    CHECK(page2.groups[0].rows[0].action == RowAction::Download);

    // Remove: Mailpit's says whether the emails stay; the others' don't change.
    CHECK(runtimes_remove_params("mailpit", "1.27.3", true) ==
          json({{"name", "mailpit"}, {"version", "1.27.3"}, {"keep_emails", true}}));
    CHECK(runtimes_remove_params("mailpit", "1.27.3", false) ==
          json({{"name", "mailpit"}, {"version", "1.27.3"}, {"keep_emails", false}}));
    CHECK(runtimes_remove_params("php", "8.5.1", true) == json({{"name", "php"}, {"version", "8.5.1"}}));
}

TEST_CASE("mail: first run offers Mailpit, ticked, beside Composer") {
    const json reply = json::parse(R"({
        "installed": [], "list": {"sequence": 12},
        "available": [
            {"name": "php", "version": "8.5.12", "variant": "nts", "size": 31000000},
            {"name": "composer", "version": "2.10.3", "variant": "", "size": 3000000},
            {"name": "mailpit", "version": "1.27.2", "variant": "", "size": 14000000},
            {"name": "mailpit", "version": "1.27.3", "variant": "", "size": 15000000},
            {"name": "python", "version": "3.13.9", "variant": "", "size": 25000000},
            {"name": "nginx", "version": "1.30.6", "variant": "", "size": 2000000}
        ]})");
    const std::vector<FirstRunOffer> offers = first_run_offers(reply);
    REQUIRE(offers.size() == 5);
    CHECK(offers[1].name == "composer");
    CHECK(offers[2].name == "mailpit");
    CHECK(offers[2].version == "1.27.3");
    CHECK(offers[2].label == "Mailpit 1.27.3");
    CHECK(offers[2].what == Str::FirstRunWhatMailpit);
    CHECK(offers[2].size == 15000000U);
    CHECK(offers[2].ticked);
    CHECK_FALSE(offers[2].always);
    const std::vector<bool> ticks = first_run_default_ticks(offers);
    CHECK(ticks == std::vector<bool>{true, true, true, false, true});
    const std::vector<Download> wanted = first_run_downloads(offers, ticks);
    CHECK(std::find(wanted.begin(), wanted.end(), Download{"mailpit", "1.27.3"}) != wanted.end());
    CHECK(text(Lang::En, Str::FirstRunWhatMailpit) == "catches the emails your sites send");
}

TEST_CASE("mail: Settings' sidebar has Mail after Database; every new string is in every language") {
    REQUIRE(kSettingsSidebar.size() == 9);
    CHECK(kSettingsSidebar[4] == Str::SettingsDatabase);
    CHECK(kSettingsSidebar[5] == Str::SettingsMail);
    CHECK(kSettingsSidebar[6] == Str::SettingsApache);
    CHECK(text(Lang::En, Str::SettingsMail) == "Mail");
    for (int i = static_cast<int>(Str::SettingsMail); i < static_cast<int>(Str::Count_); ++i) {
        const auto key = static_cast<Str>(i);
        CAPTURE(i);
        for (const Lang lang : {Lang::En, Lang::Ar, Lang::Ku}) {
            CHECK_FALSE(text(lang, key).empty());
        }
        // Western digits only, in every language.
        for (const Lang lang : {Lang::Ar, Lang::Ku}) {
            const std::string s(text(lang, key));
            CHECK(s.find("\xD9\xA0") == std::string::npos);  // no Arabic-Indic zero ... nine
            CHECK(s.find("\xD9\xA1") == std::string::npos);
            CHECK(s.find("\xDB\xB1") == std::string::npos);
        }
    }
}

namespace {

AccessNode by_id(const AccessTree& t, const std::string& id) {
    const AccessNode* n = t.node(t.find(id));
    REQUIRE(n != nullptr);
    return *n;
}

PageItem card(std::string id, AccessRect r) {
    PageItem item;
    item.node.id = std::move(id);
    item.node.role = AccessRole::Group;
    item.node.bounds = r;
    item.container = true;
    return item;
}

PageItem control(AccessNode n, AccessRect r) {
    PageItem item;
    item.node = std::move(n);
    item.node.bounds = r;
    item.absorbs = true;
    return item;
}

PageText words(std::string s, AccessRect r, TextStyle style = TextStyle::Body) { return {r, std::move(s), style, {}}; }

// Settings > Mail's Options card, as the window lays it out: the title, a switch with its note, Copy buttons.
AccessTree mail_page(Lang lang, bool php_on) {
    AccessTree t;
    AccessNode pane;
    pane.id = "page";
    pane.role = AccessRole::Pane;
    const int page = t.add(-1, pane);
    std::vector<PageItem> items;
    items.push_back(card("mail/card/addresses", {200, 60, 800, 200}));
    items.push_back(card("mail/card/options", {200, 220, 800, 360}));
    AccessNode copy = button_node("mail/copy-inbox",
                                  access_join(lang, {std::string(text(lang, Str::DbCopy)), std::string(text(lang, Str::MailInbox))}));
    items.push_back(control(copy, {600, 100, 660, 130}));
    AccessNode copy_smtp = button_node(
        "mail/copy-smtp", access_join(lang, {std::string(text(lang, Str::DbCopy)), std::string(text(lang, Str::MailSmtpServer))}));
    items.push_back(control(copy_smtp, {600, 140, 660, 170}));
    items.push_back(control(switch_node("mail/switch/0", {}, php_on, true, lang), {216, 260, 784, 300}));
    const std::vector<PageText> texts{
        words(std::string(text(lang, Str::SettingsMail)), {200, 10, 800, 38}, TextStyle::Title),
        words(std::string(text(lang, Str::MailAddresses)), {216, 70, 784, 94}, TextStyle::Heading),
        words(std::string(text(lang, Str::MailOptions)), {216, 230, 784, 254}, TextStyle::Heading),
        words(std::string(text(lang, Str::MailPhp)), {216, 262, 700, 282}),
        words(std::string(text(lang, Str::MailPhpNote)), {216, 282, 700, 300}),
    };
    add_page(t, page, "mail/", std::move(items), texts, {196, 0, 820, 600}, lang);
    t.finish();
    return t;
}

}  // namespace

TEST_CASE("mail settings access: cards, switch and the two Copy buttons, in each language") {
    for (const Lang lang : {Lang::En, Lang::Ar, Lang::Ku}) {
        CAPTURE(static_cast<int>(lang));
        const AccessTree t = mail_page(lang, true);
        CHECK(by_id(t, "mail/card/addresses").name == std::string(text(lang, Str::MailAddresses)));
        CHECK(by_id(t, "mail/card/options").name == std::string(text(lang, Str::MailOptions)));
        // The two Copy buttons say what they copy.
        const AccessNode inbox = by_id(t, "mail/copy-inbox");
        const AccessNode smtp = by_id(t, "mail/copy-smtp");
        CHECK(inbox.name != smtp.name);
        CHECK(inbox.name.find(std::string(text(lang, Str::MailInbox))) != std::string::npos);
        CHECK(smtp.role == AccessRole::Button);
        // The switch is named by its label, helped by its note, and says its state.
        const AccessNode sw = by_id(t, "mail/switch/0");
        CHECK(sw.role == AccessRole::Switch);
        CHECK(sw.toggled == true);
        CHECK(access_text(sw.name) == access_text(text(lang, Str::MailPhp)));
        CHECK(access_text(sw.help) == access_text(text(lang, Str::MailPhpNote)));
    }
    CHECK(by_id(mail_page(Lang::En, false), "mail/switch/0").toggled == false);
    // The sidebar's tab for it.
    const AccessNode tab = section_node(5, text(Lang::En, kSettingsSidebar.at(5)), true, 9);
    CHECK(tab.id == "nav/5");
    CHECK(tab.name == "Mail");
    CHECK(tab.set_size == 9);
    CHECK(section_node(5, text(Lang::Ar, kSettingsSidebar.at(5)), false, 9).name == "البريد");
    CHECK(section_node(5, text(Lang::Ku, kSettingsSidebar.at(5)), false, 9).name == "ئیمەیل");
}

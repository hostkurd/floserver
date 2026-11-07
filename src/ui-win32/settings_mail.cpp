// Settings > Mail (D27, approved 7 Nov): Mailpit catches the emails the
// projects send. Its state and version, the two addresses (the inbox and the
// SMTP server), four options, and the emails (a test, delete all). Built as
// Settings > Apache is: one mail.status when the section opens (the main
// window's poll keeps the state and counts current meanwhile, so nothing
// runs here), the options wait for Save (one mail.set), and the test and the
// delete are sent at once.

#include "settings_window.h"

#include "main_window_layout.h"
#include "model/theme.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace floserver::ui {

using nlohmann::json;

namespace {

constexpr int kMailEditBase = 5000;  // mail_edits_[0], the emails kept
constexpr int kGap = 12;
constexpr int kCardPad = 16;
constexpr int kControlH = 30;
constexpr int kNoteH = 18;
constexpr int kLabelW = 140;
constexpr int kSwitchW = 40;
constexpr int kSwitchH = 20;
constexpr int kMaxBoxW = 80;

std::wstring w(std::string_view s) { return win::widen(s); }
bool is_empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

int text_height(HDC dc, std::wstring_view s, HFONT font, int width, bool rtl) {
    if (s.empty() || width <= 0) {
        return 0;
    }
    const win::Selected selected(dc, font);
    RECT r{0, 0, width, 0};
    DrawTextW(dc, s.data(), static_cast<int>(s.size()), &r,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | (rtl ? UINT{DT_RTLREADING} : 0U));
    return static_cast<int>(r.bottom - r.top);
}

constexpr std::array<Str, 3> kSwitchLabels = {Str::MailPhp, Str::MailNotices, Str::MailKeepRunning};
constexpr std::array<Str, 3> kSwitchNotes = {Str::MailPhpNote, Str::MailNoticesNote, Str::MailKeepRunningNote};

}  // namespace

// --- loading, saving, leaving ----------------------------------------------------------

bool SettingsWindow::settle_mail_changes(AfterSave then) {
    if (!mail_ || mail_changes_.empty()) {
        return true;
    }
    if (asking_ || mail_saving_) {
        return false;
    }
    asking_ = true;
    const SaveChoice choice = ask_to_save(hwnd_, lang_, std::string(text(lang_, Str::ApacheAskSave)));
    asking_ = false;
    modal_closed();
    if (quit_after_ask_) {
        PostMessageW(owner_, closed_message_, 1, 0);
        return false;
    }
    if (hwnd_ == nullptr) {
        return false;
    }
    switch (choice) {
        case SaveChoice::Save:
            if (save_mail()) {
                after_save_ = then;
            }
            return false;
        case SaveChoice::DontSave:
            discard_mail();
            return true;
        case SaveChoice::Cancel:
            break;
    }
    return false;
}

void SettingsWindow::load_mail() {
    mail_changes_ = {};
    mail_loading_ = !mail_.has_value();  // what the poll already said stays on show
    // The one request the section makes.
    worker_.database_call("mail.status", json::object());
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::save_mail() {
    if (!mail_ || mail_changes_.empty() || mail_saving_ || mail_max_bad(mail_changes_)) {
        return false;
    }
    if (!connected_) {
        show_message(fill(lang_, Str::PhpSaveFailed, {text(lang_, Str::ConnectionLost)}), true);
        return false;
    }
    mail_saving_ = true;
    set_mail_read_only(true);
    clear_toast();
    worker_.database_call("mail.set", mail_set_params(mail_changes_));
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void SettingsWindow::discard_mail() {
    if (mail_saving_) {
        return;
    }
    mail_changes_ = {};
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::mailpit_installing() const {
    if (starting_.contains(ServiceWorker::install_key(settings_install_key("mailpit", "")))) {
        return true;
    }
    if (std::any_of(jobs_.begin(), jobs_.end(), [](const RuntimeJob& j) { return j.name == "mailpit"; })) {
        return true;
    }
    // Installed, and the next poll hasn't said so yet.
    const auto it = installed_.find("mailpit");
    return it != installed_.end() && !it->second.empty() && mail_ && !mail_->installed;
}

void SettingsWindow::install_mailpit() {
    if (!connected_ || mailpit_installing()) {
        return;
    }
    // As Runtimes' Download: the newest Mailpit, followed as a job.
    const std::string key = settings_install_key("mailpit", "");
    starting_.insert(ServiceWorker::install_key(key));
    clear_toast();
    worker_.install(key, {{"mailpit", ""}});
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::copy_mail_text(std::string copy) {
    if (put_secret_on_clipboard(hwnd_, copy)) {
        show_message(std::string(text(lang_, Str::MailCopied)), false);
    }
}

void SettingsWindow::open_inbox() {
    if (!mail_) {
        return;
    }
    const std::string address = mail_inbox_address(*mail_);
    if (!open_address_in_browser(hwnd_, address)) {
        show_message(fill(lang_, Str::MailOpenFailed, {address}), true);
    }
}

void SettingsWindow::send_test_mail() {
    if (!connected_ || mail_busy_ || !mail_) {
        return;
    }
    mail_busy_ = true;
    clear_toast();
    worker_.database_call("mail.test", json::object());
    relayout();
}

void SettingsWindow::delete_all_mail() {
    if (!connected_ || mail_busy_ || !mail_ || asking_) {
        return;
    }
    asking_ = true;
    const bool yes = ask_delete_mails(hwnd_, lang_, fill(lang_, Str::MailDeleteAsk, {std::to_string(mail_->total)}));
    asking_ = false;
    modal_closed();
    if (hwnd_ == nullptr || !yes) {
        return;
    }
    mail_busy_ = true;
    clear_toast();
    worker_.database_call("mail.clear", json::object());
    relayout();
}

bool SettingsWindow::on_mail_answers(const Snapshot& snap) {
    bool close_now = false;
    const auto service_words = [this](const std::string& s) { return rtl_ ? isolate_ltr(s) : s; };
    for (const Snapshot::DbAnswer& answer : snap.db) {
        if (!answer.method.starts_with("mail.")) {
            continue;
        }
        const bool failed = !answer.error.empty() || !answer.reply;
        if (answer.method == "mail.status") {
            mail_loading_ = false;
            if (failed) {
                show_message(fill(lang_, Str::MailReadFailed, {service_words(answer.error)}), true);
            } else if (auto status = mail_status_from_reply(*answer.reply); status && !mail_saving_) {
                mail_ = std::move(status);
            }
        } else if (answer.method == "mail.set") {
            mail_saving_ = false;
            set_mail_read_only(false);
            if (failed) {
                after_save_ = AfterSave::Stay;  // refused: stay here with the reason shown
                show_message(fill(lang_, Str::PhpSaveFailed, {service_words(answer.error)}), true);
            } else {
                // The reply is only {"ok":true}: what was saved is what's shown until the poll says.
                if (mail_) {
                    mail_apply(*mail_, mail_changes_);
                }
                mail_changes_ = {};
                show_message(std::string(text(lang_, Str::PhpSaved)), false);
                const AfterSave then = std::exchange(after_save_, AfterSave::Stay);
                if (then == AfterSave::Close) {
                    close_now = true;
                } else if (then == AfterSave::Runtimes) {
                    open_section(after_save_section_);
                }
            }
        } else if (answer.method == "mail.test") {
            mail_busy_ = false;
            show_message(failed ? fill(lang_, Str::MailTestFailed, {service_words(answer.error)})
                                : std::string(text(lang_, Str::MailTestSent)),
                         failed);
        } else if (answer.method == "mail.clear") {
            mail_busy_ = false;
            if (failed) {
                show_message(fill(lang_, Str::MailDeleteFailed, {service_words(answer.error)}), true);
            } else {
                if (mail_) {
                    mail_->total = 0;
                    mail_->unread = 0;
                    mail_->size_bytes = 0;
                }
                show_message(std::string(text(lang_, Str::MailDeleted)), false);
            }
        }
    }
    return close_now;
}

// --- the number box --------------------------------------------------------------------

void SettingsWindow::make_mail_edits() {
    if (!mail_edits_.empty()) {
        return;
    }
    auto edit = std::make_unique<FieldEdit>(hwnd_, kMailEditBase, fonts_.button.get(), rtl_,
                                            [this](WPARAM key) { return on_edit_key(kMailEditBase, key); });
    edit->set_cue_color(palette(dark_).text_subtle);
    set_accessible_name(edit->hwnd(), text(lang_, Str::MailKeepLast));  // its label, as drawn over it
    mail_edits_.push_back(std::move(edit));
}

void SettingsWindow::set_mail_read_only(bool read_only) {
    for (const auto& edit : mail_edits_) {
        edit->set_read_only(read_only);
    }
}

void SettingsWindow::place_mail_edits() {
    if (section_ != Section::Mail || mail_edits_.empty()) {
        return;
    }
    FieldEdit& edit = *mail_edits_.front();
    const bool shown = mail_ && mail_->installed && !is_empty(geometry_.mail.max_box);
    // What's saved and what's typed, kept in the box (never while it's being typed in).
    if (mail_ && !edit.focused()) {
        if (const std::string want = mail_max_text(*mail_, mail_changes_); edit.text() != want) {
            edit.set_text(want);
        }
    }
    edit.place(geometry_.mail.max_box, px(8), px(20), shown);
}

// --- geometry ------------------------------------------------------------------------------

void SettingsWindow::layout_mail(HDC dc, Geometry& g, int x0, int x1, int y) const {
    MailGeometry& mg = g.mail;
    const auto measure = [&](std::string_view s, HFONT font) { return Canvas::measure(dc, w(s), font, rtl_); };
    const auto button_w = [&](std::string_view label) { return measure(label, fonts_.button.get()) + px(24); };
    const auto height = [&](std::string_view s, HFONT font, int width) { return text_height(dc, w(s), font, width, rtl_); };

    // The title and what the section does, with the state and the version beside them.
    const bool known = mail_.has_value();
    int right_w = 0;
    std::string pill_text;
    std::string version_text;
    if (known) {
        pill_text = mail_state_text(mail_->state, lang_);
        const int pill_w = measure(pill_text, fonts_.small_bold.get()) + px(20);
        if (mail_->installed && !mail_->version.empty()) {
            version_text = fill(lang_, Str::MailVersion, {mail_->version});
        }
        const int version_w = version_text.empty() ? 0 : measure(version_text, fonts_.caption.get()) + px(2);
        right_w = pill_w + (version_w > 0 ? px(10) + version_w : 0) + px(kGap);
    }
    const int hint_w = std::max(px(100), x1 - x0 - right_w);
    const int hint_h = height(text(lang_, Str::MailHint), fonts_.caption.get(), hint_w);
    g.title = {x0, y, x0 + hint_w, y + px(28)};
    g.hint = {x0, g.title.bottom, x0 + hint_w, g.title.bottom + hint_h};
    if (known) {
        const int pill_w = measure(pill_text, fonts_.small_bold.get()) + px(20);
        const int version_w = version_text.empty() ? 0 : measure(version_text, fonts_.caption.get()) + px(2);
        const int bottom = static_cast<int>(g.hint.bottom);
        if (version_w > 0) {
            mg.version = {x1 - version_w, bottom - px(22), x1, bottom};
        }
        const int pill_right = version_w > 0 ? static_cast<int>(mg.version.left) - px(10) : x1;
        mg.pill = {pill_right - pill_w, bottom - px(22), pill_right, bottom};
    }
    y = static_cast<int>(g.hint.bottom) + px(kGap);

    if (!known) {
        mg.note = {x0, y, x1, y + px(24)};
        g.content_h = static_cast<int>(mg.note.bottom) + scroll_ + px(8);
        return;
    }
    if (!mail_->installed) {
        const int nh = height(text(lang_, Str::MailNotInstalledNote), fonts_.button.get(), x1 - x0);
        mg.install_note = {x0, y, x1, y + nh};
        y += nh + px(kGap);
        if (mailpit_installing()) {
            // The install's progress, as a Runtimes row shows it.
            const auto job = std::find_if(jobs_.begin(), jobs_.end(), [](const RuntimeJob& j) { return j.name == "mailpit"; });
            const bool has_job = job != jobs_.end();
            mg.progress_text = {x0, y, x1, y + px(kControlH)};
            if (has_job && job_fraction(*job)) {
                mg.progress_bar = {x0, y + px(kControlH) + px(2), x0 + std::min(px(240), x1 - x0),
                                   y + px(kControlH) + px(6)};
                y += px(6);
            }
            y += px(kControlH);
        } else {
            mg.install = {x0, y, x0 + button_w(text(lang_, Str::MailInstall)) + px(12), y + px(kControlH)};
            y += px(kControlH);
        }
        g.content_h = y + scroll_ + px(16);
        return;
    }

    const int cx0 = x0 + px(kCardPad);
    const int cx1 = x1 - px(kCardPad);
    const int avail = cx1 - cx0;

    // Why it failed.
    if (mail_->state == MailState::Failed && !mail_->error.empty()) {
        const std::string words = fill(lang_, Str::MailFailed, {rtl_ ? isolate_ltr(mail_->error) : mail_->error});
        const int h = height(words, fonts_.caption.get(), x1 - x0 - px(24)) + px(14);
        mg.failed = {x0, y, x1, y + h};
        y += h + px(kGap);
    }

    // Addresses.
    int top = y + px(14);
    mg.addr_card = {x0, y, x1, y};
    mg.addr_title = {cx0, top, cx1, top + px(24)};
    int yy = static_cast<int>(mg.addr_title.bottom) + px(10);
    const int label_w = std::min(px(kLabelW), avail / 4);
    const int copy_w = button_w(text(lang_, Str::DbCopy));
    const int open_w = button_w(text(lang_, Str::MailOpenInbox));
    // Inbox: the link, Copy, and Open inbox (the main button).
    mg.inbox_label = {cx0, yy, cx0 + label_w, yy + px(kControlH)};
    mg.inbox_open = {std::max(cx0 + label_w, cx1 - open_w), yy, cx1, yy + px(kControlH)};
    mg.inbox_copy = {std::max(cx0 + label_w, static_cast<int>(mg.inbox_open.left) - px(8) - copy_w), yy,
                     static_cast<int>(mg.inbox_open.left) - px(8), yy + px(kControlH)};
    const std::string address = mail_inbox_address(*mail_);
    const int link_w = std::min(measure(address, fonts_.button.get()) + px(4),
                                std::max(px(40), static_cast<int>(mg.inbox_copy.left) - px(kGap) - cx0 - label_w));
    mg.inbox_link = {cx0 + label_w, yy, cx0 + label_w + link_w, yy + px(kControlH)};
    yy += px(kControlH) + px(kGap);
    // SMTP server: the address, what it needs, and Copy.
    mg.smtp_label = {cx0, yy, cx0 + label_w, yy + px(kControlH)};
    mg.smtp_copy = {std::max(cx0 + label_w, cx1 - copy_w), yy, cx1, yy + px(kControlH)};
    const int value_w = measure(mail_smtp_text(*mail_), fonts_.mono.get()) + px(4);
    mg.smtp_value = {cx0 + label_w, yy, cx0 + label_w + value_w, yy + px(kControlH)};
    const int note_left = static_cast<int>(mg.smtp_value.right) + px(kGap);
    mg.smtp_note = {note_left, yy, std::max(note_left, static_cast<int>(mg.smtp_copy.left) - px(kGap)), yy + px(kControlH)};
    yy += px(kControlH) + px(8);
    // Why the inbox address isn't served, when it isn't.
    if (const std::string why = mail_not_served_text(*mail_, lang_); !why.empty()) {
        const int h = height(why, fonts_.caption.get(), avail - px(24)) + px(14);
        mg.not_served = {cx0, yy, cx1, yy + h};
        yy += h + px(8);
    }
    const int env_h = std::max(px(kNoteH), height(text(lang_, Str::MailEnvNote), fonts_.caption.get(), avail));
    mg.env_note = {cx0, yy, cx1, yy + env_h};
    yy += env_h;
    mg.addr_card.bottom = yy + px(14);
    y = static_cast<int>(mg.addr_card.bottom) + px(kGap);

    // Options: three switches, then the number of emails kept.
    top = y + px(14);
    mg.opt_card = {x0, y, x1, y};
    mg.opt_title = {cx0, top, cx1, top + px(24)};
    yy = static_cast<int>(mg.opt_title.bottom) + px(6);
    const int text_w = std::max(px(80), avail - px(kSwitchW) - px(kGap));
    for (std::size_t i = 0; i < mg.rows.size(); ++i) {
        const int note_h = height(text(lang_, kSwitchNotes.at(i)), fonts_.caption.get(), text_w);
        const int row_h = std::max(px(kControlH), px(20) + note_h + px(8));
        mg.rows.at(i) = {cx0 - px(6), yy, cx1 + px(6), yy + row_h};
        mg.labels.at(i) = {cx0, yy + px(4), cx0 + text_w, yy + px(24)};
        mg.notes.at(i) = {cx0, static_cast<int>(mg.labels.at(i).bottom), cx0 + text_w,
                          static_cast<int>(mg.labels.at(i).bottom) + note_h};
        mg.switches.at(i) = {cx1 - px(kSwitchW), yy + px(4), cx1, yy + px(4) + px(kSwitchH)};
        yy += row_h + px(2);
    }
    yy += px(4);
    const int keep_w = measure(text(lang_, Str::MailKeepLast), fonts_.button.get()) + px(2);
    mg.max_label = {cx0, yy, cx0 + keep_w, yy + px(kControlH)};
    mg.max_box = {static_cast<int>(mg.max_label.right) + px(kGap), yy,
                  static_cast<int>(mg.max_label.right) + px(kGap) + px(kMaxBoxW), yy + px(kControlH)};
    const int after_left = static_cast<int>(mg.max_box.right) + px(kGap);
    mg.max_after = {after_left, yy, std::max(after_left, cx1), yy + px(kControlH)};
    yy += px(kControlH);
    if (mail_max_bad(mail_changes_)) {
        mg.max_bad = {cx0, yy + px(4), cx1, yy + px(4) + px(kNoteH)};
        yy += px(4) + px(kNoteH);
    }
    mg.opt_card.bottom = yy + px(14);
    y = static_cast<int>(mg.opt_card.bottom) + px(kGap);

    // Emails: how many, and the two buttons.
    top = y + px(14);
    mg.emails_card = {x0, y, x1, y};
    const int del_w = button_w(text(lang_, Str::MailDeleteAll));
    const int test_w = button_w(text(lang_, Str::MailSendTest));
    mg.del = {std::max(cx0, cx1 - del_w), top, cx1, top + px(kControlH)};
    mg.test = {std::max(cx0, static_cast<int>(mg.del.left) - px(8) - test_w), top,
               static_cast<int>(mg.del.left) - px(8), top + px(kControlH)};
    const int left_end = std::max(cx0 + px(40), static_cast<int>(mg.test.left) - px(kGap));
    mg.emails_title = {cx0, top - px(2), left_end, top - px(2) + px(22)};
    mg.emails_count = {cx0, static_cast<int>(mg.emails_title.bottom), left_end,
                       static_cast<int>(mg.emails_title.bottom) + px(kNoteH)};
    mg.emails_card.bottom = std::max(static_cast<int>(mg.emails_count.bottom), top + px(kControlH)) + px(14);
    y = static_cast<int>(mg.emails_card.bottom) + px(kGap);

    // The footer: at the window's bottom when there's room, else after the cards.
    const int save_w = std::max(measure(text(lang_, Str::PhpSave), fonts_.button.get()),
                                measure(text(lang_, Str::PhpSaving), fonts_.button.get())) +
                       px(24);
    const int discard_w = button_w(text(lang_, Str::PhpDiscard));
    const std::string applies = mail_footer_text(php_projects_, lang_);
    const int footer_text_w = std::max(px(80), x1 - x0 - save_w - discard_w - px(8) - px(kGap));
    const int footer_text_h = height(applies, fonts_.caption.get(), footer_text_w);
    const int footer_h = std::max(px(kControlH), footer_text_h);
    const int footer_page_top = std::max(y + scroll_, height_ - px(16) - footer_h);
    const int footer_top = footer_page_top - scroll_;
    mg.save = {x1 - save_w, footer_top, x1, footer_top + px(kControlH)};
    mg.discard = {static_cast<int>(mg.save.left) - px(8) - discard_w, footer_top, static_cast<int>(mg.save.left) - px(8),
                  footer_top + px(kControlH)};
    const int text_top = footer_top + std::max(0, (px(kControlH) - footer_text_h) / 2);
    mg.footer_text = {x0, text_top, x0 + footer_text_w, text_top + footer_text_h};
    g.content_h = footer_page_top + footer_h + px(16);
}

const RECT* SettingsWindow::mail_rect_of(const Hit& hit) const {
    const MailGeometry& g = geometry_.mail;
    switch (hit.part) {
        case Part::MailInstall:
            return &g.install;
        case Part::MailInboxLink:
            return &g.inbox_link;
        case Part::MailCopyInbox:
            return &g.inbox_copy;
        case Part::MailOpenInbox:
            return &g.inbox_open;
        case Part::MailCopySmtp:
            return &g.smtp_copy;
        case Part::MailSwitch:
            return hit.row >= 0 && std::cmp_less(hit.row, g.rows.size()) ? &g.rows[static_cast<std::size_t>(hit.row)]
                                                                         : nullptr;
        case Part::MailMax:
            return &g.max_box;
        case Part::MailTest:
            return &g.test;
        case Part::MailDelete:
            return &g.del;
        case Part::MailDiscard:
            return &g.discard;
        case Part::MailSave:
            return &g.save;
        default:
            break;
    }
    return nullptr;
}

void SettingsWindow::mail_focus_order(std::vector<Hit>& order) const {
    const MailGeometry& g = geometry_.mail;
    const auto add = [&](Part part, const RECT& r, int row = -1) {
        if (!is_empty(r)) {
            order.push_back({part, -1, row});
        }
    };
    add(Part::MailInstall, g.install);
    if (!mail_ || !mail_->installed) {
        return;
    }
    add(Part::MailInboxLink, g.inbox_link);
    add(Part::MailCopyInbox, g.inbox_copy);
    add(Part::MailOpenInbox, g.inbox_open);
    add(Part::MailCopySmtp, g.smtp_copy);
    for (std::size_t i = 0; i < g.rows.size(); ++i) {
        add(Part::MailSwitch, g.rows[i], static_cast<int>(i));
    }
    add(Part::MailMax, g.max_box);
    add(Part::MailTest, g.test);
    add(Part::MailDelete, g.del);
    add(Part::MailDiscard, g.discard);
    add(Part::MailSave, g.save);
}

// --- input ---------------------------------------------------------------------------------

void SettingsWindow::on_mail_click(const Hit& hit) {
    switch (hit.part) {
        case Part::MailInstall:
            install_mailpit();
            break;
        case Part::MailInboxLink:
        case Part::MailOpenInbox:
            open_inbox();
            break;
        case Part::MailCopyInbox:
            if (mail_) {
                copy_mail_text(mail_inbox_address(*mail_));
            }
            break;
        case Part::MailCopySmtp:
            if (mail_) {
                copy_mail_text(mail_smtp_copy(*mail_));
            }
            break;
        case Part::MailSwitch:
            if (mail_ && !mail_saving_ && hit.row >= 0) {
                switch (hit.row) {
                    case 0:
                        mail_toggle_php(*mail_, mail_changes_);
                        break;
                    case 1:
                        mail_toggle_notices(*mail_, mail_changes_);
                        break;
                    default:
                        mail_toggle_keep_running(*mail_, mail_changes_);
                        break;
                }
                relayout();
            }
            break;
        case Part::MailMax:
            if (!mail_edits_.empty()) {
                SetFocus(mail_edits_.front()->hwnd());
            }
            break;
        case Part::MailTest:
            send_test_mail();
            break;
        case Part::MailDelete:
            delete_all_mail();
            break;
        case Part::MailDiscard:
            discard_mail();
            break;
        case Part::MailSave:
            (void)save_mail();
            break;
        default:
            break;
    }
}

// --- painting ------------------------------------------------------------------------------

void SettingsWindow::paint_mail_shapes(Canvas& c) const {
    const Palette& p = palette(dark_);
    const MailGeometry& mg = geometry_.mail;
    const float border = std::max(1.0F, pxf(1.0F));
    c.clip({main_left() - px(4), 0, width_, height_});
    if (!is_empty(mg.pill) && mail_) {
        const MailPill kind = mail_pill(mail_->state);
        const StateColors& sc = kind == MailPill::Good      ? p.on
                                : kind == MailPill::Waiting ? p.asleep
                                : kind == MailPill::Bad     ? p.problem
                                                            : p.off;
        if (sc.border != sc.fill) {
            c.round_rect(mg.pill, pxf(11), sc.fill, sc.border, border);
        } else {
            c.round_rect(mg.pill, pxf(11), sc.fill);
        }
    }
    if (!is_empty(mg.failed)) {
        c.round_rect(mg.failed, pxf(4), p.banner_error.fill);
    }
    if (!is_empty(mg.install)) {
        const Hit hit{Part::MailInstall, -1, -1};
        if (connected_) {
            c.round_rect(mg.install, pxf(4), p.accent);
        if (hover_ == hit) {
            c.round_outline(mg.install, pxf(4), p.accent_text, pxf(1));
        }
        } else {
            paint_button(c, mg.install, false, false, false);
        }
        if (is_focused(hit)) {
            paint_ring(c, mg.install);
        }
    }
    if (!is_empty(mg.progress_bar) && mail_) {
        const auto job = std::find_if(jobs_.begin(), jobs_.end(), [](const RuntimeJob& j) { return j.name == "mailpit"; });
        c.round_rect(mg.progress_bar, pxf(2), p.bar_track);
        if (job != jobs_.end()) {
            if (const auto fraction = job_fraction(*job)) {
                const int done = static_cast<int>(static_cast<double>(layout::width_of(mg.progress_bar)) * *fraction);
                if (done > 0) {
                    c.round_rect({mg.progress_bar.left, mg.progress_bar.top, mg.progress_bar.left + done,
                                  mg.progress_bar.bottom},
                                 pxf(2), p.accent);
                }
            }
        }
    }
    if (mail_ && mail_->installed) {
        // Addresses.
        c.round_rect(mg.addr_card, pxf(8), p.card, p.card_border, border);
        if (is_focused({Part::MailInboxLink, -1, -1})) {
            paint_ring(c, mg.inbox_link);
        }
        const Hit copy_inbox{Part::MailCopyInbox, -1, -1};
        paint_button(c, mg.inbox_copy, hover_ == copy_inbox, is_focused(copy_inbox), true);
        const Hit open{Part::MailOpenInbox, -1, -1};
        c.round_rect(mg.inbox_open, pxf(4), p.accent);
        if (hover_ == open) {
            c.round_outline(mg.inbox_open, pxf(4), p.accent_text, pxf(1));
        }
        if (is_focused(open)) {
            paint_ring(c, mg.inbox_open);
        }
        const Hit copy_smtp{Part::MailCopySmtp, -1, -1};
        paint_button(c, mg.smtp_copy, hover_ == copy_smtp, is_focused(copy_smtp), true);
        if (!is_empty(mg.not_served)) {
            c.round_rect(mg.not_served, pxf(4), p.banner_error.fill);
        }

        // Options.
        c.round_rect(mg.opt_card, pxf(8), p.card, p.card_border, border);
        for (std::size_t i = 0; i < mg.rows.size(); ++i) {
            const Hit hit{Part::MailSwitch, -1, static_cast<int>(i)};
            const bool on = i == 0   ? mail_php(*mail_, mail_changes_)
                            : i == 1 ? mail_notices(*mail_, mail_changes_)
                                     : mail_keep_running(*mail_, mail_changes_);
            if (hover_ == hit && !mail_saving_) {
                c.round_rect(mg.rows[i], pxf(4), p.row_hover);
            }
            paint_switch(c, mg.switches[i], on, hover_ == hit && !mail_saving_);
            if (is_focused(hit)) {
                paint_ring(c, mg.rows[i]);
            }
        }
        const bool bad = mail_max_bad(mail_changes_);
        const bool max_focused = !mail_edits_.empty() && mail_edits_.front()->focused();
        c.round_rect(mg.max_box, pxf(4), p.input, bad ? p.banner_error.text : p.input_border, border);
        if (max_focused) {
            c.round_outline(mg.max_box, pxf(4), p.accent, pxf(2));
        }

        // Emails.
        c.round_rect(mg.emails_card, pxf(8), p.card, p.card_border, border);
        const bool can = connected_ && !mail_busy_;
        const Hit test{Part::MailTest, -1, -1};
        paint_button(c, mg.test, hover_ == test && can, is_focused(test), can);
        const Hit del{Part::MailDelete, -1, -1};
        // Delete: the outline in red, as the mockup has it.
        c.round_rect(mg.del, pxf(4), hover_ == del && can ? p.row_hover : p.button, can ? p.problem.text : p.button_border,
                     border);
        if (is_focused(del)) {
            paint_ring(c, mg.del);
        }

        // The footer.
        const bool changes = !mail_changes_.empty();
        const Hit discard{Part::MailDiscard, -1, -1};
        paint_button(c, mg.discard, hover_ == discard && changes, is_focused(discard), changes && !mail_saving_);
        const Hit save{Part::MailSave, -1, -1};
        if (changes && !mail_saving_ && connected_ && !bad) {
            c.round_rect(mg.save, pxf(4), p.accent);
            if (is_focused(save)) {
                paint_ring(c, mg.save);
            }
        } else {
            paint_button(c, mg.save, false, is_focused(save), false);
        }
    }
    scroll_bar_.paint(c, p);
    c.unclip();
}

void SettingsWindow::paint_mail_text(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const MailGeometry& mg = g.mail;
    c.clip_text({main_left() - px(4), 0, width_, height_});
    c.text(g.title, w(text(lang_, Str::SettingsMail)), fonts_.title.get(), p.text, Align::Start, false);
    c.paragraph(g.hint, w(text(lang_, Str::MailHint)), fonts_.caption.get(), p.text_subtle);
    if (!is_empty(mg.note)) {
        c.text(mg.note, w(text(lang_, Str::SettingsLoading)), fonts_.button.get(), p.text_subtle, Align::Start, false);
    }
    if (!mail_) {
        c.unclip_text();
        return;
    }
    if (!is_empty(mg.pill)) {
        const MailPill kind = mail_pill(mail_->state);
        const StateColors& sc = kind == MailPill::Good      ? p.on
                                : kind == MailPill::Waiting ? p.asleep
                                : kind == MailPill::Bad     ? p.problem
                                                            : p.off;
        c.text(mg.pill, w(mail_state_text(mail_->state, lang_)), fonts_.small_bold.get(), sc.text, Align::Center, false);
    }
    if (!is_empty(mg.version)) {
        c.text(mg.version, w(fill(lang_, Str::MailVersion, {mail_->version})), fonts_.caption.get(), p.text_subtle,
               Align::End, false);
    }
    if (!mail_->installed) {
        c.paragraph(mg.install_note, w(text(lang_, Str::MailNotInstalledNote)), fonts_.button.get(), p.text);
        if (!is_empty(mg.install)) {
            c.text(mg.install, w(text(lang_, Str::MailInstall)), fonts_.button.get(),
                   connected_ ? p.accent_text : p.button_disabled_text, Align::Center, false);
        }
        if (!is_empty(mg.progress_text)) {
            const auto job = std::find_if(jobs_.begin(), jobs_.end(), [](const RuntimeJob& j) { return j.name == "mailpit"; });
            c.text(mg.progress_text, w(job != jobs_.end() ? job_text(*job, lang_) : std::string(text(lang_, Str::SettingsStarting))),
                   fonts_.button.get(), p.text_subtle, Align::Start, false);
        }
        c.unclip_text();
        return;
    }
    if (!is_empty(mg.failed)) {
        RECT inner = mg.failed;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(fill(lang_, Str::MailFailed, {rtl_ ? isolate_ltr(mail_->error) : mail_->error})),
                    fonts_.caption.get(), p.banner_error.text);
    }

    // Addresses.
    c.text(mg.addr_title, w(text(lang_, Str::MailAddresses)), fonts_.body_bold.get(), p.text, Align::Start, false);
    c.text(mg.inbox_label, w(text(lang_, Str::MailInbox)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    c.text(mg.inbox_link, w(mail_inbox_address(*mail_)), fonts_.button.get(), p.link, Align::Start, true);
    c.text(mg.inbox_copy, w(text(lang_, Str::DbCopy)), fonts_.button.get(), p.text, Align::Center, false);
    c.text(mg.inbox_open, w(text(lang_, Str::MailOpenInbox)), fonts_.button.get(), p.accent_text, Align::Center, false);
    c.text(mg.smtp_label, w(text(lang_, Str::MailSmtpServer)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    c.text(mg.smtp_value, w(mail_smtp_text(*mail_)), fonts_.mono.get(), p.text, Align::Start, true);
    c.text(mg.smtp_note, w(text(lang_, Str::MailNoLogin)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    c.text(mg.smtp_copy, w(text(lang_, Str::DbCopy)), fonts_.button.get(), p.text, Align::Center, false);
    if (!is_empty(mg.not_served)) {
        RECT inner = mg.not_served;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(mail_not_served_text(*mail_, lang_)), fonts_.caption.get(), p.banner_error.text);
    }
    c.paragraph(mg.env_note, w(text(lang_, Str::MailEnvNote)), fonts_.caption.get(), p.text_subtle);

    // Options.
    c.text(mg.opt_title, w(text(lang_, Str::MailOptions)), fonts_.body_bold.get(), p.text, Align::Start, false);
    for (std::size_t i = 0; i < mg.rows.size(); ++i) {
        c.text(mg.labels[i], w(text(lang_, kSwitchLabels.at(i))), fonts_.body.get(), p.text, Align::Start, false);
        c.paragraph(mg.notes[i], w(text(lang_, kSwitchNotes.at(i))), fonts_.caption.get(), p.text_subtle);
    }
    c.text(mg.max_label, w(text(lang_, Str::MailKeepLast)), fonts_.button.get(), p.text, Align::Start, false);
    c.text(mg.max_after, w(text(lang_, Str::MailKeepLastAfter)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    if (!is_empty(mg.max_bad)) {
        c.text(mg.max_bad, w(text(lang_, Str::MailMaxBad)), fonts_.caption.get(), p.banner_error.text, Align::Start, false);
    }

    // Emails.
    c.text(mg.emails_title, w(text(lang_, Str::MailEmails)), fonts_.body_bold.get(), p.text, Align::Start, false);
    c.text(mg.emails_count, w(mail_count_text(*mail_, lang_)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    const bool can = connected_ && !mail_busy_;
    c.text(mg.test, w(text(lang_, Str::MailSendTest)), fonts_.button.get(), can ? p.text : p.button_disabled_text,
           Align::Center, false);
    c.text(mg.del, w(text(lang_, Str::MailDeleteAll)), fonts_.button.get(), can ? p.problem.text : p.button_disabled_text,
           Align::Center, false);

    // The footer.
    c.paragraph(mg.footer_text, w(mail_footer_text(php_projects_, lang_)), fonts_.caption.get(), p.text_subtle);
    const bool changes = !mail_changes_.empty();
    const bool bad = mail_max_bad(mail_changes_);
    c.text(mg.discard, w(text(lang_, Str::PhpDiscard)), fonts_.button.get(),
           changes && !mail_saving_ ? p.text : p.button_disabled_text, Align::Center, false);
    c.text(mg.save, w(text(lang_, mail_saving_ ? Str::PhpSaving : Str::PhpSave)), fonts_.button.get(),
           changes && !mail_saving_ && connected_ && !bad ? p.accent_text : p.button_disabled_text, Align::Center, false);
    c.unclip_text();
}

}  // namespace floserver::ui

// Settings > Redis (D30, approved 11 Nov): one shared Garnet server for the
// projects. Its state and version, the address and the password (Copy only:
// the password is never drawn), Keep running and the memory limit, and the
// data (how many keys, who uses it, Flush all…). Built as Settings > Mail is:
// one redis.status {"projects": true} when the section opens (the main
// window's poll keeps the state and the key count current meanwhile), the
// options wait for Save (one redis.set), and Flush all and Copy are sent at once.

#include "settings_window.h"

#include "main_window_layout.h"
#include "model/theme.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace floserver::ui {

using nlohmann::json;

namespace {

constexpr int kRedisEditBase = 5100;  // redis_edits_[0], the memory limit
constexpr int kGap = 12;
constexpr int kCardPad = 16;
constexpr int kControlH = 30;
constexpr int kNoteH = 18;
constexpr int kLabelW = 140;
constexpr int kSwitchW = 40;
constexpr int kSwitchH = 20;
constexpr int kMemoryBoxW = 80;

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

bool is_garnet_job(const RuntimeJob& j) { return j.name == "garnet"; }

}  // namespace

// --- loading, saving, leaving ----------------------------------------------------------

bool SettingsWindow::settle_redis_changes(AfterSave then) {
    if (!redis_ || redis_changes_.empty()) {
        return true;
    }
    if (asking_ || redis_saving_) {
        return false;
    }
    asking_ = true;
    const SaveChoice choice = ask_to_save(hwnd_, lang_, std::string(text(lang_, Str::RedisAskSave)));
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
            if (save_redis()) {
                after_save_ = then;
            }
            return false;
        case SaveChoice::DontSave:
            discard_redis();
            return true;
        case SaveChoice::Cancel:
            break;
    }
    return false;
}

void SettingsWindow::load_redis() {
    redis_changes_ = {};
    redis_loading_ = !redis_.has_value();  // what the poll already said stays on show
    // The one request the section makes: with the projects that use it.
    worker_.database_call("redis.status", json{{"projects", true}});
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::save_redis() {
    if (!redis_ || redis_changes_.empty() || redis_saving_) {
        return false;
    }
    if (redis_memory_bad(redis_changes_)) {
        show_message(std::string(text(lang_, Str::RedisMemoryBad)), true);
        return false;
    }
    if (!connected_) {
        show_message(fill(lang_, Str::PhpSaveFailed, {text(lang_, Str::ConnectionLost)}), true);
        return false;
    }
    redis_saving_ = true;
    set_redis_read_only(true);
    clear_toast();
    worker_.database_call("redis.set", redis_set_params(redis_changes_));
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void SettingsWindow::discard_redis() {
    if (redis_saving_) {
        return;
    }
    redis_changes_ = {};
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::garnet_installing() const {
    if (starting_.contains(ServiceWorker::install_key(settings_install_key("garnet", "")))) {
        return true;
    }
    if (std::any_of(jobs_.begin(), jobs_.end(), is_garnet_job)) {
        return true;
    }
    const auto it = installed_.find("garnet");
    return it != installed_.end() && !it->second.empty() && redis_ && !redis_->installed;
}

void SettingsWindow::install_garnet() {
    if (!connected_ || garnet_installing()) {
        return;
    }
    // As Runtimes' Download: the newest Garnet (its .NET runtime and phpredis come with it).
    const std::string key = settings_install_key("garnet", "");
    starting_.insert(ServiceWorker::install_key(key));
    clear_toast();
    worker_.install(key, {{"garnet", ""}});
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::copy_redis_password() {
    if (!connected_ || redis_blocked() || !redis_) {
        return;
    }
    // Asked only now: the answer goes on the clipboard and is wiped.
    redis_busy_ = true;
    clear_toast();
    worker_.database_call("redis.password", json::object());
    relayout();
}

void SettingsWindow::flush_redis() {
    if (!connected_ || redis_blocked() || !redis_ || asking_) {
        return;
    }
    asking_ = true;
    const bool yes = ask_flush_redis(hwnd_, lang_, redis_flush_question(*redis_, lang_));
    asking_ = false;
    modal_closed();
    if (hwnd_ == nullptr || !yes) {
        return;
    }
    redis_busy_ = true;
    clear_toast();
    worker_.database_call("redis.flush", json::object());
    relayout();
}

bool SettingsWindow::on_redis_answers(const Snapshot& snap) {
    bool close_now = false;
    const auto service_words = [this](const std::string& s) { return rtl_ ? isolate_ltr(s) : s; };
    for (const Snapshot::DbAnswer& answer : snap.db) {
        if (!answer.method.starts_with("redis.")) {
            continue;
        }
        const bool failed = !answer.error.empty() || !answer.reply;
        if (answer.method == "redis.status") {
            redis_loading_ = false;
            if (failed) {
                show_message(fill(lang_, Str::RedisReadFailed, {service_words(answer.error)}), true);
            } else if (auto status = redis_status_from_reply(*answer.reply); status && !redis_saving_) {
                redis_ = std::move(status);
            }
        } else if (answer.method == "redis.set") {
            redis_saving_ = false;
            set_redis_read_only(false);
            if (failed) {
                after_save_ = AfterSave::Stay;
                show_message(fill(lang_, Str::PhpSaveFailed, {service_words(answer.error)}), true);
            } else {
                if (redis_) {
                    redis_apply(*redis_, redis_changes_);
                }
                redis_changes_ = {};
                show_message(std::string(text(lang_, Str::PhpSaved)), false);
                const AfterSave then = std::exchange(after_save_, AfterSave::Stay);
                if (then == AfterSave::Close) {
                    close_now = true;
                } else if (then == AfterSave::Runtimes) {
                    open_section(after_save_section_);
                }
            }
        } else if (answer.method == "redis.password") {
            redis_busy_ = false;
            if (failed || !answer.reply->is_object() || !answer.reply->contains("password") ||
                !(*answer.reply)["password"].is_string()) {
                show_message(fill(lang_, Str::RedisCopyFailed, {service_words(answer.error)}), true);
            } else {
                std::string password = (*answer.reply)["password"].get<std::string>();
                if (put_secret_on_clipboard(hwnd_, password)) {
                    show_message(std::string(text(lang_, Str::RedisCopied)), false);
                }
                SecureZeroMemory(password.data(), password.size());
            }
        } else if (answer.method == "redis.flush") {
            redis_busy_ = false;
            if (failed) {
                show_message(fill(lang_, Str::RedisFlushFailed, {service_words(answer.error)}), true);
            } else {
                if (redis_) {
                    redis_->keys = 0;
                }
                show_message(std::string(text(lang_, Str::RedisFlushed)), false);
            }
        }
    }
    return close_now;
}

// --- the number box --------------------------------------------------------------------

void SettingsWindow::make_redis_edits() {
    if (!redis_edits_.empty()) {
        return;
    }
    auto edit = std::make_unique<FieldEdit>(hwnd_, kRedisEditBase, fonts_.button.get(), rtl_,
                                            [this](WPARAM key) { return on_edit_key(kRedisEditBase, key); });
    edit->set_cue_color(palette(dark_).text_subtle);
    set_accessible_name(edit->hwnd(), text(lang_, Str::RedisMemory));  // its label, as drawn beside it
    redis_edits_.push_back(std::move(edit));
}

void SettingsWindow::set_redis_read_only(bool read_only) {
    for (const auto& edit : redis_edits_) {
        edit->set_read_only(read_only);
    }
}

void SettingsWindow::place_redis_edits() {
    if (section_ != Section::Redis || redis_edits_.empty()) {
        return;
    }
    FieldEdit& edit = *redis_edits_.front();
    const bool shown = redis_ && redis_->installed && !is_empty(geometry_.redis.memory_box);
    if (redis_ && !edit.focused()) {
        if (const std::string want = redis_memory_text(*redis_, redis_changes_); edit.text() != want) {
            edit.set_text(want);
        }
    }
    edit.place(geometry_.redis.memory_box, px(8), px(20), shown);
}

// --- geometry ------------------------------------------------------------------------------

void SettingsWindow::layout_redis(HDC dc, Geometry& g, int x0, int x1, int y) const {
    RedisGeometry& rg = g.redis;
    const auto measure = [&](std::string_view s, HFONT font) { return Canvas::measure(dc, w(s), font, rtl_); };
    const auto button_w = [&](std::string_view label) { return measure(label, fonts_.button.get()) + px(24); };
    const auto height = [&](std::string_view s, HFONT font, int width) { return text_height(dc, w(s), font, width, rtl_); };

    const bool known = redis_.has_value();
    std::string pill_text;
    std::string version_text;
    int right_w = 0;
    if (known) {
        pill_text = redis_state_text(*redis_, lang_);
        if (redis_->installed && !redis_->version.empty()) {
            version_text = fill(lang_, Str::RedisVersion, {redis_->version});
        }
        const int pill_w = measure(pill_text, fonts_.small_bold.get()) + px(20);
        const int version_w = version_text.empty() ? 0 : measure(version_text, fonts_.caption.get()) + px(2);
        right_w = pill_w + (version_w > 0 ? px(10) + version_w : 0) + px(kGap);
    }
    const int hint_w = std::max(px(100), x1 - x0 - right_w);
    const int hint_h = height(text(lang_, Str::RedisHint), fonts_.caption.get(), hint_w);
    g.title = {x0, y, x0 + hint_w, y + px(28)};
    g.hint = {x0, g.title.bottom, x0 + hint_w, g.title.bottom + hint_h};
    if (known) {
        const int pill_w = measure(pill_text, fonts_.small_bold.get()) + px(20);
        const int version_w = version_text.empty() ? 0 : measure(version_text, fonts_.caption.get()) + px(2);
        const int bottom = static_cast<int>(g.hint.bottom);
        if (version_w > 0) {
            rg.version = {x1 - version_w, bottom - px(22), x1, bottom};
        }
        const int pill_right = version_w > 0 ? static_cast<int>(rg.version.left) - px(10) : x1;
        rg.pill = {pill_right - pill_w, bottom - px(22), pill_right, bottom};
    }
    y = static_cast<int>(g.hint.bottom) + px(kGap);

    if (!known) {
        rg.note = {x0, y, x1, y + px(24)};
        g.content_h = static_cast<int>(rg.note.bottom) + scroll_ + px(8);
        return;
    }
    if (!redis_->installed) {
        const int nh = height(text(lang_, Str::RedisNotInstalledNote), fonts_.button.get(), x1 - x0);
        rg.install_note = {x0, y, x1, y + nh};
        y += nh + px(kGap);
        if (garnet_installing()) {
            const auto job = std::find_if(jobs_.begin(), jobs_.end(), is_garnet_job);
            rg.progress_text = {x0, y, x1, y + px(kControlH)};
            if (job != jobs_.end() && job_fraction(*job)) {
                rg.progress_bar = {x0, y + px(kControlH) + px(2), x0 + std::min(px(240), x1 - x0),
                                   y + px(kControlH) + px(6)};
                y += px(6);
            }
            y += px(kControlH);
        } else {
            rg.install = {x0, y, x0 + button_w(text(lang_, Str::RedisInstall)) + px(12), y + px(kControlH)};
            y += px(kControlH);
        }
        g.content_h = y + scroll_ + px(16);
        return;
    }

    const int cx0 = x0 + px(kCardPad);
    const int cx1 = x1 - px(kCardPad);
    const int avail = cx1 - cx0;

    if (redis_->state == MailState::Failed && !redis_->error.empty()) {
        const std::string words = fill(lang_, Str::RedisFailed, {rtl_ ? isolate_ltr(redis_->error) : redis_->error});
        const int h = height(words, fonts_.caption.get(), x1 - x0 - px(24)) + px(14);
        rg.failed = {x0, y, x1, y + h};
        y += h + px(kGap);
    }
    if (const std::string words = redis_save_failed_text(*redis_, lang_); !words.empty()) {
        const int h = height(words, fonts_.caption.get(), x1 - x0 - px(24)) + px(14);
        rg.save_failed = {x0, y, x1, y + h};
        y += h + px(kGap);
    }

    // Address: the server and the password, each with Copy.
    int top = y + px(14);
    rg.addr_card = {x0, y, x1, y};
    rg.addr_title = {cx0, top, cx1, top + px(24)};
    int yy = static_cast<int>(rg.addr_title.bottom) + px(10);
    const int label_w = std::min(px(kLabelW), avail / 4);
    const int copy_w = button_w(text(lang_, Str::DbCopy));
    rg.server_label = {cx0, yy, cx0 + label_w, yy + px(kControlH)};
    rg.server_copy = {std::max(cx0 + label_w, cx1 - copy_w), yy, cx1, yy + px(kControlH)};
    const int server_w = measure(redis_address_text(*redis_), fonts_.mono.get()) + px(4);
    rg.server_value = {cx0 + label_w, yy, cx0 + label_w + server_w, yy + px(kControlH)};
    yy += px(kControlH) + px(kGap);
    rg.password_label = {cx0, yy, cx0 + label_w, yy + px(kControlH)};
    rg.password_copy = {std::max(cx0 + label_w, cx1 - copy_w), yy, cx1, yy + px(kControlH)};
    const int mask_w = measure(redis_password_mask(), fonts_.mono.get()) + px(4);
    rg.password_value = {cx0 + label_w, yy, cx0 + label_w + mask_w, yy + px(kControlH)};
    const int note_left = static_cast<int>(rg.password_value.right) + px(kGap);
    rg.password_note = {note_left, yy, std::max(note_left, static_cast<int>(rg.password_copy.left) - px(kGap)),
                        yy + px(kControlH)};
    yy += px(kControlH) + px(8);
    const int env_h = std::max(px(kNoteH), height(text(lang_, Str::RedisEnvNote), fonts_.caption.get(), avail));
    rg.env_note = {cx0, yy, cx1, yy + env_h};
    yy += env_h;
    rg.addr_card.bottom = yy + px(14);
    y = static_cast<int>(rg.addr_card.bottom) + px(kGap);

    // Options: Keep running, then the memory limit.
    top = y + px(14);
    rg.opt_card = {x0, y, x1, y};
    rg.opt_title = {cx0, top, cx1, top + px(24)};
    yy = static_cast<int>(rg.opt_title.bottom) + px(6);
    const int text_w = std::max(px(80), avail - px(kSwitchW) - px(kGap));
    const int note_h = height(text(lang_, Str::RedisKeepRunningNote), fonts_.caption.get(), text_w);
    const int row_h = std::max(px(kControlH), px(20) + note_h + px(8));
    rg.keep_row = {cx0 - px(6), yy, cx1 + px(6), yy + row_h};
    rg.keep_label = {cx0, yy + px(4), cx0 + text_w, yy + px(24)};
    rg.keep_note = {cx0, static_cast<int>(rg.keep_label.bottom), cx0 + text_w,
                    static_cast<int>(rg.keep_label.bottom) + note_h};
    rg.keep_switch = {cx1 - px(kSwitchW), yy + px(4), cx1, yy + px(4) + px(kSwitchH)};
    yy += row_h + px(6);
    const int memory_w = measure(text(lang_, Str::RedisMemory), fonts_.button.get()) + px(2);
    rg.memory_label = {cx0, yy, cx0 + memory_w, yy + px(kControlH)};
    rg.memory_box = {static_cast<int>(rg.memory_label.right) + px(kGap), yy,
                     static_cast<int>(rg.memory_label.right) + px(kGap) + px(kMemoryBoxW), yy + px(kControlH)};
    const int unit_left = static_cast<int>(rg.memory_box.right) + px(8);
    rg.memory_unit = {unit_left, yy, std::max(unit_left, cx1), yy + px(kControlH)};
    yy += px(kControlH);
    if (redis_memory_bad(redis_changes_)) {
        rg.memory_bad = {cx0, yy + px(4), cx1, yy + px(4) + px(kNoteH)};
        yy += px(4) + px(kNoteH);
    }
    rg.opt_card.bottom = yy + px(14);
    y = static_cast<int>(rg.opt_card.bottom) + px(kGap);

    // Data: how many keys and who uses it, Flush all…, and what Garnet doesn't do.
    top = y + px(14);
    rg.data_card = {x0, y, x1, y};
    const int flush_w = button_w(text(lang_, Str::RedisFlush));
    rg.flush = {std::max(cx0, cx1 - flush_w), top, cx1, top + px(kControlH)};
    const int left_end = std::max(cx0 + px(40), static_cast<int>(rg.flush.left) - px(kGap));
    rg.data_title = {cx0, top - px(2), left_end, top - px(2) + px(22)};
    rg.data_count = {cx0, static_cast<int>(rg.data_title.bottom), left_end,
                     static_cast<int>(rg.data_title.bottom) + px(kNoteH)};
    yy = std::max(static_cast<int>(rg.data_count.bottom), top + px(kControlH)) + px(10);
    const int unsupported_h = height(text(lang_, Str::RedisUnsupported), fonts_.caption.get(), avail);
    rg.unsupported = {cx0, yy, cx1, yy + unsupported_h};
    rg.data_card.bottom = static_cast<int>(rg.unsupported.bottom) + px(14);
    y = static_cast<int>(rg.data_card.bottom) + px(kGap);

    // The footer: at the window's bottom when there's room, else after the cards.
    const int save_w = std::max(measure(text(lang_, Str::PhpSave), fonts_.button.get()),
                                measure(text(lang_, Str::PhpSaving), fonts_.button.get())) +
                       px(24);
    const int discard_w = button_w(text(lang_, Str::PhpDiscard));
    const std::string applies(text(lang_, Str::RedisFooter));
    const int footer_text_w = std::max(px(80), x1 - x0 - save_w - discard_w - px(8) - px(kGap));
    const int footer_text_h = height(applies, fonts_.caption.get(), footer_text_w);
    const int footer_h = std::max(px(kControlH), footer_text_h);
    const int footer_page_top = std::max(y + scroll_, height_ - px(16) - footer_h);
    const int footer_top = footer_page_top - scroll_;
    rg.save = {x1 - save_w, footer_top, x1, footer_top + px(kControlH)};
    rg.discard = {static_cast<int>(rg.save.left) - px(8) - discard_w, footer_top, static_cast<int>(rg.save.left) - px(8),
                  footer_top + px(kControlH)};
    const int text_top = footer_top + std::max(0, (px(kControlH) - footer_text_h) / 2);
    rg.footer_text = {x0, text_top, x0 + footer_text_w, text_top + footer_text_h};
    g.content_h = footer_page_top + footer_h + px(16);
}

const RECT* SettingsWindow::redis_rect_of(const Hit& hit) const {
    const RedisGeometry& g = geometry_.redis;
    switch (hit.part) {
        case Part::RedisInstall:
            return &g.install;
        case Part::RedisCopyAddress:
            return &g.server_copy;
        case Part::RedisCopyPassword:
            return &g.password_copy;
        case Part::RedisKeep:
            return &g.keep_row;
        case Part::RedisMemory:
            return &g.memory_box;
        case Part::RedisFlush:
            return &g.flush;
        case Part::RedisDiscard:
            return &g.discard;
        case Part::RedisSave:
            return &g.save;
        default:
            break;
    }
    return nullptr;
}

void SettingsWindow::redis_focus_order(std::vector<Hit>& order) const {
    const RedisGeometry& g = geometry_.redis;
    const auto add = [&](Part part, const RECT& r) {
        if (!is_empty(r)) {
            order.push_back({part, -1, -1});
        }
    };
    add(Part::RedisInstall, g.install);
    if (!redis_ || !redis_->installed) {
        return;
    }
    add(Part::RedisCopyAddress, g.server_copy);
    add(Part::RedisCopyPassword, g.password_copy);
    add(Part::RedisKeep, g.keep_row);
    add(Part::RedisMemory, g.memory_box);
    add(Part::RedisFlush, g.flush);
    add(Part::RedisDiscard, g.discard);
    add(Part::RedisSave, g.save);
}

// --- input ---------------------------------------------------------------------------------

void SettingsWindow::on_redis_click(const Hit& hit) {
    switch (hit.part) {
        case Part::RedisInstall:
            install_garnet();
            break;
        case Part::RedisCopyAddress:
            if (redis_) {
                std::string copy = redis_address_copy(*redis_);
                if (put_secret_on_clipboard(hwnd_, copy)) {
                    show_message(std::string(text(lang_, Str::RedisCopied)), false);
                }
            }
            break;
        case Part::RedisCopyPassword:
            copy_redis_password();
            break;
        case Part::RedisKeep:
            if (redis_ && !redis_saving_) {
                redis_toggle_keep_running(*redis_, redis_changes_);
                relayout();
            }
            break;
        case Part::RedisMemory:
            if (!redis_edits_.empty()) {
                SetFocus(redis_edits_.front()->hwnd());
            }
            break;
        case Part::RedisFlush:
            flush_redis();
            break;
        case Part::RedisDiscard:
            discard_redis();
            break;
        case Part::RedisSave:
            (void)save_redis();
            break;
        default:
            break;
    }
}

// --- painting ------------------------------------------------------------------------------

void SettingsWindow::paint_redis_shapes(Canvas& c) const {
    const Palette& p = palette(dark_);
    const RedisGeometry& rg = geometry_.redis;
    const float border = std::max(1.0F, pxf(1.0F));
    c.clip({main_left() - px(4), 0, width_, height_});
    if (!is_empty(rg.pill) && redis_) {
        const MailPill kind = mail_pill(redis_->state);
        const StateColors& sc = kind == MailPill::Good      ? p.on
                                : kind == MailPill::Waiting ? p.asleep
                                : kind == MailPill::Bad     ? p.problem
                                                            : p.off;
        if (sc.border != sc.fill) {
            c.round_rect(rg.pill, pxf(11), sc.fill, sc.border, border);
        } else {
            c.round_rect(rg.pill, pxf(11), sc.fill);
        }
    }
    if (!is_empty(rg.failed)) {
        c.round_rect(rg.failed, pxf(4), p.banner_error.fill);
    }
    if (!is_empty(rg.save_failed)) {
        c.round_rect(rg.save_failed, pxf(4), p.banner_error.fill);
    }
    if (!is_empty(rg.install)) {
        const Hit hit{Part::RedisInstall, -1, -1};
        if (connected_) {
            c.round_rect(rg.install, pxf(4), p.accent);
            if (hover_ == hit) {
                c.round_outline(rg.install, pxf(4), p.accent_text, pxf(1));
            }
        } else {
            paint_button(c, rg.install, false, false, false);
        }
        if (is_focused(hit)) {
            paint_ring(c, rg.install);
        }
    }
    if (!is_empty(rg.progress_bar)) {
        const auto job = std::find_if(jobs_.begin(), jobs_.end(), is_garnet_job);
        c.round_rect(rg.progress_bar, pxf(2), p.bar_track);
        if (job != jobs_.end()) {
            if (const auto fraction = job_fraction(*job)) {
                const int done = static_cast<int>(static_cast<double>(layout::width_of(rg.progress_bar)) * *fraction);
                if (done > 0) {
                    c.round_rect({rg.progress_bar.left, rg.progress_bar.top, rg.progress_bar.left + done,
                                  rg.progress_bar.bottom},
                                 pxf(2), p.accent);
                }
            }
        }
    }
    if (redis_ && redis_->installed) {
        c.round_rect(rg.addr_card, pxf(8), p.card, p.card_border, border);
        const Hit copy_address{Part::RedisCopyAddress, -1, -1};
        paint_button(c, rg.server_copy, hover_ == copy_address, is_focused(copy_address), true);
        const Hit copy_password{Part::RedisCopyPassword, -1, -1};
        const bool can = connected_ && !redis_blocked();
        paint_button(c, rg.password_copy, hover_ == copy_password && can, is_focused(copy_password), can);

        c.round_rect(rg.opt_card, pxf(8), p.card, p.card_border, border);
        const Hit keep{Part::RedisKeep, -1, -1};
        if (hover_ == keep && !redis_saving_) {
            c.round_rect(rg.keep_row, pxf(4), p.row_hover);
        }
        paint_switch(c, rg.keep_switch, redis_keep_running(*redis_, redis_changes_), hover_ == keep && !redis_saving_);
        if (is_focused(keep)) {
            paint_ring(c, rg.keep_row);
        }
        const bool bad = redis_memory_bad(redis_changes_);
        const bool memory_focused = !redis_edits_.empty() && redis_edits_.front()->focused();
        c.round_rect(rg.memory_box, pxf(4), p.input, bad ? p.banner_error.text : p.input_border, border);
        if (memory_focused) {
            c.round_outline(rg.memory_box, pxf(4), p.accent, pxf(2));
        }

        c.round_rect(rg.data_card, pxf(8), p.card, p.card_border, border);
        const Hit flush{Part::RedisFlush, -1, -1};
        // Flush all: the outline in red, as Mail's Delete all.
        c.round_rect(rg.flush, pxf(4), hover_ == flush && can ? p.row_hover : p.button,
                     can ? p.problem.text : p.button_border, border);
        if (is_focused(flush)) {
            paint_ring(c, rg.flush);
        }

        const bool changes = !redis_changes_.empty();
        const Hit discard{Part::RedisDiscard, -1, -1};
        paint_button(c, rg.discard, hover_ == discard && changes, is_focused(discard), changes && !redis_saving_);
        const Hit save{Part::RedisSave, -1, -1};
        if (changes && !redis_saving_ && connected_ && !bad) {
            c.round_rect(rg.save, pxf(4), p.accent);
            if (is_focused(save)) {
                paint_ring(c, rg.save);
            }
        } else {
            paint_button(c, rg.save, false, is_focused(save), false);
        }
    }
    scroll_bar_.paint(c, p);
    c.unclip();
}

void SettingsWindow::paint_redis_text(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const RedisGeometry& rg = g.redis;
    c.clip_text({main_left() - px(4), 0, width_, height_});
    c.text(g.title, w(text(lang_, Str::SettingsRedis)), fonts_.title.get(), p.text, Align::Start, false);
    c.paragraph(g.hint, w(text(lang_, Str::RedisHint)), fonts_.caption.get(), p.text_subtle);
    if (!is_empty(rg.note)) {
        c.text(rg.note, w(text(lang_, Str::SettingsLoading)), fonts_.button.get(), p.text_subtle, Align::Start, false);
    }
    if (!redis_) {
        c.unclip_text();
        return;
    }
    if (!is_empty(rg.pill)) {
        const MailPill kind = mail_pill(redis_->state);
        const StateColors& sc = kind == MailPill::Good      ? p.on
                                : kind == MailPill::Waiting ? p.asleep
                                : kind == MailPill::Bad     ? p.problem
                                                            : p.off;
        c.text(rg.pill, w(redis_state_text(*redis_, lang_)), fonts_.small_bold.get(), sc.text, Align::Center, false);
    }
    if (!is_empty(rg.version)) {
        c.text(rg.version, w(fill(lang_, Str::RedisVersion, {redis_->version})), fonts_.caption.get(), p.text_subtle,
               Align::End, false);
    }
    if (!redis_->installed) {
        c.paragraph(rg.install_note, w(text(lang_, Str::RedisNotInstalledNote)), fonts_.button.get(), p.text);
        if (!is_empty(rg.install)) {
            c.text(rg.install, w(text(lang_, Str::RedisInstall)), fonts_.button.get(),
                   connected_ ? p.accent_text : p.button_disabled_text, Align::Center, false);
        }
        if (!is_empty(rg.progress_text)) {
            const auto job = std::find_if(jobs_.begin(), jobs_.end(), is_garnet_job);
            c.text(rg.progress_text,
                   w(job != jobs_.end() ? job_text(*job, lang_) : std::string(text(lang_, Str::SettingsStarting))),
                   fonts_.button.get(), p.text_subtle, Align::Start, false);
        }
        c.unclip_text();
        return;
    }
    if (!is_empty(rg.failed)) {
        RECT inner = rg.failed;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(fill(lang_, Str::RedisFailed, {rtl_ ? isolate_ltr(redis_->error) : redis_->error})),
                    fonts_.caption.get(), p.banner_error.text);
    }
    if (!is_empty(rg.save_failed)) {
        RECT inner = rg.save_failed;
        InflateRect(&inner, -px(12), -px(7));
        const std::string words = redis_save_failed_text(*redis_, lang_);
        c.paragraph(inner, w(words), fonts_.caption.get(), p.banner_error.text);
    }

    c.text(rg.addr_title, w(text(lang_, Str::RedisAddress)), fonts_.body_bold.get(), p.text, Align::Start, false);
    c.text(rg.server_label, w(text(lang_, Str::RedisServer)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    c.text(rg.server_value, w(redis_address_text(*redis_)), fonts_.mono.get(), p.text, Align::Start, true);
    c.text(rg.server_copy, w(text(lang_, Str::DbCopy)), fonts_.button.get(), p.text, Align::Center, false);
    c.text(rg.password_label, w(text(lang_, Str::RedisPassword)), fonts_.caption.get(), p.text_subtle, Align::Start,
           false);
    c.text(rg.password_value, w(redis_password_mask()), fonts_.mono.get(), p.text, Align::Start, true);
    c.text(rg.password_note, w(text(lang_, Str::RedisPasswordNote)), fonts_.caption.get(), p.text_subtle, Align::Start,
           false);
    const bool can = connected_ && !redis_blocked();
    c.text(rg.password_copy, w(text(lang_, Str::DbCopy)), fonts_.button.get(), can ? p.text : p.button_disabled_text,
           Align::Center, false);
    c.paragraph(rg.env_note, w(text(lang_, Str::RedisEnvNote)), fonts_.caption.get(), p.text_subtle);

    c.text(rg.opt_title, w(text(lang_, Str::RedisOptions)), fonts_.body_bold.get(), p.text, Align::Start, false);
    c.text(rg.keep_label, w(text(lang_, Str::RedisKeepRunning)), fonts_.body.get(), p.text, Align::Start, false);
    c.paragraph(rg.keep_note, w(text(lang_, Str::RedisKeepRunningNote)), fonts_.caption.get(), p.text_subtle);
    c.text(rg.memory_label, w(text(lang_, Str::RedisMemory)), fonts_.button.get(), p.text, Align::Start, false);
    c.text(rg.memory_unit, w(text(lang_, Str::RedisMemoryUnit)), fonts_.caption.get(), p.text_subtle, Align::Start,
           false);
    if (!is_empty(rg.memory_bad)) {
        c.text(rg.memory_bad, w(text(lang_, Str::RedisMemoryBad)), fonts_.caption.get(), p.banner_error.text,
               Align::Start, false);
    }

    c.text(rg.data_title, w(text(lang_, Str::RedisData)), fonts_.body_bold.get(), p.text, Align::Start, false);
    c.text(rg.data_count, w(redis_data_text(*redis_, lang_)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    c.text(rg.flush, w(text(lang_, Str::RedisFlush)), fonts_.button.get(), can ? p.problem.text : p.button_disabled_text,
           Align::Center, false);
    c.paragraph(rg.unsupported, w(text(lang_, Str::RedisUnsupported)), fonts_.caption.get(), p.text_subtle);

    c.paragraph(rg.footer_text, w(text(lang_, Str::RedisFooter)), fonts_.caption.get(), p.text_subtle);
    const bool changes = !redis_changes_.empty();
    const bool bad = redis_memory_bad(redis_changes_);
    c.text(rg.discard, w(text(lang_, Str::PhpDiscard)), fonts_.button.get(),
           changes && !redis_saving_ ? p.text : p.button_disabled_text, Align::Center, false);
    c.text(rg.save, w(text(lang_, redis_saving_ ? Str::PhpSaving : Str::PhpSave)), fonts_.button.get(),
           changes && !redis_saving_ && connected_ && !bad ? p.accent_text : p.button_disabled_text, Align::Center,
           false);
    c.unclip_text();
}

}  // namespace floserver::ui

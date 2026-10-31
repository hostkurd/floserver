// Settings > Database (MariaDB): one series' server (its state with Start,
// Stop and Restart, its port, Keep running), the my.ini values people change
// most as fields with custom.ini and the final my.ini opened in Notepad, and
// root's password (shown or copied only when asked, and changed). Built as
// Settings > PHP is: database.get when the section opens or the version
// changes, changes wait for Save (one database.set), and the server's state
// comes from the main window's 2 s snapshot, so nothing polls.

#include "settings_window.h"

#include "main_window_layout.h"
#include "model/theme.h"

#include <shellapi.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace floserver::ui {

using layout::width_of;
using nlohmann::json;

namespace {

constexpr int kDbEditBase = 3000;  // db_edits_[i] has id kDbEditBase + i
constexpr UINT kFirstChoice = 100;
constexpr int kGap = 12;
constexpr int kCardPad = 16;
constexpr int kControlH = 30;
constexpr int kFieldMinW = 170;
constexpr int kLabelH = 16;
constexpr int kNoteH = 18;
constexpr int kCheckBox = 16;
constexpr int kPortW = 140;
constexpr int kPasswordW = 240;

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

const DbIniField& field_at(int index) { return settings_db_ini_fields()[static_cast<std::size_t>(index)]; }

// What stands for root's password while it's hidden.
constexpr std::string_view kMasked = "\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F"
                            "\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F";

void wipe(std::string& s) {
    SecureZeroMemory(s.data(), s.size());
    s.clear();
}

}  // namespace

// --- loading, saving, leaving ----------------------------------------------------------

std::vector<std::pair<std::string, std::string>> SettingsWindow::db_versions() const {
    return settings_db_versions(has_installed_, installed_, reply_);
}

bool SettingsWindow::settle_db_changes(AfterSave then, const std::string& series) {
    if (!db_ || db_changes_.empty()) {
        return true;
    }
    if (asking_ || db_saving_) {
        return false;
    }
    asking_ = true;
    const SaveChoice choice = ask_to_save(hwnd_, lang_, fill(lang_, Str::DbAskSave, {db_series_}));
    asking_ = false;
    if (quit_after_ask_) {
        PostMessageW(owner_, closed_message_, 1, 0);
        return false;
    }
    if (hwnd_ == nullptr) {
        return false;
    }
    switch (choice) {
        case SaveChoice::Save:
            if (save_db()) {
                after_save_ = then;
                after_save_series_ = series;
            }
            return false;
        case SaveChoice::DontSave:
            discard_db();
            return true;
        case SaveChoice::Cancel:
            break;
    }
    return false;
}

void SettingsWindow::load_db(const std::string& series) {
    db_series_ = series;
    db_error_key_.clear();
    db_error_.clear();
    db_.reset();
    db_changes_ = {};
    wipe(root_shown_);
    password_error_.clear();
    db_server_ = db_server_from_list(databases_, series);
    db_loading_ = true;
    worker_.database_call("database.get", {{"series", series}});
    if (db_edits_.size() > settings_db_ini_fields().size() + 1) {
        db_edits_.back()->set_text({});  // a password typed for another version isn't kept
    }
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SettingsWindow::on_db_answers(const Snapshot& snap) {
    bool close_now = false;
    for (const Snapshot::DbAnswer& answer : snap.db) {
        const bool save = answer.method == "database.set";
        if (save && db_saving_) {
            db_saving_ = false;
            set_db_read_only(false);
        }
        // A late answer for a version the user has left: dropped, errors too.
        if (answer.series != db_series_) {
            continue;
        }
        if (answer.method == "database.get" || save) {
            if (!answer.error.empty() || !answer.reply) {
                if (!save) {
                    db_loading_ = false;
                    show_message(fill(lang_, Str::DbReadFailed, {db_series_, answer.error}), true);
                    continue;
                }
                after_save_ = AfterSave::Stay;  // refused: stay here with the reason shown
                std::vector<std::string> keys;
                for (const DbIniField& field : settings_db_ini_fields()) {
                    keys.push_back(field.key);
                }
                db_error_key_ = ini_error_key(answer.error, keys);
                db_error_ = db_error_key_.empty() ? std::string() : answer.error;
                show_message(fill(lang_, Str::PhpSaveFailed, {answer.error}), true);
                continue;
            }
            auto series = db_series_from_reply(*answer.reply);
            if (!series || series->series != db_series_) {
                continue;
            }
            db_server_ = series->server;
            const bool restarting = series->restarting;
            db_ = std::move(series);
            db_loading_ = false;
            if (save) {
                db_changes_ = {};
                db_error_key_.clear();
                db_error_.clear();
                show_message(restarting ? fill(lang_, Str::DbRestarting, {db_series_}) : std::string(text(lang_, Str::PhpSaved)),
                             false);
            }
            fill_db_edits();
            if (save) {
                const AfterSave then = std::exchange(after_save_, AfterSave::Stay);
                const std::string target = std::exchange(after_save_series_, {});
                if (then == AfterSave::Close) {
                    close_now = true;
                } else if (then == AfterSave::Runtimes) {
                    open_section(after_save_section_);
                } else if (then == AfterSave::Version && !target.empty()) {
                    load_db(target);
                }
            }
        } else if (answer.method == "database.server") {
            if (!answer.error.empty() || !answer.reply) {
                show_message(fill(lang_, Str::DbServerFailed, {db_series_, answer.error}), true);
            } else if (answer.reply->is_object() && answer.reply->contains("server")) {
                db_server_ = db_server_from_json((*answer.reply)["server"]);
            }
        } else if (answer.method == "database.root") {
            root_busy_ = false;
            if (!answer.error.empty() || !answer.reply) {
                show_message(fill(lang_, Str::DbRootReadFailed, {answer.error}), true);
                continue;
            }
            std::string password = answer.reply->is_object() && answer.reply->contains("password") &&
                                           (*answer.reply)["password"].is_string()
                                       ? (*answer.reply)["password"].get<std::string>()
                                       : std::string();
            if (answer.purpose == "copy") {
                const bool done = put_secret_on_clipboard(hwnd_, password);
                show_message(done ? std::string(text(lang_, Str::DbPasswordCopied))
                                  : fill(lang_, Str::OpenFailed, {"clipboard"}),
                             !done);
            } else {
                wipe(root_shown_);
                root_shown_ = std::move(password);
            }
            wipe(password);
        } else if (answer.method == "database.root_set") {
            root_busy_ = false;
            if (!answer.error.empty() || !answer.reply) {
                password_error_ = fill(lang_, Str::DbPasswordFailed, {answer.error});
                continue;
            }
            wipe(root_shown_);  // the old one: Show asks again
            password_error_.clear();
            show_message(std::string(text(lang_, Str::DbPasswordSet)), false);
        } else if (answer.method == "database.custom_ini" || answer.method == "database.final_ini") {
            const std::string path = answer.reply && answer.reply->is_object() && answer.reply->contains("path") &&
                                             (*answer.reply)["path"].is_string()
                                         ? (*answer.reply)["path"].get<std::string>()
                                         : std::string();
            if (path.empty()) {
                show_message(fill(lang_, Str::DbOpenIniFailed, {answer.error}), true);
                continue;
            }
            // Notepad, so the user edits (or reads) it.
            const std::wstring quoted = L"\"" + w(path) + L"\"";
            const auto opened = reinterpret_cast<INT_PTR>(
                ShellExecuteW(nullptr, L"open", L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL));
            if (opened <= 32) {
                show_message(fill(lang_, Str::DbOpenIniFailed, {path}), true);
            } else {
                show_message(std::string(text(lang_, answer.method == "database.final_ini" ? Str::DbFinalOpened
                                                                                        : Str::DbIniOpened)),
                             false);
            }
        }
    }
    // The section was opened before anything installed was known.
    if (section_ == Section::Database && db_series_.empty() && !db_loading_) {
        const auto versions = db_versions();
        if (!versions.empty()) {
            load_db(versions.front().first);
        }
    }
    return close_now;
}

bool SettingsWindow::save_db() {
    if (!db_ || db_changes_.empty() || db_saving_) {
        return false;
    }
    if (db_port_bad(db_changes_)) {
        show_message(std::string(text(lang_, Str::DbPortBad)), true);
        return false;
    }
    if (!connected_) {
        show_message(fill(lang_, Str::PhpSaveFailed, {text(lang_, Str::ConnectionLost)}), true);
        return false;
    }
    db_saving_ = true;
    set_db_read_only(true);
    message_.clear();
    worker_.database_call("database.set", database_set_params(db_series_, db_changes_));
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void SettingsWindow::discard_db() {
    if (db_saving_) {
        return;
    }
    db_changes_ = {};
    db_error_key_.clear();
    db_error_.clear();
    fill_db_edits();
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::set_root_password() {
    const FieldEdit* edit = edit_of({Part::DbNewPassword, -1, -1});
    if (edit == nullptr || !db_ || root_busy_ || !connected_) {
        return;
    }
    std::string password = edit->text();
    if (const auto problem = db_password_problem(password)) {
        password_error_ = std::string(text(lang_, *problem));
        wipe(password);
    } else {
        password_error_.clear();
        root_busy_ = true;
        message_.clear();
        worker_.database_call("database.root_set", {{"series", db_series_}, {"password", password}});
        wipe(password);
        db_edits_.back()->set_text({});
    }
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- the edit boxes ---------------------------------------------------------------------

void SettingsWindow::make_db_edits() {
    if (!db_edits_.empty()) {
        return;
    }
    const Palette& p = palette(dark_);
    const auto make = [&](std::string_view cue) {
        const int id = kDbEditBase + static_cast<int>(db_edits_.size());
        auto edit = std::make_unique<FieldEdit>(hwnd_, id, fonts_.button.get(), rtl_,
                                                [this, id](WPARAM key) { return on_edit_key(id, key); });
        edit->set_cue(cue);
        edit->set_cue_color(p.text_subtle);
        db_edits_.push_back(std::move(edit));
    };
    make(text(lang_, Str::DbPortCue));
    for (std::size_t i = 0; i < settings_db_ini_fields().size(); ++i) {
        make(text(lang_, Str::PhpDefaultCue));
    }
    make(text(lang_, Str::DbNewPasswordCue));
    db_edits_.back()->set_password(true);
    fill_db_edits();
}

void SettingsWindow::fill_db_edits() {
    if (!db_ || db_edits_.empty()) {
        return;
    }
    db_edits_.front()->set_text(db_port_text(*db_, db_changes_));
    const auto& fields = settings_db_ini_fields();
    for (std::size_t i = 0; i < fields.size() && i + 1 < db_edits_.size(); ++i) {
        db_edits_[i + 1]->set_text(db_ini_text(*db_, db_changes_, fields[i].key));
    }
}

void SettingsWindow::place_db_edits() {
    if (section_ != Section::Database || db_edits_.empty()) {
        return;
    }
    const DbGeometry& g = geometry_.db;
    const bool shown = db_.has_value();
    const int h = px(20);
    db_edits_.front()->place(g.port, px(8), h, shown && !is_empty(g.port));
    const auto& fields = settings_db_ini_fields();
    for (std::size_t i = 0; i < fields.size() && i + 1 < db_edits_.size(); ++i) {
        const RECT box = i < g.boxes.size() ? g.boxes[i] : RECT{};
        db_edits_[i + 1]->place(box, px(8), h, shown && !is_empty(box));
    }
    db_edits_.back()->place(g.new_password, px(8), h, shown && !is_empty(g.new_password));
}

void SettingsWindow::set_db_read_only(bool read_only) {
    // The port and the fields; the new password has its own button.
    for (std::size_t i = 0; i + 1 < db_edits_.size(); ++i) {
        db_edits_[i]->set_read_only(read_only);
    }
}

void SettingsWindow::on_db_edit_change(const Hit& hit, FieldEdit& edit) {
    if (hit.part == Part::DbNewPassword) {
        if (!password_error_.empty()) {
            password_error_.clear();
            relayout();
        }
        return;
    }
    if (!db_ || db_saving_) {
        return;
    }
    if (hit.part == Part::DbPort) {
        db_set_port_text(*db_, db_changes_, edit.text());
    } else if (hit.part == Part::DbField) {
        db_set_ini_text(*db_, db_changes_, field_at(hit.row).key, edit.text());
        if (field_at(hit.row).key == db_error_key_) {
            db_error_key_.clear();
            db_error_.clear();
        }
        InvalidateRect(edit.hwnd(), nullptr, TRUE);  // its tint follows
    }
    relayout();
}

// --- geometry ----------------------------------------------------------------------------

void SettingsWindow::layout_db(HDC dc, Geometry& g, int x0, int x1, int y) const {
    DbGeometry& dg = g.db;
    const auto measure = [&](std::string_view s, HFONT font) { return Canvas::measure(dc, w(s), font, rtl_); };
    const auto button_w = [&](Str label) { return measure(text(lang_, label), fonts_.button.get()) + px(24); };

    // The title and what the section does, with the version picker beside them.
    const auto versions = db_versions();
    int picker_w = 0;
    int label_w = 0;
    if (!versions.empty()) {
        picker_w = px(170);
        for (const auto& v : versions) {
            picker_w = std::max(picker_w, Canvas::measure(dc, w("MariaDB " + v.second), fonts_.button.get(), false) + px(34));
        }
        label_w = measure(text(lang_, Str::PhpVersion), fonts_.caption.get());
    }
    const int right_w = picker_w > 0 ? picker_w + px(8) + label_w + px(kGap) : 0;
    const int hint_w = std::max(px(100), x1 - x0 - right_w);
    const int hint_h = text_height(dc, w(text(lang_, Str::DbHint)), fonts_.caption.get(), hint_w, rtl_);
    g.title = {x0, y, x0 + hint_w, y + px(28)};
    g.hint = {x0, g.title.bottom, x0 + hint_w, g.title.bottom + hint_h};
    if (picker_w > 0) {
        const int top = std::max(y, static_cast<int>(g.hint.bottom) - px(kControlH));
        dg.version = {x1 - picker_w, top, x1, top + px(kControlH)};
        dg.version_label = {static_cast<int>(dg.version.left) - px(8) - label_w, top,
                            static_cast<int>(dg.version.left) - px(8), top + px(kControlH)};
    }
    y = std::max(g.hint.bottom, dg.version.bottom) + px(kGap);

    if (!message_.empty()) {
        const int mh = std::max(px(32), text_height(dc, w(message_), fonts_.button.get(), x1 - x0 - px(24), rtl_) + px(14));
        g.message = {x0, y, x1, y + mh};
        y += mh + px(kGap);
    }
    if ((!has_installed_ && !has_reply_) || (!db_ && db_loading_)) {
        dg.note = {x0, y, x1, y + px(24)};
        g.content_h = dg.note.bottom + scroll_ + px(8);
        return;
    }
    if (versions.empty()) {
        const int nh = text_height(dc, w(text(lang_, Str::DbNoneInstalled)), fonts_.button.get(), x1 - x0, rtl_);
        dg.note = {x0, y, x1, y + nh};
        y += nh + px(kGap);
        dg.runtimes = {x0, y, x0 + button_w(Str::PhpGoToRuntimes), y + px(kControlH)};
        g.content_h = dg.runtimes.bottom + scroll_ + px(8);
        return;
    }
    if (!db_) {
        g.content_h = y + scroll_ + px(8);
        return;
    }
    if (!db_->problems.empty()) {
        std::string all;
        for (const std::string& problem : db_->problems) {
            all += (all.empty() ? "" : "; ") + problem;
        }
        const int ph = text_height(dc, w(fill(lang_, Str::DbProblems, {all})), fonts_.caption.get(), x1 - x0 - px(24), rtl_) +
                       px(14);
        dg.problems = {x0, y, x1, y + ph};
        y += ph + px(kGap);
    }
    const int cx0 = x0 + px(kCardPad);
    const int cx1 = x1 - px(kCardPad);
    const int avail = cx1 - cx0;

    // The server: its state with Start, Stop and Restart, then the port and Keep running.
    int top = y + px(14);
    dg.server_card = {x0, y, x1, y};
    const int restart_w = button_w(Str::DbRestart);
    const int stop_w = button_w(Str::DbStop);
    const int start_w = button_w(Str::DbStart);
    dg.restart = {cx1 - restart_w, top, cx1, top + px(kControlH)};
    dg.stop = {static_cast<int>(dg.restart.left) - px(8) - stop_w, top, static_cast<int>(dg.restart.left) - px(8),
               top + px(kControlH)};
    dg.start = {static_cast<int>(dg.stop.left) - px(8) - start_w, top, static_cast<int>(dg.stop.left) - px(8),
                top + px(kControlH)};
    dg.server_title = {cx0, top, std::max(cx0, static_cast<int>(dg.start.left) - px(kGap)), top + px(kControlH)};
    int yy = top + px(kControlH) + px(8);
    const std::string state = db_state_text(*db_, db_server_, lang_);
    const int state_h = std::max(px(kNoteH), text_height(dc, w(state), fonts_.button.get(), avail, rtl_));
    dg.state = {cx0, yy, cx1, yy + state_h};
    yy += state_h + px(4);
    const std::string used = db_->used_by.empty() ? std::string(text(lang_, Str::DbUsedByNobody))
                                                  : fill_pattern(text(lang_, Str::DbUsedBy), false,
                                                                 {db_names_text(db_->used_by, lang_)});
    const int used_h = std::max(px(kNoteH), text_height(dc, w(used), fonts_.caption.get(), avail, rtl_));
    dg.used_by = {cx0, yy, cx1, yy + used_h};
    yy += used_h + px(kGap);
    // The port: a label over its box; Keep running beside it with its note.
    dg.port_label = {cx0, yy, cx0 + px(kPortW), yy + px(kLabelH)};
    dg.port = {cx0, yy + px(kLabelH) + px(4), cx0 + px(kPortW), yy + px(kLabelH) + px(4) + px(kControlH)};
    const int kx = static_cast<int>(dg.port.right) + px(24);
    const int keep_w = px(kCheckBox) + px(8) + measure(text(lang_, Str::DbKeepRunning), fonts_.button.get()) + px(6);
    dg.keep = {kx, static_cast<int>(dg.port.top), std::min(cx1, kx + keep_w), static_cast<int>(dg.port.bottom)};
    std::string keep_note(text(lang_, Str::DbKeepRunningNote));
    if (!db_->kept_by.empty()) {
        keep_note += " " + fill_pattern(text(lang_, Str::DbKeptBy), false, {db_names_text(db_->kept_by, lang_)});
    }
    const int note_x = static_cast<int>(dg.keep.right) + px(8);
    if (cx1 - note_x >= px(160)) {
        const int nh = text_height(dc, w(keep_note), fonts_.caption.get(), cx1 - note_x, rtl_);
        const int ntop = std::max(static_cast<int>(dg.port_label.top),
                                  (static_cast<int>(dg.port.top + dg.port.bottom) - nh) / 2);
        dg.keep_note = {note_x, ntop, cx1, ntop + nh};
        yy = std::max(static_cast<int>(dg.port.bottom), static_cast<int>(dg.keep_note.bottom));
    } else {
        // Narrow: the note under both.
        yy = static_cast<int>(dg.port.bottom) + px(6);
        const int nh = text_height(dc, w(keep_note), fonts_.caption.get(), avail, rtl_);
        dg.keep_note = {cx0, yy, cx1, yy + nh};
        yy += nh;
    }
    if (db_port_bad(db_changes_) || db_changes_.port) {
        const std::string note(text(lang_, db_port_bad(db_changes_) ? Str::DbPortBad : Str::DbPortNote));
        const int nh = std::max(px(kNoteH), text_height(dc, w(note), fonts_.caption.get(), avail, rtl_));
        dg.port_note = {cx0, yy + px(4), cx1, yy + px(4) + nh};
        yy = static_cast<int>(dg.port_note.bottom);
    }
    dg.server_card.bottom = yy + px(14);
    y = static_cast<int>(dg.server_card.bottom) + px(kGap);

    // my.ini: the fields, your own lines, the final file.
    top = y + px(14);
    dg.ini_card = {x0, y, x1, y};
    const int edit_w = button_w(Str::DbEditFull);
    dg.edit_ini = {std::max(cx0, cx1 - edit_w), top, cx1, top + px(kControlH)};
    const int final_w = button_w(Str::DbShowFinal);
    dg.final_ini = {std::max(cx0, static_cast<int>(dg.edit_ini.left) - px(8) - final_w), top,
                    static_cast<int>(dg.edit_ini.left) - px(8), top + px(kControlH)};
    const int ini_end = static_cast<int>(dg.final_ini.left) - px(kGap);
    const int ini_w = Canvas::measure(dc, L"my.ini", fonts_.body_bold.get(), false);
    dg.ini_title = {cx0, top, std::min(ini_end, cx0 + ini_w), top + px(kControlH)};
    dg.ini_hint = {std::min(ini_end, static_cast<int>(dg.ini_title.right) + px(8)), top, ini_end, top + px(kControlH)};
    yy = top + px(kControlH) + px(kGap);
    const auto& fields = settings_db_ini_fields();
    const int fcols = std::clamp((avail + px(kGap)) / (px(kFieldMinW) + px(kGap)), 1, 4);
    const int fcol_w = (avail - (fcols - 1) * px(kGap)) / fcols;
    const int cell_h = px(kLabelH) + px(4) + px(kControlH);
    const int reset_w = measure(text(lang_, Str::PhpReset), fonts_.caption.get()) + px(2);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (!db_error_key_.empty() && fields[i].key == db_error_key_) {
            dg.error_field = static_cast<int>(i);
        }
    }
    const int error_h = px(kNoteH);
    const int flines = (static_cast<int>(fields.size()) + fcols - 1) / fcols;
    for (int line = 0; line < flines; ++line) {
        const int cell_top = yy;
        for (int col = 0; col < fcols; ++col) {
            const auto i = static_cast<std::size_t>(line * fcols + col);
            if (i >= fields.size()) {
                break;
            }
            const int left = cx0 + col * (fcol_w + px(kGap));
            RECT label{left, cell_top, left + fcol_w, cell_top + px(kLabelH)};
            RECT reset{};
            if (db_ini_marked(*db_, db_changes_, fields[i].key)) {
                reset = {left + fcol_w - reset_w, cell_top, left + fcol_w, cell_top + px(kLabelH) + px(2)};
                label.right = reset.left - px(6);
            }
            dg.labels.push_back(label);
            dg.resets.push_back(reset);
            dg.boxes.push_back({left, cell_top + px(kLabelH) + px(4), left + fcol_w, cell_top + cell_h});
            if (dg.error_field == static_cast<int>(i)) {
                dg.field_error = {left, cell_top + cell_h + px(2), cx1, cell_top + cell_h + px(2) + error_h};
            }
        }
        yy += cell_h;
        if (dg.error_field >= 0 && dg.error_field / fcols == line) {
            yy += error_h;
        }
        if (line + 1 < flines) {
            yy += px(kGap);
        }
    }
    // custom.ini: lines left out, and names MariaDB doesn't know (it wouldn't start).
    if (!db_->custom_problems.empty()) {
        std::string all;
        for (const std::string& problem : db_->custom_problems) {
            all += (all.empty() ? "" : "; ") + problem;
        }
        const std::string line = fill(lang_, Str::DbCustomProblems, {all});
        const int ph = text_height(dc, w(line), fonts_.caption.get(), avail - px(24), rtl_) + px(14);
        yy += px(10);
        dg.custom_problems = {cx0, yy, cx1, yy + ph};
        yy += ph;
    }
    for (const std::string& key : db_->custom_keys) {
        if (const auto suggestion = unknown_db_option(db_->known_options, key)) {
            const std::string words = unknown_db_option_text(db_series_, key, *suggestion, lang_);
            const int nh = std::max(error_h, text_height(dc, w(words), fonts_.caption.get(), avail, rtl_));
            dg.warnings.emplace_back(RECT{cx0, yy + px(4), cx1, yy + px(4) + nh}, words);
            yy += px(4) + nh;
        }
    }
    yy += px(10);
    const int custom_h = std::max(px(kNoteH), text_height(dc, w(text(lang_, Str::DbCustomIniUsed)), fonts_.caption.get(),
                                                           avail, rtl_));
    dg.custom_note = {cx0, yy, cx1, yy + custom_h};
    yy += custom_h;
    dg.ini_card.bottom = yy + px(14);
    y = static_cast<int>(dg.ini_card.bottom) + px(kGap);

    // Root access.
    top = y + px(14);
    dg.root_card = {x0, y, x1, y};
    dg.root_title = {cx0, top, cx1, top + px(kControlH)};
    yy = top + px(kControlH) + px(4);
    const int copy_w = button_w(Str::DbCopy);
    const int show_w = std::max(button_w(Str::DbShow), button_w(Str::DbHide));
    dg.root_copy = {cx1 - copy_w, yy, cx1, yy + px(kControlH)};
    dg.root_show = {static_cast<int>(dg.root_copy.left) - px(8) - show_w, yy, static_cast<int>(dg.root_copy.left) - px(8),
                    yy + px(kControlH)};
    const int rl_w = measure(text(lang_, Str::DbRootPassword), fonts_.caption.get()) + px(2);
    dg.root_label = {cx0, yy, std::min(static_cast<int>(dg.root_show.left) - px(8), cx0 + rl_w), yy + px(kControlH)};
    dg.root_value = {static_cast<int>(dg.root_label.right) + px(12), yy, static_cast<int>(dg.root_show.left) - px(kGap),
                     yy + px(kControlH)};
    yy += px(kControlH) + px(kGap);
    dg.new_label = {cx0, yy, cx1, yy + px(kLabelH)};
    yy += px(kLabelH) + px(4);
    const int set_w = button_w(Str::DbSetPassword);
    const int pw_w = std::min(px(kPasswordW), std::max(px(100), avail - set_w - px(8)));
    dg.new_password = {cx0, yy, cx0 + pw_w, yy + px(kControlH)};
    dg.set_password = {static_cast<int>(dg.new_password.right) + px(8), yy,
                       static_cast<int>(dg.new_password.right) + px(8) + set_w, yy + px(kControlH)};
    yy += px(kControlH);
    if (!password_error_.empty()) {
        const int eh = std::max(px(kNoteH), text_height(dc, w(password_error_), fonts_.caption.get(), avail, rtl_));
        dg.password_error = {cx0, yy + px(2), cx1, yy + px(2) + eh};
        yy = static_cast<int>(dg.password_error.bottom);
    }
    yy += px(kGap);
    const int rn_h = text_height(dc, w(text(lang_, Str::DbRootNote)), fonts_.caption.get(), avail, rtl_);
    dg.root_note = {cx0, yy, cx1, yy + rn_h};
    yy += rn_h + px(4);
    const std::string dbs = db_->databases.empty() ? std::string(text(lang_, Str::DbNoDatabases))
                                                   : fill_pattern(text(lang_, Str::DbDatabases), false,
                                                                  {db_names_text(db_->databases, lang_)});
    const int dh = std::max(px(kNoteH), text_height(dc, w(dbs), fonts_.caption.get(), avail, rtl_));
    dg.databases = {cx0, yy, cx1, yy + dh};
    yy += dh;
    dg.root_card.bottom = yy + px(14);
    y = static_cast<int>(dg.root_card.bottom) + px(kGap);

    // The footer: at the window's bottom when there's room, else after the cards.
    const int save_w = std::max(button_w(Str::PhpSave), button_w(Str::PhpSaving));
    const int discard_w = button_w(Str::PhpDiscard);
    const std::string restart = db_restart_text(*db_, db_server_, lang_);
    const int text_w = std::max(px(80), x1 - x0 - save_w - discard_w - px(8) - px(kGap));
    const int text_h = text_height(dc, w(restart), fonts_.caption.get(), text_w, rtl_);
    const int footer_h = std::max(px(kControlH), text_h);
    const int footer_top = std::max(y, height_ - px(16) - footer_h);
    dg.save = {x1 - save_w, footer_top, x1, footer_top + px(kControlH)};
    dg.discard = {static_cast<int>(dg.save.left) - px(8) - discard_w, footer_top, static_cast<int>(dg.save.left) - px(8),
                  footer_top + px(kControlH)};
    const int text_top = footer_top + std::max(0, (px(kControlH) - text_h) / 2);
    dg.footer_text = {x0, text_top, x0 + text_w, text_top + text_h};
    g.content_h = footer_top + footer_h + scroll_ + px(16);
}

const RECT* SettingsWindow::db_rect_of(const Hit& hit) const {
    const DbGeometry& g = geometry_.db;
    const auto in = [&](const std::vector<RECT>& list) -> const RECT* {
        return hit.row >= 0 && std::cmp_less(hit.row, list.size()) ? &list[static_cast<std::size_t>(hit.row)] : nullptr;
    };
    switch (hit.part) {
        case Part::DbVersion:
            return &g.version;
        case Part::DbRuntimes:
            return &g.runtimes;
        case Part::DbStart:
            return &g.start;
        case Part::DbStop:
            return &g.stop;
        case Part::DbRestart:
            return &g.restart;
        case Part::DbPort:
            return &g.port;
        case Part::DbKeep:
            return &g.keep;
        case Part::DbFinalIni:
            return &g.final_ini;
        case Part::DbEditIni:
            return &g.edit_ini;
        case Part::DbField:
            return in(g.boxes);
        case Part::DbReset:
            return in(g.resets);
        case Part::DbRootShow:
            return &g.root_show;
        case Part::DbRootCopy:
            return &g.root_copy;
        case Part::DbNewPassword:
            return &g.new_password;
        case Part::DbSetPassword:
            return &g.set_password;
        case Part::DbDiscard:
            return &g.discard;
        case Part::DbSave:
            return &g.save;
        default:
            break;
    }
    return nullptr;
}

void SettingsWindow::db_focus_order(std::vector<Hit>& order) const {
    const DbGeometry& g = geometry_.db;
    const auto add = [&](Part part, const RECT& r, int row = -1) {
        if (!is_empty(r)) {
            order.push_back({part, -1, row});
        }
    };
    add(Part::DbVersion, g.version);
    add(Part::DbRuntimes, g.runtimes);
    if (!db_) {
        return;
    }
    add(Part::DbStart, g.start);
    add(Part::DbStop, g.stop);
    add(Part::DbRestart, g.restart);
    add(Part::DbPort, g.port);
    add(Part::DbKeep, g.keep);
    add(Part::DbFinalIni, g.final_ini);
    add(Part::DbEditIni, g.edit_ini);
    for (std::size_t i = 0; i < g.boxes.size(); ++i) {
        add(Part::DbField, g.boxes[i], static_cast<int>(i));
        add(Part::DbReset, g.resets[i], static_cast<int>(i));
    }
    add(Part::DbRootShow, g.root_show);
    add(Part::DbRootCopy, g.root_copy);
    add(Part::DbNewPassword, g.new_password);
    add(Part::DbSetPassword, g.set_password);
    add(Part::DbDiscard, g.discard);
    add(Part::DbSave, g.save);
}

// --- input -----------------------------------------------------------------------------------

void SettingsWindow::on_db_click(const Hit& hit) {
    const DbActions actions = db_actions(db_server_);
    const auto server = [this](const char* action, const char* state) {
        message_.clear();
        db_server_.state = state;  // until the next snapshot says how it went
        worker_.database_call("database.server", {{"series", db_series_}, {"action", action}});
        relayout();
    };
    switch (hit.part) {
        case Part::DbVersion:
            db_version_menu();
            break;
        case Part::DbRuntimes:
            open_section(Section::Runtimes);
            break;
        case Part::DbStart:
            if (db_ && connected_ && actions.start) {
                server("start", "starting");
            }
            break;
        case Part::DbStop:
            if (db_ && connected_ && actions.stop) {
                server("stop", "stopping");
            }
            break;
        case Part::DbRestart:
            if (db_ && connected_ && actions.restart) {
                server("restart", "stopping");
            }
            break;
        case Part::DbPort:
        case Part::DbField:
        case Part::DbNewPassword:
            if (FieldEdit* edit = edit_of(hit); edit != nullptr) {
                SetFocus(edit->hwnd());
            }
            break;
        case Part::DbKeep:
            if (db_ && !db_saving_) {
                db_toggle_keep_running(*db_, db_changes_);
                relayout();
            }
            break;
        case Part::DbFinalIni:
        case Part::DbEditIni:
            if (connected_ && !db_series_.empty()) {
                message_.clear();
                worker_.database_call(hit.part == Part::DbFinalIni ? "database.final_ini" : "database.custom_ini",
                                      {{"series", db_series_}});
                relayout();
            }
            break;
        case Part::DbReset:
            if (db_ && !db_saving_ && hit.row >= 0) {
                const std::string& key = field_at(hit.row).key;
                db_reset_ini(*db_, db_changes_, key);
                if (FieldEdit* edit = edit_of({Part::DbField, -1, hit.row}); edit != nullptr) {
                    edit->set_text(db_ini_text(*db_, db_changes_, key));
                    InvalidateRect(edit->hwnd(), nullptr, TRUE);
                }
                relayout();
            }
            break;
        case Part::DbRootShow:
            if (!root_shown_.empty()) {
                wipe(root_shown_);
                relayout();
            } else if (db_ && connected_ && !root_busy_) {
                root_busy_ = true;
                worker_.database_call("database.root", {{"series", db_series_}}, "show");
            }
            break;
        case Part::DbRootCopy:
            if (db_ && connected_ && !root_busy_) {
                root_busy_ = true;
                worker_.database_call("database.root", {{"series", db_series_}}, "copy");
            }
            break;
        case Part::DbSetPassword:
            set_root_password();
            break;
        case Part::DbDiscard:
            discard_db();
            break;
        case Part::DbSave:
            (void)save_db();
            break;
        default:
            break;
    }
}

void SettingsWindow::db_version_menu() {
    const auto versions = db_versions();
    if (versions.empty() || db_saving_) {
        return;
    }
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    std::vector<std::wstring> labels;
    labels.reserve(versions.size());
    for (std::size_t i = 0; i < versions.size(); ++i) {
        labels.push_back(w("MariaDB " + versions[i].second));
        AppendMenuW(menu.get(), MF_STRING | (versions[i].first == db_series_ ? UINT{MF_CHECKED} : 0U),
                    kFirstChoice + static_cast<UINT>(i), labels.back().c_str());
    }
    const RECT box = geometry_.db.version;
    POINT at{box.left, box.bottom};
    ClientToScreen(hwnd_, &at);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
    flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    if (command < kFirstChoice || command - kFirstChoice >= versions.size() || hwnd_ == nullptr) {
        return;
    }
    const std::string& series = versions[command - kFirstChoice].first;
    if (series != db_series_ && settle_db_changes(AfterSave::Version, series)) {
        load_db(series);
    }
}

// --- painting -----------------------------------------------------------------------------------

void SettingsWindow::paint_db_shapes(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const DbGeometry& dg = g.db;
    const float border = std::max(1.0F, pxf(1.0F));
    const RECT column{main_left() - px(4), 0, width_, height_};
    c.clip(column);
    const auto field_box = [&](const RECT& r, bool marked, bool hover, bool edit_focused, bool refused, const Hit& hit) {
        const Rgb outline = refused ? p.banner_error.text : marked ? p.accent : p.input_border;
        c.round_rect(r, pxf(4), marked ? p.input_set : (hover ? p.row_hover : p.input), outline, refused ? pxf(2) : border);
        if (edit_focused) {
            c.round_outline(r, pxf(4), p.accent, pxf(2));
        } else if (is_focused(hit)) {
            paint_ring(c, r);
        }
    };
    if (!is_empty(dg.version)) {
        const Hit hit{Part::DbVersion, -1, -1};
        field_box(dg.version, false, hover_ == hit, false, false, hit);
        const float vx = static_cast<float>(dg.version.right) - pxf(14);
        const float vy = static_cast<float>(dg.version.top + dg.version.bottom) / 2.0F;
        c.line(vx - pxf(4), vy - pxf(2), vx, vy + pxf(2), p.text_subtle, pxf(1.3F));
        c.line(vx, vy + pxf(2), vx + pxf(4), vy - pxf(2), p.text_subtle, pxf(1.3F));
    }
    if (!is_empty(g.message)) {
        c.round_rect(g.message, pxf(4), message_error_ ? p.banner_error.fill : p.banner_info.fill);
    }
    if (!is_empty(dg.runtimes)) {
        const Hit hit{Part::DbRuntimes, -1, -1};
        paint_button(c, dg.runtimes, hover_ == hit, is_focused(hit), connected_);
    }
    if (!is_empty(dg.problems)) {
        c.round_rect(dg.problems, pxf(4), p.banner_error.fill);
    }
    if (db_) {
        const DbActions actions = db_actions(db_server_);
        const auto button = [&](Part part, const RECT& r, bool enabled) {
            const Hit hit{part, -1, -1};
            paint_button(c, r, hover_ == hit && enabled, is_focused(hit), enabled);
        };
        c.round_rect(dg.server_card, pxf(8), p.card, p.card_border, border);
        button(Part::DbStart, dg.start, actions.start && connected_);
        button(Part::DbStop, dg.stop, actions.stop && connected_);
        button(Part::DbRestart, dg.restart, actions.restart && connected_);
        {
            const Hit hit{Part::DbPort, -1, -1};
            const FieldEdit* edit = edit_of(hit);
            field_box(dg.port, db_changes_.port.has_value() || db_->port > 0, false, edit != nullptr && edit->focused(),
                      db_port_bad(db_changes_), hit);
        }
        {
            const Hit hit{Part::DbKeep, -1, -1};
            if (hover_ == hit) {
                c.round_rect(dg.keep, pxf(4), p.row_hover);
            }
            const int mid = (dg.keep.top + dg.keep.bottom) / 2;
            const RECT box{dg.keep.left + px(2), mid - px(kCheckBox) / 2, dg.keep.left + px(2) + px(kCheckBox),
                           mid + px(kCheckBox) / 2};
            const bool on = db_keep_running(*db_, db_changes_);
            if (on) {
                c.round_rect(box, pxf(3), p.accent);
                const auto bx = static_cast<float>(box.left);
                const auto by = static_cast<float>(box.top);
                c.line(bx + pxf(3.5F), by + pxf(8), bx + pxf(6.5F), by + pxf(11), p.accent_text, pxf(1.6F));
                c.line(bx + pxf(6.5F), by + pxf(11), bx + pxf(12.5F), by + pxf(5), p.accent_text, pxf(1.6F));
            } else {
                c.round_rect(box, pxf(3), p.input, p.switch_off_border, border);
            }
            if (is_focused(hit)) {
                paint_ring(c, dg.keep);
            }
        }

        c.round_rect(dg.ini_card, pxf(8), p.card, p.card_border, border);
        button(Part::DbEditIni, dg.edit_ini, connected_);
        button(Part::DbFinalIni, dg.final_ini, connected_);
        const auto& fields = settings_db_ini_fields();
        for (std::size_t i = 0; i < dg.boxes.size() && i < fields.size(); ++i) {
            const Hit hit{Part::DbField, -1, static_cast<int>(i)};
            const FieldEdit* edit = edit_of(hit);
            field_box(dg.boxes[i], db_ini_marked(*db_, db_changes_, fields[i].key), false,
                      edit != nullptr && edit->focused(), static_cast<int>(i) == dg.error_field, hit);
            const Hit reset{Part::DbReset, -1, static_cast<int>(i)};
            if (!is_empty(dg.resets[i]) && is_focused(reset)) {
                paint_ring(c, dg.resets[i]);
            }
        }
        if (!is_empty(dg.custom_problems)) {
            c.round_rect(dg.custom_problems, pxf(4), p.banner_error.fill);
        }

        c.round_rect(dg.root_card, pxf(8), p.card, p.card_border, border);
        button(Part::DbRootShow, dg.root_show, connected_ || !root_shown_.empty());
        button(Part::DbRootCopy, dg.root_copy, connected_ && !root_busy_);
        {
            const Hit hit{Part::DbNewPassword, -1, -1};
            const FieldEdit* edit = edit_of(hit);
            field_box(dg.new_password, false, false, edit != nullptr && edit->focused(), !password_error_.empty(), hit);
        }
        button(Part::DbSetPassword, dg.set_password, connected_ && !root_busy_);

        const bool changes = !db_changes_.empty();
        button(Part::DbDiscard, dg.discard, changes && !db_saving_);
        const Hit save{Part::DbSave, -1, -1};
        if (changes && !db_saving_ && connected_) {
            c.round_rect(dg.save, pxf(4), p.accent);
            if (is_focused(save)) {
                paint_ring(c, dg.save);
            }
        } else {
            paint_button(c, dg.save, false, is_focused(save), false);
        }
    }
    if (g.content_h > height_ && height_ > 0) {
        const int thumb = std::max(px(24), height_ * height_ / g.content_h);
        const int top = (height_ - thumb) * scroll_ / (g.content_h - height_);
        c.round_rect({width_ - px(6), top + px(2), width_ - px(2), top + thumb - px(2)}, pxf(2), p.switch_off_border);
    }
    c.unclip();
}

void SettingsWindow::paint_db_text(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const DbGeometry& dg = g.db;
    const RECT column{main_left() - px(4), 0, width_, height_};
    c.clip_text(column);
    c.text(g.title, w(text(lang_, Str::DbTitle)), fonts_.title.get(), p.text, Align::Start, false);
    c.paragraph(g.hint, w(text(lang_, Str::DbHint)), fonts_.caption.get(), p.text_subtle);
    if (!is_empty(dg.version)) {
        c.text(dg.version_label, w(text(lang_, Str::PhpVersion)), fonts_.caption.get(), p.text_subtle, Align::End, false);
        std::string label = "MariaDB " + db_series_;
        for (const auto& v : db_versions()) {
            if (v.first == db_series_) {
                label = "MariaDB " + v.second;
            }
        }
        RECT box = dg.version;
        box.left += px(8);
        box.right -= px(26);
        c.text(box, w(label), fonts_.button.get(), p.text, Align::Start, true);
    }
    if (!is_empty(g.message)) {
        RECT inner = g.message;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(message_), fonts_.button.get(), message_error_ ? p.banner_error.text : p.banner_info.text);
    }
    if (!is_empty(dg.note)) {
        if (is_empty(dg.runtimes)) {
            c.text(dg.note, w(text(lang_, Str::SettingsLoading)), fonts_.button.get(), p.text_subtle, Align::Start, false);
        } else {
            c.paragraph(dg.note, w(text(lang_, Str::DbNoneInstalled)), fonts_.button.get(), p.text);
        }
    }
    if (!is_empty(dg.runtimes)) {
        c.text(dg.runtimes, w(text(lang_, Str::PhpGoToRuntimes)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (!db_) {
        c.unclip_text();
        return;
    }
    const auto service_words = [this](const std::string& s) { return rtl_ ? isolate_ltr(s) : s; };
    if (!is_empty(dg.problems)) {
        std::string all;
        for (const std::string& problem : db_->problems) {
            all += (all.empty() ? "" : "; ") + problem;
        }
        RECT inner = dg.problems;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(fill(lang_, Str::DbProblems, {all})), fonts_.caption.get(), p.banner_error.text);
    }
    const DbActions actions = db_actions(db_server_);
    const auto label = [&](const RECT& r, Str s, bool enabled) {
        c.text(r, w(text(lang_, s)), fonts_.button.get(), enabled ? p.text : p.button_disabled_text, Align::Center, false);
    };

    // The server.
    c.text(dg.server_title, w(text(lang_, Str::DbServer)), fonts_.body_bold.get(), p.text, Align::Start, false);
    label(dg.start, Str::DbStart, actions.start && connected_);
    label(dg.stop, Str::DbStop, actions.stop && connected_);
    label(dg.restart, Str::DbRestart, actions.restart && connected_);
    c.paragraph(dg.state, w(db_state_text(*db_, db_server_, lang_)), fonts_.button.get(),
                db_server_.state == "failed" ? p.banner_error.text : p.text);
    const std::string used = db_->used_by.empty() ? std::string(text(lang_, Str::DbUsedByNobody))
                                                  : fill_pattern(text(lang_, Str::DbUsedBy), false,
                                                                 {db_names_text(db_->used_by, lang_)});
    c.paragraph(dg.used_by, w(used), fonts_.caption.get(), p.text_subtle);
    c.text(dg.port_label, w(text(lang_, Str::DbPort)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    c.text({dg.keep.left + px(2) + px(kCheckBox) + px(8), dg.keep.top, dg.keep.right, dg.keep.bottom},
           w(text(lang_, Str::DbKeepRunning)), fonts_.button.get(), p.text, Align::Start, false);
    {
        std::string keep_note(text(lang_, Str::DbKeepRunningNote));
        if (!db_->kept_by.empty()) {
            keep_note += " " + fill_pattern(text(lang_, Str::DbKeptBy), false, {db_names_text(db_->kept_by, lang_)});
        }
        c.paragraph(dg.keep_note, w(keep_note), fonts_.caption.get(), p.text_subtle);
    }
    if (!is_empty(dg.port_note)) {
        const bool bad = db_port_bad(db_changes_);
        c.paragraph(dg.port_note, w(text(lang_, bad ? Str::DbPortBad : Str::DbPortNote)), fonts_.caption.get(),
                    bad ? p.banner_error.text : p.asleep.text);
    }

    // my.ini.
    c.text(dg.ini_title, L"my.ini", fonts_.body_bold.get(), p.text, Align::Start, true);
    c.text(dg.ini_hint, w(text(lang_, Str::PhpIniHint)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    label(dg.edit_ini, Str::DbEditFull, connected_);
    label(dg.final_ini, Str::DbShowFinal, connected_);
    const auto& fields = settings_db_ini_fields();
    for (std::size_t i = 0; i < dg.boxes.size() && i < fields.size(); ++i) {
        c.text(dg.labels[i], w(fields[i].label), fonts_.caption.get(), p.text_subtle, Align::Start, true);
        if (!is_empty(dg.resets[i])) {
            c.text(dg.resets[i], w(text(lang_, Str::PhpReset)), fonts_.caption.get(), p.link, Align::End, false);
        }
    }
    if (!is_empty(dg.field_error)) {
        c.text(dg.field_error, w(service_words(db_error_)), fonts_.caption.get(), p.banner_error.text, Align::Start, false);
    }
    if (!is_empty(dg.custom_problems)) {
        std::string all;
        for (const std::string& problem : db_->custom_problems) {
            all += (all.empty() ? "" : "; ") + problem;
        }
        RECT inner = dg.custom_problems;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(fill(lang_, Str::DbCustomProblems, {all})), fonts_.caption.get(), p.banner_error.text);
    }
    for (const auto& [r, words] : dg.warnings) {
        c.paragraph(r, w(words), fonts_.caption.get(), p.asleep.text);
    }
    c.paragraph(dg.custom_note, w(text(lang_, Str::DbCustomIniUsed)), fonts_.caption.get(), p.text_subtle);

    // Root access.
    c.text(dg.root_title, w(text(lang_, Str::DbRoot)), fonts_.body_bold.get(), p.text, Align::Start, false);
    c.text(dg.root_label, w(text(lang_, Str::DbRootPassword)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    if (!is_empty(dg.root_value)) {
        c.text(dg.root_value, w(root_shown_.empty() ? kMasked : std::string_view(root_shown_)), fonts_.mono.get(), p.text, Align::Start, true);
    }
    label(dg.root_show, root_shown_.empty() ? Str::DbShow : Str::DbHide, connected_ || !root_shown_.empty());
    label(dg.root_copy, Str::DbCopy, connected_ && !root_busy_);
    c.text(dg.new_label, w(text(lang_, Str::DbNewPassword)), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    label(dg.set_password, Str::DbSetPassword, connected_ && !root_busy_);
    if (!is_empty(dg.password_error)) {
        c.paragraph(dg.password_error, w(password_error_), fonts_.caption.get(), p.banner_error.text);
    }
    c.paragraph(dg.root_note, w(text(lang_, Str::DbRootNote)), fonts_.caption.get(), p.text_subtle);
    const std::string dbs = db_->databases.empty() ? std::string(text(lang_, Str::DbNoDatabases))
                                                   : fill_pattern(text(lang_, Str::DbDatabases), false,
                                                                  {db_names_text(db_->databases, lang_)});
    c.paragraph(dg.databases, w(dbs), fonts_.caption.get(), p.text_subtle);

    // The footer.
    const bool changes = !db_changes_.empty();
    c.paragraph(dg.footer_text, w(db_restart_text(*db_, db_server_, lang_)), fonts_.caption.get(), p.text_subtle);
    label(dg.discard, Str::PhpDiscard, changes && !db_saving_);
    const bool can_save = changes && !db_saving_ && connected_;
    c.text(dg.save, w(text(lang_, db_saving_ ? Str::PhpSaving : Str::PhpSave)), fonts_.button.get(),
           can_save ? p.accent_text : p.button_disabled_text, Align::Center, false);
    c.unclip_text();
}

}  // namespace floserver::ui

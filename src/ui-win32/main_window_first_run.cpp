// The first-run page (approved 2 Nov): Language, Runtimes (then their
// downloads) and Ready, inside the main window over everything else, as the
// New project page is over the list. It shows once, when nothing is
// installed and there are no projects, until it's finished, skipped or the
// window is closed (window.json's first_run_done); FLOSERVER_FIRST_RUN=1
// shows it anyway.
//
// It does nothing of its own: the language is Settings → General's (saved in
// window.json, both windows made again), the downloads are the service's
// runtimes.install jobs (followed by the worker's jobs.get while they run,
// as Settings → Runtimes' are), the certificate is trusted as Settings →
// Ports & HTTPS does it, and the projects folder is changed as Settings →
// Folders does. Its state lives in this object, so a new language keeps it.

#include "main_window.h"

#include "main_window_layout.h"
#include "model/pages_access.h"
#include "model/ports_page.h"
#include "model/theme.h"
#include "window_prefs.h"

#include "floserver/core/encoding.h"
#include "floserver/core/process.h"
#include "floserver/core/trust_store.h"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>

namespace floserver::ui {

using namespace layout;
using nlohmann::json;

namespace {

constexpr int kSide = 48;        // the page's margin, from the mockups
constexpr int kNarrowSide = 20;  // in a narrow window
constexpr int kFooterHeight = 64;
constexpr int kButtonH = 32;
constexpr int kLangRowH = 48;
constexpr int kItemH = 40;
constexpr int kCancelH = 28;
constexpr int kBarW = 180;
constexpr int kNameW = 150;
constexpr int kSizeW = 70;
constexpr int kBox = 18;  // a check box or radio button
constexpr int kBigH = 108;

std::wstring w(std::string_view s) { return win::widen(s); }

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

bool is_empty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

std::string string_field(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return {};
}

// "first-run-folders#3": a folders.set's purpose, so Settings never takes its answer.
std::string folders_purpose() {
    static unsigned asks = 0;
    return "first-run-folders#" + std::to_string(++asks);
}

}  // namespace

// --- showing it, leaving it -----------------------------------------------------

void MainWindow::decide_first_run() {
    if (first_run_.decided || quitting_) {
        return;
    }
    const auto env = core::get_env("FLOSERVER_FIRST_RUN");
    const bool forced = first_run_forced(env ? env->c_str() : nullptr);
    if (!forced) {
        if (prefs_.first_run_done) {
            first_run_.decided = true;
            return;
        }
        // What's installed and the projects, from the service, first.
        if (snap_.link != Link::Connected || !snap_.has_list || !snap_.has_installed) {
            return;
        }
    }
    first_run_.decided = true;
    if (first_run_wanted(forced, prefs_.first_run_done, snap_.installed, snap_.rows.size())) {
        open_first_run();
    }
}

void MainWindow::open_first_run() {
    if (wizard_open()) {
        close_wizard();
    }
    if (panel_open()) {
        (void)leave_php_part(false);
        close_panel();
    }
    first_run_.open = true;
    first_run_.step = FirstRunStep::Language;
    first_run_.system_lang = window_lang(WindowPrefs{});  // "Same as Windows" now
    first_run_.focus = -1;
    first_run_.scroll = 0;
    hover_ = {};
    SetFocus(hwnd_);
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::finish_first_run() {
    if (!first_run_.open) {
        return;
    }
    first_run_.open = false;
    first_run_bar_.cancel(hwnd_);
    hover_ = {};
    pressed_ = {};
    if (!prefs_.first_run_done) {
        prefs_.first_run_done = true;
        // The same safe write as Settings → General: the keys this version
        // doesn't know are kept, a damaged file is kept beside it.
        std::string error;
        std::string kept_as;
        if (!save_window_prefs(prefs_, error, kept_as)) {
            show_notice(fill(lang_, Str::GeneralSaveFailed, {error}), true);
        }
    }
    SetFocus(hwnd_);
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- what the service says ------------------------------------------------------------

void MainWindow::first_run_ask_list() {
    if (!worker_ || snap_.link != Link::Connected) {
        return;  // asked once it's there (on_snapshot)
    }
    first_run_.list = FirstRunList::Loading;
    first_run_.refreshing = false;
    first_run_.list_error.clear();
    worker_->list_runtimes(false);  // the list in hand or kept on disk: no network
}

void MainWindow::on_first_run_answers(Snapshot& s) {
    // How the page's installs ended; kept while the page's rows are about them.
    for (auto& [name, end] : s.first_run_ends) {
        if (first_run_.started && !name.empty()) {
            first_run_.ends[name] = end;
        }
    }

    // runtimes.list: the offers on step 2.
    if (first_run_.list == FirstRunList::Loading && (s.runtimes || !s.runtimes_error.empty()) &&
        !(first_run_.refreshing && !s.runtimes_refresh)) {  // waiting for the fresh one
        const FirstRunListAnswer answer = first_run_list_answer(s.runtimes, s.runtimes_error, s.runtimes_refresh);
        switch (answer.kind) {
            case FirstRunListAnswer::Kind::Offers:
                first_run_.offers = answer.offers;
                first_run_.ticks = first_run_default_ticks(first_run_.offers);
                first_run_.list = FirstRunList::Ready;
                first_run_.refreshing = false;
                break;
            case FirstRunListAnswer::Kind::Refresh:
                // None on this computer yet: fetched, as Settings does the first time.
                if (!first_run_.refreshing && worker_) {
                    first_run_.refreshing = true;
                    worker_->list_runtimes(true);
                }
                break;
            case FirstRunListAnswer::Kind::Failed:
                first_run_.list = FirstRunList::Failed;
                first_run_.list_error = answer.error;
                first_run_.refreshing = false;
                break;
        }
    }

    // certs.ensure_ca: trusted already, or Windows is asked (outside this snapshot's handling).
    for (const Snapshot::WebSettingsAnswer& answer : s.web_settings) {
        if (answer.what != ServiceWorker::kFirstRunCa || first_run_.trust != FirstRunTrust::Checking) {
            continue;
        }
        PortsSettings ca;
        if (!answer.error.empty() || !answer.reply) {
            first_run_.trust = FirstRunTrust::Failed;
            first_run_.trust_error = answer.error;  // empty: worded as unreadable when shown
        } else if (!apply_ca_reply(ca, *answer.reply)) {
            first_run_.trust = FirstRunTrust::Failed;
            first_run_.trust_error.clear();  // worded as unreadable when shown
        } else if (ca.ca == CaState::Trusted) {
            first_run_.trust = FirstRunTrust::Trusted;  // nothing to ask
        } else if (!first_run_.open) {
            first_run_.trust = FirstRunTrust::NotAsked;  // the page was left: Windows isn't asked out of the blue
        } else {
            first_run_.ca_der = ca.ca_der;
            first_run_.trust = FirstRunTrust::Asking;
            PostMessageW(hwnd_, kFirstRunTrustMessage, 0, 0);
        }
    }

    // web.settings.get's CA, asked on reaching step 3 without downloads (skipped).
    for (const Snapshot::WebSettingsAnswer& answer : s.web_settings) {
        if (answer.what != ServiceWorker::kFirstRunCaStatus || first_run_.trust != FirstRunTrust::NotAsked) {
            continue;
        }
        if (answer.error.empty() && answer.reply) {
            if (const auto settings = ports_settings_from_reply(*answer.reply); settings && settings->ca == CaState::Trusted) {
                first_run_.trust = FirstRunTrust::Trusted;
            }
        }
    }

    // folders.set from Change…; Settings never sees it (its purpose isn't Settings').
    for (auto it = s.db.begin(); it != s.db.end();) {
        if (first_run_.folder_ask.empty() || it->method != "folders.set" || it->purpose != first_run_.folder_ask) {
            ++it;
            continue;
        }
        first_run_.folder_ask.clear();
        if (!it->error.empty() || !it->reply || !it->reply->is_object()) {
            show_notice(fill(lang_, Str::FoldersChangeFailed, {it->error}), true);
        } else {
            const std::string now = string_field(*it->reply, "projects_dir");
            first_run_.folder = now.empty() ? first_run_.folder_picked : now;
        }
        first_run_.folder_picked.clear();
        it = s.db.erase(it);
    }
}

void MainWindow::first_run_start_downloads() {
    if (!worker_ || snap_.link != Link::Connected) {
        show_notice(std::string(text(lang_, Str::ConnectionLost)), true);
        return;
    }
    first_run_.wanted = first_run_downloads(first_run_.offers, first_run_.ticks);
    first_run_.ends.clear();
    first_run_.started = true;
    first_run_.scroll = 0;
    first_run_.focus = -1;
    // The service installs them one after another, nginx first.
    worker_->install(std::string(kFirstRunKey), first_run_.wanted);
    // And Windows is asked to trust the CA meanwhile, as Settings → Ports &
    // HTTPS does: made (or read) by the service first.
    if (first_run_.trust == FirstRunTrust::NotAsked || first_run_.trust == FirstRunTrust::Failed) {
        first_run_.trust = FirstRunTrust::Checking;
        first_run_.trust_error.clear();
        worker_->web_settings(ServiceWorker::kFirstRunCa);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::first_run_to_ready() {
    first_run_.step = FirstRunStep::Ready;
    first_run_.scroll = 0;
    first_run_.focus = -1;
    // Without downloads the CA wasn't looked at: trusted already (an earlier install) says so.
    if (first_run_check_ca(first_run_.trust, first_run_.ca_checked) && worker_) {
        first_run_.ca_checked = true;
        worker_->web_settings(ServiceWorker::kFirstRunCaStatus);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::first_run_trust_now() {
    if (first_run_.trust != FirstRunTrust::Asking || quitting_) {
        return;
    }
    if (asking_) {
        // A question or the folder picker is open: asked once it's answered (first_run_after_ask).
        return;
    }
    const auto der = core::from_base64(first_run_.ca_der);
    if (!der) {
        first_run_.trust = FirstRunTrust::Failed;
        first_run_.trust_error.clear();  // worded as unreadable when shown
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    // The banner says what Windows is asking before its window opens.
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    // Windows shows its own question here and waits for the answer. As
    // while a save question is open, a quit or a new language waits for it.
    asking_ = true;
    bool cancelled = false;
    std::string error;
    const bool trusted = core::trust_ca(*der, cancelled, error);
    asking_ = false;
    if (trusted) {
        first_run_.trust = FirstRunTrust::Trusted;
        if (worker_) {
            worker_->web_settings(ServiceWorker::kFirstRunReload);  // projects move to https now
        }
    } else {
        first_run_.trust = cancelled ? FirstRunTrust::Declined : FirstRunTrust::Failed;
        first_run_.trust_error = error;
    }
    if (quit_after_ask_) {
        quit_after_ask_ = false;
        quit();
        return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::first_run_after_ask() {
    if (first_run_.trust == FirstRunTrust::Asking && hwnd_ != nullptr) {
        PostMessageW(hwnd_, kFirstRunTrustMessage, 0, 0);
    }
}

std::string MainWindow::first_run_folder() const {
    if (!first_run_.folder_picked.empty()) {
        return first_run_.folder_picked;  // its folders.set is on its way
    }
    if (!first_run_.folder.empty()) {
        return first_run_.folder;
    }
    // What the New project page uses.
    return !snap_.projects_dir.empty() ? snap_.projects_dir : default_projects_parent();
}

void MainWindow::first_run_change_folder() {
    if (!worker_ || snap_.link != Link::Connected || picking_folder_ || asking_ || !first_run_.folder_ask.empty()) {
        return;
    }
    // The picker runs its own message loop: no second one, and a quit or a
    // new language waits for it.
    picking_folder_ = true;
    asking_ = true;
    pressed_ = {};
    hover_ = {};
    const std::string start = first_run_folder();
    const auto picked = pick_folder(hwnd_, std::string(text(lang_, Str::FoldersPickProjects)), start);
    picking_folder_ = false;
    asking_ = false;
    first_run_after_ask();
    if (quit_after_ask_) {
        quit_after_ask_ = false;
        quit();
        return;
    }
    if (!picked) {
        show_notice(fill(lang_, Str::OpenFailed, {"folder picker"}), true);
        return;
    }
    if (picked->empty() || *picked == start || !worker_ || snap_.link != Link::Connected) {
        return;
    }
    first_run_.folder_picked = *picked;
    first_run_.folder_ask = folders_purpose();
    worker_->database_call("folders.set", {{"projects_dir", *picked}}, first_run_.folder_ask);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::vector<FirstRunRow> MainWindow::first_run_rows_now() const {
    return first_run_rows(first_run_.wanted, snap_.installed, snap_.runtime_jobs, first_run_.ends, lang_);
}

// --- input ------------------------------------------------------------------------

void MainWindow::on_first_run_click(const Hit& hit) {
    const std::vector<Hit> order = first_run_focus_order();
    if (const auto it = std::find(order.begin(), order.end(), hit); it != order.end()) {
        first_run_.focus = static_cast<int>(it - order.begin());
    }
    switch (hit.part) {
        case Part::FrLang:
            // Not while Windows' question or a picker is open: the window can't be made again then.
            if (!asking_ && hit.row >= 0 && static_cast<std::size_t>(hit.row) < kLangChoices.size() &&
                kLangChoices.at(static_cast<std::size_t>(hit.row)) != prefs_.lang) {
                // As Settings → General does it: saved, and both windows made
                // again once this click is over.
                PostMessageW(hwnd_, kPrefsMessage, static_cast<WPARAM>(PrefChange::Language), hit.row);
            }
            break;
        case Part::FrTick:
            if (hit.row >= 0 && static_cast<std::size_t>(hit.row) < first_run_.offers.size() &&
                !first_run_.offers[static_cast<std::size_t>(hit.row)].always) {
                first_run_.ticks.resize(first_run_.offers.size(), false);
                const auto i = static_cast<std::size_t>(hit.row);
                first_run_.ticks[i] = !first_run_.ticks[i];
            }
            break;
        case Part::FrCancel: {
            const std::vector<FirstRunRow> rows = first_run_rows_now();
            if (hit.row >= 0 && static_cast<std::size_t>(hit.row) < rows.size() && worker_) {
                if (const std::int64_t job = rows[static_cast<std::size_t>(hit.row)].job; job != 0) {
                    worker_->cancel_job(job);
                }
            }
            break;
        }
        case Part::FrRetry:
            if (worker_ && snap_.link == Link::Connected) {
                first_run_.list = FirstRunList::Loading;
                first_run_.list_error.clear();
                first_run_.refreshing = true;
                worker_->list_runtimes(true);
            }
            break;
        case Part::FrChange:
            first_run_change_folder();
            return;
        case Part::FrCreate:
            finish_first_run();
            open_wizard();
            return;
        case Part::FrAdd:
            finish_first_run();
            add_existing_folder();
            return;
        case Part::FrSkipSetup:
            // What closing the page does: marked done, the list shown.
            finish_first_run();
            return;
        case Part::FrWithout:
            // Nothing downloaded (not even nginx), Windows not asked about the certificate.
            if (first_run_.step == FirstRunStep::Runtimes && !first_run_.started) {
                first_run_.without = true;
                first_run_to_ready();
            }
            break;
        case Part::FrBack:
            if (first_run_.step == FirstRunStep::Runtimes) {
                first_run_.step = FirstRunStep::Language;
                first_run_.scroll = 0;
                first_run_.focus = -1;
            }
            break;
        case Part::FrNext:
            switch (first_run_.step) {
                case FirstRunStep::Language:
                    first_run_.step = FirstRunStep::Runtimes;
                    first_run_.scroll = 0;
                    first_run_.focus = -1;
                    if (first_run_.list == FirstRunList::Idle) {
                        first_run_ask_list();
                    }
                    break;
                case FirstRunStep::Runtimes:
                    if (!first_run_.started && first_run_.list == FirstRunList::Ready) {
                        first_run_start_downloads();  // the downloads, still on step 2
                    } else if (first_run_.started || first_run_.list == FirstRunList::Failed) {
                        first_run_to_ready();  // downloads go on in the background
                    }
                    break;
                case FirstRunStep::Ready:
                    finish_first_run();
                    return;
            }
            break;
        default:
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::on_first_run_key(WPARAM key) {
    const std::vector<Hit> order = first_run_focus_order();
    const int count = static_cast<int>(order.size());
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (key) {
        case VK_TAB:
        case VK_DOWN:
        case VK_UP: {
            if (count == 0) {
                return;
            }
            const bool back = key == VK_UP || (key == VK_TAB && shift);
            int next = first_run_.focus;
            if (next < 0) {
                next = back ? count - 1 : 0;
            } else {
                next = (next + (back ? count - 1 : 1)) % count;
            }
            first_run_focus_at(next);
            return;
        }
        case VK_SPACE:
        case VK_RETURN:
            if (first_run_.focus >= 0 && first_run_.focus < count) {
                on_first_run_click(order[static_cast<std::size_t>(first_run_.focus)]);
            } else if (key == VK_RETURN && !is_empty(first_run_geometry_.next)) {
                on_first_run_click({Part::FrNext, -1, {}});
            }
            return;
        case VK_PRIOR:
        case VK_NEXT:
            first_run_scroll_to(first_run_.scroll + (key == VK_NEXT ? 1 : -1) *
                                                        std::max(px(40), height_of(first_run_geometry_.content) - px(40)));
            return;
        default:
            return;
    }
}

void MainWindow::first_run_focus_at(int index) {
    const std::vector<Hit> order = first_run_focus_order();
    if (index < 0 || index >= static_cast<int>(order.size())) {
        return;
    }
    first_run_.focus = index;
    show_focus_ = true;
    // Into view (the footer's buttons are always in view).
    const RECT r = first_run_rect_of(order[static_cast<std::size_t>(index)]);
    const RECT& content = first_run_geometry_.content;
    if (!is_empty(r) && r.top < first_run_geometry_.footer.top) {
        if (r.top < content.top) {
            first_run_scroll_to(first_run_.scroll - static_cast<int>(content.top - r.top) - px(8));
        } else if (r.bottom > content.bottom) {
            first_run_scroll_to(first_run_.scroll + static_cast<int>(r.bottom - content.bottom) + px(8));
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::string MainWindow::first_run_footer_words() const {
    switch (first_run_.step) {
        case FirstRunStep::Language:
            return std::string(text(lang_, Str::FirstRunPrivacy));
        case FirstRunStep::Ready:
            return std::string(text(lang_, Str::FirstRunNextScreen));
        case FirstRunStep::Runtimes:
            break;
    }
    return fill(lang_, Str::FirstRunTotal, {size_text(first_run_total(first_run_.offers, first_run_.ticks))});
}

void MainWindow::first_run_scroll_to(int target) {
    const RECT& content = first_run_geometry_.content;
    const int most = std::max(0, first_run_geometry_.content_h - height_of(content));
    const int value = std::clamp(target, 0, most);
    if (value != first_run_.scroll) {
        first_run_.scroll = value;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// --- geometry -------------------------------------------------------------------------

MainWindow::FirstRunGeometry MainWindow::first_run_geometry(HDC dc, int scroll) const {
    FirstRunGeometry g;
    g.area = {0, 0, width_, height_};
    const int side = width_ >= px(640) ? px(kSide) : px(kNarrowSide);
    const int x0 = side;
    const int x1 = std::max(x0 + px(120), width_ - side);
    const int span = x1 - x0;
    const RECT banner = banner_rect();
    const int top = banner.bottom > 0 ? banner.bottom + px(12) : px(20);
    g.steps = {x0, top, x1, top + px(20)};
    {
        // The steps' words, the current one bold, as many as fit.
        int x = g.steps.left;
        for (const FirstRunStep step : {FirstRunStep::Language, FirstRunStep::Runtimes, FirstRunStep::Ready}) {
            HFONT font = (step == first_run_.step ? fonts_.button_bold : fonts_.button).get();
            const int sw = Canvas::measure(dc, w(first_run_step_label(step, first_run_.step, lang_)), font, rtl_) + px(4);
            g.step_rects.push_back({x, g.steps.top, std::min<int>(g.steps.right, x + sw), g.steps.bottom});
            x += sw + px(28);
            if (x >= g.steps.right) {
                break;
            }
        }
    }

    // The footer (approved 2 Nov): Skip setup at the reading start, the
    // words after it, Back, Continue without downloading and the step's own
    // button at the end. When they don't fit on one line (a narrow window,
    // Arabic or Kurdish), the end's two go on a line above.
    const FirstRunFooter f = first_run_footer(first_run_.step, first_run_.list, first_run_.started);
    g.next_label = f.next.value_or(Str::WizardNext);
    g.next_primary = f.next_primary;
    const auto button_w = [&](Str label, bool bold) {
        return Canvas::measure(dc, w(text(lang_, label)), (bold ? fonts_.button_bold : fonts_.button).get(), rtl_) + px(32);
    };
    const int gap = px(8);
    const int skip_w = button_w(Str::FirstRunSkipSetup, false);
    const int back_w = f.back ? button_w(Str::WizardBack, false) : 0;
    const int without_w = f.without ? button_w(Str::FirstRunWithout, false) : 0;
    const int next_w = f.next ? button_w(*f.next, f.next_primary) : 0;
    const int end_w = next_w + (without_w > 0 ? without_w + gap : 0) + (back_w > 0 ? back_w + gap : 0);
    const bool two_lines = skip_w + px(16) + end_w > span;
    const int footer_h = two_lines ? px(kFooterHeight) + px(kButtonH) + px(8) : px(kFooterHeight);
    g.footer = {0, std::max<int>(g.steps.bottom, height_ - footer_h), width_, height_};
    const int gap_y = first_run_.step == FirstRunStep::Language ? px(20) : px(14);
    g.content = {0, std::min(g.steps.bottom + gap_y, g.footer.top), width_, g.footer.top};
    const int line_h = two_lines ? height_of(g.footer) / 2 : height_of(g.footer);
    const int upper_mid = g.footer.top + (two_lines ? line_h / 2 + px(4) : line_h / 2);
    const int lower_mid = two_lines ? g.footer.bottom - line_h / 2 - px(4) : upper_mid;
    const auto at = [&](int left, int width, int mid) {
        return RECT{left, mid - px(kButtonH) / 2, left + width, mid + px(kButtonH) / 2};
    };
    int right = x1;
    if (next_w > 0) {
        g.next = at(right - next_w, next_w, upper_mid);
        right = g.next.left - gap;
    }
    if (without_w > 0) {
        g.without = at(right - without_w, without_w, upper_mid);
        right = g.without.left - gap;
    }
    if (two_lines) {
        right = x1;  // Back starts the lower line's end
    }
    if (back_w > 0) {
        g.back = at(right - back_w, back_w, lower_mid);
        right = g.back.left - gap;
    }
    g.skip = at(x0, std::min(skip_w, std::max(0, right - x0)), lower_mid);
    const int words_left = g.skip.right + px(16);
    const int words_right = right - gap;
    if (f.words && words_right - words_left >= px(60)) {
        const int words_top = two_lines ? lower_mid - line_h / 2 : g.footer.top;
        const int words_bottom = two_lines ? lower_mid + line_h / 2 : g.footer.bottom;
        g.footer_text = {words_left, words_top, words_right, words_bottom};
    }

    int y = g.content.top - scroll;
    const auto title = [&]() {
        g.title = {x0, y, x1, y + px(34)};
        y += px(34) + px(6);
    };
    const auto intro = [&](const std::string& words, int width) {
        const int h = text_height(dc, w(words), fonts_.button.get(), width, rtl_);
        g.intro = {x0, y, x0 + width, y + h};
        y += h;
    };
    switch (first_run_.step) {
        case FirstRunStep::Language: {
            title();
            intro(std::string(text(lang_, Str::FirstRunIntro)), std::min(span, px(620)));
            y += px(20);
            g.legend = {x0, y, x1, y + px(20)};
            y += px(20) + px(8);
            const int cw = std::min(span, px(460));
            g.card = {x0, y, x0 + cw, y + static_cast<int>(kLangChoices.size()) * px(kLangRowH)};
            for (std::size_t i = 0; i < kLangChoices.size(); ++i) {
                const int ry = y + static_cast<int>(i) * px(kLangRowH);
                g.rows.push_back({x0, ry, x0 + cw, ry + px(kLangRowH)});
            }
            y = g.card.bottom + px(8);
            const int h = text_height(dc, w(text(lang_, Str::FirstRunLanguageNote)), fonts_.caption.get(), cw, rtl_);
            g.note = {x0, y, x0 + cw, y + h};
            y += h;
            break;
        }
        case FirstRunStep::Runtimes: {
            if (first_run_.started) {
                const FirstRunProgress progress = first_run_progress(first_run_rows_now());
                title();
                intro(fill(lang_, Str::FirstRunDownloadingIntro,
                           {std::to_string(progress.ready), std::to_string(progress.total)}),
                      span);
                y += px(14);
                const std::vector<FirstRunRow> rows = first_run_rows_now();
                g.card = {x0, y, x1, y + static_cast<int>(rows.size()) * px(kItemH)};
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    const int ry = y + static_cast<int>(i) * px(kItemH);
                    const RECT row{x0, ry, x1, ry + px(kItemH)};
                    g.rows.push_back(row);
                    RECT cancel{};
                    if (rows[i].state == FirstRunRowState::Running && rows[i].job != 0) {
                        const int mid = (row.top + row.bottom) / 2;
                        const int cw = Canvas::measure(dc, w(text(lang_, Str::WizardCancel)), fonts_.button.get(), rtl_) + px(20);
                        cancel = {row.right - px(16) - cw, mid - px(kCancelH) / 2, row.right - px(16), mid + px(kCancelH) / 2};
                    }
                    g.cancels.push_back(cancel);
                }
                y = g.card.bottom + px(14);
                if (first_run_.trust != FirstRunTrust::NotAsked) {
                    const std::string words = first_run_https_line(first_run_.trust, first_run_.trust_error, lang_);
                    const int tw = span - px(14 + 16 + 10 + 14);
                    const int h = std::max(px(20), text_height(dc, w(words), fonts_.button.get(), tw, rtl_)) + px(20);
                    g.info = {x0, y, x1, y + h};
                    y += h;
                    if (first_run_.trust == FirstRunTrust::Asking || first_run_.trust == FirstRunTrust::Checking) {
                        y += px(10);
                        const int nh =
                            text_height(dc, w(text(lang_, Str::FirstRunTrustAskingNote)), fonts_.caption.get(), span, rtl_);
                        g.info_note = {x0, y, x1, y + nh};
                        y += nh;
                    }
                }
                break;
            }
            title();
            intro(std::string(text(lang_, Str::FirstRunRuntimesIntro)), span);
            y += px(14);
            switch (first_run_.list) {
                case FirstRunList::Ready: {
                    g.card = {x0, y, x1, y + static_cast<int>(first_run_.offers.size()) * px(kItemH)};
                    for (std::size_t i = 0; i < first_run_.offers.size(); ++i) {
                        const int ry = y + static_cast<int>(i) * px(kItemH);
                        g.rows.push_back({x0, ry, x1, ry + px(kItemH)});
                    }
                    y = g.card.bottom + px(14);
                    const int tw = span - px(14 + 16 + 10 + 14);
                    const int h = text_height(dc, w(text(lang_, Str::FirstRunCertNote)), fonts_.caption.get(), tw, rtl_) + px(20);
                    g.info = {x0, y, x1, y + h};
                    y += h;
                    break;
                }
                case FirstRunList::Failed: {
                    const int h = text_height(dc, w(first_run_list_error(first_run_.list_error, lang_)), fonts_.button.get(), span, rtl_);
                    g.message = {x0, y, x1, y + h};
                    y += h + px(12);
                    const int bw = Canvas::measure(dc, w(text(lang_, Str::FirstRunTryAgain)), fonts_.button.get(), rtl_) + px(32);
                    g.retry = {x0, y, x0 + bw, y + px(kButtonH)};
                    y += px(kButtonH);
                    break;
                }
                case FirstRunList::Idle:
                case FirstRunList::Loading:
                    g.message = {x0, y, x1, y + px(20)};
                    y += px(20);
                    break;
            }
            break;
        }
        case FirstRunStep::Ready: {
            title();
            y += px(10);
            const int lw = Canvas::measure(dc, w(text(lang_, Str::FirstRunChange)), fonts_.button.get(), rtl_) + px(4);
            const int tx = x0 + px(16 + 16 + 12);
            const std::vector<FirstRunRow> rows = first_run_rows_now();
            const std::string details[] = {first_run_runtimes_line(rows, lang_, first_run_.without),
                                           first_run_https_line(first_run_.trust, first_run_.trust_error, lang_)};
            const int card_top = y;
            for (int i = 0; i < 3; ++i) {
                const int end = i == 2 ? x1 - px(16) - lw - px(12) : x1 - px(16);
                const int h = i == 2 ? px(20)
                                     : std::max(px(20), text_height(dc, w(details[i]), fonts_.button.get(), end - tx, rtl_));
                const RECT row{x0, y, x1, y + px(12) + px(20) + px(2) + h + px(12)};
                g.rows.push_back(row);
                g.details.push_back({tx, row.top + px(34), end, row.top + px(34) + h});
                if (i == 2) {
                    const int mid = (row.top + row.bottom) / 2;
                    g.change = {x1 - px(16) - lw, mid - px(12), x1 - px(16), mid + px(12)};
                }
                y = row.bottom;
            }
            g.card = {x0, card_top, x1, y};
            y += px(16);
            const bool side_by_side = span >= px(480);
            const int bw = side_by_side ? (span - px(12)) / 2 : span;
            const auto big_h = [&](Str note) {
                return std::max(px(kBigH),
                                px(18) + px(22) + px(6) + text_height(dc, w(text(lang_, note)), fonts_.button.get(), bw - px(40), rtl_) + px(18));
            };
            const int h = std::max(big_h(Str::FirstRunCreateNote), big_h(Str::FirstRunAddFolderNote));
            g.create = {x0, y, x0 + bw, y + h};
            if (side_by_side) {
                g.add = {x0 + bw + px(12), y, x1, y + h};
            } else {
                y += h + px(12);
                g.add = {x0, y, x1, y + h};
            }
            y = g.add.bottom;
            break;
        }
    }
    g.content_h = y + scroll - g.content.top + px(16);
    return g;
}

MainWindow::Hit MainWindow::first_run_hit_test(int x, int y) const {
    const FirstRunGeometry& g = first_run_geometry_;
    if (contains(g.skip, x, y)) {
        return {Part::FrSkipSetup, -1, {}};
    }
    if (contains(g.without, x, y)) {
        return {Part::FrWithout, -1, {}};
    }
    if (contains(g.back, x, y)) {
        return {Part::FrBack, -1, {}};
    }
    if (contains(g.next, x, y)) {
        return {Part::FrNext, -1, {}};
    }
    if (!contains(g.content, x, y)) {
        return {};
    }
    for (std::size_t i = 0; i < g.cancels.size(); ++i) {
        if (contains(g.cancels[i], x, y)) {
            return {Part::FrCancel, static_cast<int>(i), {}};
        }
    }
    if (contains(g.retry, x, y)) {
        return {Part::FrRetry, -1, {}};
    }
    if (contains(g.change, x, y)) {
        return {Part::FrChange, -1, {}};
    }
    if (contains(g.create, x, y)) {
        return {Part::FrCreate, -1, {}};
    }
    if (contains(g.add, x, y)) {
        return {Part::FrAdd, -1, {}};
    }
    for (std::size_t i = 0; i < g.rows.size(); ++i) {
        if (!contains(g.rows[i], x, y)) {
            continue;
        }
        if (first_run_.step == FirstRunStep::Language) {
            return {Part::FrLang, static_cast<int>(i), {}};
        }
        if (first_run_.step == FirstRunStep::Runtimes && !first_run_.started && i < first_run_.offers.size() &&
            !first_run_.offers[i].always) {
            return {Part::FrTick, static_cast<int>(i), {}};
        }
    }
    return {};
}

std::vector<MainWindow::Hit> MainWindow::first_run_focus_order() const {
    const FirstRunGeometry& g = first_run_geometry_;
    std::vector<Hit> out;
    switch (first_run_.step) {
        case FirstRunStep::Language:
            for (std::size_t i = 0; i < g.rows.size(); ++i) {
                out.push_back({Part::FrLang, static_cast<int>(i), {}});
            }
            break;
        case FirstRunStep::Runtimes:
            if (first_run_.started) {
                for (std::size_t i = 0; i < g.cancels.size(); ++i) {
                    if (!is_empty(g.cancels[i])) {
                        out.push_back({Part::FrCancel, static_cast<int>(i), {}});
                    }
                }
            } else if (first_run_.list == FirstRunList::Ready) {
                for (std::size_t i = 0; i < first_run_.offers.size() && i < g.rows.size(); ++i) {
                    if (!first_run_.offers[i].always) {
                        out.push_back({Part::FrTick, static_cast<int>(i), {}});
                    }
                }
            } else if (!is_empty(g.retry)) {
                out.push_back({Part::FrRetry, -1, {}});
            }
            break;
        case FirstRunStep::Ready:
            out.push_back({Part::FrChange, -1, {}});
            out.push_back({Part::FrCreate, -1, {}});
            out.push_back({Part::FrAdd, -1, {}});
            break;
    }
    // The footer in reading order.
    if (!is_empty(g.skip)) {
        out.push_back({Part::FrSkipSetup, -1, {}});
    }
    if (!is_empty(g.back)) {
        out.push_back({Part::FrBack, -1, {}});
    }
    if (!is_empty(g.without)) {
        out.push_back({Part::FrWithout, -1, {}});
    }
    if (!is_empty(g.next)) {
        out.push_back({Part::FrNext, -1, {}});
    }
    return out;
}

RECT MainWindow::first_run_rect_of(const Hit& hit) const {
    const FirstRunGeometry& g = first_run_geometry_;
    const auto at = [](const std::vector<RECT>& v, int i) {
        return i >= 0 && static_cast<std::size_t>(i) < v.size() ? v[static_cast<std::size_t>(i)] : RECT{};
    };
    switch (hit.part) {
        case Part::FrLang:
        case Part::FrTick:
            return at(g.rows, hit.row);
        case Part::FrCancel:
            return at(g.cancels, hit.row);
        case Part::FrRetry:
            return g.retry;
        case Part::FrChange:
            return g.change;
        case Part::FrCreate:
            return g.create;
        case Part::FrAdd:
            return g.add;
        case Part::FrSkipSetup:
            return g.skip;
        case Part::FrWithout:
            return g.without;
        case Part::FrBack:
            return g.back;
        case Part::FrNext:
            return g.next;
        default:
            return {};
    }
}

// --- painting -------------------------------------------------------------------------

void MainWindow::paint_first_run(Canvas& c, HDC dc, bool text_pass) const {
    const FirstRunGeometry& g = first_run_geometry_;
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    const bool has_focus = GetFocus() == hwnd_;
    const std::vector<Hit> order = first_run_focus_order();
    const auto focused = [&](const Hit& h) {
        return show_focus_ && has_focus && first_run_.focus >= 0 &&
               first_run_.focus < static_cast<int>(order.size()) && order[static_cast<std::size_t>(first_run_.focus)] == h;
    };
    const auto hovered = [&](Part part, int row) { return hover_.part == part && hover_.row == row; };
    const std::vector<FirstRunRow> rows =
        first_run_.started && first_run_.step != FirstRunStep::Language ? first_run_rows_now() : std::vector<FirstRunRow>{};

    // 16 px icons on a 16 grid at (x, y), the top-left corner.
    const auto check = [&](float x, float y, Rgb color) {
        const float k = pxf(16) / 16.0F;
        c.line(x + 3 * k, y + 8.5F * k, x + 6 * k, y + 11.5F * k, color, 1.8F * k);
        c.line(x + 6 * k, y + 11.5F * k, x + 13 * k, y + 4.5F * k, color, 1.8F * k);
    };
    const auto arrow = [&](float x, float y, Rgb color) {
        const float k = pxf(16) / 16.0F;
        c.line(x + 8 * k, y + 2.5F * k, x + 8 * k, y + 10.5F * k, color, 1.5F * k);
        c.line(x + 4.5F * k, y + 7.5F * k, x + 8 * k, y + 11 * k, color, 1.5F * k);
        c.line(x + 8 * k, y + 11 * k, x + 11.5F * k, y + 7.5F * k, color, 1.5F * k);
        c.line(x + 3 * k, y + 13.5F * k, x + 13 * k, y + 13.5F * k, color, 1.5F * k);
    };
    const auto cross = [&](float x, float y, Rgb color) {
        const float k = pxf(16) / 16.0F;
        c.line(x + 4 * k, y + 4 * k, x + 12 * k, y + 12 * k, color, 1.6F * k);
        c.line(x + 12 * k, y + 4 * k, x + 4 * k, y + 12 * k, color, 1.6F * k);
    };
    const auto info = [&](float x, float y, Rgb color) {
        const float k = pxf(16) / 16.0F;
        c.circle(x + 8 * k, y + 8 * k, 6.5F * k, color, 1.3F * k);
        c.line(x + 8 * k, y + 7 * k, x + 8 * k, y + 11 * k, color, 1.5F * k);
        c.ellipse(x + 8 * k, y + 4.8F * k, 0.9F * k, color);
    };
    const auto lock = [&](float x, float y, Rgb color) {
        const float k = pxf(16) / 16.0F;
        const RECT body{static_cast<int>(x + 3 * k), static_cast<int>(y + 7 * k), static_cast<int>(x + 13 * k),
                        static_cast<int>(y + 14 * k)};
        c.round_outline(body, 1.5F * k, color, 1.3F * k);
        c.line(x + 5.5F * k, y + 7 * k, x + 5.5F * k, y + 5 * k, color, 1.3F * k);
        c.line(x + 10.5F * k, y + 7 * k, x + 10.5F * k, y + 5 * k, color, 1.3F * k);
        c.line(x + 5.5F * k, y + 5 * k, x + 8 * k, y + 2.5F * k, color, 1.3F * k);
        c.line(x + 8 * k, y + 2.5F * k, x + 10.5F * k, y + 5 * k, color, 1.3F * k);
    };
    const auto folder = [&](float x, float y, Rgb color) {
        const float k = pxf(16) / 16.0F;
        c.line(x + 2 * k, y + 4.5F * k, x + 6 * k, y + 4.5F * k, color, 1.3F * k);
        c.line(x + 6 * k, y + 4.5F * k, x + 7.5F * k, y + 6 * k, color, 1.3F * k);
        c.line(x + 7.5F * k, y + 6 * k, x + 14 * k, y + 6 * k, color, 1.3F * k);
        c.line(x + 14 * k, y + 6 * k, x + 14 * k, y + 12.5F * k, color, 1.3F * k);
        c.line(x + 14 * k, y + 12.5F * k, x + 2 * k, y + 12.5F * k, color, 1.3F * k);
        c.line(x + 2 * k, y + 12.5F * k, x + 2 * k, y + 4.5F * k, color, 1.3F * k);
    };
    const auto ring = [&](const RECT& r, float radius) {
        RECT around = r;
        InflateRect(&around, px(2), px(2));
        c.round_outline(around, radius + pxf(2), p.focus, pxf(2));
    };
    const auto mid_y = [](const RECT& r) { return static_cast<float>(r.top + r.bottom) / 2.0F; };

    // A download row's pieces: its name, the line and, while it runs, the bar before Cancel.
    const auto download_parts = [&](std::size_t i, RECT& name, RECT& line, RECT& bar) {
        const RECT& r = g.rows[i];
        const int x = r.left + px(16) + px(kBox) + px(12);
        name = {x, r.top, std::min<int>(r.right - px(16), x + px(kNameW)), r.bottom};
        const RECT cancel = i < g.cancels.size() ? g.cancels[i] : RECT{};
        int end = is_empty(cancel) ? r.right - px(16) : cancel.left - px(12);
        bar = {};
        if (i < rows.size() && rows[i].state == FirstRunRowState::Running && end - px(kBarW) > name.right + px(120)) {
            const int mid = (r.top + r.bottom) / 2;
            bar = {end - px(kBarW), mid - px(2), end, mid + px(2)};
            end = bar.left - px(12);
        }
        line = {name.right + px(12), r.top, std::max<int>(name.right + px(12), end), r.bottom};
    };

    if (!text_pass) {
        c.fill(g.footer, p.footer);
        c.fill({g.footer.left, g.footer.top, g.footer.right, g.footer.top + static_cast<int>(border)}, p.footer_border);
        c.clip(g.content);
        if (!is_empty(g.card)) {
            c.round_rect(g.card, pxf(8), p.card, p.card_border, border);
        }
        RECT inner_card = g.card;
        InflateRect(&inner_card, -static_cast<int>(border), -static_cast<int>(border));
        for (std::size_t i = 0; i < g.rows.size(); ++i) {
            const RECT& r = g.rows[i];
            const int index = static_cast<int>(i);
            RECT fill_r{inner_card.left, std::max(r.top, inner_card.top), inner_card.right, std::min(r.bottom, inner_card.bottom)};
            if (i + 1 < g.rows.size()) {
                c.fill({r.left + px(1), r.bottom - static_cast<int>(border), r.right - px(1), r.bottom}, p.divider);
                fill_r.bottom -= static_cast<int>(border);
            }
            const auto x = static_cast<float>(r.left + px(16));
            const float y = mid_y(r) - pxf(8);
            switch (first_run_.step) {
                case FirstRunStep::Language: {
                    const bool on = i < kLangChoices.size() && kLangChoices.at(i) == prefs_.lang;
                    if (on) {
                        c.fill(fill_r, p.selected);
                    } else if (hovered(Part::FrLang, index)) {
                        c.fill(fill_r, p.row_hover);
                    }
                    const float cx = static_cast<float>(r.left + px(16)) + pxf(kBox / 2.0F);
                    const float cy = mid_y(r);
                    if (on) {
                        c.ellipse(cx, cy, pxf(kBox / 2.0F), p.accent);
                        c.ellipse(cx, cy, pxf(4.0F), p.accent_text);
                    } else {
                        c.ellipse(cx, cy, pxf(kBox / 2.0F), p.input);
                        c.circle(cx, cy, pxf(kBox / 2.0F) - border / 2.0F, p.switch_off_border, border);
                    }
                    if (focused({Part::FrLang, index, {}})) {
                        RECT around = fill_r;
                        InflateRect(&around, -px(3), -px(3));
                        c.round_outline(around, pxf(4), p.focus, pxf(2));
                    }
                    break;
                }
                case FirstRunStep::Runtimes:
                    if (!first_run_.started) {
                        if (i >= first_run_.offers.size()) {
                            break;
                        }
                        const FirstRunOffer& o = first_run_.offers[i];
                        const bool on = o.always || (i < first_run_.ticks.size() && first_run_.ticks[i]);
                        if (hovered(Part::FrTick, index)) {
                            c.fill(fill_r, p.row_hover);
                        }
                        const RECT box{r.left + px(16), (r.top + r.bottom) / 2 - px(kBox) / 2, r.left + px(16) + px(kBox),
                                       (r.top + r.bottom) / 2 + px(kBox) / 2};
                        if (o.always) {
                            c.round_rect(box, pxf(3), p.button_disabled, p.button_border, border);
                        } else if (on) {
                            c.round_rect(box, pxf(3), p.accent);
                        } else {
                            c.round_rect(box, pxf(3), p.input, p.switch_off_border, border);
                        }
                        if (on) {
                            check(static_cast<float>(box.left) + pxf(1), static_cast<float>(box.top) + pxf(1),
                                  o.always ? p.button_disabled_text : p.accent_text);
                        }
                        if (focused({Part::FrTick, index, {}})) {
                            RECT around = fill_r;
                            InflateRect(&around, -px(3), -px(3));
                            c.round_outline(around, pxf(4), p.focus, pxf(2));
                        }
                    } else if (i < rows.size()) {
                        switch (rows[i].state) {
                            case FirstRunRowState::Ready:
                                check(x, y, p.on.text);
                                break;
                            case FirstRunRowState::Running:
                                arrow(x, y, p.accent);
                                break;
                            case FirstRunRowState::Failed:
                                cross(x, y, p.problem.text);
                                break;
                            case FirstRunRowState::Cancelled:
                                cross(x, y, p.text_subtle);
                                break;
                            case FirstRunRowState::Waiting:
                                break;
                        }
                        RECT name{};
                        RECT line{};
                        RECT bar{};
                        download_parts(i, name, line, bar);
                        if (!is_empty(bar)) {
                            c.round_rect(bar, pxf(2), p.bar_track);
                            if (rows[i].fraction) {
                                RECT done = bar;
                                done.right = bar.left + static_cast<int>(static_cast<double>(width_of(bar)) * *rows[i].fraction);
                                if (done.right > done.left) {
                                    c.round_rect(done, pxf(2), p.accent);
                                }
                            }
                        }
                        if (i < g.cancels.size() && !is_empty(g.cancels[i])) {
                            paint_button(c, g.cancels[i], hovered(Part::FrCancel, index), focused({Part::FrCancel, index, {}}));
                        }
                    }
                    break;
                case FirstRunStep::Ready: {
                    const float iy = static_cast<float>(r.top + px(14));
                    if (i == 0) {
                        const FirstRunProgress progress = first_run_progress(rows);
                        if (progress.total > 0 && progress.running()) {
                            arrow(x, iy, p.accent);
                        } else if (progress.total > 0 && progress.failed == 0) {
                            check(x, iy, p.on.text);
                        } else {
                            info(x, iy, progress.failed > 0 ? p.problem.text : p.text_subtle);
                        }
                    } else if (i == 1) {
                        if (first_run_.trust == FirstRunTrust::Trusted) {
                            check(x, iy, p.on.text);
                        } else {
                            info(x, iy, p.text_subtle);
                        }
                    } else {
                        folder(x, iy, p.text_subtle);
                    }
                    break;
                }
            }
        }
        if (!is_empty(g.info)) {
            const bool problem = first_run_.trust == FirstRunTrust::Failed;
            const StateColors& box = problem ? p.banner_error : p.banner_info;
            c.round_rect(g.info, pxf(6), box.fill);
            const auto ix = static_cast<float>(g.info.left + px(14));
            const auto iy = static_cast<float>(g.info.top + px(10)) + pxf(1);
            if (!first_run_.started) {
                info(ix, iy, p.accent);
            } else if (first_run_.trust == FirstRunTrust::Trusted) {
                check(ix, iy, p.on.text);
            } else if (first_run_.trust == FirstRunTrust::Asking || first_run_.trust == FirstRunTrust::Checking) {
                lock(ix, iy, p.accent);
            } else {
                info(ix, iy, problem ? box.text : p.text_subtle);
            }
        }
        if (!is_empty(g.retry)) {
            paint_button(c, g.retry, hovered(Part::FrRetry, -1), focused({Part::FrRetry, -1, {}}));
        }
        if (!is_empty(g.change) && focused({Part::FrChange, -1, {}})) {
            ring(g.change, pxf(4));
        }
        if (!is_empty(g.create)) {
            c.round_rect(g.create, pxf(8), p.accent);
            if (focused({Part::FrCreate, -1, {}})) {
                ring(g.create, pxf(8));
            }
            c.round_rect(g.add, pxf(8), hovered(Part::FrAdd, -1) ? p.row_hover : p.card, p.card_border, border);
            if (focused({Part::FrAdd, -1, {}})) {
                ring(g.add, pxf(8));
            }
        }
        first_run_bar_.paint(c, p);
        c.unclip();
        if (!is_empty(g.skip)) {
            paint_button(c, g.skip, hovered(Part::FrSkipSetup, -1), focused({Part::FrSkipSetup, -1, {}}));
        }
        if (!is_empty(g.back)) {
            paint_button(c, g.back, hovered(Part::FrBack, -1), focused({Part::FrBack, -1, {}}));
        }
        if (!is_empty(g.without)) {
            paint_button(c, g.without, hovered(Part::FrWithout, -1), focused({Part::FrWithout, -1, {}}));
        }
        if (!is_empty(g.next)) {
            if (g.next_primary) {
                c.round_rect(g.next, pxf(4), p.accent);
                if (focused({Part::FrNext, -1, {}})) {
                    ring(g.next, pxf(4));
                }
            } else {
                paint_button(c, g.next, hovered(Part::FrNext, -1), focused({Part::FrNext, -1, {}}));
            }
        }
        return;
    }

    // The steps, the current one in the accent colour.
    {
        const FirstRunStep steps[] = {FirstRunStep::Language, FirstRunStep::Runtimes, FirstRunStep::Ready};
        for (std::size_t k = 0; k < g.step_rects.size() && k < std::size(steps); ++k) {
            const bool current = steps[k] == first_run_.step;
            HFONT font = (current ? fonts_.button_bold : fonts_.button).get();
            c.text(g.step_rects[k], w(first_run_step_label(steps[k], first_run_.step, lang_)), font,
                   current ? p.accent : p.text_subtle, Align::Start, false);
        }
    }

    c.clip_text(g.content);
    // The title and the words under it.
    Str title = Str::FirstRunWelcome;
    std::string intro;
    switch (first_run_.step) {
        case FirstRunStep::Language:
            intro = std::string(text(lang_, Str::FirstRunIntro));
            break;
        case FirstRunStep::Runtimes:
            if (first_run_.started) {
                const FirstRunProgress progress = first_run_progress(rows);
                title = progress.running() ? Str::FirstRunDownloadingTitle : Str::FirstRunDownloadedTitle;
                intro = fill(lang_, Str::FirstRunDownloadingIntro, {std::to_string(progress.ready), std::to_string(progress.total)});
            } else {
                title = Str::FirstRunRuntimesTitle;
                intro = std::string(text(lang_, Str::FirstRunRuntimesIntro));
            }
            break;
        case FirstRunStep::Ready:
            title = Str::FirstRunReadyTitle;
            break;
    }
    c.text(g.title, w(text(lang_, title)), fonts_.hero.get(), p.text, Align::Start, false);
    if (!intro.empty()) {
        c.paragraph(g.intro, w(intro), fonts_.button.get(), p.text_subtle);
    }

    switch (first_run_.step) {
        case FirstRunStep::Language: {
            c.text(g.legend, w(text(lang_, Str::GeneralLanguage)), fonts_.body_bold.get(), p.text, Align::Start, false);
            for (std::size_t i = 0; i < g.rows.size() && i < kLangChoices.size(); ++i) {
                const RECT& r = g.rows[i];
                const auto [label, note] = first_run_language_row(kLangChoices.at(i), first_run_.system_lang, lang_);
                const int start = r.left + px(16) + px(kBox) + px(12);
                int end = r.right - px(16);
                if (!note.empty()) {
                    const int nw = Canvas::measure(dc, w(note), fonts_.caption.get(), rtl_) + px(4);
                    const RECT note_r{std::max(start, end - nw), r.top, end, r.bottom};
                    c.text(note_r, w(note), fonts_.caption.get(), p.text_subtle, Align::End, false);
                    end = note_r.left - px(8);
                }
                c.text({start, r.top, std::max(start, end), r.bottom}, w(label), fonts_.body.get(), p.text, Align::Start, false);
            }
            c.paragraph(g.note, w(text(lang_, Str::FirstRunLanguageNote)), fonts_.caption.get(), p.text_subtle);
            break;
        }
        case FirstRunStep::Runtimes:
            if (first_run_.started) {
                for (std::size_t i = 0; i < g.rows.size() && i < rows.size(); ++i) {
                    RECT name{};
                    RECT line{};
                    RECT bar{};
                    download_parts(i, name, line, bar);
                    c.text(name, w(rows[i].label), fonts_.body_bold.get(), p.text, Align::Start, true);
                    const bool failed = rows[i].state == FirstRunRowState::Failed;
                    c.text(line, w(rows[i].line), fonts_.button.get(), failed ? p.problem.text : p.text_subtle, Align::Start,
                           false);
                    if (i < g.cancels.size() && !is_empty(g.cancels[i])) {
                        c.text(g.cancels[i], w(text(lang_, Str::WizardCancel)), fonts_.button.get(), p.text, Align::Center,
                               false);
                    }
                }
            } else if (first_run_.list == FirstRunList::Ready) {
                for (std::size_t i = 0; i < g.rows.size() && i < first_run_.offers.size(); ++i) {
                    const RECT& r = g.rows[i];
                    const FirstRunOffer& o = first_run_.offers[i];
                    const int x = r.left + px(16) + px(kBox) + px(12);
                    const RECT name{x, r.top, std::min<int>(r.right - px(16), x + px(kNameW)), r.bottom};
                    const RECT size{std::max(name.right, r.right - px(16) - px(kSizeW)), r.top, r.right - px(16), r.bottom};
                    const RECT what{name.right + px(12), r.top, std::max(name.right + px(12), size.left - px(12)), r.bottom};
                    c.text(name, w(o.label), fonts_.body_bold.get(), p.text, Align::Start, true);
                    c.text(what, w(text(lang_, o.what)), fonts_.button.get(), p.text_subtle, Align::Start, false);
                    if (o.size > 0) {
                        c.text(size, w(size_text(o.size)), fonts_.caption.get(), p.text_subtle, Align::End, true);
                    }
                }
            } else if (first_run_.list == FirstRunList::Failed) {
                c.paragraph(g.message, w(first_run_list_error(first_run_.list_error, lang_)), fonts_.button.get(), p.problem.text);
                c.text(g.retry, w(text(lang_, Str::FirstRunTryAgain)), fonts_.button.get(), p.text, Align::Center, false);
            } else {
                c.text(g.message, w(text(lang_, Str::FirstRunLoading)), fonts_.button.get(), p.text_subtle, Align::Start,
                       false);
            }
            if (!is_empty(g.info)) {
                RECT inner{g.info.left + px(14 + 16 + 10), g.info.top + px(10), g.info.right - px(14), g.info.bottom - px(10)};
                if (first_run_.started) {
                    const bool problem = first_run_.trust == FirstRunTrust::Failed;
                    c.paragraph(inner, w(first_run_https_line(first_run_.trust, first_run_.trust_error, lang_)),
                                fonts_.button.get(), problem ? p.banner_error.text : p.text);
                } else {
                    c.paragraph(inner, w(text(lang_, Str::FirstRunCertNote)), fonts_.caption.get(), p.text);
                }
            }
            if (!is_empty(g.info_note)) {
                c.paragraph(g.info_note, w(text(lang_, Str::FirstRunTrustAskingNote)), fonts_.caption.get(), p.text_subtle);
            }
            break;
        case FirstRunStep::Ready: {
            const Str titles[] = {Str::SettingsRuntimes, Str::FirstRunHttps, Str::FirstRunFolderTitle};
            const std::string details[] = {first_run_runtimes_line(rows, lang_, first_run_.without),
                                           first_run_https_line(first_run_.trust, first_run_.trust_error, lang_)};
            for (std::size_t i = 0; i < g.rows.size() && i < g.details.size() && i < 3; ++i) {
                const RECT& r = g.rows[i];
                const RECT& d = g.details[i];
                c.text({d.left, r.top + px(12), d.right, r.top + px(32)}, w(text(lang_, titles[i])), fonts_.body_bold.get(),
                       p.text, Align::Start, false);
                if (i < 2) {
                    c.paragraph(d, w(details[i]), fonts_.button.get(), p.text_subtle);
                } else {
                    c.text(d, w(first_run_folder()), fonts_.button.get(), p.text_subtle, Align::Start, true);
                }
            }
            if (!is_empty(g.change)) {
                const bool busy = !first_run_.folder_ask.empty();
                c.text(g.change, w(text(lang_, Str::FirstRunChange)), fonts_.button.get(), busy ? p.text_subtle : p.link,
                       Align::End, false);
            }
            const auto big = [&](const RECT& r, Str head, Str note, Rgb head_color, Rgb note_color) {
                c.text({r.left + px(20), r.top + px(18), r.right - px(20), r.top + px(40)}, w(text(lang_, head)),
                       fonts_.body_bold.get(), head_color, Align::Start, false);
                c.paragraph({r.left + px(20), r.top + px(46), r.right - px(20), r.bottom - px(12)}, w(text(lang_, note)),
                            fonts_.button.get(), note_color);
            };
            if (!is_empty(g.create)) {
                big(g.create, Str::FirstRunCreate, Str::FirstRunCreateNote, p.accent_text, p.accent_text);
                big(g.add, Str::FirstRunAddFolder, Str::FirstRunAddFolderNote, p.text, p.text_subtle);
            }
            break;
        }
    }
    c.unclip_text();

    // The footer: its words, then the buttons.
    if (!is_empty(g.footer_text)) {
        const std::wstring s = w(first_run_footer_words());
        if (Canvas::measure(dc, s, fonts_.button.get(), rtl_) <= width_of(g.footer_text)) {
            c.text(g.footer_text, s, fonts_.button.get(), p.text_subtle, Align::Start, false);
        } else {
            const int h = text_height(dc, s, fonts_.button.get(), width_of(g.footer_text), rtl_);
            const int top = std::max<int>(static_cast<int>(g.footer_text.top) + px(4), (g.footer_text.top + g.footer_text.bottom - h) / 2);
            c.paragraph({g.footer_text.left, top, g.footer_text.right, g.footer_text.bottom - px(4)}, s, fonts_.button.get(),
                        p.text_subtle);
        }
    }
    if (!is_empty(g.skip)) {
        c.text(g.skip, w(text(lang_, Str::FirstRunSkipSetup)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (!is_empty(g.back)) {
        c.text(g.back, w(text(lang_, Str::WizardBack)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (!is_empty(g.without)) {
        c.text(g.without, w(text(lang_, Str::FirstRunWithout)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (!is_empty(g.next)) {
        c.text(g.next, w(text(lang_, g.next_label)), (g.next_primary ? fonts_.button_bold : fonts_.button).get(),
               g.next_primary ? p.accent_text : p.text, Align::Center, false);
    }
}

}  // namespace floserver::ui

// Settings > Ports & HTTPS (D20): which ports FloServer's web server listens
// on, HTTPS for projects and for shared sites' https://localhost/<folder>/,
// and FloServer's certificate authority in Windows' trusted list.
//
// Opening the section asks web.settings.get once; the service checks the
// ports then and names the program holding one. Nothing is asked again
// until the user changes something (web.settings.set, sent at once: there's
// no Save) or presses F5. Adding the CA to Windows, or removing it, is done
// here in the window, because Windows asks the user to confirm.

#include "settings_window.h"

#include "main_window_layout.h"
#include "model/theme.h"

#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/trust_store.h"

#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <string_view>
#include <utility>

namespace floserver::ui {

using layout::height_of;
using nlohmann::json;

namespace {

constexpr int kGap = 12;
constexpr int kCardPad = 16;
constexpr int kControlH = 30;
constexpr int kLabelH = 16;
constexpr int kRadio = 16;
constexpr int kSwitchW = 40;
constexpr int kSwitchH = 20;
constexpr int kPortBoxW = 110;

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

}  // namespace

// --- loading, answers ---------------------------------------------------------------

void SettingsWindow::load_ports() {
    ports_loading_ = true;
    worker_.web_settings("get");
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::on_ports_answers(const Snapshot& snap) {
    const auto service_words = [this](const std::string& s) { return rtl_ ? isolate_ltr(s) : s; };
    for (const Snapshot::WebSettingsAnswer& answer : snap.web_settings) {
        const bool change = answer.what != "get";
        if (change && ports_waiting_ > 0) {
            --ports_waiting_;
        }
        if (answer.what == "ca") {
            if (!answer.error.empty() || !answer.reply || !ports_ || !apply_ca_reply(*ports_, *answer.reply)) {
                trust_after_ca_ = false;
                show_message(fill_pattern(text(lang_, Str::PortsTrustFailed), false,
                                          {answer.error.empty() ? std::string(text(lang_, Str::PortsCertUnreadable))
                                                                : service_words(answer.error)}),
                             true);
            } else if (std::exchange(trust_after_ca_, false) && section_ == Section::Ports) {
                trust_ca_now();  // only while the section that asked is shown
            }
            continue;
        }
        if (!change) {
            ports_loading_ = false;
        }
        if (!answer.error.empty() || !answer.reply) {
            show_message(fill_pattern(text(lang_, change ? Str::PortsChangeFailed : Str::PortsReadFailed), false,
                                      {service_words(answer.error)}),
                         true);
        } else if (auto settings = ports_settings_from_reply(*answer.reply)) {
            ports_ = std::move(settings);
            if (answer.what == "set" && ports_->ports == "custom") {
                custom_open_ = false;  // it's the saved choice now
            }
            fill_port_edits();
        }
    }
    if (ports_waiting_ == 0) {
        // Everything sent is answered: what's shown is the service's again.
        pending_choice_.reset();
        pending_switch_ = {};
    }
}

void SettingsWindow::send_ports(const json& params) {
    if (!connected_ || !ports_) {
        show_message(fill(lang_, Str::PortsChangeFailed, {text(lang_, Str::ConnectionLost)}), true);
        return;
    }
    ++ports_waiting_;
    message_.clear();
    worker_.web_settings("set", params);
}

std::size_t SettingsWindow::shown_port_choice() const {
    if (pending_choice_) {
        return *pending_choice_;
    }
    if (custom_open_) {
        return kPortChoices.size() - 1;
    }
    return ports_ ? port_choice_index(ports_->ports) : 0;
}

bool SettingsWindow::shown_switch(int index) const {
    const auto& pending = pending_switch_[static_cast<std::size_t>(index)];
    if (pending) {
        return *pending;
    }
    return ports_ && (index == 0 ? ports_->https : ports_->shared_https);
}

// --- the port boxes ----------------------------------------------------------------

void SettingsWindow::make_port_edits() {
    if (!port_edits_.empty()) {
        return;
    }
    for (int i = 0; i < 2; ++i) {
        const int id = kPortEditBase + i;
        // Port numbers read left to right in every language.
        auto edit = std::make_unique<FieldEdit>(hwnd_, id, fonts_.button.get(), false,
                                                [this, id](WPARAM key) { return on_edit_key(id, key); });
        SendMessageW(edit->hwnd(), EM_LIMITTEXT, 5, 0);
        edit->set_cue(i == 0 ? "8000" : "8443");
        edit->set_cue_color(palette(dark_).text_subtle);
        port_edits_.push_back(std::move(edit));
    }
    fill_port_edits();
}

void SettingsWindow::fill_port_edits() {
    if (port_edits_.size() != 2 || !ports_) {
        return;
    }
    // Not while the user is typing in one.
    if (port_edits_[0]->focused() || port_edits_[1]->focused()) {
        return;
    }
    port_edits_[0]->set_text(ports_->http_port ? std::to_string(*ports_->http_port) : std::string());
    port_edits_[1]->set_text(ports_->https_port ? std::to_string(*ports_->https_port) : std::string());
}

void SettingsWindow::place_port_edits() {
    if (section_ != Section::Ports || port_edits_.size() != 2) {
        return;
    }
    const PortsGeometry& g = geometry_.ports;
    const int h = px(20);
    port_edits_[0]->place(g.http_box, px(8), h, !is_empty(g.http_box), 0, height_);
    port_edits_[1]->place(g.https_box, px(8), h, !is_empty(g.https_box), 0, height_);
}

void SettingsWindow::use_custom_ports() {
    if (port_edits_.size() != 2) {
        return;
    }
    Str problem = Str::Count_;
    const auto ports = parse_custom_ports(port_edits_[0]->text(), port_edits_[1]->text(), problem);
    if (!ports) {
        ports_problem_ = problem;
        relayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    ports_problem_ = Str::Count_;
    pending_choice_ = kPortChoices.size() - 1;
    send_ports(port_choice_params(kPortChoices.size() - 1, CustomPorts{ports->http, ports->https}));
    relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// --- the certificate authority -------------------------------------------------------

void SettingsWindow::trust_ca() {
    if (!ports_ || !connected_ || ports_waiting_ > 0) {
        return;
    }
    if (ports_->ca == CaState::None || ports_->ca_der.empty()) {
        // Made by the service first; trusted when its answer comes.
        trust_after_ca_ = true;
        ++ports_waiting_;
        message_.clear();
        worker_.web_settings("ca");
        return;
    }
    trust_ca_now();
}

void SettingsWindow::trust_ca_now() {
    if (!ports_) {
        return;
    }
    const auto der = floserver::core::from_base64(ports_->ca_der);
    if (!der) {
        show_message(fill_pattern(text(lang_, Str::PortsTrustFailed), false, {text(lang_, Str::PortsCertUnreadable)}), true);
        return;
    }
    bool cancelled = false;
    std::string error;
    // Windows shows its own question here and waits for the answer.
    if (!floserver::core::trust_ca(*der, cancelled, error)) {
        show_message(cancelled ? std::string(text(lang_, Str::PortsTrustNo))
                               : fill_pattern(text(lang_, Str::PortsTrustFailed), false, {rtl_ ? isolate_ltr(error) : error}),
                     !cancelled);
        return;
    }
    show_message(std::string(text(lang_, Str::PortsTrustDone)), false);
    ++ports_waiting_;
    worker_.web_settings("reload");  // projects move to https now
}

void SettingsWindow::remove_ca() {
    if (!ports_ || ports_->ca_der.empty() || ports_waiting_ > 0) {
        return;
    }
    const auto der = floserver::core::from_base64(ports_->ca_der);
    if (!der) {
        show_message(fill_pattern(text(lang_, Str::PortsRemoveFailed), false, {text(lang_, Str::PortsCertUnreadable)}), true);
        return;
    }
    bool cancelled = false;
    std::string error;
    if (!floserver::core::untrust_ca(*der, cancelled, error)) {
        show_message(cancelled ? std::string(text(lang_, Str::PortsRemoveNo))
                               : fill_pattern(text(lang_, Str::PortsRemoveFailed), false, {rtl_ ? isolate_ltr(error) : error}),
                     !cancelled);
        return;
    }
    show_message(std::string(text(lang_, Str::PortsRemoveDone)), false);
    if (connected_) {
        ++ports_waiting_;
        worker_.web_settings("reload");  // projects go back to http now
    }
}

void SettingsWindow::open_certificate() {
    if (!ports_ || ports_->ca_viewer.empty()) {
        return;
    }
    // Only FloServer's own certificate file: a .crt that's there.
    const std::filesystem::path file = floserver::core::path_from_utf8(ports_->ca_viewer);
    std::error_code ec;
    if (file.extension() != ".crt" || !std::filesystem::is_regular_file(file, ec)) {
        show_message(fill(lang_, Str::PortsOpenFailed, {ports_->ca_viewer}), true);
        return;
    }
    // A .crt opens in Windows' own certificate viewer.
    const auto opened = reinterpret_cast<INT_PTR>(
        ShellExecuteW(hwnd_, L"open", w(ports_->ca_viewer).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (opened <= 32) {
        show_message(fill(lang_, Str::PortsOpenFailed, {ports_->ca_viewer}), true);
    }
}

// --- geometry ------------------------------------------------------------------------

void SettingsWindow::layout_ports(HDC dc, Geometry& g, int x0, int x1, int y) const {
    PortsGeometry& pg = g.ports;
    const auto measure = [&](std::string_view s, HFONT font) { return Canvas::measure(dc, w(s), font, rtl_); };
    const auto height = [&](std::string_view s, HFONT font, int width) { return text_height(dc, w(s), font, width, rtl_); };

    const int hint_h = height(text(lang_, Str::PortsHint), fonts_.caption.get(), x1 - x0);
    g.title = {x0, y, x1, y + px(28)};
    g.hint = {x0, g.title.bottom, x1, g.title.bottom + hint_h};
    y = static_cast<int>(g.hint.bottom) + px(kGap);
    if (!message_.empty()) {
        const int mh = std::max(px(32), height(message_, fonts_.button.get(), x1 - x0 - px(24)) + px(14));
        g.message = {x0, y, x1, y + mh};
        y += mh + px(kGap);
    }
    if (!ports_) {
        if (ports_loading_) {
            pg.note = {x0, y, x1, y + px(24)};
            y = static_cast<int>(pg.note.bottom);
        }
        g.content_h = y + scroll_ + px(8);
        return;
    }
    const int cx0 = x0 + px(kCardPad);
    const int cx1 = x1 - px(kCardPad);
    const int text_x = cx0 + px(kRadio) + px(10);  // after a radio button or a switch
    const int text_w = std::max(px(80), cx1 - text_x);

    // Web ports: four choices, the boxes for other ports, what's in use and what's taken.
    pg.web_card = {x0, y, x1, y};
    int yy = y + px(14);
    pg.web_title = {cx0, yy, cx1, yy + px(24)};
    yy = static_cast<int>(pg.web_title.bottom) + px(6);
    const std::size_t shown = shown_port_choice();
    for (std::size_t i = 0; i < kPortChoices.size(); ++i) {
        const std::string note = port_choice_note(i, lang_);
        const int label_h = px(20);
        const int note_h = height(note, fonts_.caption.get(), text_w);
        const int row_h = std::max(px(kControlH), label_h + note_h + px(8));
        pg.choices[i] = {cx0 - px(6), yy, cx1, yy + row_h};
        pg.choice_labels[i] = {text_x, yy + px(4), cx1, yy + px(4) + label_h};
        pg.choice_notes[i] = {text_x, static_cast<int>(pg.choice_labels[i].bottom), cx1,
                              static_cast<int>(pg.choice_labels[i].bottom) + note_h};
        yy += row_h + px(2);
        if (i == kPortChoices.size() - 1 && shown == i) {
            // Other ports: two boxes and Use these ports, under the choice.
            const int label_w = std::max(measure(text(lang_, Str::PortsHttpLabel), fonts_.caption.get()),
                                         measure(text(lang_, Str::PortsHttpsLabel), fonts_.caption.get()));
            const int box_w = std::max(px(kPortBoxW), label_w);
            const int use_w = measure(text(lang_, Str::PortsUse), fonts_.button.get()) + px(24);
            int bx = text_x;
            pg.http_label = {bx, yy, bx + box_w, yy + px(kLabelH)};
            pg.http_box = {bx, yy + px(kLabelH) + px(4), bx + box_w, yy + px(kLabelH) + px(4) + px(kControlH)};
            bx += box_w + px(kGap);
            pg.https_label = {bx, yy, bx + box_w, yy + px(kLabelH)};
            pg.https_box = {bx, pg.http_box.top, bx + box_w, pg.http_box.bottom};
            bx += box_w + px(kGap);
            if (bx + use_w > cx1) {  // a narrow window: the button goes under the boxes
                yy = static_cast<int>(pg.http_box.bottom) + px(8);
                pg.use = {text_x, yy, text_x + use_w, yy + px(kControlH)};
            } else {
                pg.use = {bx, pg.http_box.top, bx + use_w, pg.http_box.bottom};
            }
            yy = static_cast<int>(pg.use.bottom) + px(6);
            if (ports_problem_ != Str::Count_) {
                pg.problem = {text_x, yy, cx1, yy + px(18)};
                yy = static_cast<int>(pg.problem.bottom) + px(4);
            }
        }
    }
    yy += px(6);
    const std::string in_use = ports_in_use_text(*ports_, lang_);
    pg.in_use = {cx0, yy, cx1, yy + std::max(px(18), height(in_use, fonts_.caption.get(), cx1 - cx0))};
    yy = static_cast<int>(pg.in_use.bottom);
    for (std::string& line : ports_taken_lines(*ports_, lang_)) {
        const int lh = std::max(px(18), height(line, fonts_.caption.get(), cx1 - cx0));
        pg.taken.emplace_back(RECT{cx0, yy + px(2), cx1, yy + px(2) + lh}, std::move(line));
        yy += px(2) + lh;
    }
    pg.web_card.bottom = yy + px(14);
    y = static_cast<int>(pg.web_card.bottom) + px(kGap);

    // HTTPS: two switches.
    pg.https_card = {x0, y, x1, y};
    yy = y + px(14);
    pg.https_title = {cx0, yy, cx1, yy + px(24)};
    yy = static_cast<int>(pg.https_title.bottom) + px(6);
    const int switch_text_x = cx0 + px(kSwitchW) + px(12);
    const int switch_text_w = std::max(px(80), cx1 - switch_text_x);
    const std::array<std::string, 2> notes = {
        fill(lang_, Str::PortsHttpsProjectsNote, {"https://<name>.localhost"}),
        fill(lang_, Str::PortsHttpsSharedNote, {"https://localhost/<folder>/", "<folder>.localhost"})};
    for (std::size_t i = 0; i < 2; ++i) {
        const int note_h = height(notes[i], fonts_.caption.get(), switch_text_w);
        const int row_h = std::max(px(kControlH), px(20) + note_h + px(8));
        pg.switch_rows[i] = {cx0 - px(6), yy, cx1, yy + row_h};
        pg.switches[i] = {cx0, yy + px(4), cx0 + px(kSwitchW), yy + px(4) + px(kSwitchH)};
        pg.switch_labels[i] = {switch_text_x, yy + px(4), cx1, yy + px(4) + px(20)};
        pg.switch_notes[i] = {switch_text_x, static_cast<int>(pg.switch_labels[i].bottom), cx1,
                              static_cast<int>(pg.switch_labels[i].bottom) + note_h};
        yy += row_h + px(4);
    }
    if (ports_->ca != CaState::Trusted && (shown_switch(0) || shown_switch(1))) {
        const int nh = height(text(lang_, Str::PortsNeedsTrust), fonts_.caption.get(), cx1 - cx0);
        pg.needs_trust = {cx0, yy, cx1, yy + nh};
        yy = static_cast<int>(pg.needs_trust.bottom);
    }
    pg.https_card.bottom = yy + px(14);
    y = static_cast<int>(pg.https_card.bottom) + px(kGap);

    // The certificate authority: what Windows thinks of it, the buttons, the plain sentence.
    pg.ca_card = {x0, y, x1, y};
    yy = y + px(14);
    pg.ca_title = {cx0, yy, cx1, yy + px(24)};
    yy = static_cast<int>(pg.ca_title.bottom) + px(4);
    const std::string status = ca_status_text(*ports_, lang_);
    pg.ca_status = {cx0, yy, cx1, yy + std::max(px(20), height(status, fonts_.button.get(), cx1 - cx0))};
    yy = static_cast<int>(pg.ca_status.bottom) + px(8);
    int bx = cx0;
    const auto button = [&](RECT& r, Str label) {
        const int bw = measure(text(lang_, label), fonts_.button.get()) + px(24);
        if (bx > cx0 && bx + bw > cx1) {  // no room on this line
            bx = cx0;
            yy += px(kControlH) + px(8);
        }
        r = {bx, yy, bx + bw, yy + px(kControlH)};
        bx += bw + px(8);
    };
    if (ports_->ca == CaState::Trusted) {
        button(pg.remove, Str::SettingsRemove);
    } else if (ports_->ca != CaState::Damaged) {
        button(pg.trust, Str::PortsTrust);
    }
    if (!ports_->ca_viewer.empty()) {
        button(pg.open, Str::PortsOpenCert);
    }
    if (bx > cx0) {
        yy += px(kControlH) + px(10);
    }
    const int safe_h = height(text(lang_, Str::PortsCaSafe), fonts_.caption.get(), cx1 - cx0);
    pg.ca_safe = {cx0, yy, cx1, yy + safe_h};
    yy = static_cast<int>(pg.ca_safe.bottom);
    pg.ca_card.bottom = yy + px(14);
    y = static_cast<int>(pg.ca_card.bottom) + px(kGap);
    g.content_h = y + scroll_ + px(8);
}

const RECT* SettingsWindow::ports_rect_of(const Hit& hit) const {
    const PortsGeometry& g = geometry_.ports;
    const auto at = [&](const auto& list) -> const RECT* {
        return hit.row >= 0 && std::cmp_less(hit.row, list.size()) ? &list[static_cast<std::size_t>(hit.row)] : nullptr;
    };
    switch (hit.part) {
        case Part::PortsChoice:
            return at(g.choices);
        case Part::PortsHttpEdit:
            return &g.http_box;
        case Part::PortsHttpsEdit:
            return &g.https_box;
        case Part::PortsUse:
            return &g.use;
        case Part::PortsSwitch:
            return at(g.switch_rows);
        case Part::PortsTrust:
            return &g.trust;
        case Part::PortsRemove:
            return &g.remove;
        case Part::PortsOpenCert:
            return &g.open;
        default:
            break;
    }
    return nullptr;
}

void SettingsWindow::ports_focus_order(std::vector<Hit>& order) const {
    if (!ports_) {
        return;
    }
    const PortsGeometry& g = geometry_.ports;
    const auto add = [&](Part part, const RECT& r, int row = -1) {
        if (!is_empty(r)) {
            order.push_back({part, -1, row});
        }
    };
    for (std::size_t i = 0; i < g.choices.size(); ++i) {
        add(Part::PortsChoice, g.choices[i], static_cast<int>(i));
    }
    add(Part::PortsHttpEdit, g.http_box);
    add(Part::PortsHttpsEdit, g.https_box);
    add(Part::PortsUse, g.use);
    for (std::size_t i = 0; i < g.switch_rows.size(); ++i) {
        add(Part::PortsSwitch, g.switch_rows[i], static_cast<int>(i));
    }
    add(Part::PortsTrust, g.trust);
    add(Part::PortsRemove, g.remove);
    add(Part::PortsOpenCert, g.open);
}

// --- input -----------------------------------------------------------------------------

void SettingsWindow::on_ports_click(const Hit& hit) {
    if (!ports_) {
        return;
    }
    switch (hit.part) {
        case Part::PortsChoice: {
            if (hit.row < 0 || std::cmp_greater_equal(hit.row, kPortChoices.size())) {
                break;
            }
            const auto index = static_cast<std::size_t>(hit.row);
            if (index == kPortChoices.size() - 1) {
                // Other ports: the boxes open; Use these ports sends them.
                custom_open_ = true;
                relayout();
                if (port_edits_.size() == 2) {
                    SetFocus(port_edits_[0]->hwnd());
                    SendMessageW(port_edits_[0]->hwnd(), EM_SETSEL, 0, -1);
                }
                break;
            }
            custom_open_ = false;
            ports_problem_ = Str::Count_;
            if (index != shown_port_choice() || ports_->ports == "custom") {
                pending_choice_ = index;
                send_ports(port_choice_params(index, std::nullopt));
            }
            relayout();
            break;
        }
        case Part::PortsHttpEdit:
        case Part::PortsHttpsEdit:
            if (FieldEdit* edit = edit_of(hit); edit != nullptr) {
                SetFocus(edit->hwnd());
            }
            break;
        case Part::PortsUse:
            use_custom_ports();
            break;
        case Part::PortsSwitch:
            if (hit.row == 0 || hit.row == 1) {
                const bool value = !shown_switch(hit.row);
                pending_switch_[static_cast<std::size_t>(hit.row)] = value;
                send_ports({{hit.row == 0 ? "https" : "shared_https", value}});
                relayout();
            }
            break;
        case Part::PortsTrust:
            trust_ca();
            break;
        case Part::PortsRemove:
            remove_ca();
            break;
        case Part::PortsOpenCert:
            open_certificate();
            break;
        default:
            break;
    }
}

// --- painting ----------------------------------------------------------------------

void SettingsWindow::paint_ports_shapes(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const PortsGeometry& pg = g.ports;
    const float border = std::max(1.0F, pxf(1.0F));
    const RECT column{main_left() - px(4), 0, width_, height_};
    c.clip(column);
    if (!is_empty(g.message)) {
        c.round_rect(g.message, pxf(4), message_error_ ? p.banner_error.fill : p.banner_info.fill);
    }
    if (ports_) {
        const bool can = connected_;
        c.round_rect(pg.web_card, pxf(8), p.card, p.card_border, border);
        const std::size_t shown = shown_port_choice();
        for (std::size_t i = 0; i < pg.choices.size(); ++i) {
            const Hit hit{Part::PortsChoice, -1, static_cast<int>(i)};
            if (hover_ == hit && can) {
                c.round_rect(pg.choices[i], pxf(4), p.row_hover);
            }
            // A radio button beside the label's first line.
            const float cx = static_cast<float>(pg.choices[i].left + px(6)) + pxf(kRadio / 2.0F);
            const float cy = static_cast<float>(pg.choice_labels[i].top + pg.choice_labels[i].bottom) / 2.0F;
            if (i == shown) {
                c.ellipse(cx, cy, pxf(kRadio / 2.0F), p.accent);
                c.ellipse(cx, cy, pxf(3.5F), p.accent_text);
            } else {
                c.ellipse(cx, cy, pxf(kRadio / 2.0F), p.input);
                c.circle(cx, cy, pxf(kRadio / 2.0F) - border / 2.0F, p.switch_off_border, border);
            }
            if (is_focused(hit)) {
                paint_ring(c, pg.choices[i]);
            }
        }
        const auto box = [&](const RECT& r, Part part) {
            if (is_empty(r)) {
                return;
            }
            const Hit hit{part, -1, -1};
            const FieldEdit* edit = edit_of(hit);
            const bool refused = ports_problem_ != Str::Count_;
            c.round_rect(r, pxf(4), p.input, refused ? p.banner_error.text : p.input_border, refused ? pxf(2) : border);
            if (edit != nullptr && edit->focused()) {
                c.round_outline(r, pxf(4), p.accent, pxf(2));
            }
        };
        box(pg.http_box, Part::PortsHttpEdit);
        box(pg.https_box, Part::PortsHttpsEdit);
        if (!is_empty(pg.use)) {
            const Hit hit{Part::PortsUse, -1, -1};
            paint_button(c, pg.use, hover_ == hit, is_focused(hit), can && ports_waiting_ == 0);
        }

        c.round_rect(pg.https_card, pxf(8), p.card, p.card_border, border);
        for (std::size_t i = 0; i < pg.switches.size(); ++i) {
            const Hit hit{Part::PortsSwitch, -1, static_cast<int>(i)};
            if (hover_ == hit && can) {
                c.round_rect(pg.switch_rows[i], pxf(4), p.row_hover);
            }
            const RECT& r = pg.switches[i];
            const float radius = static_cast<float>(height_of(r)) / 2.0F;
            const float knob = pxf(hover_ == hit ? 6.0F : 5.0F);
            const float ky = static_cast<float>(r.top) + radius;
            if (shown_switch(static_cast<int>(i))) {
                c.round_rect(r, radius, p.accent);
                c.ellipse(static_cast<float>(r.right) - pxf(9), ky, knob, p.switch_on_knob);
            } else {
                c.round_rect(r, radius, p.switch_off_fill, p.switch_off_border, border);
                c.ellipse(static_cast<float>(r.left) + pxf(9), ky, knob, p.switch_off_knob);
            }
            if (is_focused(hit)) {
                paint_ring(c, pg.switch_rows[i]);
            }
        }

        c.round_rect(pg.ca_card, pxf(8), p.card, p.card_border, border);
        const bool idle = can && ports_waiting_ == 0;
        for (const auto& [part, r] : {std::pair{Part::PortsTrust, pg.trust}, std::pair{Part::PortsRemove, pg.remove}}) {
            if (!is_empty(r)) {
                const Hit hit{part, -1, -1};
                paint_button(c, r, hover_ == hit && idle, is_focused(hit), idle);
            }
        }
        if (!is_empty(pg.open)) {
            const Hit hit{Part::PortsOpenCert, -1, -1};
            paint_button(c, pg.open, hover_ == hit, is_focused(hit), true);
        }
    }
    if (g.content_h > height_ && height_ > 0) {
        const int thumb = std::max(px(24), height_ * height_ / g.content_h);
        const int top = (height_ - thumb) * scroll_ / (g.content_h - height_);
        c.round_rect({width_ - px(6), top + px(2), width_ - px(2), top + thumb - px(2)}, pxf(2), p.switch_off_border);
    }
    c.unclip();
}

void SettingsWindow::paint_ports_text(Canvas& c) const {
    const Palette& p = palette(dark_);
    const Geometry& g = geometry_;
    const PortsGeometry& pg = g.ports;
    const RECT column{main_left() - px(4), 0, width_, height_};
    c.clip_text(column);
    c.text(g.title, w(text(lang_, Str::SettingsPorts)), fonts_.title.get(), p.text, Align::Start, false);
    c.paragraph(g.hint, w(text(lang_, Str::PortsHint)), fonts_.caption.get(), p.text_subtle);
    if (!is_empty(g.message)) {
        RECT inner = g.message;
        InflateRect(&inner, -px(12), -px(7));
        c.paragraph(inner, w(message_), fonts_.button.get(), message_error_ ? p.banner_error.text : p.banner_info.text);
    }
    if (!is_empty(pg.note)) {
        c.text(pg.note, w(text(lang_, Str::SettingsLoading)), fonts_.button.get(), p.text_subtle, Align::Start, false);
    }
    if (!ports_) {
        c.unclip_text();
        return;
    }
    const bool can = connected_;
    const bool idle = can && ports_waiting_ == 0;

    c.text(pg.web_title, w(text(lang_, Str::PortsWebTitle)), fonts_.body_bold.get(), p.text, Align::Start, false);
    for (std::size_t i = 0; i < pg.choices.size(); ++i) {
        c.text(pg.choice_labels[i], w(port_choice_label(i, lang_)), fonts_.body.get(), p.text, Align::Start, false);
        c.paragraph(pg.choice_notes[i], w(port_choice_note(i, lang_)), fonts_.caption.get(), p.text_subtle);
    }
    if (!is_empty(pg.http_box)) {
        c.text(pg.http_label, w(text(lang_, Str::PortsHttpLabel)), fonts_.caption.get(), p.text_subtle, Align::Start,
               false);
        c.text(pg.https_label, w(text(lang_, Str::PortsHttpsLabel)), fonts_.caption.get(), p.text_subtle,
               Align::Start, false);
        c.text(pg.use, w(text(lang_, ports_waiting_ > 0 ? Str::PortsChanging : Str::PortsUse)), fonts_.button.get(),
               idle ? p.text : p.button_disabled_text, Align::Center, false);
    }
    if (!is_empty(pg.problem)) {
        c.text(pg.problem, w(text(lang_, ports_problem_)), fonts_.caption.get(), p.banner_error.text, Align::Start,
               false);
    }
    c.paragraph(pg.in_use, w(ports_in_use_text(*ports_, lang_)), fonts_.caption.get(), p.text_subtle);
    for (const auto& [r, line] : pg.taken) {
        c.paragraph(r, w(line), fonts_.caption.get(), p.asleep.text);
    }

    c.text(pg.https_title, w(text(lang_, Str::PortsHttpsTitle)), fonts_.body_bold.get(), p.text, Align::Start, false);
    const std::array<Str, 2> labels = {Str::PortsHttpsProjects, Str::PortsHttpsShared};
    const std::array<std::string, 2> notes = {
        fill(lang_, Str::PortsHttpsProjectsNote, {"https://<name>.localhost"}),
        fill(lang_, Str::PortsHttpsSharedNote, {"https://localhost/<folder>/", "<folder>.localhost"})};
    for (std::size_t i = 0; i < 2; ++i) {
        c.text(pg.switch_labels[i], w(text(lang_, labels[i])), fonts_.body.get(), p.text, Align::Start, false);
        c.paragraph(pg.switch_notes[i], w(notes[i]), fonts_.caption.get(), p.text_subtle);
    }
    if (!is_empty(pg.needs_trust)) {
        c.paragraph(pg.needs_trust, w(text(lang_, Str::PortsNeedsTrust)), fonts_.caption.get(), p.asleep.text);
    }

    c.text(pg.ca_title, w(text(lang_, Str::PortsCaTitle)), fonts_.body_bold.get(), p.text, Align::Start, false);
    c.paragraph(pg.ca_status, w(ca_status_text(*ports_, lang_)), fonts_.button.get(),
                ports_->ca == CaState::Damaged ? p.banner_error.text : p.text);
    if (!is_empty(pg.trust)) {
        c.text(pg.trust, w(text(lang_, Str::PortsTrust)), fonts_.button.get(), idle ? p.text : p.button_disabled_text,
               Align::Center, false);
    }
    if (!is_empty(pg.remove)) {
        c.text(pg.remove, w(text(lang_, Str::SettingsRemove)), fonts_.button.get(),
               idle ? p.text : p.button_disabled_text, Align::Center, false);
    }
    if (!is_empty(pg.open)) {
        c.text(pg.open, w(text(lang_, Str::PortsOpenCert)), fonts_.button.get(), p.text, Align::Center, false);
    }
    c.paragraph(pg.ca_safe, w(text(lang_, Str::PortsCaSafe)), fonts_.caption.get(), p.text_subtle);
    c.unclip_text();
}

}  // namespace floserver::ui

// New project (D9): the approved mockup's three steps (template, name and
// folder, versions) and then the commands as they run. It's a page over the
// list and the panel rather than a separate window; the top bar and footer
// stay. Everything is done by the service (projects.create); this only
// gathers the choices and shows the job.

#include "main_window.h"

#include "main_window_layout.h"
#include "model/theme.h"
#include "model/view_model.h"

#include <shlobj.h>

#include <algorithm>
#include <utility>

namespace floserver::ui {

using namespace layout;

namespace {

constexpr int kWizardPad = 28;
constexpr int kCardH = 140;
constexpr int kCardChipH = 18;
constexpr int kCardGap = 12;
constexpr int kWizardFooterH = 64;
constexpr int kButtonH = 32;
constexpr int kLogLineH = 17;
constexpr UINT_PTR kNameSubclass = 2;

std::wstring w(std::string_view s) { return win::widen(s); }

// Documents\FloServer: where new projects go when the service has no
// projects folder of its own yet.
std::string default_parent() {
    PWSTR documents = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents != nullptr) {
        out = win::narrow(std::wstring(documents) + L"\\FloServer");
    }
    CoTaskMemFree(documents);
    return out;
}

}  // namespace

bool MainWindow::wizard_open() const { return wizard_.open; }

const TemplateCard* MainWindow::wizard_card() const {
    if (wizard_.card < snap_.templates.size()) {
        return &snap_.templates[wizard_.card];
    }
    return nullptr;
}

RECT MainWindow::wizard_rect() const {
    const RECT b = banner_rect();
    const int top = b.bottom > 0 ? b.bottom + px(8) : px(kTopBarH);
    return {px(kPad), top, width_ - px(kPad), std::max(top, height_ - px(kFooterH) - px(kGap))};
}

void MainWindow::open_wizard() {
    if (!worker_ || snap_.link != Link::Connected) {
        return;
    }
    if (wizard_.open && wizard_.step == 3 && snap_.create.state == "running") {
        return;  // a project is being made; its page stays
    }
    wizard_ = {};
    wizard_.open = true;
    if (!snap_.templates_loaded || !snap_.templates_error.empty() || snap_.templates.empty()) {
        snap_.templates_loaded = false;
        worker_->load_templates();
    }
    if (name_edit_ == nullptr) {
        name_edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_LOWERCASE, 0, 0, 0, 0,
                                     hwnd_, nullptr, instance_, nullptr);
        SetWindowSubclass(name_edit_, name_proc, kNameSubclass, reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(name_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body.get()), TRUE);
    }
    ShowWindow(name_edit_, SW_HIDE);
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::close_wizard() {
    const bool made = wizard_.step == 3 && snap_.create.state == "done" && !snap_.create.project.empty();
    const std::string project = made ? snap_.create.project : std::string();
    wizard_ = {};
    if (name_edit_ != nullptr) {
        ShowWindow(name_edit_, SW_HIDE);
    }
    SetFocus(hwnd_);
    if (!project.empty()) {
        if (const int row = visible_index(project); row >= 0) {
            focus_ = row;
            ensure_visible(row);
        }
        select(project);
    }
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::string MainWindow::wizard_name() const {
    if (name_edit_ == nullptr) {
        return {};
    }
    const int n = GetWindowTextLengthW(name_edit_);
    std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(name_edit_, text.data(), n + 1);
    text.resize(static_cast<std::size_t>(n));
    return win::narrow(text);
}

void MainWindow::wizard_next() {
    const TemplateCard* card = wizard_card();
    switch (wizard_.step) {
        case 0:
            if (card == nullptr) {
                return;
            }
            wizard_.step = 1;
            if (wizard_.parent.empty()) {
                wizard_.parent = !snap_.projects_dir.empty() ? snap_.projects_dir : default_parent();
            }
            SetWindowTextW(name_edit_, w(free_project_name(card->id, snap_.rows)).c_str());
            ShowWindow(name_edit_, SW_SHOW);
            layout();
            SetFocus(name_edit_);
            SendMessageW(name_edit_, EM_SETSEL, 0, -1);
            break;
        case 1: {
            const std::string name = wizard_name();
            if (!is_project_name(name) || wizard_.parent.empty()) {
                show_notice(std::string(text(lang_, Str::WizardNameRule)), true);
                SetFocus(name_edit_);
                return;
            }
            wizard_.name = name;
            wizard_.fields = template_fields(*card, snap_.installed, lang_);
            wizard_.step = 2;
            ShowWindow(name_edit_, SW_HIDE);
            SetFocus(hwnd_);
            break;
        }
        case 2: {
            // The parent folder is made when it's the default one and isn't there yet.
            const std::wstring parent = w(wizard_.parent);
            if (GetFileAttributesW(parent.c_str()) == INVALID_FILE_ATTRIBUTES) {
                SHCreateDirectoryExW(nullptr, parent.c_str(), nullptr);
            }
            std::map<std::string, std::optional<std::string>> versions;
            for (std::size_t i = 0; i < wizard_.fields.size(); ++i) {
                const VersionField& f = wizard_.fields[i];
                const std::size_t pick = wizard_.picks.contains(f.key) ? wizard_.picks.at(f.key) : f.current;
                if (pick < f.choices.size()) {
                    versions[f.key] = f.choices[pick].value;
                }
            }
            snap_.create = {};
            wizard_.step = 3;
            change_error_.clear();
            worker_->create_project(card->id, wizard_.name, wizard_.parent, versions);
            break;
        }
        default:
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::wizard_back() {
    if (wizard_.step == 3) {
        if (snap_.create.state == "running") {
            return;
        }
        wizard_.step = 2;
    } else if (wizard_.step == 2) {
        wizard_.step = 1;
        ShowWindow(name_edit_, SW_SHOW);
        layout();
        SetFocus(name_edit_);
    } else if (wizard_.step == 1) {
        wizard_.name = wizard_name();
        wizard_.step = 0;
        ShowWindow(name_edit_, SW_HIDE);
        SetFocus(hwnd_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::wizard_browse() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
        dialog == nullptr) {
        return;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    const std::wstring title = w(text(lang_, Str::WizardFolder));
    dialog->SetTitle(title.c_str());
    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR chosen = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) && chosen != nullptr) {
                wizard_.parent = win::narrow(chosen);
                CoTaskMemFree(chosen);
            }
            item->Release();
        }
    }
    dialog->Release();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::wizard_field_menu(std::size_t index) {
    if (index >= wizard_.fields.size() || index >= wizard_geometry_.fields.size()) {
        return;
    }
    const VersionField field = wizard_.fields[index];
    const win::UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    const std::size_t shown = wizard_.picks.contains(field.key) ? wizard_.picks.at(field.key) : field.current;
    std::vector<std::wstring> labels;
    labels.reserve(field.choices.size());
    for (std::size_t i = 0; i < field.choices.size(); ++i) {
        labels.push_back(w(field.choices[i].label));
        AppendMenuW(menu.get(), MF_STRING | (i == shown ? UINT{MF_CHECKED} : 0U), 100 + static_cast<UINT>(i),
                    labels.back().c_str());
    }
    const RECT box = wizard_geometry_.fields[index].second;
    POINT at{box.left, box.bottom};
    ClientToScreen(hwnd_, &at);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTBUTTON | TPM_TOPALIGN;
    flags |= rtl_ ? UINT{TPM_RIGHTALIGN | TPM_LAYOUTRTL} : UINT{TPM_LEFTALIGN};
    const auto command = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, at.x, at.y, hwnd_, nullptr));
    if (command >= 100 && command - 100 < field.choices.size() && wizard_.open && wizard_.step == 2) {
        wizard_.picks[field.key] = command - 100;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::on_wizard_click(const Hit& hit) {
    switch (hit.part) {
        case Part::WizCard:
            if (hit.row >= 0) {
                wizard_.card = static_cast<std::size_t>(hit.row);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        case Part::WizField:
            if (hit.row >= 0) {
                wizard_field_menu(static_cast<std::size_t>(hit.row));
            }
            break;
        case Part::WizBrowse:
            wizard_browse();
            break;
        case Part::WizAddFolder:
            close_wizard();
            add_existing_folder();
            break;
        case Part::WizCancel:
            close_wizard();
            break;
        case Part::WizBack:
            wizard_back();
            break;
        case Part::WizNext:
            if (wizard_.step == 3) {
                close_wizard();  // "Open project" or "Close"
            } else {
                wizard_next();
            }
            break;
        default:
            break;
    }
}

void MainWindow::on_wizard_key(WPARAM key) {
    const int count = static_cast<int>(snap_.templates.size());
    switch (key) {
        case VK_ESCAPE:
            if (!(wizard_.step == 3 && snap_.create.state == "running")) {
                close_wizard();
            }
            break;
        case VK_RETURN:
            if (wizard_.step == 3) {
                if (snap_.create.state != "running") {
                    close_wizard();
                }
            } else {
                wizard_next();
            }
            break;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if (wizard_.step == 0 && count > 0) {
                int i = static_cast<int>(wizard_.card);
                const bool forward = key == VK_DOWN || (key == VK_RIGHT) != rtl_;
                const int stride = key == VK_UP || key == VK_DOWN ? 4 : 1;
                i = std::clamp(i + (forward ? stride : -stride), 0, count - 1);
                wizard_.card = static_cast<std::size_t>(i);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        case VK_BACK:
            wizard_back();
            break;
        default:
            break;
    }
}

LRESULT CALLBACK MainWindow::name_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR self_ptr) {
    auto* self = win::int_to_ptr<MainWindow*>(self_ptr);
    switch (msg) {
        case WM_KEYDOWN:
            if (wparam == VK_RETURN) {
                self->wizard_next();
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                self->close_wizard();
                return 0;
            }
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
                return 0;  // no beep
            }
            // Only what a project name may hold (ES_LOWERCASE makes letters small).
            if (wparam >= 0x20 && !((wparam >= L'a' && wparam <= L'z') || (wparam >= L'A' && wparam <= L'Z') ||
                                     (wparam >= L'0' && wparam <= L'9') || wparam == L'-')) {
                MessageBeep(MB_OK);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, name_proc, id);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

// --- geometry ----------------------------------------------------------------

MainWindow::WizardGeometry MainWindow::wizard_geometry(HDC dc) const {
    WizardGeometry g;
    g.area = wizard_rect();
    const int x0 = g.area.left + px(kWizardPad);
    const int x1 = g.area.right - px(kWizardPad);
    int y = g.area.top + px(16);
    g.steps = {x0, y, x1, y + px(20)};
    y += px(20) + px(12);
    g.title = {x0, y, x1, y + px(30)};
    y += px(30) + px(16);
    g.footer = {g.area.left + 1, g.area.bottom - px(kWizardFooterH), g.area.right - 1, g.area.bottom - 1};
    const int footer_mid = (g.footer.top + g.footer.bottom) / 2;
    const auto button = [&](Str label, int right, bool bold) {
        const int bw = Canvas::measure(dc, w(text(lang_, label)), (bold ? fonts_.button_bold : fonts_.button).get(), rtl_) + px(28);
        return RECT{right - bw, footer_mid - px(kButtonH) / 2, right, footer_mid + px(kButtonH) / 2};
    };
    const bool running = wizard_.step == 3 && snap_.create.state == "running";
    const bool done = wizard_.step == 3 && snap_.create.state == "done";
    const Str next_label = wizard_.step == 2 ? Str::WizardCreate
                           : done           ? Str::WizardOpenProject
                           : wizard_.step == 3 ? Str::WizardClose
                                               : Str::WizardNext;
    if (!running) {
        g.next = button(next_label, x1, true);
        g.next_label = next_label;
    }
    int right = running ? x1 : g.next.left - px(8);
    if (wizard_.step > 0 && !running && !done) {
        g.back = button(Str::WizardBack, right, false);
        right = g.back.left - px(8);
    }
    if (wizard_.step < 3) {
        g.cancel = button(Str::WizardCancel, right, false);
    }
    if (wizard_.step == 0) {
        const int bw = Canvas::measure(dc, w(text(lang_, Str::AddExistingFolder)), fonts_.button.get(), rtl_) + px(28);
        g.add_folder = {x0, footer_mid - px(kButtonH) / 2, x0 + bw, footer_mid + px(kButtonH) / 2};
    }

    const int bottom = g.footer.top - px(12);
    switch (wizard_.step) {
        case 0: {
            const int cols = std::max(1, std::min(4, (x1 - x0 + px(kCardGap)) / (px(180) + px(kCardGap))));
            const int card_w = (x1 - x0 - (cols - 1) * px(kCardGap)) / cols;
            for (std::size_t i = 0; i < snap_.templates.size(); ++i) {
                const int col = static_cast<int>(i) % cols;
                const int line = static_cast<int>(i) / cols;
                const int cx = x0 + col * (card_w + px(kCardGap));
                const int cy = y + line * (px(kCardH) + px(kCardGap));
                g.cards.push_back({cx, cy, cx + card_w, cy + px(kCardH)});
            }
            g.message = {x0, y, x1, bottom};
            break;
        }
        case 1: {
            g.name_label = {x0, y, x1, y + px(18)};
            y += px(22);
            g.name_box = {x0, y, std::min(x1, x0 + px(360)), y + px(kButtonH)};
            y += px(kButtonH) + px(6);
            g.name_hint = {x0, y, x1, y + px(18)};
            y += px(18) + px(20);
            g.folder_label = {x0, y, x1, y + px(18)};
            y += px(22);
            const int bw = Canvas::measure(dc, w(text(lang_, Str::WizardBrowse)), fonts_.button.get(), rtl_) + px(28);
            g.browse = {x1 - bw, y, x1, y + px(kButtonH)};
            g.folder_box = {x0, y, g.browse.left - px(8), y + px(kButtonH)};
            y += px(kButtonH) + px(6);
            g.folder_hint = {x0, y, x1, y + px(36)};
            break;
        }
        case 2: {
            const int col_w = std::min(px(300), (x1 - x0 - px(16)) / 2);
            for (std::size_t i = 0; i < wizard_.fields.size(); ++i) {
                const int col = static_cast<int>(i % 2);
                const int line = static_cast<int>(i / 2);
                const int cx = x0 + col * (col_w + px(16));
                const int cy = y + line * (px(16) + px(4) + px(kButtonH) + px(14));
                g.fields.emplace_back(RECT{cx, cy, cx + col_w, cy + px(16)},
                                      RECT{cx, cy + px(20), cx + col_w, cy + px(20) + px(kButtonH)});
            }
            const int lines = static_cast<int>((wizard_.fields.size() + 1) / 2);
            const int after = y + lines * (px(16) + px(4) + px(kButtonH) + px(14));
            g.message = {x0, after, x1, after + px(40)};
            break;
        }
        default: {
            g.message = {x0, y, x1, y + px(20)};
            y += px(28);
            g.log_box = {x0, y, x1, std::max(y, bottom)};
            break;
        }
    }
    return g;
}

MainWindow::Hit MainWindow::wizard_hit_test(int x, int y) const {
    const WizardGeometry& g = wizard_geometry_;
    for (std::size_t i = 0; i < g.cards.size(); ++i) {
        if (contains(g.cards[i], x, y)) {
            return {Part::WizCard, static_cast<int>(i), {}};
        }
    }
    for (std::size_t i = 0; i < g.fields.size(); ++i) {
        if (contains(g.fields[i].second, x, y)) {
            return {Part::WizField, static_cast<int>(i), {}};
        }
    }
    const std::pair<const RECT*, Part> buttons[] = {{&g.browse, Part::WizBrowse}, {&g.add_folder, Part::WizAddFolder},
                                                    {&g.cancel, Part::WizCancel}, {&g.back, Part::WizBack},
                                                    {&g.next, Part::WizNext}};
    for (const auto& [rect, part] : buttons) {
        if (contains(*rect, x, y)) {
            return {part, -1, {}};
        }
    }
    return {};
}

// A card's chips along its bottom: as many as fit.
std::vector<RECT> MainWindow::card_chips(HDC dc, const RECT& card, const TemplateCard& t) const {
    std::vector<RECT> out;
    int x = card.left + px(14);
    const int y = card.bottom - px(12) - px(kCardChipH);
    for (const std::string& chip : t.chips) {
        const int cw = Canvas::measure(dc, w(chip), fonts_.caption.get(), false) + px(14);
        if (x + cw > card.right - px(14)) {
            break;
        }
        out.push_back({x, y, x + cw, y + px(kCardChipH)});
        x += cw + px(4);
    }
    return out;
}

// --- painting ----------------------------------------------------------------

void MainWindow::paint_wizard(Canvas& c, HDC dc, bool text_pass) const {
    const WizardGeometry& g = wizard_geometry_;
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    const auto hovered = [&](Part part, int index) { return hover_.part == part && hover_.row == index; };
    if (!text_pass) {
        c.round_rect(g.area, pxf(8), p.card, p.card_border, border);
        c.fill({g.footer.left, g.footer.top, g.footer.right, g.footer.top + static_cast<int>(border)}, p.divider);
        for (std::size_t i = 0; i < g.cards.size(); ++i) {
            const int index = static_cast<int>(i);
            const bool chosen = i == wizard_.card;
            const RECT& r = g.cards[i];
            if (chosen) {
                c.round_rect(r, pxf(8), p.selected, p.accent, pxf(2));
            } else {
                c.round_rect(r, pxf(8), hovered(Part::WizCard, index) ? p.row_hover : p.card, p.card_border, border);
            }
            const TemplateCard& card = snap_.templates[i];
            const RECT mark{r.left + px(14), r.top + px(14), r.left + px(46), r.top + px(46)};
            c.round_rect(mark, pxf(6), card.mark_bg);
            for (const RECT& chip : card_chips(dc, r, card)) {
                c.round_rect(chip, pxf(9), p.chip);
            }
        }
        for (std::size_t i = 0; i < g.fields.size(); ++i) {
            const RECT& box = g.fields[i].second;
            c.round_rect(box, pxf(4), hovered(Part::WizField, static_cast<int>(i)) ? p.row_hover : p.input, p.input_border,
                         border);
            const float vx = static_cast<float>(box.right) - pxf(14);
            const float vy = static_cast<float>(box.top + box.bottom) / 2.0F;
            c.line(vx - pxf(4), vy - pxf(2), vx, vy + pxf(2), p.text_subtle, pxf(1.3F));
            c.line(vx, vy + pxf(2), vx + pxf(4), vy - pxf(2), p.text_subtle, pxf(1.3F));
        }
        if (g.name_box.right > g.name_box.left) {
            c.round_rect(g.name_box, pxf(4), p.input, p.input_border, border);
            c.round_rect(g.folder_box, pxf(4), p.window, p.input_border, border);
        }
        for (const auto& [rect, part] : {std::pair{g.browse, Part::WizBrowse}, std::pair{g.add_folder, Part::WizAddFolder},
                                         std::pair{g.cancel, Part::WizCancel}, std::pair{g.back, Part::WizBack}}) {
            if (rect.right > rect.left) {
                paint_button(c, rect, hovered(part, -1), false);
            }
        }
        if (g.next.right > g.next.left) {
            c.round_rect(g.next, pxf(4), p.accent);
        }
        if (height_of(g.log_box) > 0) {
            c.round_rect(g.log_box, pxf(6), p.log);
        }
        return;
    }

    c.clip_text(g.area);
    // The steps, the current one in the accent colour.
    {
        int x = g.steps.left;
        for (const auto& [label, index] : {std::pair{Str::WizardStepTemplate, 0}, std::pair{Str::WizardStepName, 1},
                                           std::pair{Str::WizardStepVersions, 2}}) {
            const std::wstring s = w(text(lang_, label));
            const bool current = std::min(wizard_.step, 2) == index;
            const int sw = Canvas::measure(dc, s, (current ? fonts_.button_bold : fonts_.button).get(), rtl_) + px(4);
            c.text({x, g.steps.top, x + sw, g.steps.bottom}, s, current ? fonts_.button_bold.get() : fonts_.button.get(),
                   current ? p.accent : p.text_subtle, Align::Start, false);
            x += sw + px(16);
        }
    }
    std::string title;
    switch (wizard_.step) {
        case 0:
            title = std::string(text(lang_, Str::WizardWhat));
            break;
        case 1:
            title = std::string(text(lang_, Str::WizardNameTitle));
            break;
        case 2:
            title = std::string(text(lang_, Str::WizardVersionsTitle));
            break;
        default:
            title = snap_.create.state == "done"     ? fill(lang_, Str::WizardDone, {wizard_.name})
                    : snap_.create.state == "failed" ? fill(lang_, Str::WizardFailed, {""})
                                                     : fill(lang_, Str::WizardMaking, {wizard_.name});
            break;
    }
    if (wizard_.step == 3 && snap_.create.state == "failed") {
        title = std::string(text(lang_, Str::WizardFailed));
        title = title.substr(0, title.find("{0}"));
        while (!title.empty() && (title.back() == ' ' || title.back() == ':')) {
            title.pop_back();
        }
    }
    c.text(g.title, w(title), fonts_.title.get(), p.text, Align::Start, false);

    switch (wizard_.step) {
        case 0:
            if (snap_.templates.empty()) {
                const std::string message = !snap_.templates_loaded ? std::string(text(lang_, Str::WizardLoading))
                                            : !snap_.templates_error.empty()
                                                ? fill_pattern(text(lang_, Str::WizardNoTemplates), false,
                                                               {rtl_ ? isolate_ltr(snap_.templates_error) : snap_.templates_error})
                                                : fill_pattern(text(lang_, Str::WizardNoTemplates), false, {"-"});
                c.paragraph(g.message, w(message), fonts_.body.get(), p.text_subtle);
            }
            for (std::size_t i = 0; i < g.cards.size() && i < snap_.templates.size(); ++i) {
                const RECT& r = g.cards[i];
                const TemplateCard& t = snap_.templates[i];
                const RECT mark{r.left + px(14), r.top + px(14), r.left + px(46), r.top + px(46)};
                c.text(mark, w(t.mark), fonts_.small_bold.get(), t.mark_fg, Align::Center, true);
                c.text({r.left + px(14), r.top + px(52), r.right - px(14), r.top + px(72)}, w(t.title),
                       fonts_.body_bold.get(), p.text, Align::Start, false);
                c.paragraph({r.left + px(14), r.top + px(74), r.right - px(14), r.bottom - px(32)}, w(t.description),
                            fonts_.caption.get(), p.text_subtle);
                const std::vector<RECT> chips = card_chips(dc, r, t);
                for (std::size_t k = 0; k < chips.size(); ++k) {
                    c.text(chips[k], w(t.chips[k]), fonts_.caption.get(), p.chip_text, Align::Center, true);
                }
            }
            break;
        case 1: {
            c.text(g.name_label, w(text(lang_, Str::WizardName)), fonts_.button_bold.get(), p.text, Align::Start, false);
            const std::string name = wizard_name();
            const std::string hint = is_project_name(name) ? fill(lang_, Str::WizardAddress, {"https://" + name + ".localhost"})
                                                           : std::string(text(lang_, Str::WizardNameRule));
            c.text(g.name_hint, w(hint), fonts_.caption.get(), is_project_name(name) ? p.text_subtle : p.problem.text,
                   Align::Start, false);
            c.text(g.folder_label, w(text(lang_, Str::WizardFolder)), fonts_.button_bold.get(), p.text, Align::Start,
                   false);
            RECT inner = g.folder_box;
            InflateRect(&inner, -px(10), 0);
            c.text(inner, w(wizard_.parent), fonts_.button.get(), p.text, Align::Start, true);
            c.text(g.browse, w(text(lang_, Str::WizardBrowse)), fonts_.button.get(), p.text, Align::Center, false);
            std::string full = wizard_.parent;
            if (!full.empty() && full.back() != '\\' && full.back() != '/') {
                full += '\\';
            }
            full += is_project_name(name) ? name : std::string("…");
            c.paragraph(g.folder_hint, w(fill(lang_, Str::WizardMadeIn, {full})), fonts_.caption.get(), p.text_subtle);
            break;
        }
        case 2:
            for (std::size_t i = 0; i < g.fields.size() && i < wizard_.fields.size(); ++i) {
                const VersionField& f = wizard_.fields[i];
                c.text(g.fields[i].first, w(f.label), fonts_.caption.get(), p.text_subtle, Align::Start, false);
                RECT inner = g.fields[i].second;
                inner.left += px(8);
                inner.right -= px(26);
                const std::size_t pick = wizard_.picks.contains(f.key) ? wizard_.picks.at(f.key) : f.current;
                if (pick < f.choices.size()) {
                    c.text(inner, w(f.choices[pick].label), fonts_.button.get(), p.text, Align::Start, false);
                }
            }
            c.paragraph(g.message, w(text(lang_, Str::WizardDownloads)), fonts_.caption.get(), p.text_subtle);
            break;
        default: {
            std::string line = snap_.create.state == "failed" ? snap_.create.error : snap_.create.step;
            if (rtl_) {
                line = isolate_ltr(line);
            }
            c.text(g.message, w(line), fonts_.button.get(), snap_.create.state == "failed" ? p.problem.text : p.text,
                   Align::Start, false);
            if (height_of(g.log_box) > px(kLogLineH)) {
                RECT inner = g.log_box;
                InflateRect(&inner, -px(10), -px(8));
                const int fit = std::max(1, height_of(inner) / px(kLogLineH));
                const std::vector<std::string>& log = snap_.create.log;
                const std::size_t first = log.size() > static_cast<std::size_t>(fit) ? log.size() - static_cast<std::size_t>(fit) : 0;
                c.clip_text(inner);
                int ly = inner.top;
                for (std::size_t i = first; i < log.size(); ++i) {
                    const bool command = log[i].starts_with("$ ");
                    c.text({inner.left, ly, inner.right, ly + px(kLogLineH)}, w(log[i]), fonts_.mono.get(),
                           command ? p.log_text : p.log_subtle, rtl_ ? Align::End : Align::Start, true);
                    ly += px(kLogLineH);
                }
                c.clip_text(g.area);
            }
            break;
        }
    }

    // The footer buttons.
    if (g.add_folder.right > g.add_folder.left) {
        c.text(g.add_folder, w(text(lang_, Str::AddExistingFolder)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (g.cancel.right > g.cancel.left) {
        c.text(g.cancel, w(text(lang_, Str::WizardCancel)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (g.back.right > g.back.left) {
        c.text(g.back, w(text(lang_, Str::WizardBack)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (g.next.right > g.next.left) {
        c.text(g.next, w(text(lang_, g.next_label)), fonts_.button_bold.get(), p.accent_text, Align::Center, false);
    }
    c.unclip_text();
}

}  // namespace floserver::ui

// App updates on the main screen (Q5, mockups approved 5 Nov): the banner
// above the project list while the service downloads a newer FloServer
// (Cancel), and once it's downloaded and checked (Later, Update now), with
// What's new when the list gives one.
//
// Update now: Windows Installer replaces FloServer.exe, so this window
// mustn't be the one installing. FloServer.exe (and the Visual C++ DLLs
// beside it) is copied to a new folder, %TEMP%\FloServer-update-<random>\,
// that copy is started with --apply-update <the MSI> (apply_update.cpp), and
// this window quits without stopping the service: the copy stops it,
// installs, and opens the new FloServer.

#include "main_window.h"

#include "main_window_layout.h"
#include "model/general_page.h"
#include "model/main_access.h"
#include "model/settings_access.h"
#include "model/theme.h"

#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace floserver::ui {

using namespace layout;
namespace core = floserver::core;
namespace fs = std::filesystem;

namespace {

constexpr int kUpdateBannerH = 76;
constexpr int kUpdatePad = 16;

std::wstring w(std::string_view s) { return win::widen(s); }

LocalTime local_now() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return {st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute};
}

std::string last_error() { return core::error_text({static_cast<int>(GetLastError()), std::system_category()}); }

bool is_reparse_point(const fs::path& p) {
    const DWORD attributes = GetFileAttributesW(p.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// Earlier updates' copies (FloServer-update-<hex>), best effort: their files,
// never going through a link or junction, then the folder. One still running
// keeps its files, and is tried again next time.
void remove_old_updaters(const fs::path& temp) {
    std::error_code ec;
    for (fs::directory_iterator it(temp, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path folder = it->path();
        if (!is_updater_folder(core::path_to_utf8(folder.filename())) || is_reparse_point(folder)) {
            continue;
        }
        std::error_code inner;
        for (fs::directory_iterator f(folder, inner), fend; !inner && f != fend; f.increment(inner)) {
            const DWORD attributes = GetFileAttributesW(f->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                DeleteFileW(f->path().c_str());  // a file link goes as the link
            }
        }
        RemoveDirectoryW(folder.c_str());  // only when it's empty now
    }
}

// One file into the new folder, checked by its size afterwards.
bool copy_checked(const fs::path& from, const fs::path& to, std::string& error) {
    if (CopyFileW(from.c_str(), to.c_str(), TRUE) == FALSE) {
        error = core::path_to_utf8(to) + ": " + last_error();
        return false;
    }
    std::error_code a;
    std::error_code b;
    const auto want = fs::file_size(from, a);
    const auto got = fs::file_size(to, b);
    if (a || b || want != got) {
        error = core::path_to_utf8(to) + ": the copy isn't the size of " + core::path_to_utf8(from);
        return false;
    }
    return true;
}

// The copy that installs the update, with the DLLs FloServer.exe needs
// beside it, in a new folder of its own under %TEMP%: made here (never one
// that was there already, nor a link), and every file checked.
bool copy_updater(const fs::path& exe, const fs::path& temp, fs::path& copy, std::string& error) {
    if (is_reparse_point(temp)) {
        error = core::path_to_utf8(temp) + ": not a plain folder";
        return false;
    }
    remove_old_updaters(temp);
    std::random_device random;
    fs::path folder;
    for (int attempt = 0; attempt < 8 && folder.empty(); ++attempt) {
        const std::uint64_t value = (std::uint64_t{random()} << 32U) ^ random();
        const fs::path candidate = temp / core::path_from_utf8(updater_folder_name(value));
        if (CreateDirectoryW(candidate.c_str(), nullptr) != FALSE) {
            folder = candidate;
        } else if (GetLastError() != ERROR_ALREADY_EXISTS) {
            error = core::path_to_utf8(candidate) + ": " + last_error();
            return false;
        }
    }
    if (folder.empty() || is_reparse_point(folder)) {
        error = core::path_to_utf8(temp) + ": no new folder for the update";
        return false;
    }
    copy = folder / exe.filename();
    if (!copy_checked(exe, copy, error)) {
        return false;
    }
    std::error_code ec;
    for (fs::directory_iterator it(exe.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ignored;
        if (it->path().extension() == L".dll" && it->is_regular_file(ignored) &&
            !copy_checked(it->path(), folder / it->path().filename(), error)) {
            return false;
        }
    }
    if (ec) {
        error = core::path_to_utf8(exe.parent_path()) + ": " + core::error_text(ec);
        return false;
    }
    return true;
}

}  // namespace

// --- what shows ---------------------------------------------------------------------

UpdateBanner MainWindow::update_shown() const {
    if (first_run_open() || wizard_open() || quitting_ || !shows_rows()) {
        return UpdateBanner::None;
    }
    return update_banner(snap_.updates, update_later_, day_text(local_now()));
}

RECT MainWindow::update_banner_rect() const {
    if (update_shown() == UpdateBanner::None) {
        return {};
    }
    const int top = px(kTopBarH);
    return {px(kPad), top, width_ - px(kPad), top + px(kUpdateBannerH)};
}

int MainWindow::content_top() const {
    const RECT b = banner_rect();
    if (b.bottom > 0) {
        return b.bottom + px(8);
    }
    const RECT u = update_banner_rect();
    return u.bottom > 0 ? u.bottom + px(kGap) : px(kTopBarH);
}

MainWindow::UpdateGeometry MainWindow::update_geometry(HDC dc) const {
    UpdateGeometry g;
    g.kind = update_shown();
    g.banner = update_banner_rect();
    if (g.kind == UpdateBanner::None || width_of(g.banner) <= 0) {
        return {};
    }
    const auto measure = [&](Str key, HFONT font) { return Canvas::measure(dc, w(text(lang_, key)), font, rtl_); };
    const RECT& b = g.banner;
    const int mid = (b.top + b.bottom) / 2;
    const int button_top = mid - px(kControlH) / 2;
    // From the end: the buttons, then What's new, then the words take what's left.
    int end = b.right - px(kUpdatePad);
    const auto button = [&](RECT& r, Str key, HFONT font) {
        const int bw = measure(key, font) + px(24);
        r = {end - bw, button_top, end, button_top + px(kControlH)};
        end = r.left - px(8);
    };
    if (g.kind == UpdateBanner::Ready) {
        button(g.now, Str::UpdateNow, fonts_.button_bold.get());
        button(g.later, Str::UpdateLater, fonts_.button.get());
    } else {
        button(g.cancel, Str::UpdateCancel, fonts_.button.get());
    }
    g.icon = {b.left + px(kUpdatePad), b.top + px(14), b.left + px(kUpdatePad) + px(20), b.top + px(34)};
    const int text_start = g.icon.right + px(12);
    if (!snap_.updates.notes_url.empty()) {
        const int nw = measure(Str::UpdateWhatsNew, fonts_.button.get()) + px(12);
        // Only while the words keep room; it's also in Settings.
        if (end - nw - px(12) - text_start >= px(kMinTextW)) {
            g.notes = {end - nw, button_top, end, button_top + px(kControlH)};
            end = g.notes.left - px(4);
        }
    }
    const int text_end = std::max(text_start, end - px(12));
    g.title = {text_start, b.top + px(14), text_end, b.top + px(34)};
    g.line = {text_start, g.title.bottom + px(2), text_end, g.title.bottom + px(20)};
    if (g.kind == UpdateBanner::Downloading) {
        g.bar = {text_start, g.line.bottom + px(6), text_end, g.line.bottom + px(12)};
    } else {
        // No bar: the two lines sit in the middle.
        const int shift = (b.bottom - px(14) - g.line.bottom) / 2;
        OffsetRect(&g.title, 0, shift);
        OffsetRect(&g.line, 0, shift);
        OffsetRect(&g.icon, 0, shift);
    }
    return g;
}

UpdateBannerText MainWindow::update_words() const {
    const auto left = snap_.updates.state == UpdateState::Downloading ? update_rate_.seconds_left(snap_.updates.size)
                                                                       : std::nullopt;
    return update_banner_text(update_geometry_.kind, snap_.updates, left, lang_);
}

std::vector<MainWindow::Part> MainWindow::update_focus_order() const {
    std::vector<Part> order;
    const UpdateGeometry& g = update_geometry_;
    if (width_of(g.notes) > 0) {
        order.push_back(Part::UpdateNotes);
    }
    if (g.kind == UpdateBanner::Downloading) {
        order.push_back(Part::UpdateCancel);
    } else if (g.kind == UpdateBanner::Ready) {
        order.push_back(Part::UpdateLater);
        order.push_back(Part::UpdateNow);
    }
    return order;
}

MainWindow::Hit MainWindow::update_hit_test(int x, int y) const {
    const UpdateGeometry& g = update_geometry_;
    for (const auto& [part, r] : {std::pair{Part::UpdateNotes, g.notes}, std::pair{Part::UpdateCancel, g.cancel},
                                  std::pair{Part::UpdateLater, g.later}, std::pair{Part::UpdateNow, g.now}}) {
        if (contains(r, x, y)) {
            return {part, -1, {}};
        }
    }
    return {};
}

// --- what it does -------------------------------------------------------------------

void MainWindow::on_update_snapshot(const UpdateBanner before) {
    if (snap_.updates.state == UpdateState::Downloading) {
        update_rate_.add(std::chrono::steady_clock::now(), snap_.updates.done);
    } else {
        update_rate_.reset();
    }
    const UpdateBanner now = update_shown();
    if (now == UpdateBanner::Ready && before != UpdateBanner::Ready) {
        // Said once as it comes, without moving the focus.
        access_.notify(fill_pattern(text(lang_, Str::UpdateReadyTitle), false, {snap_.updates.version}), false,
                       "update");
    }
    if (now != before) {
        update_focus_ = -1;  // its buttons changed; the keys go back to the list
    }
}

void MainWindow::run_update_part(Part part) {
    switch (part) {
        case Part::UpdateNotes:
            open_update_notes();
            break;
        case Part::UpdateCancel:
            if (worker_ && snap_.updates.state == UpdateState::Downloading) {
                worker_->cancel_update();
                snap_.updates.state = UpdateState::Idle;  // gone now, not at the next poll
                update_focus_ = -1;
                show_notice(std::string(text(lang_, Str::UpdateCancelled)), false);
            }
            break;
        case Part::UpdateLater:
            // Until FloServer starts again (this object goes) or the next day.
            update_later_ = UpdateLater{snap_.updates.version, day_text(local_now())};
            update_focus_ = -1;
            break;
        case Part::UpdateNow:
            start_update();
            return;  // the window may be gone
        default:
            return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::open_update_notes() {
    const std::string& url = snap_.updates.notes_url;
    // Only the signed list's https address ever reaches the shell.
    if (!is_openable_notes(url)) {
        return;
    }
    const auto opened =
        reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", w(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (opened <= 32) {
        show_notice(fill(lang_, Str::OpenFailed, {url}), true);
    }
}

void MainWindow::start_update() {
    if (quitting_ || uninstalling_ || !worker_ || asking_ || (settings_ && settings_->asking())) {
        return;
    }
    const UpdateStatus u = snap_.updates;
    if (u.state != UpdateState::Ready || u.file.empty()) {
        return;
    }
    // Windows' progress and question for a folder's move belong to this window.
    if (recycle_bin_.busy()) {
        show_window();
        show_notice(std::string(text(lang_, Str::UninstallWait)), true);
        return;
    }
    std::string error;
    const auto exe = core::executable_path();
    std::error_code ec;
    const fs::path temp = fs::temp_directory_path(ec);
    fs::path copy;
    if (!exe || ec) {
        error = !exe ? std::string("FloServer.exe") : core::error_text(ec);
    } else if (copy_updater(*exe, temp, copy, error)) {
        ApplyUpdateArgs args;
        args.msi = u.file;
        args.instance = instance_suffix(core::ipc::default_instance());
        args.lang = lang_;
        std::wstring command = w(apply_update_command(core::path_to_utf8(copy), args));
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        const std::wstring dir = copy.parent_path().wstring();
        if (command.empty()) {
            error = u.file;
        } else if (CreateProcessW(copy.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si,
                                  &pi) == FALSE) {
            error = core::error_text({static_cast<int>(GetLastError()), std::system_category()});
        } else {
            // Its dialog comes to the front: this window is about to go.
            AllowSetForegroundWindow(pi.dwProcessId);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    if (!error.empty()) {
        show_window();
        show_notice(fill(lang_, Str::UpdateStartFailed, {error}), true);
        return;
    }
    // Gone without stopping the service: the copy stops it, as Quit would.
    if (settings_) {
        settings_->destroy_now();
        settings_.reset();
    }
    quitting_ = true;
    DestroyWindow(hwnd_);
}

void MainWindow::on_update_key(WPARAM key) {
    const std::vector<Part> order = update_focus_order();
    const int count = static_cast<int>(order.size());
    if (count == 0) {
        update_focus_ = -1;
        return;
    }
    update_focus_ = std::clamp(update_focus_, 0, count - 1);
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (key) {
        case VK_LEFT:
        case VK_RIGHT: {
            // Right moves on in left-to-right reading, back in right-to-left.
            const int step = (key == VK_RIGHT) != rtl_ ? 1 : -1;
            update_focus_ = std::clamp(update_focus_ + step, 0, count - 1);
            break;
        }
        case VK_TAB:
            if (shift && update_focus_ == 0) {
                update_focus_ = -1;
                SetFocus(search_);
                return;
            }
            if (!shift && update_focus_ == count - 1) {
                // On to the list below it.
                update_focus_ = -1;
                if (!visible_.empty()) {
                    focus_ = std::max(0, focus_);
                    ensure_visible(focus_);
                } else if (width_of(shared_geometry_.row) > 0) {
                    shared_focused_ = true;
                }
                break;
            }
            update_focus_ += shift ? -1 : 1;
            break;
        case VK_DOWN:
        case VK_ESCAPE:
            update_focus_ = -1;
            if (!visible_.empty()) {
                focus_ = std::max(0, focus_);
                ensure_visible(focus_);
            }
            break;
        case VK_SPACE:
        case VK_RETURN:
            run_update_part(order[static_cast<std::size_t>(update_focus_)]);
            return;
        default:
            return;
    }
    show_focus_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::focus_update_banner(bool last) {
    const std::vector<Part> order = update_focus_order();
    if (order.empty()) {
        return false;
    }
    update_focus_ = last ? static_cast<int>(order.size()) - 1 : 0;
    shared_focused_ = false;
    panel_focus_ = -1;
    show_focus_ = true;
    if (GetFocus() != hwnd_) {
        SetFocus(hwnd_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool MainWindow::update_part_focused(Part part) const {
    const std::vector<Part> order = update_focus_order();
    return show_focus_ && GetFocus() == hwnd_ && update_focus_ >= 0 && std::cmp_less(update_focus_, order.size()) &&
           order[static_cast<std::size_t>(update_focus_)] == part;
}

// --- painting ----------------------------------------------------------------------

void MainWindow::paint_update_banner(Canvas& c, bool text_pass) const {
    const UpdateGeometry& g = update_geometry_;
    if (g.kind == UpdateBanner::None) {
        return;
    }
    const Palette& p = palette(dark_);
    const float border = std::max(1.0F, pxf(1.0F));
    if (!text_pass) {
        c.round_rect(g.banner, pxf(8), p.card, p.card_border, border);
        // The reading start's edge in the accent, as the selected row's.
        c.fill({g.banner.left + static_cast<int>(border), g.banner.top + px(10), g.banner.left + px(3),
                g.banner.bottom - px(10)},
               p.accent);
        const float cx = static_cast<float>(g.icon.left + g.icon.right) / 2.0F;
        const float cy = static_cast<float>(g.icon.top + g.icon.bottom) / 2.0F;
        c.glyph(g.kind == UpdateBanner::Ready ? Glyph::Tick : Glyph::Download, cx, cy, pxf(20) / 16.0F, p.accent,
                pxf(1.6F));
        if (width_of(g.bar) > 0) {
            c.round_rect(g.bar, pxf(3), p.bar_track);
            const double done = update_fraction(snap_.updates);
            if (done > 0) {
                RECT fill = g.bar;
                fill.right = fill.left + std::max(height_of(g.bar), static_cast<int>(width_of(g.bar) * done));
                c.round_rect(fill, pxf(3), p.accent);
            }
        }
        for (const auto& [part, r] : {std::pair{Part::UpdateCancel, g.cancel}, std::pair{Part::UpdateLater, g.later}}) {
            if (width_of(r) > 0) {
                paint_button(c, r, hover_.part == part, update_part_focused(part));
            }
        }
        if (width_of(g.now) > 0) {
            // The primary button, as New project.
            c.round_rect(g.now, pxf(4), p.accent);
            if (update_part_focused(Part::UpdateNow)) {
                RECT ring = g.now;
                InflateRect(&ring, px(2), px(2));
                c.round_outline(ring, pxf(6), p.focus, pxf(2));
            }
        }
        if (width_of(g.notes) > 0) {
            if (hover_.part == Part::UpdateNotes) {
                c.round_rect(g.notes, pxf(4), p.row_hover);
            }
            if (update_part_focused(Part::UpdateNotes)) {
                c.round_outline(g.notes, pxf(4), p.focus, pxf(2));
            }
        }
        return;
    }
    const UpdateBannerText words = update_words();
    c.text(g.title, w(words.title), fonts_.body_bold.get(), p.text, Align::Start, false);
    c.text(g.line, w(words.line), fonts_.caption.get(), p.text_subtle, Align::Start, false);
    if (width_of(g.notes) > 0) {
        c.text(g.notes, w(text(lang_, Str::UpdateWhatsNew)), fonts_.button.get(), p.link, Align::Center, false);
    }
    if (width_of(g.cancel) > 0) {
        c.text(g.cancel, w(text(lang_, Str::UpdateCancel)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (width_of(g.later) > 0) {
        c.text(g.later, w(text(lang_, Str::UpdateLater)), fonts_.button.get(), p.text, Align::Center, false);
    }
    if (width_of(g.now) > 0) {
        c.text(g.now, w(text(lang_, Str::UpdateNow)), fonts_.button_bold.get(), p.accent_text, Align::Center, false);
    }
}

// --- screen readers ------------------------------------------------------------------

void MainWindow::add_update_access(AccessTree& t) const {
    const UpdateGeometry& g = update_geometry_;
    if (g.kind == UpdateBanner::None) {
        return;
    }
    const auto ar = [](const RECT& r) { return AccessRect{r.left, r.top, r.right, r.bottom}; };
    const UpdateBannerText words = update_words();
    AccessNode group;
    group.id = "update";
    group.role = AccessRole::Group;
    group.name = words.title;
    group.bounds = ar(g.banner);
    const int at = t.add(-1, std::move(group));
    AccessNode line = text_node("update/text", words.line);
    line.bounds = ar(g.line);
    t.add(at, std::move(line));
    if (width_of(g.bar) > 0) {
        // The moving words are its help, not its name: a name change would be said at each poll.
        AccessNode bar = progress_node("update/progress", words.title, update_fraction(snap_.updates));
        bar.help = words.line;
        bar.bounds = ar(g.bar);
        t.add(at, std::move(bar));
    }
    for (const auto& [part, r, key, id] :
         {std::tuple{Part::UpdateNotes, g.notes, Str::UpdateWhatsNew, "update/notes"},
          std::tuple{Part::UpdateCancel, g.cancel, Str::UpdateCancel, "update/cancel"},
          std::tuple{Part::UpdateLater, g.later, Str::UpdateLater, "update/later"},
          std::tuple{Part::UpdateNow, g.now, Str::UpdateNow, "update/now"}}) {
        if (width_of(r) <= 0) {
            continue;
        }
        AccessNode n = button_node(id, text(lang_, key));
        n.bounds = ar(r);
        n.target = {static_cast<int>(part), -1, {}};
        t.add(at, std::move(n));
    }
}

}  // namespace floserver::ui

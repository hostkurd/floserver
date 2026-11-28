#include "cleanup_window.h"

#include "publish.h"
#include "purge_token.h"
#include "r2.h"
#include "r2_key.h"

#include "win_util.h"
#include "floserver/core/builtin.h"
#include "floserver/core/http.h"
#include "floserver/core/manifest.h"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace floserver::publisher {

namespace {

namespace fs = std::filesystem;
namespace mt = floserver::manifest_tool;
using floserver::ui::win::int_to_ptr;
using floserver::ui::win::widen;

constexpr UINT kMsgChecked = WM_APP + 40;  // lParam: CleanupCheck*
constexpr UINT kMsgWorking = WM_APP + 41;  // wParam: the row being deleted
constexpr UINT kMsgStep = WM_APP + 42;     // lParam: CleanupStep*
constexpr UINT kMsgRan = WM_APP + 43;      // lParam: CleanupRun*
constexpr UINT kMsgSay = WM_APP + 44;      // lParam: std::wstring* (what the run is doing)

enum : int { IdList = 400, IdCheckAgain, IdCheckAll, IdUncheckAll, IdDelete, IdClose };

constexpr COLORREF kWindowBg = RGB(0xF3, 0xF3, 0xF3);
constexpr COLORREF kText = RGB(0x1B, 0x1B, 0x1B);
constexpr COLORREF kMuted = RGB(0x5F, 0x5F, 0x5F);
constexpr COLORREF kGrey = RGB(0x8A, 0x8A, 0x8A);
constexpr COLORREF kAmber = RGB(0x7A, 0x4A, 0x00);
constexpr COLORREF kRed = RGB(0xB3, 0x26, 0x1E);
constexpr COLORREF kGreen = RGB(0x0F, 0x6B, 0x2E);

constexpr int kNoteColumn = 4;

// A label whose text changes: set, then painted again whole.
void set_text(HWND label, const wchar_t* text) {
    SetWindowTextW(label, text);
    InvalidateRect(label, nullptr, TRUE);
}

std::chrono::sys_seconds now_utc() {
    return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
}

// Documents\FloServer Publisher; empty when Documents can't be found.
fs::path publisher_folder() {
    PWSTR docs = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        out = fs::path(docs) / L"FloServer Publisher";
    }
    CoTaskMemFree(docs);
    return out;
}

std::wstring size_text(std::uint64_t bytes) {
    std::array<wchar_t, 32> text{};
    const double b = static_cast<double>(bytes);
    if (b >= 1024.0 * 1024.0 * 1024.0) {
        (void)swprintf(text.data(), text.size(), L"%.1f GB", b / (1024.0 * 1024.0 * 1024.0));
    } else if (b >= 1024.0 * 1024.0) {
        (void)swprintf(text.data(), text.size(), L"%.1f MB", b / (1024.0 * 1024.0));
    } else {
        (void)swprintf(text.data(), text.size(), L"%.0f KB", std::max(b / 1024.0, bytes == 0 ? 0.0 : 1.0));
    }
    return text.data();
}

std::wstring files_text(std::size_t n) { return std::to_wstring(n) + (n == 1 ? L" file" : L" files"); }

// What a check (or the check before deleting) found.
struct Plan {
    std::string error;  // nothing may be deleted
    std::vector<mt::CleanupRow> rows;
    std::size_t objects = 0;
    std::uint64_t bytes = 0;
    std::size_t saved = 0;
};

// The clock, the live list, the bucket and the saved lists, then the plan.
Plan make_plan(core::HttpTransport& http, const mt::R2Key& key, const std::vector<std::string>& next,
               const std::atomic<bool>& cancel, const std::function<void(const wchar_t*)>& say) {
    Plan out;
    const auto now = now_utc();
    say(L"Checking this PC's clock against Cloudflare's…");
    const mt::CdnClock clock = mt::read_cdn_clock(http, mt::kCdn, now, &cancel);
    if (cancel) {
        out.error = "Stopped";
        return out;
    }
    if (!clock.error.empty()) {
        out.error = clock.error;
        return out;
    }
    // A clock days ahead would make everything look old enough.
    if (std::string problem = mt::clock_problem(clock.server_date, now); !problem.empty()) {
        out.error = problem;
        return out;
    }
    say(L"Reading the live list…");
    const mt::LiveResult live = mt::fetch_live_list(http, mt::kCdn, core::builtin_manifest_keys(), now, &cancel);
    if (!live.list) {
        out.error = "Couldn't read the live list: " + live.error;
        return out;
    }
    const auto live_paths = mt::list_paths(*live.list, mt::kCdn);
    if (!live_paths) {
        out.error = "The live list has a file that isn't on cdn.floserver.org, so what it uses can't be told";
        return out;
    }
    if (live_paths->empty()) {
        out.error = "The live list has no files";
        return out;
    }
    say(L"Listing the files on R2…");
    mt::R2Bucket bucket(http, key);
    std::string error;
    auto objects = bucket.list_objects(error, &cancel);
    if (!objects) {
        out.error = cancel ? std::string("Stopped") : "Couldn't list R2: " + error;
        return out;
    }
    const fs::path folder = publisher_folder();
    if (folder.empty()) {
        out.error = "Can't find the Documents folder, where the signed lists are kept";
        return out;
    }
    mt::CleanupInput in;
    in.saved = mt::read_saved_lists(folder, mt::kCdn);
    in.live_sequence = live.list->sequence;
    in.live_paths = *live_paths;
    in.next_paths = next;
    in.now = now;
    out.objects = objects->size();
    for (const mt::R2Object& o : *objects) {
        out.bytes += o.size;
    }
    out.saved = in.saved.size();
    in.objects = std::move(*objects);
    out.rows = mt::plan_cleanup(in);
    return out;
}

// Documents\FloServer Publisher\cleanup <date> <time>.txt (local time), not
// one already there.
fs::path record_path() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    std::array<wchar_t, 32> stamp{};
    (void)wcsftime(stamp.data(), stamp.size(), L"%Y-%m-%d %H-%M", &local);
    const fs::path folder = publisher_folder();
    if (folder.empty()) {
        return {};
    }
    fs::path p = folder / (std::wstring(L"cleanup ") + stamp.data() + L".txt");
    std::error_code ec;
    for (int n = 2; fs::exists(p, ec) && n < 100; ++n) {
        p = folder / (std::wstring(L"cleanup ") + stamp.data() + L" (" + std::to_wstring(n) + L").txt");
    }
    return p;
}

void post_text(HWND target, const wchar_t* text) {
    auto copy = std::make_unique<std::wstring>(text);
    if (PostMessageW(target, kMsgSay, 0, reinterpret_cast<LPARAM>(copy.get())) != FALSE) {
        (void)copy.release();
    }
}

}  // namespace

struct CleanupCheck {
    Plan plan;
};

struct CleanupStep {
    std::size_t row = 0;
    std::string problem;  // empty: deleted
    bool skipped = false;
};

struct CleanupRun {
    std::string error;  // nothing was deleted
    std::string purge_note;
    fs::path record;
    std::string record_problem;
    std::size_t deleted = 0;
    std::uint64_t freed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    bool stopped = false;
};

CleanupWindow::~CleanupWindow() {
    stop_worker();
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
    }
    for (HGDIOBJ o : {static_cast<HGDIOBJ>(font_), static_cast<HGDIOBJ>(heading_font_),
                      static_cast<HGDIOBJ>(small_font_), static_cast<HGDIOBJ>(bg_)}) {
        if (o != nullptr) {
            DeleteObject(o);
        }
    }
}

void CleanupWindow::make_fonts() {
    const auto font = [this](int pt, int weight) {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, static_cast<int>(dpi_), 72);
        lf.lfWeight = weight;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        return CreateFontIndirectW(&lf);
    };
    font_ = font(9, FW_NORMAL);
    heading_font_ = font(11, FW_SEMIBOLD);
    small_font_ = font(8, FW_NORMAL);
}

void CleanupWindow::open(HWND owner) {
    if (hwnd_ == nullptr) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &CleanupWindow::proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FloServerPublisherCleanup";
        RegisterClassExW(&wc);  // a second open finds it registered: fine
        hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Clean up - FloServer Publisher",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 900, 620, owner,
                                nullptr, inst, this);
        if (hwnd_ == nullptr) {
            return;
        }
        dpi_ = GetDpiForWindow(hwnd_);
        dpi_before_ = dpi_;
        make_fonts();
        bg_ = CreateSolidBrush(kWindowBg);
        create_controls();
        RECT r{0, 0, px(900), px(620)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, dpi_);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        layout();
    }
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    // Every open reads R2 again: the main window may have published meanwhile.
    if (!checking_ && !deleting_ && !prompting_) {
        check();
    }
}

LRESULT CALLBACK CleanupWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = int_to_ptr<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<CleanupWindow*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = int_to_ptr<CleanupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->handle(msg, wp, lp);
}

void CleanupWindow::create_controls() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT f, DWORD ex = 0) {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                                 int_to_ptr<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        return h;
    };
    heading_ = make(L"STATIC", L"Clean up", SS_LEFT | SS_NOPREFIX, -1, heading_font_);
    hint_ = make(L"STATIC",
                 L"Files on R2 that neither the live list nor the next list uses. Ticked: they left the list more "
                 L"than 7 days ago. Files a list still needs are never shown.",
                 SS_LEFT | SS_NOPREFIX, -1, font_);
    check_again_ = make(L"BUTTON", L"Check again", WS_TABSTOP | BS_PUSHBUTTON, IdCheckAgain, font_);
    check_all_ = make(L"BUTTON", L"Check all", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IdCheckAll, font_);
    uncheck_all_ = make(L"BUTTON", L"Uncheck all", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IdUncheckAll, font_);
    summary_ = make(L"STATIC", L"", SS_RIGHT | SS_NOPREFIX | SS_ENDELLIPSIS, -1, font_);
    list_ = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                 IdList, font_, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP |
                                                 LVS_EX_CHECKBOXES);
    const std::array<std::pair<const wchar_t*, int>, 5> columns = {
        {{L"File", 210}, {L"Path", 300}, {L"Size", 70}, {L"Left the list", 100}, {L"", 200}}};
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 2 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.pszText = const_cast<wchar_t*>(columns.at(static_cast<std::size_t>(i)).first);
        c.cx = px(columns.at(static_cast<std::size_t>(i)).second);
        ListView_InsertColumn(list_, i, &c);
    }
    status_ = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, -1, font_);
    warning_ = make(L"STATIC", L"Deleting can't be undone. A record is saved in Documents\\FloServer Publisher.",
                    SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, -1, small_font_);
    delete_ = make(L"BUTTON", L"Delete selected", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IdDelete, font_);
    close_ = make(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, IdClose, font_);
}

// Top: the heading and the hint, Check again on the right. Above the list:
// Check all / Uncheck all and the totals. Below it the status line, then the
// warning beside Delete selected and Close.
void CleanupWindow::layout() {
    if (list_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int pad = px(18);
    const int w = client.right - 2 * pad;
    const int right = pad + w;
    const auto put = [](HWND c, int x, int y, int cw, int ch) {
        SetWindowPos(c, nullptr, x, y, std::max(cw, 0), std::max(ch, 0), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const int button_h = px(30);

    put(check_again_, right - px(110), px(14), px(110), button_h);
    put(heading_, pad, px(14), w - px(124), px(26));
    put(hint_, pad, px(42), w - px(124), px(36));  // two lines at narrow widths

    const int tools_y = px(86);
    put(check_all_, pad, tools_y, px(96), px(28));
    put(uncheck_all_, pad + px(104), tools_y, px(104), px(28));
    put(summary_, pad + px(220), tools_y + px(6), w - px(220), px(20));

    const int bottom = client.bottom - px(16);
    const int buttons_y = bottom - px(32);
    put(close_, right - px(96), buttons_y, px(96), px(32));
    put(delete_, right - px(96) - px(8) - px(140), buttons_y, px(140), px(32));
    put(warning_, pad, buttons_y + px(8), w - px(96) - px(8) - px(140) - px(12), px(18));

    const int status_y = buttons_y - px(12) - px(20);
    put(status_, pad, status_y, w, px(20));

    const int list_top = tools_y + px(38);
    put(list_, pad, list_top, w, status_y - px(10) - list_top);
    int fixed = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_) + px(4);
    for (int c = 0; c < kNoteColumn; ++c) {
        fixed += ListView_GetColumnWidth(list_, c);
    }
    ListView_SetColumnWidth(list_, kNoteColumn, std::max(px(160), w - fixed));
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void CleanupWindow::stop_worker() {
    cancel_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
    cancel_ = false;
}

void CleanupWindow::check() {
    if (checking_ || deleting_ || prompting_) {
        return;
    }
    std::optional<mt::R2Key> key = load_r2_key();
    if (!key) {
        prompting_ = true;
        key = ask_r2_key(hwnd_);
        prompting_ = false;
        if (hwnd_ == nullptr || checking_ || deleting_) {
            return;  // closed, or something started, while it asked
        }
    }
    if (!key) {
        set_text(status_, L"Clean up needs the R2 key (Object Read & Write on floserver-cdn). Press Check again "
                          L"to give it.");
        return;
    }
    stop_worker();
    checking_ = true;
    ran_ = false;
    ++generation_;
    rows_.clear();
    plan_notes_.clear();
    outcome_.clear();
    outcome_text_.clear();
    ListView_DeleteAllItems(list_);
    set_text(summary_, L"");
    set_text(status_, L"Reading R2 and the saved lists…");
    set_buttons();
    std::vector<std::string> next = next_paths ? next_paths() : std::vector<std::string>{};
    HWND target = hwnd_;
    worker_ = std::thread([target, this, key = *key, next = std::move(next)] {
        auto found = std::make_unique<CleanupCheck>();
        if (auto http = core::make_system_transport()) {
            found->plan = make_plan(*http, key, next, cancel_, [target](const wchar_t* text) { post_text(target, text); });
        } else {
            found->plan.error = "No HTTPS on this computer";
        }
        // The window may be gone (the Publisher closed): then nothing is handed over.
        if (PostMessageW(target, kMsgChecked, 0, reinterpret_cast<LPARAM>(found.get())) != FALSE) {
            (void)found.release();
        }
    });
}

void CleanupWindow::checked(CleanupCheck& found) {
    checking_ = false;
    if (!found.plan.error.empty()) {
        set_text(status_, (L"Nothing is offered: " + widen(found.plan.error) + L".").c_str());
        set_buttons();
        return;
    }
    rows_ = std::move(found.plan.rows);
    plan_notes_.clear();
    plan_notes_.reserve(rows_.size());
    for (const mt::CleanupRow& r : rows_) {
        plan_notes_.push_back(r.note);
    }
    outcome_.assign(rows_.size(), Outcome::None);
    outcome_text_.assign(rows_.size(), std::wstring());
    fill();
    std::wstring status = L"Read R2 (" + files_text(found.plan.objects) + L", " + size_text(found.plan.bytes) +
                          L") and " + std::to_wstring(found.plan.saved) +
                          (found.plan.saved == 1 ? L" saved list. " : L" saved lists. ");
    status += rows_.empty() ? L"Nothing to clean up: every file there is in a list, or isn't one Clean up deletes."
                            : L"Tick what you want gone, then Delete selected.";
    set_text(status_, status.c_str());
    set_buttons();
}

void CleanupWindow::fill() {
    filling_ = true;
    ListView_DeleteAllItems(list_);
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const mt::CleanupRow& r = rows_[i];
        const std::wstring title = widen(r.title);
        LVITEMW item{};
        // Inserted with its tick already set, so no notification changes the row meanwhile.
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
        item.state = INDEXTOSTATEIMAGEMASK(r.ticked ? 2U : 1U);
        item.stateMask = LVIS_STATEIMAGEMASK;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(title.c_str());
        item.lParam = static_cast<LPARAM>(i);
        const int at = ListView_InsertItem(list_, &item);
        // And set again: a list with check boxes may give a new row its own (unticked) state.
        ListView_SetCheckState(list_, at, r.ticked ? TRUE : FALSE);
        refresh_row(i);
    }
    filling_ = false;
    update_summary();
}

// The row's texts and tick from rows_ and its outcome.
void CleanupWindow::refresh_row(std::size_t row) {
    const auto at = static_cast<int>(row);
    const mt::CleanupRow& r = rows_[row];
    const std::wstring path = widen(r.path);
    const std::wstring size = size_text(r.size);
    const std::wstring left = widen(mt::left_text(r, now_utc()));
    std::wstring note = widen(r.note);
    if (row < outcome_.size() && outcome_[row] != Outcome::None) {
        note = outcome_text_[row];
    }
    ListView_SetItemText(list_, at, 1, const_cast<wchar_t*>(path.c_str()));
    ListView_SetItemText(list_, at, 2, const_cast<wchar_t*>(size.c_str()));
    ListView_SetItemText(list_, at, 3, const_cast<wchar_t*>(left.c_str()));
    ListView_SetItemText(list_, at, kNoteColumn, const_cast<wchar_t*>(note.c_str()));
    const bool was = filling_;
    filling_ = true;
    if ((ListView_GetCheckState(list_, at) != FALSE) != r.ticked) {
        ListView_SetCheckState(list_, at, r.ticked ? TRUE : FALSE);
    }
    filling_ = was;
}

// Kept sources and files already deleted can't be ticked.
bool CleanupWindow::tickable(std::size_t row) const {
    return row < rows_.size() && rows_[row].kind != mt::CleanupKind::Kept &&
           (row >= outcome_.size() || outcome_[row] != Outcome::Deleted);
}

// After the user's ticks: a source goes only with all its binaries.
void CleanupWindow::ticks_changed() {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].source && i < plan_notes_.size()) {
            rows_[i].note = plan_notes_[i];
        }
    }
    // Rows deleted in a run are gone: they don't hold a source back.
    std::vector<mt::CleanupRow> present;
    std::vector<std::size_t> where;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (i >= outcome_.size() || outcome_[i] != Outcome::Deleted) {
            present.push_back(rows_[i]);
            where.push_back(i);
        }
    }
    mt::tie_sources(present);
    for (std::size_t k = 0; k < present.size(); ++k) {
        rows_[where[k]] = std::move(present[k]);
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        refresh_row(i);
    }
    update_summary();
    set_buttons();
}

void CleanupWindow::tick_all(bool ticked) {
    if (checking_ || deleting_) {
        return;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        rows_[i].ticked = ticked && tickable(i);
    }
    ticks_changed();
}

void CleanupWindow::update_summary() {
    std::size_t ticked = 0;
    std::uint64_t bytes = 0;
    std::size_t waiting = 0;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const mt::CleanupRow& r = rows_[i];
        if (i < outcome_.size() && outcome_[i] == Outcome::Deleted) {
            continue;
        }
        if (r.kind == mt::CleanupKind::Kept) {
            ++kept;
        } else if (r.kind == mt::CleanupKind::Wait || r.kind == mt::CleanupKind::Unknown) {
            waiting += r.ticked ? 0U : 1U;
        }
        if (r.ticked) {
            ++ticked;
            bytes += r.size;
        }
    }
    std::wstring text = ran_ ? std::to_wstring(deleted_) + L" deleted · " + size_text(freed_) + L" freed · "
                             : std::to_wstring(ticked) + L" ticked · " + size_text(bytes) + L" · ";
    text += std::to_wstring(waiting) + L" waiting · " + std::to_wstring(kept) + L" kept";
    set_text(summary_, rows_.empty() ? L"" : text.c_str());
}

void CleanupWindow::set_buttons() {
    const bool idle = !checking_ && !deleting_;
    const bool any = std::any_of(rows_.begin(), rows_.end(), [](const mt::CleanupRow& r) { return r.ticked; });
    bool can_tick = false;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        can_tick = can_tick || (tickable(i) && !rows_[i].ticked);
    }
    EnableWindow(check_again_, idle ? TRUE : FALSE);
    EnableWindow(check_all_, idle && can_tick ? TRUE : FALSE);
    EnableWindow(uncheck_all_, idle && any ? TRUE : FALSE);
    SetWindowTextW(delete_, deleting_ ? L"Stop" : L"Delete selected");
    EnableWindow(delete_, deleting_ || (idle && any) ? TRUE : FALSE);
}

// D37.4: asks, then deletes the ticked rows on the worker after checking
// everything again.
void CleanupWindow::delete_selected() {
    if (checking_ || deleting_ || prompting_) {
        return;
    }
    if (main_busy) {
        if (const std::wstring why = main_busy(); !why.empty()) {
            set_text(status_, why.c_str());
            return;
        }
    }
    std::vector<std::size_t> picked;
    std::uint64_t bytes = 0;
    std::size_t early = 0;  // left less than 7 days ago, or when isn't known
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].ticked && tickable(i)) {
            picked.push_back(i);
            bytes += rows_[i].size;
            early += rows_[i].kind == mt::CleanupKind::Ready ? 0U : 1U;
        }
    }
    if (picked.empty()) {
        return;
    }
    const std::optional<mt::R2Key> key = load_r2_key();
    if (!key) {
        set_text(status_, L"The R2 key isn't saved on this PC any more: press Check again.");
        return;
    }
    const std::optional<mt::PurgeToken> purge = load_purge_token();

    const bool one = picked.size() == 1;
    const std::wstring title = L"Delete " + files_text(picked.size()) + L" (" + size_text(bytes) + L") from floserver-cdn?";
    std::wstring body = one ? L"This can't be undone. It's in no list now" : L"This can't be undone. They're in no list now";
    if (early == 0) {
        body += one ? L", and it left the list more than 7 days ago." : L", and they left the list more than 7 days ago.";
    } else {
        body += one ? L". It" : (early == 1 ? L". 1 of them" : L". " + std::to_wstring(early) + L" of them");
        body += L" left the list less than 7 days ago, or when isn't known: a FloServer that read an older list may "
                L"still ask for ";
        body += early == 1 ? L"it." : L"them.";
    }
    if (purge) {
        body += one ? L" It'll also be purged from Cloudflare's cache." : L" They'll also be purged from Cloudflare's cache.";
    } else {
        body += L" No purge token is saved, so Cloudflare serves cached copies until they expire.";
    }
    const std::array<TASKDIALOG_BUTTON, 2> buttons = {{{IDOK, L"Delete"}, {IDCANCEL, L"Cancel"}}};
    TASKDIALOGCONFIG tdc{};
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = hwnd_;
    tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tdc.pszWindowTitle = L"FloServer Publisher";
    tdc.pszMainIcon = TD_WARNING_ICON;
    tdc.pszMainInstruction = title.c_str();
    tdc.pszContent = body.c_str();
    tdc.cButtons = static_cast<UINT>(buttons.size());
    tdc.pButtons = buttons.data();
    tdc.nDefaultButton = IDCANCEL;
    int pressed = IDCANCEL;
    const std::uint64_t asked_on = generation_;
    prompting_ = true;
    if (FAILED(TaskDialogIndirect(&tdc, &pressed, nullptr, nullptr)) && hwnd_ != nullptr) {
        pressed = MessageBoxW(hwnd_, (title + L"\n\n" + body).c_str(), L"FloServer Publisher",
                              MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    }
    prompting_ = false;
    // Closed, checked again or busy while it asked: the rows asked about may be gone.
    if (pressed != IDOK || hwnd_ == nullptr || checking_ || deleting_ || generation_ != asked_on) {
        return;
    }
    if (main_busy) {
        if (const std::wstring why = main_busy(); !why.empty()) {
            set_text(status_, why.c_str());
            return;
        }
    }
    for (const std::size_t i : picked) {
        if (i >= rows_.size()) {
            return;
        }
    }

    stop_worker();
    deleting_ = true;
    ran_ = false;
    run_total_ = picked.size();
    run_done_ = 0;
    for (std::size_t i = 0; i < outcome_.size(); ++i) {
        if (outcome_[i] != Outcome::Deleted) {
            outcome_[i] = Outcome::None;
            outcome_text_[i].clear();
        }
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        refresh_row(i);
    }
    set_text(status_, L"Checking again before deleting…");
    set_buttons();
    std::vector<mt::CleanupRow> chosen;
    chosen.reserve(picked.size());
    for (const std::size_t i : picked) {
        chosen.push_back(rows_[i]);
    }
    std::vector<std::string> next = next_paths ? next_paths() : std::vector<std::string>{};
    HWND target = hwnd_;
    worker_ = std::thread([target, this, key = *key, purge, picked, chosen = std::move(chosen),
                           next = std::move(next)] {
        auto run = std::make_unique<CleanupRun>();
        const auto post_step = [target](std::size_t row, std::string problem, bool skipped) {
            auto s = std::make_unique<CleanupStep>();
            s->row = row;
            s->problem = std::move(problem);
            s->skipped = skipped;
            if (PostMessageW(target, kMsgStep, 0, reinterpret_cast<LPARAM>(s.get())) != FALSE) {
                (void)s.release();
            }
        };
        auto http = core::make_system_transport();
        if (!http) {
            run->error = "No HTTPS on this computer";
        }
        // The live list, the bucket and the next list as they are now.
        Plan now;
        if (http) {
            now = make_plan(*http, key, next, cancel_, [target](const wchar_t* text) { post_text(target, text); });
            run->error = now.error;
        }
        std::vector<mt::CleanupDone> done;
        if (run->error.empty()) {
            std::map<std::string, const mt::CleanupRow*> fresh;
            for (const mt::CleanupRow& r : now.rows) {
                fresh[r.path] = &r;
            }
            std::set<std::string> chosen_paths;
            for (const mt::CleanupRow& r : chosen) {
                chosen_paths.insert(r.path);
            }
            const auto folder_of = [](const std::string& p) { return p.substr(0, p.rfind('/')); };
            std::set<std::string> binary_stays;  // folders where a binary isn't deleted (GPL)
            for (const mt::CleanupRow& r : now.rows) {
                if (!r.source && !chosen_paths.contains(r.path)) {
                    binary_stays.insert(folder_of(r.path));
                }
            }
            mt::R2Bucket bucket(*http, key);
            // Binaries first: a source goes only once every binary in its folder went.
            for (const bool sources : {false, true}) {
                for (std::size_t k = 0; k < chosen.size(); ++k) {
                    const mt::CleanupRow& want = chosen[k];
                    if (want.source != sources) {
                        continue;
                    }
                    const std::size_t row = picked[k];
                    mt::CleanupDone d{want.path, want.size, want.sha256, "", false};
                    const auto it = fresh.find(want.path);
                    if (cancel_) {
                        d.problem = "stopped before it";
                        d.skipped = true;
                    } else if (it == fresh.end() || !mt::may_delete(want.path)) {
                        d.problem = "a list uses it now, or it's gone from R2";
                        d.skipped = true;
                    } else if (want.kind == mt::CleanupKind::Ready && it->second->kind != mt::CleanupKind::Ready) {
                        d.problem = "it left a list again since the check; press Check again";
                        d.skipped = true;
                    } else if (it->second->kind == mt::CleanupKind::Kept ||
                               (want.source && binary_stays.contains(folder_of(want.path)))) {
                        d.problem = "a binary in its folder stays, so its source does (GPL)";
                        d.skipped = true;
                    } else {
                        PostMessageW(target, kMsgWorking, row, 0);
                        std::string error;
                        // Stop waits for this one: a DELETE cut off may still have happened.
                        if (!bucket.delete_object(want.path, error, nullptr)) {
                            d.problem = error.empty() ? std::string("not deleted") : error;
                        }
                    }
                    if (!d.problem.empty() && !want.source) {
                        binary_stays.insert(folder_of(want.path));
                    }
                    post_step(row, d.problem, d.skipped);
                    done.push_back(std::move(d));
                }
            }
        }
        std::vector<std::string> gone;
        for (const mt::CleanupDone& d : done) {
            if (d.problem.empty()) {
                gone.push_back(d.path);
                ++run->deleted;
                run->freed += d.size;
            } else if (d.skipped) {
                ++run->skipped;
            } else {
                ++run->failed;
            }
        }
        run->stopped = cancel_;
        // D37.5: off Cloudflare's cache too.
        if (!gone.empty() && purge) {
            post_text(target, L"Purging the deleted files from Cloudflare's cache…");
            std::atomic<bool> no_cancel{false};  // a purge isn't stopped half-way
            const std::string problem = mt::purge_deleted(*http, *purge, gone, mt::kCdn, &no_cancel);
            run->purge_note = problem.empty() ? "purged" : "not all purged: " + problem;
        } else if (!gone.empty()) {
            run->purge_note = "not purged: no purge token saved, so cached copies expire on their own";
        }
        // D37.6: the record.
        if (!done.empty()) {
            run->record = record_path();
        }
        if (!done.empty() && run->record.empty()) {
            run->record_problem = "the record couldn't be written (no Documents folder)";
        } else if (!done.empty()) {
            std::error_code ec;
            fs::create_directories(run->record.parent_path(), ec);
            std::ofstream f(run->record, std::ios::binary | std::ios::trunc);
            f << mt::cleanup_record(done, now_utc(), run->purge_note);
            f.close();
            if (!f) {
                run->record_problem = "the record couldn't be written";
            }
        }
        if (PostMessageW(target, kMsgRan, 0, reinterpret_cast<LPARAM>(run.get())) != FALSE) {
            (void)run.release();
        }
    });
}

void CleanupWindow::step(const CleanupStep& s) {
    if (s.row >= rows_.size()) {
        return;
    }
    ++run_done_;
    if (s.problem.empty()) {
        outcome_[s.row] = Outcome::Deleted;
        outcome_text_[s.row] = L"Deleted";
        rows_[s.row].ticked = false;
    } else {
        outcome_[s.row] = s.skipped ? Outcome::Skipped : Outcome::Failed;
        outcome_text_[s.row] = (s.skipped ? L"Not deleted: " : L"Failed: ") + widen(s.problem);
    }
    refresh_row(s.row);
}

void CleanupWindow::ran(const CleanupRun& run) {
    deleting_ = false;
    if (!run.error.empty()) {
        set_text(status_, (L"Nothing deleted: " + widen(run.error) + L".").c_str());
        set_buttons();
        return;
    }
    ran_ = true;
    deleted_ = run.deleted;
    freed_ = run.freed;
    std::wstring text = run.stopped ? L"Stopped: " : L"Done: ";
    text += files_text(run.deleted) + L" deleted";
    if (run.deleted > 0) {
        text += run.purge_note == "purged" ? L" and purged from Cloudflare's cache" : L" (" + widen(run.purge_note) + L")";
    }
    if (run.failed > 0) {
        text += L", " + std::to_wstring(run.failed) + L" failed";
    }
    if (run.skipped > 0) {
        text += L", " + std::to_wstring(run.skipped) + L" not deleted";
    }
    text += L".";
    if (!run.record_problem.empty()) {
        text += L" But " + widen(run.record_problem) + L".";
    } else if (!run.record.empty()) {
        text += L" Record: " + run.record.filename().wstring();
    }
    set_text(status_, text.c_str());
    update_summary();
    set_buttons();
}

LRESULT CleanupWindow::custom_draw(NMLVCUSTOMDRAW* cd) const {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        const auto row = static_cast<std::size_t>(cd->nmcd.lItemlParam);
        COLORREF color = kText;
        if (row < rows_.size()) {
            const mt::CleanupRow& r = rows_[row];
            const Outcome o = row < outcome_.size() ? outcome_[row] : Outcome::None;
            if (r.kind == mt::CleanupKind::Kept) {
                color = cd->iSubItem == kNoteColumn ? kMuted : kGrey;
            } else if (cd->iSubItem == kNoteColumn) {
                switch (o) {
                case Outcome::Deleted:
                    color = kGreen;
                    break;
                case Outcome::Failed:
                    color = kRed;
                    break;
                case Outcome::Skipped:
                    color = kAmber;
                    break;
                case Outcome::Working:
                    color = kText;
                    break;
                case Outcome::None:
                    color = kAmber;  // why it waits
                    break;
                }
            }
        }
        cd->clrText = color;
        return CDRF_DODEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

LRESULT CleanupWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = int_to_ptr<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {px(760), px(420)};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        HFONT old_font = font_;
        HFONT old_heading = heading_font_;
        HFONT old_small = small_font_;
        make_fonts();
        for (HWND c : {hint_, list_, status_, summary_, check_again_, check_all_, uncheck_all_, delete_, close_}) {
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
        SendMessageW(heading_, WM_SETFONT, reinterpret_cast<WPARAM>(heading_font_), TRUE);
        SendMessageW(warning_, WM_SETFONT, reinterpret_cast<WPARAM>(small_font_), TRUE);
        DeleteObject(old_font);
        DeleteObject(old_heading);
        DeleteObject(old_small);
        for (int c = 0; c < kNoteColumn; ++c) {
            ListView_SetColumnWidth(list_, c, MulDiv(ListView_GetColumnWidth(list_, c), static_cast<int>(dpi_),
                                                     static_cast<int>(dpi_before_)));
        }
        dpi_before_ = dpi_;
        const auto* r = int_to_ptr<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, bg_);  // NOLINT(performance-no-int-to-ptr)
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wp);  // NOLINT(performance-no-int-to-ptr)
        const auto ctl = int_to_ptr<HWND>(lp);
        if (ctl == summary_ || ctl == status_) {
            // Text that changes: drawn on the window's colour, so a shorter
            // text never leaves the old one showing around it.
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, kWindowBg);
        } else {
            SetBkMode(dc, TRANSPARENT);
        }
        SetTextColor(dc, ctl == hint_ || ctl == status_ || ctl == warning_ ? kMuted : kText);
        return reinterpret_cast<LRESULT>(bg_);
    }
    case WM_NOTIFY: {
        const auto* nm = int_to_ptr<const NMHDR*>(lp);
        if (nm->hwndFrom == list_ && nm->code == NM_CUSTOMDRAW) {
            return custom_draw(int_to_ptr<NMLVCUSTOMDRAW*>(lp));
        }
        if (nm->hwndFrom == list_ && nm->code == LVN_ITEMCHANGED) {
            const auto* lv = int_to_ptr<const NMLISTVIEW*>(lp);
            const auto row = static_cast<std::size_t>(lv->lParam);
            if (!filling_ && (lv->uChanged & LVIF_STATE) != 0 &&
                ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) != 0 && row < rows_.size()) {
                const bool ticked = ListView_GetCheckState(list_, lv->iItem) != FALSE;
                if (checking_ || deleting_ || (ticked && !tickable(row))) {
                    refresh_row(row);  // the run has its rows; kept and deleted ones stay unticked
                } else {
                    rows_[row].ticked = ticked;
                    ticks_changed();
                }
            }
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IdCheckAgain:
            check();
            return 0;
        case IdCheckAll:
        case IdUncheckAll:
            tick_all(LOWORD(wp) == IdCheckAll);
            return 0;
        case IdDelete:
            if (deleting_) {
                cancel_ = true;  // Stop: after the file being deleted
                set_text(status_, L"Stopping after this file…");
                EnableWindow(delete_, FALSE);
            } else {
                delete_selected();
            }
            return 0;
        case IdClose:
        case IDCANCEL:
            SendMessageW(hwnd_, WM_CLOSE, 0, 0);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        if (deleting_) {
            if (MessageBoxW(hwnd_, L"Stop deleting? The file being deleted now finishes; the rest stay.",
                            L"FloServer Publisher", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
            if (!deleting_) {
                return 0;  // it finished while you were asked: its result is showing
            }
            cancel_ = true;
            set_text(status_, L"Stopping after this file…");
        }
        ShowWindow(hwnd_, SW_HIDE);  // kept, with its rows, until the Publisher closes
        return 0;
    case WM_DESTROY:
        cancel_ = true;  // destroyed with the Publisher's window: the work stops
        break;
    case WM_NCDESTROY: {
        HWND gone = hwnd_;
        hwnd_ = nullptr;
        list_ = nullptr;
        return DefWindowProcW(gone, msg, wp, lp);
    }
    case kMsgSay: {
        const std::unique_ptr<std::wstring> text(int_to_ptr<std::wstring*>(lp));
        if (checking_ || deleting_) {
            set_text(status_, text->c_str());
        }
        return 0;
    }
    case kMsgChecked: {
        const std::unique_ptr<CleanupCheck> found(int_to_ptr<CleanupCheck*>(lp));
        if (!checking_) {
            return 0;  // not this window's check any more
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        checked(*found);
        return 0;
    }
    case kMsgWorking: {
        const auto row = static_cast<std::size_t>(wp);
        if (row < rows_.size()) {
            outcome_[row] = Outcome::Working;
            outcome_text_[row] = L"Deleting…";
            refresh_row(row);
            set_text(status_, (L"Deleting " + std::to_wstring(run_done_ + 1) + L" of " + std::to_wstring(run_total_) +
                               L": " + widen(rows_[row].path) + L"…")
                                  .c_str());
        }
        return 0;
    }
    case kMsgStep: {
        const std::unique_ptr<CleanupStep> s(int_to_ptr<CleanupStep*>(lp));
        step(*s);
        return 0;
    }
    case kMsgRan: {
        const std::unique_ptr<CleanupRun> run(int_to_ptr<CleanupRun*>(lp));
        if (!deleting_) {
            return 0;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        cancel_ = false;
        ran(*run);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace floserver::publisher

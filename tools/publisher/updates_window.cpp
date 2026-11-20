#include "updates_window.h"

#include "fetch.h"

#include "win_util.h"
#include "floserver/core/http.h"

#include <commctrl.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <filesystem>

namespace floserver::publisher {

namespace {

namespace fs = std::filesystem;
namespace mt = floserver::manifest_tool;
using floserver::ui::win::int_to_ptr;
using floserver::ui::win::widen;

constexpr UINT kMsgAsking = WM_APP + 20;  // lParam: std::string* (the runtime being asked)
constexpr UINT kMsgFound = WM_APP + 21;   // lParam: std::map<std::string, FinderAnswer>*
constexpr UINT kMsgFetchStep = WM_APP + 22;  // lParam: std::wstring* (what Download only is doing)
constexpr UINT kMsgFetched = WM_APP + 23;    // lParam: FetchDone*

enum : int { IdList = 300, IdKeep, IdCheckAgain, IdDownload, IdGet, IdClose };

constexpr COLORREF kWindowBg = RGB(0xF3, 0xF3, 0xF3);
constexpr COLORREF kText = RGB(0x1B, 0x1B, 0x1B);
constexpr COLORREF kMuted = RGB(0x5F, 0x5F, 0x5F);
constexpr COLORREF kBlue = RGB(0x0B, 0x53, 0x94);
constexpr COLORREF kAmber = RGB(0x7A, 0x4A, 0x00);
constexpr COLORREF kRed = RGB(0xB3, 0x26, 0x1E);
constexpr COLORREF kGreen = RGB(0x0F, 0x6B, 0x2E);

// Documents\FloServer Publisher\<what> <date> <time>.
fs::path dated_folder(const std::wstring& what) {
    PWSTR docs = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        base = docs;
    }
    CoTaskMemFree(docs);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::array<wchar_t, 32> stamp{};
    (void)wcsftime(stamp.data(), stamp.size(), L"%Y-%m-%d %H%M%S", &local);
    return base / L"FloServer Publisher" / (what + L" " + stamp.data());
}

std::wstring megabytes(std::uint64_t bytes) {
    std::array<wchar_t, 32> text{};
    (void)swprintf(text.data(), text.size(), L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text.data();
}

std::wstring kind_text(const mt::UpdateRow& r) {
    switch (r.kind) {
    case mt::UpdateKind::Update:
        return r.detail.empty() ? L"Update" : L"Update (" + widen(r.detail) + L")";
    case mt::UpdateKind::NewSeries:
        return L"New series";
    case mt::UpdateKind::Failed:
        return L"Couldn't check: " + widen(r.detail);
    case mt::UpdateKind::UpToDate:
        break;
    }
    return r.detail.empty() ? L"Up to date" : L"Up to date (" + widen(r.detail) + L")";
}

}  // namespace

struct FetchDone {
    fs::path folder;
    std::vector<std::size_t> rows;          // the rows asked for
    std::vector<std::string> problems;      // per row: empty when all its files were written
    std::vector<std::string> notes;         // per row
    std::vector<std::string> cdn_paths;     // every file written
    bool stopped = false;
};

UpdatesWindow::~UpdatesWindow() {
    stop_worker();
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
    }
    for (HGDIOBJ o : {static_cast<HGDIOBJ>(font_), static_cast<HGDIOBJ>(heading_font_), static_cast<HGDIOBJ>(bg_)}) {
        if (o != nullptr) {
            DeleteObject(o);
        }
    }
}

void UpdatesWindow::open(HWND owner, std::map<std::string, std::vector<std::string>> listed) {
    // The same list as last time: the rows (and their ticks) are kept, no new
    // check (GitHub allows 60 an hour). A check still running isn't swapped.
    const bool same = listed == listed_ && !rows_.empty();
    if (!busy_) {
        listed_ = listed;
    }
    if (hwnd_ == nullptr) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &UpdatesWindow::proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FloServerPublisherUpdates";
        RegisterClassExW(&wc);  // a second open finds it registered: fine
        hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Updates - FloServer Publisher",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 760, 560, owner,
                                nullptr, inst, this);
        if (hwnd_ == nullptr) {
            return;
        }
        dpi_ = GetDpiForWindow(hwnd_);
        dpi_before_ = dpi_;
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
        bg_ = CreateSolidBrush(kWindowBg);
        create_controls();
        RECT r{0, 0, px(760), px(560)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, dpi_);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        layout();
    }
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    if (!same && !busy_ && !downloading_) {
        check();
    } else if ((busy_ || downloading_) && listed != listed_) {
        SetWindowTextW(status_, L"The list changed while checking: press Check again when this check ends.");
    }
}

LRESULT CALLBACK UpdatesWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = int_to_ptr<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<UpdatesWindow*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = int_to_ptr<UpdatesWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->handle(msg, wp, lp);
}

void UpdatesWindow::create_controls() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT f, DWORD ex = 0) {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                                 int_to_ptr<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        return h;
    };
    heading_ = make(L"STATIC", L"Updates", SS_LEFT | SS_NOPREFIX, -1, heading_font_);
    hint_ = make(L"STATIC",
                 L"Each runtime's official site, compared with the files in the next list. Updates are ticked; "
                 L"a new series is your choice.",
                 SS_LEFT | SS_NOPREFIX, -1, font_);
    list_ = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                 IdList, font_, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP |
                                                 LVS_EX_CHECKBOXES);
    const std::array<std::pair<const wchar_t*, int>, 4> columns = {
        {{L"Runtime", 170}, {L"In the list", 100}, {L"Newest", 100}, {L"", 300}}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(columns.at(static_cast<std::size_t>(i)).first);
        c.cx = px(columns.at(static_cast<std::size_t>(i)).second);
        ListView_InsertColumn(list_, i, &c);
    }
    status_ = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, -1, font_);
    keep_answers_ = make(L"BUTTON", L"Keep what each site answered (to send if a row can't be read)",
                         WS_TABSTOP | BS_AUTOCHECKBOX, IdKeep, font_);
    check_again_ = make(L"BUTTON", L"Check again", WS_TABSTOP | BS_PUSHBUTTON, IdCheckAgain, font_);
    download_ = make(L"BUTTON", L"Download only", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IdDownload, font_);
    get_ = make(L"BUTTON", L"Get selected", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IdGet, font_);
    close_ = make(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, IdClose, font_);
}

void UpdatesWindow::layout() {
    if (list_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int pad = px(16);
    const int w = client.right - 2 * pad;
    const auto put = [](HWND c, int x, int y, int cw, int ch) {
        SetWindowPos(c, nullptr, x, y, std::max(cw, 0), std::max(ch, 0), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    put(heading_, pad, px(12), w, px(24));
    put(hint_, pad, px(38), w, px(36));
    const int bottom = client.bottom - pad;
    const int buttons_y = bottom - px(30);
    put(close_, pad + w - px(90), buttons_y, px(90), px(30));
    put(get_, pad + w - px(90) - px(8) - px(120), buttons_y, px(120), px(30));
    put(download_, pad + w - px(90) - px(8) - px(120) - px(8) - px(120), buttons_y, px(120), px(30));
    put(check_again_, pad + w - px(90) - px(8) - px(120) - px(8) - px(120) - px(8) - px(110), buttons_y, px(110),
        px(30));
    put(keep_answers_, pad, buttons_y - px(30), w, px(24));
    // The status line has its own row above the buttons: Download only's steps are long.
    put(status_, pad, buttons_y - px(30) - px(24), w, px(20));
    const int list_top = px(78);
    put(list_, pad, list_top, w, buttons_y - px(30) - px(30) - list_top);
    const int fixed = ListView_GetColumnWidth(list_, 0) + ListView_GetColumnWidth(list_, 1) +
                      ListView_GetColumnWidth(list_, 2) + GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_) + px(4);
    ListView_SetColumnWidth(list_, 3, std::max(px(160), w - fixed));
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void UpdatesWindow::stop_worker() {
    cancel_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
    cancel_ = false;
}

void UpdatesWindow::check() {
    if (busy_ || downloading_) {
        return;
    }
    stop_worker();
    busy_ = true;
    set_buttons();
    rows_.clear();
    outcome_.clear();
    outcome_ok_.clear();
    ListView_DeleteAllItems(list_);
    SetWindowTextW(status_, L"Asking each site…");
    saved_to_.clear();
    if (Button_GetCheck(keep_answers_) == BST_CHECKED) {
        const fs::path folder = dated_folder(L"site answers");
        std::error_code ec;
        fs::create_directories(folder, ec);
        saved_to_ = floserver::ui::win::narrow(folder.wstring());
    }
    HWND target = hwnd_;
    worker_ = std::thread([target, this, listed = listed_, save = saved_to_] {
        auto found = std::make_unique<std::map<std::string, mt::FinderAnswer>>();
        if (auto http = core::make_system_transport()) {
            *found = mt::find_updates(
                *http, listed, &cancel_,
                [target](std::string_view runtime) {
                    auto text = std::make_unique<std::string>(runtime);
                    if (PostMessageW(target, kMsgAsking, 0, reinterpret_cast<LPARAM>(text.get())) != FALSE) {
                        (void)text.release();  // the window owns it now
                    }
                },
                save);
        }
        // The window may be gone (the Publisher closed): then nothing is handed over.
        if (PostMessageW(target, kMsgFound, 0, reinterpret_cast<LPARAM>(found.get())) != FALSE) {
            (void)found.release();
        }
    });
}

void UpdatesWindow::fill() {
    filling_ = true;
    ListView_DeleteAllItems(list_);
    int updates = 0;
    int fresh = 0;
    int failed = 0;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const mt::UpdateRow& r = rows_[i];
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
        const std::wstring listed = r.listed.empty() ? L"—" : widen(r.listed);
        const std::wstring latest = r.latest.empty() ? L"—" : widen(r.latest);
        const std::wstring kind = i < outcome_.size() && !outcome_[i].empty() ? outcome_[i] : kind_text(r);
        ListView_SetItemText(list_, at, 1, const_cast<wchar_t*>(listed.c_str()));
        ListView_SetItemText(list_, at, 2, const_cast<wchar_t*>(latest.c_str()));
        ListView_SetItemText(list_, at, 3, const_cast<wchar_t*>(kind.c_str()));
        updates += r.kind == mt::UpdateKind::Update ? 1 : 0;
        fresh += r.kind == mt::UpdateKind::NewSeries ? 1 : 0;
        failed += r.kind == mt::UpdateKind::Failed ? 1 : 0;
    }
    filling_ = false;
    std::wstring status = std::to_wstring(updates) + (updates == 1 ? L" update, " : L" updates, ") +
                          std::to_wstring(fresh) + (fresh == 1 ? L" new series" : L" new series");
    if (failed > 0) {
        status += L", " + std::to_wstring(failed) + L" couldn't be checked";
    }
    status += updates + fresh > 0 ? L". Tick what you want, then Download only." : L".";
    SetWindowTextW(status_, status.c_str());
    std::error_code ec;
    if (!saved_to_.empty() && !fs::is_empty(floserver::ui::win::widen(saved_to_), ec) && !ec) {
        ShellExecuteW(hwnd_, L"explore", widen(saved_to_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

LRESULT UpdatesWindow::custom_draw(NMLVCUSTOMDRAW* cd) const {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        const auto row = static_cast<std::size_t>(cd->nmcd.lItemlParam);
        COLORREF color = kText;
        if (row < rows_.size() && cd->iSubItem == 3 && row < outcome_.size() && !outcome_[row].empty()) {
            color = outcome_ok_[row] ? kGreen : kRed;
        } else if (row < rows_.size() && cd->iSubItem == 3) {
            switch (rows_[row].kind) {
            case mt::UpdateKind::Update:
                color = kBlue;
                break;
            case mt::UpdateKind::NewSeries:
                color = kAmber;
                break;
            case mt::UpdateKind::Failed:
                color = kRed;
                break;
            case mt::UpdateKind::UpToDate:
                color = kMuted;
                break;
            }
        }
        cd->clrText = color;
        return CDRF_DODEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

LRESULT UpdatesWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = int_to_ptr<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {px(600), px(380)};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        // Fonts at the new DPI for every control.
        const auto font = [this](int pt, int weight) {
            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(pt, static_cast<int>(dpi_), 72);
            lf.lfWeight = weight;
            lf.lfQuality = CLEARTYPE_QUALITY;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            return CreateFontIndirectW(&lf);
        };
        HFONT old_font = font_;
        HFONT old_heading = heading_font_;
        font_ = font(9, FW_NORMAL);
        heading_font_ = font(11, FW_SEMIBOLD);
        for (HWND c : {hint_, list_, status_, keep_answers_, check_again_, download_, get_, close_}) {
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
        SendMessageW(heading_, WM_SETFONT, reinterpret_cast<WPARAM>(heading_font_), TRUE);
        DeleteObject(old_font);
        DeleteObject(old_heading);
        const UINT before = dpi_before_;
        for (int c = 0; c < 3; ++c) {
            ListView_SetColumnWidth(list_, c, MulDiv(ListView_GetColumnWidth(list_, c), static_cast<int>(dpi_),
                                                     static_cast<int>(before)));
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
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, int_to_ptr<HWND>(lp) == hint_ || int_to_ptr<HWND>(lp) == status_ ? kMuted : kText);
        return reinterpret_cast<LRESULT>(bg_);
    }
    case WM_NOTIFY: {
        const auto* nm = int_to_ptr<const NMHDR*>(lp);
        if (nm->hwndFrom == list_ && nm->code == NM_CUSTOMDRAW) {
            return custom_draw(int_to_ptr<NMLVCUSTOMDRAW*>(lp));
        }
        if (nm->hwndFrom == list_ && nm->code == LVN_ITEMCHANGED) {
            // A tick is kept with its row: the next Publisher gets what's ticked.
            const auto* lv = int_to_ptr<const NMLISTVIEW*>(lp);
            const auto row = static_cast<std::size_t>(lv->lParam);
            if (!filling_ && (lv->uChanged & LVIF_STATE) != 0 &&
                ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) != 0 && row < rows_.size()) {
                const bool actionable =
                    rows_[row].kind == mt::UpdateKind::Update || rows_[row].kind == mt::UpdateKind::NewSeries;
                const bool ticked = ListView_GetCheckState(list_, lv->iItem) == TRUE;
                if (downloading_) {
                    // The run has its rows: a tick changed meanwhile goes back.
                    filling_ = true;
                    ListView_SetCheckState(list_, lv->iItem, rows_[row].ticked ? TRUE : FALSE);
                    filling_ = false;
                } else if (ticked && !actionable) {
                    ListView_SetCheckState(list_, lv->iItem, FALSE);  // nothing to get
                } else {
                    rows_[row].ticked = ticked;
                    set_buttons();
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
        case IdDownload:
            if (downloading_) {
                cancel_ = true;  // Stop
                SetWindowTextW(status_, L"Stopping…");
                EnableWindow(download_, FALSE);
            } else {
                download();
            }
            return 0;
        case IdClose:
        case IDCANCEL:
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        cancel_ = true;  // destroyed with the Publisher's window: the check stops
        break;
    case WM_NCDESTROY: {
        HWND gone = hwnd_;
        hwnd_ = nullptr;
        list_ = nullptr;
        return DefWindowProcW(gone, msg, wp, lp);
    }
    case WM_CLOSE:
        ShowWindow(hwnd_, SW_HIDE);  // kept, with its rows, until the Publisher closes
        return 0;
    case kMsgAsking: {
        const std::unique_ptr<std::string> runtime(int_to_ptr<std::string*>(lp));
        SetWindowTextW(status_, (L"Asking about " + widen(*runtime) + L"…").c_str());
        return 0;
    }
    case kMsgFound: {
        const std::unique_ptr<std::map<std::string, mt::FinderAnswer>> found(
            int_to_ptr<std::map<std::string, mt::FinderAnswer>*>(lp));
        if (worker_.joinable()) {
            worker_.join();
        }
        busy_ = false;
        rows_ = mt::compare_updates(listed_, *found);
        outcome_.assign(rows_.size(), std::wstring());
        outcome_ok_.assign(rows_.size(), false);
        fill();
        set_buttons();
        return 0;
    }
    case kMsgFetchStep: {
        const std::unique_ptr<std::wstring> text(int_to_ptr<std::wstring*>(lp));
        if (downloading_) {
            SetWindowTextW(status_, text->c_str());
        }
        return 0;
    }
    case kMsgFetched: {
        const std::unique_ptr<FetchDone> done(int_to_ptr<FetchDone*>(lp));
        if (worker_.joinable()) {
            worker_.join();
        }
        cancel_ = false;
        downloading_ = false;
        fetched(*done);
        set_buttons();
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void UpdatesWindow::set_buttons() {
    const bool any = std::any_of(rows_.begin(), rows_.end(), [](const mt::UpdateRow& r) { return r.ticked; });
    EnableWindow(check_again_, busy_ || downloading_ ? FALSE : TRUE);
    SetWindowTextW(download_, downloading_ ? L"Stop" : L"Download only");
    EnableWindow(download_, downloading_ || (!busy_ && any) ? TRUE : FALSE);
}

// D34.3, manual upload: each ticked version from its official site, checked,
// into Documents\FloServer Publisher\downloads <date> <time>\<cdn path>.
void UpdatesWindow::download() {
    if (busy_ || downloading_) {
        return;
    }
    std::vector<std::size_t> picked;
    std::vector<mt::UpdateRow> rows;
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].ticked) {
            picked.push_back(i);
            rows.push_back(rows_[i]);
        }
    }
    if (picked.empty()) {
        return;
    }
    stop_worker();
    downloading_ = true;
    set_buttons();
    for (const std::size_t i : picked) {
        outcome_[i].clear();
    }
    InvalidateRect(list_, nullptr, TRUE);
    SetWindowTextW(status_, L"Starting…");
    const fs::path folder = dated_folder(L"downloads");
    HWND target = hwnd_;
    worker_ = std::thread([target, this, folder, picked, rows = std::move(rows), listed = listed_] {
        auto done = std::make_unique<FetchDone>();
        done->folder = folder;
        done->rows = picked;
        const auto say = [target](std::wstring text) {
            auto t = std::make_unique<std::wstring>(std::move(text));
            if (PostMessageW(target, kMsgFetchStep, 0, reinterpret_cast<LPARAM>(t.get())) != FALSE) {
                (void)t.release();
            }
        };
        auto http = core::make_system_transport();
        for (std::size_t n = 0; n < rows.size(); ++n) {
            const mt::UpdateRow& r = rows[n];
            const std::wstring count = L"(" + std::to_wstring(n + 1) + L" of " + std::to_wstring(rows.size()) + L") ";
            if (cancel_) {
                done->stopped = true;
                done->problems.emplace_back("stopped");
                done->notes.emplace_back();
                continue;
            }
            if (!http) {
                done->problems.emplace_back("no HTTPS on this computer");
                done->notes.emplace_back();
                continue;
            }
            say(count + widen(r.title) + L" " + widen(r.latest) + L": finding its files…");
            const mt::FetchPlan plan = mt::plan_fetch(*http, r, listed, &cancel_);
            if (!plan.problem.empty()) {
                done->problems.push_back(cancel_ ? std::string("stopped") : plan.problem);
                done->notes.push_back(plan.note);
                continue;
            }
            auto last = std::chrono::steady_clock::now() - std::chrono::seconds{1};
            const mt::FetchResult got = mt::fetch_files(
                *http, plan, folder, &cancel_, [&](const mt::FetchProgress& p) {
                    // A few lines a second, not one per chunk.
                    const auto now = std::chrono::steady_clock::now();
                    if (p.bytes != 0 && now - last < std::chrono::milliseconds{250}) {
                        return;
                    }
                    last = now;
                    say(count + widen(p.title) + L": " + widen(p.step) +
                        (p.bytes != 0 ? L" " + megabytes(p.bytes) : std::wstring(L"…")));
                });
            done->cdn_paths.insert(done->cdn_paths.end(), got.cdn_paths.begin(), got.cdn_paths.end());
            done->problems.push_back(got.problem);
            done->notes.push_back(plan.note);
        }
        done->stopped = done->stopped || cancel_;
        if (!done->cdn_paths.empty()) {
            std::ofstream steps(folder / L"upload-steps.txt", std::ios::binary | std::ios::trunc);
            steps << mt::upload_steps(done->cdn_paths, folder);
        }
        if (PostMessageW(target, kMsgFetched, 0, reinterpret_cast<LPARAM>(done.get())) != FALSE) {
            (void)done.release();
        }
    });
}

void UpdatesWindow::fetched(FetchDone& done) {
    int ok = 0;
    int failed = 0;
    for (std::size_t n = 0; n < done.rows.size(); ++n) {
        const std::size_t i = done.rows[n];
        if (i >= outcome_.size()) {
            continue;
        }
        const std::string& problem = done.problems.at(n);
        const std::string& note = done.notes.at(n);
        outcome_ok_[i] = problem.empty();
        if (problem.empty()) {
            outcome_[i] = L"Downloaded, ready to upload" + (note.empty() ? std::wstring() : L" (" + widen(note) + L")");
            rows_[i].ticked = false;
            ++ok;
        } else {
            outcome_[i] = L"Not downloaded: " + widen(problem);
            ++failed;
        }
    }
    fill();
    std::wstring status;
    if (ok > 0) {
        status = std::to_wstring(ok) + (ok == 1 ? L" version downloaded" : L" versions downloaded");
        if (failed > 0) {
            status += L", " + std::to_wstring(failed) + L" not";
        }
        status += L". Upload the folder's files as upload-steps.txt says, then Publish.";
    } else {
        status = done.stopped ? L"Stopped. Nothing was downloaded." : L"Nothing was downloaded: see each row.";
    }
    SetWindowTextW(status_, status.c_str());
    if (done.cdn_paths.empty()) {
        // The folder was made for this run: without a file in it, it goes.
        std::error_code ec;
        bool any_file = false;
        for (auto it = fs::recursive_directory_iterator(done.folder, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            any_file = any_file || it->is_regular_file(ec);
        }
        if (!any_file) {
            fs::remove_all(done.folder, ec);
        }
        return;
    }
    // Into the next list, so Publish checks them once they're on the CDN.
    if (on_fetched) {
        const std::wstring added = on_fetched(done.cdn_paths);
        if (!added.empty()) {
            MessageBoxW(hwnd_, added.c_str(), L"FloServer Publisher", MB_OK | MB_ICONINFORMATION);
        }
    }
    ShellExecuteW(hwnd_, L"explore", done.folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace floserver::publisher

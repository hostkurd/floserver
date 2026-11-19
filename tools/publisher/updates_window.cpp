#include "updates_window.h"

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
#include <filesystem>

namespace floserver::publisher {

namespace {

namespace fs = std::filesystem;
namespace mt = floserver::manifest_tool;
using floserver::ui::win::int_to_ptr;
using floserver::ui::win::widen;

constexpr UINT kMsgAsking = WM_APP + 20;  // lParam: std::string* (the runtime being asked)
constexpr UINT kMsgFound = WM_APP + 21;   // lParam: std::map<std::string, FinderAnswer>*

enum : int { IdList = 300, IdKeep, IdCheckAgain, IdGet, IdClose };

constexpr COLORREF kWindowBg = RGB(0xF3, 0xF3, 0xF3);
constexpr COLORREF kText = RGB(0x1B, 0x1B, 0x1B);
constexpr COLORREF kMuted = RGB(0x5F, 0x5F, 0x5F);
constexpr COLORREF kBlue = RGB(0x0B, 0x53, 0x94);
constexpr COLORREF kAmber = RGB(0x7A, 0x4A, 0x00);
constexpr COLORREF kRed = RGB(0xB3, 0x26, 0x1E);

// Documents\FloServer Publisher\site answers <date> <time>.
fs::path answers_folder() {
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
    return base / L"FloServer Publisher" / (std::wstring(L"site answers ") + stamp.data());
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
    if (!same && !busy_) {
        check();
    } else if (busy_ && listed != listed_) {
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
    put(check_again_, pad + w - px(90) - px(8) - px(120) - px(8) - px(110), buttons_y, px(110), px(30));
    put(keep_answers_, pad, buttons_y - px(30), w, px(24));
    put(status_, pad, buttons_y + px(6), w - px(90 + 8 + 120 + 8 + 110 + 8), px(20));
    const int list_top = px(78);
    put(list_, pad, list_top, w, buttons_y - px(36) - list_top);
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
    if (busy_) {
        return;
    }
    stop_worker();
    busy_ = true;
    EnableWindow(check_again_, FALSE);
    rows_.clear();
    ListView_DeleteAllItems(list_);
    SetWindowTextW(status_, L"Asking each site…");
    saved_to_.clear();
    if (Button_GetCheck(keep_answers_) == BST_CHECKED) {
        const fs::path folder = answers_folder();
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
        const std::wstring kind = kind_text(r);
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
    status += L". Getting them comes in the next Publisher.";
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
        if (row < rows_.size() && cd->iSubItem == 3) {
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
        for (HWND c : {hint_, list_, status_, keep_answers_, check_again_, get_, close_}) {
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
                if (ticked && !actionable) {
                    ListView_SetCheckState(list_, lv->iItem, FALSE);  // nothing to get
                } else {
                    rows_[row].ticked = ticked;
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
        EnableWindow(check_again_, TRUE);
        rows_ = mt::compare_updates(listed_, *found);
        fill();
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace floserver::publisher

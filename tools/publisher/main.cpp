// FloServer Publisher (D18): makes the next signed runtime list in one go.
// For HostKurd's release routine, not shipped to users. English only.
//
// The user uploads runtime files to the bucket by hand, adds their CDN paths
// here and presses Publish: the files are checked (publish.h), the next list
// is built and signed with the key file picked for this run, and
// manifest.json + manifest.json.sig are written to a dated folder under
// Documents\FloServer Publisher. Uploading them and the CloudFront
// invalidation stay by hand.
//
// The key file's path is kept only while the window is open; its contents
// are read just before signing and wiped right after.

#include "build.h"
#include "publish.h"
#include "signing.h"
#include "templates_json.h"
#include "win_util.h"
#include "floserver/core/builtin.h"
#include "floserver/core/http.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace mt = floserver::manifest_tool;
using floserver::ui::win::int_to_ptr;
using floserver::ui::win::narrow;
using floserver::ui::win::widen;

constexpr UINT kMsgLive = WM_APP + 1;      // lParam: LiveResult*
constexpr UINT kMsgRow = WM_APP + 2;       // wParam: row, lParam: PublishFile*
constexpr UINT kMsgProgress = WM_APP + 3;  // wParam: row, lParam: bytes (capped)
constexpr UINT kMsgDone = WM_APP + 4;      // lParam: Outcome*
constexpr UINT kMsgStep = WM_APP + 5;      // wParam: step being worked on

enum Id : int {
    IdCheckLive = 100,
    IdPath,
    IdAdd,
    IdList,
    IdRemove,
    IdAccept,
    IdDetail,
    IdChooseKey,
    IdPublish,
    IdOpenFolder,
    IdCopyPaths,
    IdDot0 = 200,  // four step dots
};

constexpr COLORREF kWindowBg = RGB(0xF3, 0xF3, 0xF3);
constexpr COLORREF kCard = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kCardEdge = RGB(0xE1, 0xE1, 0xE1);
constexpr COLORREF kText = RGB(0x1B, 0x1B, 0x1B);
constexpr COLORREF kMuted = RGB(0x5F, 0x5F, 0x5F);
constexpr COLORREF kGreen = RGB(0x0E, 0x70, 0x0E);
constexpr COLORREF kGreenBg = RGB(0xDF, 0xF6, 0xDD);
constexpr COLORREF kBlue = RGB(0x0B, 0x53, 0x94);
constexpr COLORREF kBlueBg = RGB(0xE8, 0xF1, 0xFB);
constexpr COLORREF kRed = RGB(0xB3, 0x26, 0x1E);
constexpr COLORREF kRedBg = RGB(0xFD, 0xEC, 0xEA);
constexpr COLORREF kAmber = RGB(0x7A, 0x4A, 0x00);
constexpr COLORREF kGrayBg = RGB(0xF0, 0xF0, 0xF0);

enum class StepState : std::uint8_t { Waiting, Working, Done, Failed };

struct Outcome {
    std::array<StepState, 4> steps{};
    std::array<std::string, 4> details;
    std::string error;  // shown in a message box when set
    fs::path folder;    // where the files went, when published
    std::int64_t sequence = 0;
    std::vector<mt::PublishFile> files;
};

std::string month_day_year(std::chrono::sys_seconds t) {
    static constexpr std::array<const char*, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(t)};
    return std::to_string(static_cast<unsigned>(ymd.day())) + " " +
           kMonths.at(static_cast<unsigned>(ymd.month()) - 1) + " " +
           std::to_string(static_cast<int>(ymd.year()));
}

std::chrono::sys_seconds now_utc() {
    return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
}

// Reads the key file straight into the string that gets wiped, with no
// stream buffer keeping a copy.
std::optional<std::string> read_key_file(const fs::path& p) {
    constexpr DWORD kMax = 4096;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::string data(kMax + 1, '\0');
    DWORD total = 0;
    bool ok = true;
    while (total < kMax + 1) {
        DWORD got = 0;
        if (!ReadFile(h, data.data() + total, kMax + 1 - total, &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        total += got;
    }
    CloseHandle(h);
    if (!ok || total > kMax) {
        SecureZeroMemory(data.data(), data.size());
        return std::nullopt;
    }
    // Shrinking never moves the characters, so the one buffer is what gets wiped.
    data.resize(total);
    return data;
}

fs::path documents_folder() {
    PWSTR path = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path))) {
        out = path;
    }
    CoTaskMemFree(path);
    return out;
}

BOOL CALLBACK set_font(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
    return TRUE;
}

// A folder in Documents\FloServer Publisher holding a list with this sequence.
std::optional<fs::path> made_before(std::int64_t sequence) {
    const fs::path base = documents_folder() / L"FloServer Publisher";
    const std::wstring tag = L" sequence " + std::to_wstring(sequence);
    std::error_code ec;
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        const auto at = name.find(tag);
        const std::size_t after = at == std::wstring::npos ? at : at + tag.size();
        if (at != std::wstring::npos && (after == name.size() || name[after] == L' ') &&
            fs::exists(it->path() / L"manifest.json.sig")) {
            return it->path();
        }
    }
    return std::nullopt;
}

class Publisher {
public:
    Publisher() = default;
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    Publisher(Publisher&&) = delete;
    Publisher& operator=(Publisher&&) = delete;
    ~Publisher() { stop_worker(); }

    bool create(HINSTANCE instance, int show);
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd() const { return hwnd_; }

private:
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void create_controls();
    void make_fonts();
    void layout();
    void paint();
    int px(int dip) const { return MulDiv(dip, static_cast<int>(dpi_), 96); }

    void check_live();
    void add_path();
    void remove_selected();
    void choose_key();
    void publish();
    void stop_worker();

    void on_live(std::unique_ptr<mt::LiveResult> live);
    void on_done(std::unique_ptr<Outcome> outcome);
    void fill_list();
    void update_row(int row);
    void update_selection();
    void update_steps();
    void set_busy(bool busy);
    void reset_steps();
    int selected_row() const;
    std::wstring check_text(int row) const;
    LRESULT list_custom_draw(NMLVCUSTOMDRAW* cd) const;
    void draw_dot(const DRAWITEMSTRUCT* di) const;

    HWND hwnd_ = nullptr;
    HWND live_label_ = nullptr, live_value_ = nullptr, expires_label_ = nullptr, expires_value_ = nullptr;
    HWND check_live_ = nullptr;
    HWND files_heading_ = nullptr, files_hint_ = nullptr, path_ = nullptr, add_ = nullptr, list_ = nullptr;
    HWND remove_ = nullptr, accept_ = nullptr, detail_ = nullptr;
    HWND steps_heading_ = nullptr;
    std::array<HWND, 4> dots_{}, step_titles_{}, step_details_{};
    HWND choose_key_ = nullptr, key_name_ = nullptr, publish_ = nullptr;
    HWND open_folder_ = nullptr, copy_paths_ = nullptr;

    UINT dpi_ = 96;
    std::unique_ptr<std::remove_pointer_t<HFONT>, floserver::ui::win::GdiDeleter> font_, bold_, heading_, mono_;
    std::unique_ptr<std::remove_pointer_t<HBRUSH>, floserver::ui::win::GdiDeleter> bg_brush_, card_brush_;
    RECT top_card_{}, files_card_{}, steps_card_{};

    std::optional<mt::LiveList> live_;
    std::string live_error_;
    std::vector<mt::PublishFile> files_;
    int working_row_ = -1;
    std::uint64_t working_bytes_ = 0;
    std::array<StepState, 4> steps_{};
    std::array<std::string, 4> step_details_text_;
    fs::path key_file_;
    fs::path last_folder_;
    bool live_busy_ = false;
    std::vector<std::string> live_notes_;  // live runtimes that can't be carried over
    std::set<std::string> removed_;        // published files the user left out
    std::int64_t signed_sequence_ = 0;     // the last list made in this window

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    bool busy_ = false;
};

// ---- window ----

bool Publisher::create(HINSTANCE instance, int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Publisher::proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"FloServerPublisher";
    if (RegisterClassExW(&wc) == 0) {
        return false;
    }
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"FloServer Publisher",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            980, 660, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        return false;
    }
    dpi_ = GetDpiForWindow(hwnd_);
    make_fonts();
    create_controls();
    // 980 x 660 at this window's DPI, client area.
    RECT r{0, 0, px(980), px(660)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    layout();
    ShowWindow(hwnd_, show);
    check_live();
    return true;
}

LRESULT CALLBACK Publisher::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = int_to_ptr<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<Publisher*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = int_to_ptr<Publisher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->handle(msg, wp, lp);
}

void Publisher::make_fonts() {
    const auto font = [this](int pt, int weight, const wchar_t* face) {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, static_cast<int>(dpi_), 72);
        lf.lfWeight = weight;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, face);
        return CreateFontIndirectW(&lf);
    };
    font_.reset(font(9, FW_NORMAL, L"Segoe UI"));
    bold_.reset(font(9, FW_SEMIBOLD, L"Segoe UI"));
    heading_.reset(font(11, FW_SEMIBOLD, L"Segoe UI"));
    mono_.reset(font(9, FW_NORMAL, L"Consolas"));
    bg_brush_.reset(CreateSolidBrush(kWindowBg));
    card_brush_.reset(CreateSolidBrush(kCard));
}

void Publisher::create_controls() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    const auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT font,
                          DWORD ex = 0) {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                                 int_to_ptr<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return h;
    };
    const auto label = [&](const wchar_t* text, HFONT font, DWORD style = 0) {
        return make(L"STATIC", text, SS_LEFT | SS_NOPREFIX | style, -1, font);
    };

    live_label_ = label(L"Live list on cdn.floserver.org", font_.get());
    live_value_ = label(L"Reading…", bold_.get(), SS_ENDELLIPSIS);
    expires_label_ = label(L"Expires", font_.get(), SS_RIGHT);
    expires_value_ = label(L"", font_.get(), SS_RIGHT);
    check_live_ = make(L"BUTTON", L"Check live", WS_TABSTOP | BS_PUSHBUTTON, IdCheckLive, font_.get());

    files_heading_ = label(L"Files in the next list", heading_.get());
    files_hint_ = label(L"After uploading a file to the bucket, add its path", font_.get(), SS_RIGHT);
    path_ = make(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, IdPath, mono_.get());
    SendMessageW(path_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"php/8.5.13/php-8.5.13-nts-Win32-vs17-x64.zip"));
    add_ = make(L"BUTTON", L"Add", WS_TABSTOP | BS_PUSHBUTTON, IdAdd, font_.get());
    list_ = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                 IdList, font_.get(), WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const std::array<std::pair<const wchar_t*, int>, 4> columns = {{
        {L"Runtime", 130}, {L"File on the CDN", 300}, {L"Size", 75}, {L"Check", 170}}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c{};
        c.mask = static_cast<UINT>(LVCF_TEXT | LVCF_WIDTH | (i == 2 ? LVCF_FMT : 0));
        c.fmt = LVCFMT_RIGHT;
        c.pszText = const_cast<wchar_t*>(columns.at(static_cast<std::size_t>(i)).first);
        c.cx = px(columns.at(static_cast<std::size_t>(i)).second);
        ListView_InsertColumn(list_, i, &c);
    }
    remove_ = make(L"BUTTON", L"Remove", WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, IdRemove, font_.get());
    accept_ = make(L"BUTTON", L"I compared its SHA-256 by hand and it matches", WS_TABSTOP | BS_AUTOCHECKBOX | WS_DISABLED,
                   IdAccept, font_.get());
    detail_ = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IdDetail, font_.get());

    steps_heading_ = label(L"Publish", heading_.get());
    const std::array<const wchar_t*, 4> titles = {L"Check files", L"Build the list", L"Sign", L"Files to upload"};
    for (std::size_t i = 0; i < 4; ++i) {
        dots_.at(i) = make(L"STATIC", L"", SS_OWNERDRAW, IdDot0 + static_cast<int>(i), bold_.get());
        step_titles_.at(i) = label(titles.at(i), bold_.get());
        step_details_.at(i) = label(L"", font_.get());
    }
    step_details_text_ = {"New files are compared with their official checksums; the others must still be served",
                          "Next sequence, expires in 90 days, with the built-in templates",
                          "The key is read, used and forgotten",
                          "manifest.json and manifest.json.sig in a dated folder, opened for you, with the CloudFront paths to copy"};
    choose_key_ = make(L"BUTTON", L"Choose key file…", WS_TABSTOP | BS_PUSHBUTTON, IdChooseKey, font_.get());
    key_name_ = label(L"", font_.get(), SS_ENDELLIPSIS);
    open_folder_ = make(L"BUTTON", L"Open folder", WS_TABSTOP | BS_PUSHBUTTON, IdOpenFolder, font_.get());
    copy_paths_ = make(L"BUTTON", L"Copy invalidation paths", WS_TABSTOP | BS_PUSHBUTTON, IdCopyPaths, font_.get());
    ShowWindow(open_folder_, SW_HIDE);
    ShowWindow(copy_paths_, SW_HIDE);
    publish_ = make(L"BUTTON", L"Publish", WS_TABSTOP | BS_PUSHBUTTON, IdPublish, bold_.get());
    update_steps();
}

void Publisher::layout() {
    if (list_ == nullptr) {
        return;  // WM_SIZE while the window is being made
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int w = client.right;
    const int h = client.bottom;
    const int pad = px(20);
    const int gap = px(12);
    const auto put = [](HWND c, int x, int y, int cw, int ch) {
        SetWindowPos(c, nullptr, x, y, std::max(cw, 0), std::max(ch, 0), SWP_NOZORDER | SWP_NOACTIVATE);
    };

    // Live status card.
    top_card_ = {pad, px(12), w - pad, px(12) + px(60)};
    const int ty = top_card_.top + px(12);
    const int button_w = px(110);
    put(check_live_, top_card_.right - px(16) - button_w, top_card_.top + px(14), button_w, px(32));
    const int exp_right = top_card_.right - px(16) - button_w - px(16);
    put(expires_label_, exp_right - px(260), ty, px(260), px(18));
    put(expires_value_, exp_right - px(260), ty + px(19), px(260), px(20));
    put(live_label_, top_card_.left + px(16), ty, exp_right - px(276) - top_card_.left, px(18));
    put(live_value_, top_card_.left + px(16), ty + px(19), exp_right - px(276) - top_card_.left, px(20));

    // Steps card on the right, files card on the left.
    const int body_top = top_card_.bottom + gap;
    const int body_bottom = h - px(16);
    const int steps_w = px(330);
    steps_card_ = {w - pad - steps_w, body_top, w - pad, body_bottom};
    files_card_ = {pad, body_top, steps_card_.left - gap, body_bottom};

    const int fx = files_card_.left + px(12);
    const int fw = files_card_.right - files_card_.left - px(24);
    put(files_heading_, fx, body_top + px(12), px(200), px(24));
    put(files_hint_, fx + px(200), body_top + px(15), fw - px(200), px(18));
    put(path_, fx, body_top + px(42), fw - px(80), px(30));
    put(add_, fx + fw - px(72), body_top + px(41), px(72), px(32));
    const int detail_h = px(54);
    const int bottom_row = body_bottom - px(12) - px(30);
    const int list_top = body_top + px(84);
    const int list_bottom = bottom_row - detail_h - px(8);
    put(list_, fx, list_top, fw, list_bottom - list_top);
    // The file column takes what the others leave.
    const int fixed = ListView_GetColumnWidth(list_, 0) + ListView_GetColumnWidth(list_, 2) +
                      ListView_GetColumnWidth(list_, 3) + GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_) + px(4);
    ListView_SetColumnWidth(list_, 1, std::max(px(120), fw - fixed));
    put(detail_, fx, list_bottom + px(8), fw, detail_h);
    put(remove_, fx, bottom_row, px(90), px(30));
    put(accept_, fx + px(104), bottom_row, fw - px(104), px(30));

    const int sx = steps_card_.left + px(16);
    const int sw = steps_w - px(32);
    put(steps_heading_, sx, body_top + px(12), sw, px(24));
    int y = body_top + px(44);
    for (std::size_t i = 0; i < 4; ++i) {
        put(dots_.at(i), sx, y + px(1), px(22), px(22));
        put(step_titles_.at(i), sx + px(32), y + px(2), sw - px(32), px(20));
        // Height for the detail's text at this width.
        const std::wstring text = widen(step_details_text_.at(i));
        HDC dc = GetDC(hwnd_);
        HGDIOBJ old = SelectObject(dc, font_.get());
        RECT tr{0, 0, sw - px(32), 0};
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        const int dh = std::max(static_cast<int>(tr.bottom), px(16));
        put(step_details_.at(i), sx + px(32), y + px(24), sw - px(32), dh);
        y += px(24) + dh + px(8);
        if (i == 2) {
            put(choose_key_, sx + px(32), y, px(130), px(28));
            put(key_name_, sx + px(32) + px(138), y + px(6), sw - px(32) - px(138), px(18));
            y += px(36);
        }
        if (i == 3 && IsWindowVisible(open_folder_)) {
            put(open_folder_, sx + px(32), y, px(100), px(28));
            put(copy_paths_, sx + px(32) + px(108), y, sw - px(32) - px(108), px(28));
            y += px(36);
        }
        y += px(6);
    }
    put(publish_, sx, body_bottom - px(16) - px(36), sw, px(36));
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void Publisher::paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    FillRect(dc, &ps.rcPaint, bg_brush_.get());
    HPEN pen = CreatePen(PS_SOLID, 1, kCardEdge);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, card_brush_.get());
    for (const RECT& r : {top_card_, files_card_, steps_card_}) {
        RoundRect(dc, r.left, r.top, r.right, r.bottom, px(12), px(12));
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    EndPaint(hwnd_, &ps);
}

void Publisher::draw_dot(const DRAWITEMSTRUCT* di) const {
    const auto i = static_cast<std::size_t>(static_cast<int>(di->CtlID) - IdDot0);
    const StepState s = steps_.at(i);
    COLORREF bg = kGrayBg;
    COLORREF fg = kMuted;
    std::wstring text = std::to_wstring(i + 1);
    if (s == StepState::Done) {
        bg = kGreenBg;
        fg = kGreen;
        text = L"✓";
    } else if (s == StepState::Failed) {
        bg = kRedBg;
        fg = kRed;
        text = L"!";
    } else if (s == StepState::Working) {
        bg = kBlueBg;
        fg = kBlue;
    }
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    FillRect(dc, &r, card_brush_.get());
    HBRUSH b = CreateSolidBrush(bg);
    HGDIOBJ old_brush = SelectObject(dc, b);
    HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, r.left, r.top, r.right + 1, r.bottom + 1);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    HGDIOBJ old_font = SelectObject(dc, bold_.get());
    DrawTextW(dc, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);
}

LRESULT Publisher::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = int_to_ptr<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {px(820), px(560)};
        return 0;
    }
    case WM_DPICHANGED: {
        const UINT old_dpi = dpi_;
        dpi_ = HIWORD(wp);
        for (int c = 0; c < 4; ++c) {
            ListView_SetColumnWidth(list_, c, MulDiv(ListView_GetColumnWidth(list_, c), static_cast<int>(dpi_),
                                                    static_cast<int>(old_dpi)));
        }
        make_fonts();
        // Every control gets the new normal font, then the bold and heading ones theirs.
        EnumChildWindows(hwnd_, &set_font, reinterpret_cast<LPARAM>(font_.get()));
        SendMessageW(live_value_, WM_SETFONT, reinterpret_cast<WPARAM>(bold_.get()), TRUE);
        for (HWND h : step_titles_) {
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(bold_.get()), TRUE);
        }
        SendMessageW(publish_, WM_SETFONT, reinterpret_cast<WPARAM>(bold_.get()), TRUE);
        SendMessageW(files_heading_, WM_SETFONT, reinterpret_cast<WPARAM>(heading_.get()), TRUE);
        SendMessageW(steps_heading_, WM_SETFONT, reinterpret_cast<WPARAM>(heading_.get()), TRUE);
        SendMessageW(path_, WM_SETFONT, reinterpret_cast<WPARAM>(mono_.get()), TRUE);
        const auto* r = int_to_ptr<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = int_to_ptr<HDC>(wp);
        const auto ctl = int_to_ptr<HWND>(lp);
        SetBkColor(dc, kCard);
        COLORREF color = kText;
        if (ctl == live_label_ || ctl == expires_label_ || ctl == files_hint_ || ctl == key_name_ ||
            std::find(step_details_.begin(), step_details_.end(), ctl) != step_details_.end()) {
            color = kMuted;
        }
        if (ctl == expires_value_ && live_) {
            const auto left = live_->expires - now_utc();
            color = live_->expired ? kRed : (left < std::chrono::days{14} ? kAmber : kGreen);
        }
        if (ctl == live_value_ && !live_error_.empty()) {
            color = kRed;
        }
        for (std::size_t i = 0; i < 4; ++i) {
            if (ctl == step_details_.at(i) && steps_.at(i) == StepState::Failed) {
                color = kRed;
            }
            if (ctl == step_titles_.at(i) && steps_.at(i) == StepState::Waiting) {
                color = kMuted;
            }
        }
        SetTextColor(dc, color);
        return reinterpret_cast<LRESULT>(card_brush_.get());
    }
    case WM_DRAWITEM: {
        const auto* di = int_to_ptr<const DRAWITEMSTRUCT*>(lp);
        if (di->CtlID >= IdDot0 && di->CtlID < IdDot0 + 4) {
            draw_dot(di);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        const auto* nm = int_to_ptr<const NMHDR*>(lp);
        if (nm->hwndFrom == list_) {
            if (nm->code == NM_CUSTOMDRAW) {
                return list_custom_draw(int_to_ptr<NMLVCUSTOMDRAW*>(lp));
            }
            if (nm->code == LVN_ITEMCHANGED) {
                update_selection();
            }
            if (nm->code == LVN_KEYDOWN && int_to_ptr<const NMLVKEYDOWN*>(lp)->wVKey == VK_DELETE) {
                remove_selected();
            }
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            // Enter adds a typed path; Publish needs its own button.
            if (GetFocus() == path_) {
                add_path();
            }
            return 0;
        case IdCheckLive:
            check_live();
            return 0;
        case IdAdd:
            add_path();
            return 0;
        case IdRemove:
            remove_selected();
            return 0;
        case IdAccept: {
            const int row = selected_row();
            if (row >= 0) {
                files_.at(static_cast<std::size_t>(row)).accepted =
                    Button_GetCheck(accept_) == BST_CHECKED;
                update_row(row);
            }
            return 0;
        }
        case IdChooseKey:
            choose_key();
            return 0;
        case IdPublish:
            if (busy_) {
                cancel_ = true;
                EnableWindow(publish_, FALSE);
                SetWindowTextW(publish_, L"Stopping…");
            } else {
                publish();
            }
            return 0;
        case IdOpenFolder:
            if (!last_folder_.empty()) {
                ShellExecuteW(hwnd_, L"explore", last_folder_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        case IdCopyPaths: {
            const std::wstring text = L"/manifest.json\r\n/manifest.json.sig\r\n";
            if (OpenClipboard(hwnd_)) {
                EmptyClipboard();
                const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
                    std::memcpy(GlobalLock(mem), text.c_str(), bytes);
                    GlobalUnlock(mem);
                    if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
                        GlobalFree(mem);
                    }
                }
                CloseClipboard();
                SetWindowTextW(copy_paths_, L"Copied");
            }
            return 0;
        }
        default:
            break;
        }
        break;
    case kMsgLive:
        on_live(std::unique_ptr<mt::LiveResult>(int_to_ptr<mt::LiveResult*>(lp)));
        return 0;
    case kMsgRow: {
        std::unique_ptr<mt::PublishFile> f(int_to_ptr<mt::PublishFile*>(lp));
        const auto row = static_cast<std::size_t>(wp);
        if (row < files_.size()) {
            files_.at(row) = *f;  // the worker decides whether a tick still holds
            working_row_ = -1;
            update_row(static_cast<int>(row));
            update_selection();
        }
        return 0;
    }
    case kMsgProgress:
        working_row_ = static_cast<int>(wp);
        working_bytes_ = static_cast<std::uint64_t>(lp);
        update_row(working_row_);
        return 0;
    case kMsgStep:
        for (std::size_t i = 0; i < 4; ++i) {
            if (i == wp) {
                steps_.at(i) = StepState::Working;
            }
        }
        update_steps();
        return 0;
    case kMsgDone:
        on_done(std::unique_ptr<Outcome>(int_to_ptr<Outcome*>(lp)));
        return 0;
    case WM_CLOSE:
        if (busy_ && MessageBoxW(hwnd_, L"Stop the work in progress and close?", L"FloServer Publisher",
                                 MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return 0;
        }
        stop_worker();
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---- list ----

std::wstring Publisher::check_text(int row) const {
    const mt::PublishFile& f = files_.at(static_cast<std::size_t>(row));
    if (row == working_row_) {
        return f.published ? L"Checking…" : L"Downloading… " + widen(mt::format_size(working_bytes_));
    }
    switch (f.state) {
    case mt::CheckState::NotRun:
        return f.published ? L"Published" : L"New · full check";
    case mt::CheckState::Ok:
        return f.published ? L"Published · served" : L"New · matches";
    case mt::CheckState::ByHand:
        return f.accepted ? L"Compared by hand" : L"Compare by hand";
    case mt::CheckState::Failed:
        return L"Failed";
    }
    return {};
}

LRESULT Publisher::list_custom_draw(NMLVCUSTOMDRAW* cd) const {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        const auto row = static_cast<std::size_t>(cd->nmcd.dwItemSpec);
        cd->clrText = kText;
        if (cd->iSubItem == 3 && row < files_.size()) {
            const mt::PublishFile& f = files_.at(row);
            switch (f.state) {
            case mt::CheckState::NotRun:
                cd->clrText = f.published ? kGreen : kBlue;
                break;
            case mt::CheckState::Ok:
                cd->clrText = kGreen;
                break;
            case mt::CheckState::ByHand:
                cd->clrText = f.accepted ? kGreen : kAmber;
                break;
            case mt::CheckState::Failed:
                cd->clrText = kRed;
                break;
            }
        }
        return CDRF_DODEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

void Publisher::fill_list() {
    const int keep = selected_row();
    ListView_DeleteAllItems(list_);
    for (std::size_t i = 0; i < files_.size(); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        std::wstring title = widen(files_.at(i).title);
        item.pszText = title.data();
        ListView_InsertItem(list_, &item);
        update_row(static_cast<int>(i));
    }
    if (keep >= 0 && keep < static_cast<int>(files_.size())) {
        ListView_SetItemState(list_, keep, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    update_selection();
}

void Publisher::update_row(int row) {
    if (row < 0 || row >= static_cast<int>(files_.size())) {
        return;
    }
    const mt::PublishFile& f = files_.at(static_cast<std::size_t>(row));
    std::wstring path = widen(f.path);
    std::wstring size = f.size == 0 ? std::wstring(L"—") : widen(mt::format_size(f.size));
    std::wstring check = check_text(row);
    ListView_SetItemText(list_, row, 1, path.data());
    ListView_SetItemText(list_, row, 2, size.data());
    ListView_SetItemText(list_, row, 3, check.data());
}

int Publisher::selected_row() const {
    return ListView_GetNextItem(list_, -1, LVNI_SELECTED);
}

void Publisher::update_selection() {
    const int row = selected_row();
    if (row < 0 || row >= static_cast<int>(files_.size())) {
        SetWindowTextW(detail_, L"");
        EnableWindow(remove_, FALSE);
        EnableWindow(accept_, FALSE);
        Button_SetCheck(accept_, BST_UNCHECKED);
        return;
    }
    const mt::PublishFile& f = files_.at(static_cast<std::size_t>(row));
    std::string text = f.path;
    if (!f.detail.empty()) {
        text += "\r\n" + f.detail;
    } else if (f.published) {
        text += "\r\nIn the live list. Publish checks it's still served.";
    } else {
        text += "\r\nNew. Publish downloads it from the CDN and compares it with the official checksum.";
    }
    if (!f.sha256.empty() && f.detail.find(f.sha256) == std::string::npos) {
        text += "\r\nSHA-256 " + f.sha256;
    }
    SetWindowTextW(detail_, widen(text).c_str());
    EnableWindow(remove_, busy_ || live_busy_ ? FALSE : TRUE);
    const bool by_hand = f.state == mt::CheckState::ByHand;
    EnableWindow(accept_, by_hand && !busy_ ? TRUE : FALSE);
    Button_SetCheck(accept_, by_hand && f.accepted ? BST_CHECKED : BST_UNCHECKED);
}

void Publisher::update_steps() {
    if (live_) {
        SetWindowTextW(steps_heading_, (L"Publish sequence " + std::to_wstring(live_->sequence + 1)).c_str());
    } else {
        SetWindowTextW(steps_heading_, L"Publish");
    }
    for (std::size_t i = 0; i < 4; ++i) {
        SetWindowTextW(step_details_.at(i), widen(step_details_text_.at(i)).c_str());
        InvalidateRect(dots_.at(i), nullptr, TRUE);
        InvalidateRect(step_titles_.at(i), nullptr, TRUE);
    }
    std::wstring key = key_file_.empty() ? std::wstring(L"No key file chosen") : key_file_.filename().wstring();
    SetWindowTextW(key_name_, key.c_str());
    layout();
}

void Publisher::set_busy(bool busy) {
    busy_ = busy;
    for (HWND h : {check_live_, path_, add_, choose_key_}) {
        EnableWindow(h, busy ? FALSE : TRUE);
    }
    EnableWindow(publish_, TRUE);
    SetWindowTextW(publish_, busy ? L"Stop" : L"Publish");
    update_selection();
}

// ---- actions ----

void Publisher::check_live() {
    if (busy_ || live_busy_) {
        return;
    }
    live_busy_ = true;
    EnableWindow(check_live_, FALSE);
    EnableWindow(add_, FALSE);
    update_selection();
    SetWindowTextW(live_value_, L"Reading…");
    stop_worker();
    cancel_ = false;
    HWND target = hwnd_;
    worker_ = std::thread([target, this] {
        auto result = std::make_unique<mt::LiveResult>();
        if (auto http = floserver::core::make_system_transport()) {
            *result = mt::fetch_live_list(*http, mt::kCdn, floserver::core::builtin_manifest_keys(), now_utc(),
                                          &cancel_);
        } else {
            result->error = "No HTTPS on this computer";
        }
        PostMessageW(target, kMsgLive, 0, reinterpret_cast<LPARAM>(result.release()));
    });
}

void Publisher::on_live(std::unique_ptr<mt::LiveResult> live) {
    if (worker_.joinable()) {
        worker_.join();
    }
    live_busy_ = false;
    EnableWindow(check_live_, TRUE);
    EnableWindow(add_, TRUE);
    update_selection();
    if (!live->list) {
        live_error_ = live->error;
        SetWindowTextW(live_value_, widen(live->error).c_str());
        SetWindowTextW(expires_value_, L"");
        InvalidateRect(live_value_, nullptr, TRUE);
        update_steps();
        return;
    }
    live_error_.clear();
    live_ = std::move(live->list);
    std::vector<std::string> notes;
    std::vector<mt::PublishFile> next = mt::files_from_live(*live_, mt::kCdn, notes);
    live_notes_ = notes;
    // A file the user left out stays out.
    std::erase_if(next, [this](const mt::PublishFile& f) { return removed_.contains(f.path); });
    // New files added before stay, unless the live list has them now.
    for (const mt::PublishFile& f : files_) {
        if (!f.published && std::none_of(next.begin(), next.end(), [&](const mt::PublishFile& n) {
                return n.path == f.path || n.title == f.title;
            })) {
            next.push_back(f);
        }
    }
    files_ = std::move(next);
    fill_list();

    const std::string summary = "Sequence " + std::to_string(live_->sequence) + " · " +
                                std::to_string(live_->runtimes.size()) + " runtimes · " +
                                std::to_string(live_->template_count) + " templates";
    SetWindowTextW(live_value_, widen(summary).c_str());
    std::string expires = month_day_year(live_->expires);
    const auto left = std::chrono::floor<std::chrono::days>(live_->expires - now_utc()).count();
    expires += live_->expired ? " · EXPIRED" : " · " + std::to_string(left) + (left == 1 ? " day left" : " days left");
    SetWindowTextW(expires_value_, widen(expires).c_str());
    InvalidateRect(expires_value_, nullptr, TRUE);
    InvalidateRect(live_value_, nullptr, TRUE);
    if (!notes.empty()) {
        std::string text;
        for (const std::string& n : notes) {
            text += n + "\n";
        }
        text += "\nThe Publisher can't make the next list while these are in the live list.";
        MessageBoxW(hwnd_, widen(text).c_str(), L"FloServer Publisher", MB_OK | MB_ICONWARNING);
    }
    update_steps();
}

void Publisher::add_path() {
    if (busy_ || live_busy_) {
        return;
    }
    const int n = GetWindowTextLengthW(path_);
    std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(path_, text.data(), n + 1);
    text.resize(static_cast<std::size_t>(n));
    const mt::AddResult added = mt::new_file(narrow(text), mt::kCdn, files_);
    if (!added.file) {
        MessageBoxW(hwnd_, widen(added.error).c_str(), L"Can't add this file", MB_OK | MB_ICONWARNING);
        SetFocus(path_);
        return;
    }
    removed_.erase(added.file->path);
    files_.push_back(*added.file);
    SetWindowTextW(path_, L"");
    fill_list();
    const int row = static_cast<int>(files_.size()) - 1;
    ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list_, row, FALSE);
    reset_steps();
}

void Publisher::remove_selected() {
    const int row = selected_row();
    if (busy_ || live_busy_ || row < 0 || row >= static_cast<int>(files_.size())) {
        return;
    }
    // Copies: the list may change while the question is open.
    const std::string path = files_.at(static_cast<std::size_t>(row)).path;
    const bool published = files_.at(static_cast<std::size_t>(row)).published;
    if (published) {
        const std::wstring q = L"Leave " + widen(files_.at(static_cast<std::size_t>(row)).title) +
                               L" out of the next list?\n\nFloServer users can't download it any more once the "
                               L"new list is live. Projects that already have it keep working.";
        if (MessageBoxW(hwnd_, q.c_str(), L"FloServer Publisher", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
    }
    if (busy_ || live_busy_) {
        return;
    }
    const auto it = std::find_if(files_.begin(), files_.end(), [&](const mt::PublishFile& f) { return f.path == path; });
    if (it == files_.end()) {
        return;
    }
    if (published) {
        removed_.insert(path);
    }
    files_.erase(it);
    fill_list();
    reset_steps();
}

void Publisher::reset_steps() {
    steps_.fill(StepState::Waiting);
    step_details_text_ = {"New files are compared with their official checksums; the others must still be served",
                          "Next sequence, expires in 90 days, with the built-in templates",
                          "The key is read, used and forgotten",
                          "manifest.json and manifest.json.sig in a dated folder"};
    ShowWindow(open_folder_, SW_HIDE);
    ShowWindow(copy_paths_, SW_HIDE);
    update_steps();
}

void Publisher::choose_key() {
    std::array<wchar_t, MAX_PATH * 4> file{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Key files (*.key)\0*.key\0All files\0*.*\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrTitle = L"Choose the signing key file";
    // Not remembered in the recent-files lists.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        key_file_ = file.data();
        update_steps();
    }
}

void Publisher::stop_worker() {
    if (worker_.joinable()) {
        cancel_ = true;
        worker_.join();
    }
}

// The whole run, on the worker thread. Reports rows and steps as it goes.
std::unique_ptr<Outcome> run_publish(HWND target, std::vector<mt::PublishFile> files, std::int64_t live_sequence,
                                     const fs::path& key_file, const std::atomic<bool>& cancel) {
    auto out = std::make_unique<Outcome>();
    out->steps.fill(StepState::Waiting);
    const auto fail = [&](std::size_t step, std::string detail, std::string error = {}) {
        out->steps.at(step) = StepState::Failed;
        out->details.at(step) = std::move(detail);
        out->error = std::move(error);
        return std::move(out);
    };
    const auto step = [&](std::size_t i) { PostMessageW(target, kMsgStep, i, 0); };

    step(0);
    auto http = floserver::core::make_system_transport();
    if (!http) {
        return fail(0, "No HTTPS on this computer");
    }
    // The live list may have changed since it was read (published from elsewhere).
    const auto now = now_utc();
    const mt::LiveResult live =
        mt::fetch_live_list(*http, mt::kCdn, floserver::core::builtin_manifest_keys(), now, &cancel);
    if (cancel) {
        return fail(0, "Stopped");
    }
    if (!live.list) {
        return fail(0, live.error);
    }
    if (live.list->sequence != live_sequence) {
        return fail(0, "The live list changed (now sequence " + std::to_string(live.list->sequence) +
                           "). Press Check live, then publish again.");
    }
    if (live.list->key_id.empty()) {
        return fail(0, "The live list has no key id");
    }
    // The bucket must refuse direct reads. Any listed file will do; the
    // smallest published one keeps it quick. S3's Date (never cached, unlike
    // CloudFront's) is the clock check: the expiry is counted from this clock.
    const mt::PublishFile* probe = nullptr;
    for (const mt::PublishFile& f : files) {
        if (probe == nullptr || (f.published && (!probe->published || f.size < probe->size))) {
            probe = &f;
        }
    }
    if (probe == nullptr) {
        return fail(0, "There are no files");
    }
    const mt::S3Check s3 = mt::check_direct_s3(*http, mt::kDirectS3, probe->path, &cancel);
    if (cancel) {
        return fail(0, "Stopped");
    }
    if (!s3.problem.empty()) {
        return fail(0, s3.problem);
    }
    if (std::string clock = mt::clock_problem(s3.server_date, now); !clock.empty()) {
        return fail(0, clock);
    }

    mt::CheckContext ctx(*http, mt::kCdn);
    ctx.cancel = &cancel;
    std::size_t row = 0;
    auto last_post = std::chrono::steady_clock::now();
    ctx.progress = [&](std::uint64_t done) {
        const auto t = std::chrono::steady_clock::now();
        if (t - last_post > std::chrono::milliseconds{250}) {
            last_post = t;
            PostMessageW(target, kMsgProgress, row,
                         static_cast<LPARAM>(std::min<std::uint64_t>(done, 0x7fffffff)));
        }
    };
    std::size_t matched = 0;
    std::size_t served = 0;
    std::size_t problems = 0;
    for (row = 0; row < files.size(); ++row) {
        mt::PublishFile& f = files.at(row);
        // Every file is checked again on every run (it may have been
        // re-uploaded). A tick for a by-hand file holds only while its
        // SHA-256 stays the one that was compared.
        const std::string ticked_sha = f.accepted ? f.sha256 : std::string();
        PostMessageW(target, kMsgProgress, row, 0);
        mt::check_file(ctx, f);
        if (cancel) {
            return fail(0, "Stopped");
        }
        f.accepted = f.state == mt::CheckState::ByHand && !ticked_sha.empty() && f.sha256 == ticked_sha;
        PostMessageW(target, kMsgRow, row, reinterpret_cast<LPARAM>(new mt::PublishFile(f)));
        if (f.state == mt::CheckState::Ok) {
            (f.published ? served : matched) += 1;
        } else if (!(f.state == mt::CheckState::ByHand && f.accepted)) {
            ++problems;
        }
    }
    out->files = files;
    if (problems > 0) {
        return fail(0, std::to_string(problems) + (problems == 1 ? " file needs" : " files need") +
                           " attention: select the orange or red rows");
    }
    out->steps.at(0) = StepState::Done;
    out->details.at(0) = std::to_string(matched) + (matched == 1 ? " new file matches" : " new files match") +
                         " · " + std::to_string(served) + " still served · direct S3 refused";

    step(1);
    const mt::BuiltList built = mt::build_next_list(mt::kCdn, files, live_sequence + 1, now);
    if (!built.manifest_json) {
        std::string all;
        for (const std::string& e : built.errors) {
            all += e + "\n";
        }
        return fail(1, built.errors.empty() ? std::string("The list wasn't built") : built.errors.front(), all);
    }
    out->steps.at(1) = StepState::Done;
    out->details.at(1) = std::to_string(built.runtime_count) + " runtimes, " +
                         std::to_string(built.template_count) + " templates · expires " +
                         month_day_year(built.expires);

    step(2);
    mt::SignResult signed_list;
    {
        auto text = read_key_file(key_file);
        mt::SecretKey key;
        if (!text) {
            return fail(2, "Can't read the key file");
        }
        if (!mt::parse_secret_key(*text, key)) {  // wipes *text
            return fail(2, "That isn't a FloServer signing key file");
        }
        // FloServer counts sequences per key: a list signed with another key
        // would start a new count, and apps could refuse it.
        if (key.id != live.list->key_id) {
            return fail(2, "This key is \"" + key.id + "\", but the live list was signed with \"" +
                               live.list->key_id + "\". Choose that key file.");
        }
        signed_list = mt::sign_manifest(*built.manifest_json, key, now, floserver::core::builtin_manifest_keys());
    }  // the key is wiped here
    if (!signed_list.signature_file) {
        return fail(2, signed_list.error);
    }
    out->steps.at(2) = StepState::Done;
    out->details.at(2) = "Signed for FloServer, sequence " + std::to_string(signed_list.sequence);

    if (cancel) {
        return fail(2, "Stopped; nothing was written");
    }
    step(3);
    const fs::path docs = documents_folder();
    if (docs.empty()) {
        return fail(3, "Can't find the Documents folder");
    }
    const std::string day = mt::format_utc_time(now).substr(0, 10);
    const fs::path base = docs / L"FloServer Publisher";
    fs::path folder = base / widen(day + " sequence " + std::to_string(signed_list.sequence));
    for (int n = 2; fs::exists(folder) && n < 100; ++n) {
        folder = base / widen(day + " sequence " + std::to_string(signed_list.sequence) + " (" + std::to_string(n) + ")");
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    const auto write = [](const fs::path& p, const std::string& data) {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << data;
        f.close();
        return static_cast<bool>(f);
    };
    std::string steps_text =
        "FloServer runtime list, sequence " + std::to_string(signed_list.sequence) + ", made " +
        mt::format_utc_time(now) + "\r\n" + out->details.at(1) + "\r\n\r\n" +
        "1. Upload manifest.json and manifest.json.sig together to the top of the bucket,\r\n"
        "   replacing the old pair.\r\n"
        "2. In CloudFront, Invalidations -> Create invalidation, with these paths:\r\n"
        "   /manifest.json\r\n"
        "   /manifest.json.sig\r\n"
        "3. After a minute, press Check live in FloServer Publisher: it must show sequence " +
        std::to_string(signed_list.sequence) + ".\r\n";
    if (ec || !write(folder / L"manifest.json", *built.manifest_json) ||
        !write(folder / L"manifest.json.sig", *signed_list.signature_file) ||
        !write(folder / L"upload-steps.txt", steps_text)) {
        return fail(3, "Can't write the files to " + narrow(folder.wstring()));
    }
    out->steps.at(3) = StepState::Done;
    out->details.at(3) = "Saved in Documents\\FloServer Publisher\\" + narrow(folder.filename().wstring()) +
                         ". Upload both to the top of the bucket, then invalidate /manifest.json and "
                         "/manifest.json.sig in CloudFront.";
    out->folder = folder;
    out->sequence = signed_list.sequence;
    return out;
}

void Publisher::publish() {
    if (busy_ || live_busy_) {
        return;
    }
    if (!live_) {
        MessageBoxW(hwnd_, L"The live list hasn't been read. Press Check live first.", L"FloServer Publisher",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    if (!live_notes_.empty()) {
        MessageBoxW(hwnd_,
                    widen("The live list has a runtime that isn't on the CDN (" + live_notes_.front() +
                          "). FloServer Publisher only makes lists whose runtimes are all on the CDN; "
                          "make this one with floserver-manifest.").c_str(),
                    L"Not published", MB_OK | MB_ICONWARNING);
        return;
    }
    // Two different lists must never share a sequence number.
    if (signed_sequence_ > live_->sequence) {
        MessageBoxW(hwnd_,
                    (L"Sequence " + std::to_wstring(signed_sequence_) +
                     L" was already made in this window. Upload it and invalidate, then press Check live; it "
                     L"must show that sequence before the next one is made.\n\nIf you won't upload it, delete "
                     L"its folder in Documents\\FloServer Publisher and open the Publisher again.")
                        .c_str(),
                    L"Not published", MB_OK | MB_ICONWARNING);
        return;
    }
    // Also made in an earlier window? Two different lists must never both get out.
    if (const auto earlier = made_before(live_->sequence + 1)) {
        const std::wstring q =
            L"A list with sequence " + std::to_wstring(live_->sequence + 1) + L" was already made:\n" +
            earlier->wstring() +
            L"\n\nIf it was uploaded, wait for the invalidation and press Check live instead. Make a new one "
            L"only if that list was never uploaded, and delete its folder afterwards.\n\nMake a new one anyway?";
        if (MessageBoxW(hwnd_, q.c_str(), L"FloServer Publisher", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
            IDYES) {
            return;
        }
    }
    const mt::SuccessorCheck successor = mt::check_successor(*live_, mt::kBuiltInTemplates, now_utc());
    if (!successor.stop.empty()) {
        MessageBoxW(hwnd_, widen(successor.stop.front()).c_str(), L"Not published", MB_OK | MB_ICONWARNING);
        return;
    }
    for (const std::string& q : successor.confirm) {
        if (MessageBoxW(hwnd_, widen(q).c_str(), L"FloServer Publisher", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) !=
            IDYES) {
            return;
        }
    }
    if (key_file_.empty()) {
        choose_key();
        if (key_file_.empty()) {
            return;
        }
    }
    stop_worker();
    cancel_ = false;
    steps_.fill(StepState::Waiting);
    step_details_text_ = {"Checking…", "", "", ""};
    ShowWindow(open_folder_, SW_HIDE);
    ShowWindow(copy_paths_, SW_HIDE);
    set_busy(true);
    update_steps();
    HWND target = hwnd_;
    worker_ = std::thread([target, files = files_, sequence = live_->sequence, key = key_file_, this] {
        auto outcome = run_publish(target, files, sequence, key, cancel_);
        PostMessageW(target, kMsgDone, 0, reinterpret_cast<LPARAM>(outcome.release()));
    });
}

void Publisher::on_done(std::unique_ptr<Outcome> outcome) {
    if (worker_.joinable()) {
        worker_.join();
    }
    working_row_ = -1;
    if (outcome->files.size() == files_.size()) {
        files_ = outcome->files;
    }
    fill_list();
    steps_ = outcome->steps;
    const std::array<std::string, 4> idle = {"", "Next sequence, expires in 90 days, with the built-in templates",
                                             "The key is read, used and forgotten",
                                             "manifest.json and manifest.json.sig in a dated folder"};
    for (std::size_t i = 0; i < 4; ++i) {
        step_details_text_.at(i) = outcome->details.at(i).empty() ? idle.at(i) : outcome->details.at(i);
    }
    set_busy(false);
    if (!outcome->folder.empty()) {
        last_folder_ = outcome->folder;
        signed_sequence_ = outcome->sequence;
        ShowWindow(open_folder_, SW_SHOW);
        ShowWindow(copy_paths_, SW_SHOW);
        SetWindowTextW(copy_paths_, L"Copy invalidation paths");
        ShellExecuteW(hwnd_, L"explore", last_folder_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    update_steps();
    if (!outcome->error.empty()) {
        MessageBoxW(hwnd_, widen(outcome->error).c_str(), L"Not published", MB_OK | MB_ICONWARNING);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev*/, PWSTR /*cmd*/, int show) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int code = 1;
    {
        Publisher app;
        if (app.create(instance, show)) {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (!IsDialogMessageW(app.hwnd(), &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            code = static_cast<int>(msg.wParam);
        }
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return code;
}

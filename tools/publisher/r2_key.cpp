#include "r2_key.h"

#include "win_util.h"
#include "floserver/core/http.h"

#include <commctrl.h>
#include <wincred.h>

#include <algorithm>
#include <array>
#include <cwctype>

namespace floserver::publisher {

namespace {

namespace mt = floserver::manifest_tool;
using floserver::ui::win::int_to_ptr;
using floserver::ui::win::narrow;
using floserver::ui::win::widen;

constexpr const wchar_t* kTarget = L"FloServer Publisher: R2 upload key";
constexpr COLORREF kWindowBg = RGB(0xF3, 0xF3, 0xF3);
constexpr COLORREF kText = RGB(0x1B, 0x1B, 0x1B);
constexpr COLORREF kRed = RGB(0xB3, 0x26, 0x1E);
constexpr COLORREF kGreen = RGB(0x0F, 0x6B, 0x2E);

enum : int { IdAccount = 400, IdKeyId, IdSecret, IdTest, IdSave, IdForget, IdCancel };

std::string field(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring w(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(h, w.data(), n + 1);
    w.resize(static_cast<std::size_t>(n));
    // Pasted with spaces or new lines around it: those aren't the key.
    std::erase_if(w, [](wchar_t c) { return std::iswspace(c) != 0; });
    std::string s = narrow(w);
    std::transform(s.begin(), s.end(), s.begin(), [](char c) {
        return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return s;
}

class KeyWindow {
public:
    explicit KeyWindow(HWND owner) : owner_(owner) {}
    KeyWindow(const KeyWindow&) = delete;
    KeyWindow& operator=(const KeyWindow&) = delete;
    KeyWindow(KeyWindow&&) = delete;
    KeyWindow& operator=(KeyWindow&&) = delete;
    ~KeyWindow() {
        for (HGDIOBJ o : {static_cast<HGDIOBJ>(font_), static_cast<HGDIOBJ>(bold_), static_cast<HGDIOBJ>(bg_)}) {
            if (o != nullptr) {
                DeleteObject(o);
            }
        }
    }

    std::optional<mt::R2Key> run() {
        saved_key_ = load_r2_key();
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &KeyWindow::proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FloServerPublisherR2Key";
        RegisterClassExW(&wc);
        const UINT dpi = owner_ != nullptr ? GetDpiForWindow(owner_) : 96;
        const auto px = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
        RECT r{0, 0, px(560), px(390)};
        AdjustWindowRectExForDpi(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
        RECT o{};
        GetWindowRect(owner_, &o);
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName,
                                L"R2 upload key - FloServer Publisher", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                o.left + ((o.right - o.left) - w) / 2, o.top + ((o.bottom - o.top) - h) / 2, w, h,
                                owner_, nullptr, inst, this);
        if (hwnd_ == nullptr) {
            return std::nullopt;
        }
        const auto font = [dpi](int pt, int weight) {
            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(pt, static_cast<int>(dpi), 72);
            lf.lfWeight = weight;
            lf.lfQuality = CLEARTYPE_QUALITY;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            return CreateFontIndirectW(&lf);
        };
        font_ = font(9, FW_NORMAL);
        bold_ = font(9, FW_SEMIBOLD);
        bg_ = CreateSolidBrush(kWindowBg);
        const auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int cw,
                              int ch, HFONT f, DWORD ex = 0) {
            HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(cw), px(ch), hwnd_,
                                     int_to_ptr<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
            return c;
        };
        make(L"STATIC",
             L"Cloudflare dashboard \x2192 R2 \x2192 Manage API tokens \x2192 Create API token. Permission: Object "
             L"Read & Write. Apply to specific buckets only: floserver-cdn. The Access Key ID and Secret Access Key "
             L"are shown once; the account ID is on R2's overview page.\r\n\r\nKept in Windows Credential Manager on "
             L"this PC only. It can write to floserver-cdn and nothing else; lists are still signed with your "
             L"offline key.",
             SS_LEFT | SS_NOPREFIX, -1, 16, 12, 528, 112, font_);
        make(L"STATIC", L"Account ID", SS_LEFT, -1, 16, 136, 528, 18, bold_);
        account_ = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IdAccount, 16, 156, 528, 24, font_, WS_EX_CLIENTEDGE);
        make(L"STATIC", L"Access Key ID", SS_LEFT, -1, 16, 188, 528, 18, bold_);
        key_id_ = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, IdKeyId, 16, 208, 528, 24, font_, WS_EX_CLIENTEDGE);
        make(L"STATIC", L"Secret Access Key", SS_LEFT, -1, 16, 240, 528, 18, bold_);
        secret_ = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, IdSecret, 16, 260, 528, 24, font_,
                       WS_EX_CLIENTEDGE);
        status_ = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, -1, 16, 294, 528, 36, font_);
        make(L"BUTTON", L"Test", WS_TABSTOP | BS_PUSHBUTTON, IdTest, 16, 342, 90, 30, font_);
        forget_ = make(L"BUTTON", L"Forget the key", WS_TABSTOP | BS_PUSHBUTTON, IdForget, 114, 342, 120, 30, font_);
        make(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, IdSave, 356, 342, 90, 30, font_);
        make(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IdCancel, 454, 342, 90, 30, font_);
        SendMessageW(secret_, EM_SETPASSWORDCHAR, static_cast<WPARAM>(L'\x25CF'), 0);
        if (saved_key_) {
            SetWindowTextW(account_, widen(saved_key_->account_id).c_str());
            SetWindowTextW(key_id_, widen(saved_key_->access_key_id).c_str());
            SendMessageW(secret_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Saved: leave empty to keep it"));
            say(L"A key is saved on this PC.", false, false);
        } else {
            EnableWindow(forget_, FALSE);
        }
        ShowWindow(hwnd_, SW_SHOW);
        SetFocus(account_);
        // Modal: the owner waits.
        EnableWindow(owner_, FALSE);
        MSG msg{};
        while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        EnableWindow(owner_, TRUE);
        DestroyWindow(hwnd_);
        SetActiveWindow(owner_);
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
        }
        return result_;
    }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            const auto* cs = int_to_ptr<const CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = int_to_ptr<KeyWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self == nullptr ? DefWindowProcW(hwnd, msg, wp, lp) : self->handle(msg, wp, lp);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CTLCOLORSTATIC: {
            auto* dc = int_to_ptr<HDC>(wp);
            SetBkColor(dc, kWindowBg);
            SetTextColor(dc, int_to_ptr<HWND>(lp) == status_ ? status_color_ : kText);
            return reinterpret_cast<LRESULT>(bg_);
        }
        case WM_ERASEBKGND: {
            RECT r{};
            GetClientRect(hwnd_, &r);
            FillRect(int_to_ptr<HDC>(wp), &r, bg_);
            return 1;
        }
        case WM_CLOSE:
            done_ = true;
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
            case IdCancel:
            case IDCANCEL:
                done_ = true;
                return 0;
            case IdTest:
                test();
                return 0;
            case IdSave:
            case IDOK:
                save();
                return 0;
            case IdForget:
                if (MessageBoxW(hwnd_, L"Forget the R2 key on this PC? Get selected then only downloads, as Download "
                                       L"only does, until a key is entered again.",
                                L"FloServer Publisher", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                    forget_r2_key();
                    result_.reset();
                    done_ = true;
                }
                return 0;
            default:
                break;
            }
            break;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    // The fields, the saved secret standing in for an empty one.
    std::optional<mt::R2Key> typed() {
        mt::R2Key k{field(account_), field(key_id_), field(secret_)};
        if (k.secret.empty() && saved_key_ && k.access_key_id == saved_key_->access_key_id) {
            k.secret = saved_key_->secret;
        }
        if (const std::string problem = mt::r2_key_problem(k); !problem.empty()) {
            say(L"Not saved: " + widen(problem) + L".", true, true);
            return std::nullopt;
        }
        return k;
    }

    void test() {
        const auto k = typed();
        if (!k) {
            return;
        }
        say(L"Asking R2…", false, false);
        UpdateWindow(status_);
        const HCURSOR before = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        auto http = core::make_system_transport();
        std::string error;
        std::optional<std::uint64_t> size;
        if (http) {
            mt::R2Bucket bucket(*http, *k);
            size = bucket.size_of("manifest.json", error);
        } else {
            error = "no HTTPS on this computer";
        }
        SetCursor(before);
        if (error.empty()) {
            say(size ? L"The key works: R2 answered for floserver-cdn (manifest.json is there)."
                     : L"The key works: R2 answered for floserver-cdn.",
                false, true);
        } else {
            say(L"R2 refused it: " + widen(error), true, true);
        }
    }

    void save() {
        const auto k = typed();
        if (!k) {
            return;
        }
        std::string error;
        if (!save_r2_key(*k, error)) {
            say(L"Not saved: " + widen(error), true, true);
            return;
        }
        result_ = *k;
        done_ = true;
    }

    void say(const std::wstring& text, bool bad, bool colored) {
        status_color_ = !colored ? kText : bad ? kRed : kGreen;
        SetWindowTextW(status_, text.c_str());
        InvalidateRect(status_, nullptr, TRUE);
    }

    HWND owner_;
    HWND hwnd_ = nullptr;
    HWND account_ = nullptr, key_id_ = nullptr, secret_ = nullptr, status_ = nullptr, forget_ = nullptr;
    HFONT font_ = nullptr, bold_ = nullptr;
    HBRUSH bg_ = nullptr;
    COLORREF status_color_ = kText;
    bool done_ = false;
    std::optional<mt::R2Key> saved_key_;
    std::optional<mt::R2Key> result_;
};

}  // namespace

std::optional<mt::R2Key> load_r2_key() {
    PCREDENTIALW cred = nullptr;
    if (CredReadW(kTarget, CRED_TYPE_GENERIC, 0, &cred) == FALSE || cred == nullptr) {
        return std::nullopt;
    }
    mt::R2Key key;
    // UserName is "<account id>:<access key id>"; the blob is the secret.
    const std::string user = cred->UserName != nullptr ? narrow(cred->UserName) : std::string();
    if (const auto colon = user.find(':'); colon != std::string::npos) {
        key.account_id = user.substr(0, colon);
        key.access_key_id = user.substr(colon + 1);
    }
    key.secret.assign(reinterpret_cast<const char*>(cred->CredentialBlob),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                      cred->CredentialBlobSize);
    SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    if (!mt::r2_key_problem(key).empty()) {
        return std::nullopt;
    }
    return key;
}

bool save_r2_key(const mt::R2Key& key, std::string& error) {
    std::wstring user = widen(key.account_id + ":" + key.access_key_id);
    std::string secret = key.secret;
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(kTarget);
    cred.UserName = user.data();
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(secret.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;  // this PC, not roamed with the Windows account
    const BOOL ok = CredWriteW(&cred, 0);
    const DWORD code = GetLastError();
    SecureZeroMemory(secret.data(), secret.size());
    if (ok == FALSE) {
        error = "Windows Credential Manager refused it (error " + std::to_string(code) + ")";
        return false;
    }
    return true;
}

void forget_r2_key() { CredDeleteW(kTarget, CRED_TYPE_GENERIC, 0); }

std::optional<mt::R2Key> ask_r2_key(HWND owner) {
    KeyWindow w(owner);
    return w.run();
}

}  // namespace floserver::publisher

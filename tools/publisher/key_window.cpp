#include "key_window.h"

#include "win_util.h"

#include <commctrl.h>
#include <wincred.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>
#include <thread>

namespace floserver::publisher {

namespace {

using floserver::ui::win::int_to_ptr;
using floserver::ui::win::narrow;
using floserver::ui::win::widen;

constexpr COLORREF kWindowBg = RGB(0xF3, 0xF3, 0xF3);
constexpr COLORREF kText = RGB(0x1B, 0x1B, 0x1B);
constexpr COLORREF kRed = RGB(0xB3, 0x26, 0x1E);
constexpr COLORREF kGreen = RGB(0x0F, 0x6B, 0x2E);

enum : int { IdTest = 400, IdSave, IdForget, IdCancel, IdField0 = 420 };

struct TestResult {
    std::string error;
};

// Registered, so a stray post can't be read as some other window's message.
UINT tested_message() {
    static const UINT id = RegisterWindowMessageW(L"FloServerPublisher.KeyTested");
    return id;
}

std::string field_text(HWND h, bool hex) {
    const int n = GetWindowTextLengthW(h);
    std::wstring w(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(h, w.data(), n + 1);
    w.resize(static_cast<std::size_t>(n));
    // Pasted with spaces or new lines around it: those aren't the key.
    std::erase_if(w, [](wchar_t c) { return std::iswspace(c) != 0; });
    std::string s = narrow(w);
    if (hex) {
        std::transform(s.begin(), s.end(), s.begin(), [](char c) {
            return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c;
        });
    }
    return s;
}

class KeyWindow {
public:
    KeyWindow(HWND owner, const KeyWindowSpec& spec) : owner_(owner), spec_(spec) {}
    KeyWindow(const KeyWindow&) = delete;
    KeyWindow& operator=(const KeyWindow&) = delete;
    KeyWindow(KeyWindow&&) = delete;
    KeyWindow& operator=(KeyWindow&&) = delete;
    ~KeyWindow() {
        alive_->store(false);
        if (tester_.joinable()) {
            // It holds only copies; its answer to the closed window is dropped.
            tester_.detach();
        }
        for (HGDIOBJ o : {static_cast<HGDIOBJ>(font_), static_cast<HGDIOBJ>(bold_), static_cast<HGDIOBJ>(bg_)}) {
            if (o != nullptr) {
                DeleteObject(o);
            }
        }
    }

    std::optional<std::vector<std::string>> run() {
        if (spec_.load) {
            saved_ = spec_.load();
        }
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &KeyWindow::proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"FloServerPublisherKey";
        RegisterClassExW(&wc);
        const UINT dpi = owner_ != nullptr ? GetDpiForWindow(owner_) : 96;
        const auto px = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
        const int n = static_cast<int>(spec_.fields.size());
        const int fields_top = 136;
        const int status_top = fields_top + n * 52 + 2;
        const int buttons_top = status_top + 48;
        const int height = buttons_top + 48;
        RECT r{0, 0, px(560), px(height)};
        AdjustWindowRectExForDpi(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
        RECT o{};
        GetWindowRect(owner_, &o);
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, wc.lpszClassName,
                                (spec_.title + L" - FloServer Publisher").c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU,
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
        make(L"STATIC", spec_.intro.c_str(), SS_LEFT | SS_NOPREFIX, -1, 16, 12, 528, 112, font_);
        for (int i = 0; i < n; ++i) {
            const KeyField& f = spec_.fields.at(static_cast<std::size_t>(i));
            const bool secret = i == n - 1;
            const int y = fields_top + i * 52;
            make(L"STATIC", f.label.c_str(), SS_LEFT, -1, 16, y, 528, 18, bold_);
            HWND e = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | (secret ? ES_PASSWORD : 0), IdField0 + i, 16,
                          y + 20, 528, 24, font_, WS_EX_CLIENTEDGE);
            if (secret) {
                SendMessageW(e, EM_SETPASSWORDCHAR, static_cast<WPARAM>(L'\x25CF'), 0);
            }
            edits_.push_back(e);
        }
        status_ = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, -1, 16, status_top, 528, 36, font_);
        test_ = make(L"BUTTON", L"Test", WS_TABSTOP | BS_PUSHBUTTON, IdTest, 16, buttons_top, 90, 30, font_);
        forget_ = make(L"BUTTON", L"Forget the key", WS_TABSTOP | BS_PUSHBUTTON, IdForget, 114, buttons_top, 120, 30,
                       font_);
        make(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, IdSave, 356, buttons_top, 90, 30, font_);
        make(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IdCancel, 454, buttons_top, 90, 30, font_);
        if (saved_ && saved_->size() == spec_.fields.size()) {
            for (std::size_t i = 0; i + 1 < edits_.size(); ++i) {
                SetWindowTextW(edits_.at(i), widen(saved_->at(i)).c_str());
            }
            SendMessageW(edits_.back(), EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"Saved: leave empty to keep it"));
            say(L"A key is saved on this PC.", false, false);
        } else {
            saved_.reset();
            EnableWindow(forget_, FALSE);
        }
        ShowWindow(hwnd_, SW_SHOW);
        SetFocus(edits_.front());
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
            // Before CreateWindowExW returns: messages during creation go to
            // DefWindowProcW(hwnd_), which must be this window, or it fails.
            static_cast<KeyWindow*>(cs->lpCreateParams)->hwnd_ = hwnd;
        }
        auto* self = int_to_ptr<KeyWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self == nullptr ? DefWindowProcW(hwnd, msg, wp, lp) : self->handle(msg, wp, lp);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        if (msg != 0 && msg == tested_message()) {
            const std::unique_ptr<TestResult> r(int_to_ptr<TestResult*>(lp));
            if (tester_.joinable()) {
                tester_.join();
            }
            EnableWindow(test_, TRUE);
            if (r->error.empty()) {
                say(spec_.works, false, true);
            } else {
                say(spec_.refused + widen(r->error), true, true);
            }
            return 0;
        }
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
        case WM_DESTROY:
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
                if (MessageBoxW(hwnd_, spec_.forget_question.c_str(), L"FloServer Publisher",
                                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                    if (spec_.forget) {
                        spec_.forget();
                    }
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
    std::optional<std::vector<std::string>> typed() {
        std::vector<std::string> v;
        for (std::size_t i = 0; i < edits_.size(); ++i) {
            v.push_back(field_text(edits_.at(i), spec_.fields.at(i).hex));
        }
        if (v.back().empty() && saved_ && std::equal(v.begin(), v.end() - 1, saved_->begin())) {
            v.back() = saved_->back();
        }
        if (const std::string problem = spec_.problem ? spec_.problem(v) : std::string(); !problem.empty()) {
            say(L"Not saved: " + widen(problem) + L".", true, true);
            return std::nullopt;
        }
        return v;
    }

    // On a worker: a slow network mustn't freeze the window.
    void test() {
        if (tester_.joinable() || !spec_.test) {
            return;
        }
        const auto v = typed();
        if (!v) {
            return;
        }
        say(spec_.asking, false, false);
        EnableWindow(test_, FALSE);
        HWND target = hwnd_;
        tester_ = std::thread([target, values = *v, check = spec_.test, alive = alive_] {
            auto r = std::make_unique<TestResult>();
            r->error = check(values);
            // Only while the window is still there; otherwise the answer goes with the thread.
            if (alive->load() && PostMessageW(target, tested_message(), 0, reinterpret_cast<LPARAM>(r.get())) != FALSE) {
                (void)r.release();
            }
        });
    }

    void save() {
        const auto v = typed();
        if (!v || !spec_.save) {
            return;
        }
        std::string error;
        if (!spec_.save(*v, error)) {
            say(L"Not saved: " + widen(error), true, true);
            return;
        }
        result_ = *v;
        done_ = true;
    }

    void say(const std::wstring& text, bool bad, bool colored) {
        status_color_ = !colored ? kText : bad ? kRed : kGreen;
        SetWindowTextW(status_, text.c_str());
        InvalidateRect(status_, nullptr, TRUE);
    }

    HWND owner_;
    const KeyWindowSpec& spec_;
    HWND hwnd_ = nullptr;
    std::vector<HWND> edits_;
    HWND status_ = nullptr, forget_ = nullptr, test_ = nullptr;
    std::thread tester_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    HFONT font_ = nullptr, bold_ = nullptr;
    HBRUSH bg_ = nullptr;
    COLORREF status_color_ = kText;
    bool done_ = false;
    std::optional<std::vector<std::string>> saved_;
    std::optional<std::vector<std::string>> result_;
};

}  // namespace

std::optional<std::vector<std::string>> run_key_window(HWND owner, const KeyWindowSpec& spec) {
    if (spec.fields.empty()) {
        return std::nullopt;
    }
    KeyWindow w(owner, spec);
    return w.run();
}

std::optional<StoredCredential> load_credential(const wchar_t* target) {
    PCREDENTIALW cred = nullptr;
    if (CredReadW(target, CRED_TYPE_GENERIC, 0, &cred) == FALSE || cred == nullptr) {
        return std::nullopt;
    }
    StoredCredential out;
    out.user = cred->UserName != nullptr ? narrow(cred->UserName) : std::string();
    out.secret.assign(reinterpret_cast<const char*>(cred->CredentialBlob),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                      cred->CredentialBlobSize);
    SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    return out;
}

bool save_credential(const wchar_t* target, const std::string& user, const std::string& secret_in,
                     std::string& error) {
    std::wstring wuser = widen(user);
    std::string secret = secret_in;
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target);
    cred.UserName = wuser.data();
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

void forget_credential(const wchar_t* target) { CredDeleteW(target, CRED_TYPE_GENERIC, 0); }

}  // namespace floserver::publisher

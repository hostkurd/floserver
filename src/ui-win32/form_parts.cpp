#include "form_parts.h"

#include "canvas.h"
#include "win_util.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace floserver::ui {

namespace {
constexpr UINT_PTR kSubclass = 7;
}  // namespace

FieldEdit::FieldEdit(HWND parent, int id, HFONT font, bool rtl, KeyFn on_key)
    : id_(id), rtl_(rtl), on_key_(std::move(on_key)) {
    hwnd_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, parent,
                            win::int_to_ptr<HMENU>(static_cast<INT_PTR>(id)),
                            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
    if (hwnd_ != nullptr) {
        SetWindowSubclass(hwnd_, proc, kSubclass, reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(hwnd_, EM_LIMITTEXT, 1000, 0);  // a php.ini value's most
        set_font(font);
    }
}

FieldEdit::~FieldEdit() {
    if (hwnd_ != nullptr) {
        RemoveWindowSubclass(hwnd_, proc, kSubclass);
        DestroyWindow(hwnd_);
    }
}

std::string FieldEdit::text() const {
    if (hwnd_ == nullptr) {
        return {};
    }
    const int n = GetWindowTextLengthW(hwnd_);
    std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(hwnd_, text.data(), n + 1);
    text.resize(static_cast<std::size_t>(n));
    return win::narrow(text);
}

void FieldEdit::set_text(std::string_view text) {
    if (hwnd_ == nullptr) {
        return;
    }
    const std::wstring wide = win::widen(text);
    setting_ = true;
    SetWindowTextW(hwnd_, wide.c_str());
    setting_ = false;
}

void FieldEdit::set_cue(std::string_view cue) {
    cue_ = win::widen(cue);
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void FieldEdit::set_cue_color(Rgb color) {
    cue_color_ = color;
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void FieldEdit::set_font(HFONT font) {
    if (hwnd_ != nullptr && font != nullptr) {
        SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(hwnd_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
}

void FieldEdit::set_read_only(bool read_only) {
    if (hwnd_ != nullptr) {
        SendMessageW(hwnd_, EM_SETREADONLY, read_only ? TRUE : FALSE, 0);
    }
}

void FieldEdit::set_password(bool on) {
    if (hwnd_ != nullptr) {
        SendMessageW(hwnd_, EM_SETPASSWORDCHAR, on ? WPARAM{0x25CF} : WPARAM{0}, 0);  // ●
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

bool put_secret_on_clipboard(HWND owner, std::string& value) {
    std::wstring wide = win::widen(value);
    SecureZeroMemory(value.data(), value.size());
    value.clear();
    bool done = false;
    if (OpenClipboard(owner) != FALSE) {
        EmptyClipboard();
        const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL text = GlobalAlloc(GMEM_MOVEABLE, bytes); text != nullptr) {
            if (void* to = GlobalLock(text); to != nullptr) {
                std::memcpy(to, wide.c_str(), bytes);
                GlobalUnlock(text);
                done = SetClipboardData(CF_UNICODETEXT, text) != nullptr;
            }
            if (!done) {
                GlobalFree(text);
            }
        }
        // Kept out of Windows' clipboard history and cloud clipboard.
        for (const wchar_t* name : {L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard",
                                    L"ExcludeClipboardContentFromMonitorProcessing"}) {
            const UINT format = RegisterClipboardFormatW(name);
            if (HGLOBAL zero = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD)); format != 0 && zero != nullptr) {
                if (auto* d = static_cast<DWORD*>(GlobalLock(zero)); d != nullptr) {
                    *d = 0;
                    GlobalUnlock(zero);
                }
                if (SetClipboardData(format, zero) == nullptr) {
                    GlobalFree(zero);
                }
            }
        }
        CloseClipboard();
    }
    SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
    return done;
}

void FieldEdit::place(const RECT& box, int pad, int height, bool visible) {
    if (hwnd_ == nullptr) {
        return;
    }
    const int w = static_cast<int>(box.right - box.left) - 2 * pad;
    if (!visible || w <= 0) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    const int top = static_cast<int>(box.top) + (static_cast<int>(box.bottom - box.top) - height) / 2;
    const RECT at{static_cast<int>(box.left) + pad, top, static_cast<int>(box.left) + pad + w, top + height};
    if (EqualRect(&at, &placed_) != FALSE && IsWindowVisible(hwnd_) != FALSE) {
        return;  // where it is: nothing to repaint (relayout runs on every key typed)
    }
    placed_ = at;
    // SWP_NOCOPYBITS: a moved control repaints itself. Without it Windows
    // copies the pixels at its old place to the new one; when a form's
    // boxes all move (a line above them appears), a box moving onto the
    // spot another box just moved into copies that box's text, and keeps
    // showing it: collation-server showed innodb_buffer_pool_size's 64M
    // (the user's screenshot, 1 Nov) until a save set the texts again.
    SetWindowPos(hwnd_, nullptr, at.left, at.top, w, height,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
}

void FieldEdit::place(const RECT& box, int pad, int height, bool visible, int clip_top, int clip_bottom) {
    if (hwnd_ == nullptr) {
        return;
    }
    const int top = static_cast<int>(box.top) + (static_cast<int>(box.bottom - box.top) - height) / 2;
    const int from = std::max(0, clip_top - top);
    const int to = std::min(height, clip_bottom - top);
    if (!visible || to <= from) {
        place(box, pad, height, false);
        return;
    }
    place(box, pad, height, true);
    const bool cut = from > 0 || to < height;
    if (cut) {
        // Only rows are cut, so the region is the same in a mirrored window.
        const int w = static_cast<int>(box.right - box.left) - 2 * pad;
        SetWindowRgn(hwnd_, CreateRectRgn(0, from, w, to), TRUE);  // the window owns the region
    } else if (clipped_) {
        SetWindowRgn(hwnd_, nullptr, TRUE);
    }
    clipped_ = cut;
}

void FieldEdit::paint_cue() const {
    HDC dc = GetDC(hwnd_);
    if (dc == nullptr) {
        return;
    }
    RECT r{};
    SendMessageW(hwnd_, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&r));
    auto* font = win::int_to_ptr<HFONT>(SendMessageW(hwnd_, WM_GETFONT, 0, 0));
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colorref(cue_color_));
    DrawTextW(dc, cue_.c_str(), static_cast<int>(cue_.size()), &r,
              DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | (rtl_ ? UINT{DT_RTLREADING} : 0U));
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
}

LRESULT CALLBACK FieldEdit::proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR self_ptr) {
    auto* self = win::int_to_ptr<FieldEdit*>(self_ptr);
    switch (msg) {
        case WM_KEYDOWN:
            if ((wparam == VK_TAB || wparam == VK_RETURN || wparam == VK_ESCAPE) && self->on_key_ &&
                self->on_key_(wparam)) {
                return 0;
            }
            break;
        case WM_CHAR:
            if (wparam == VK_ESCAPE || wparam == VK_TAB || wparam == VK_RETURN) {
                return 0;  // no beep
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(hwnd, nullptr, TRUE);
            // The window draws the field's focus mark around the box.
            InvalidateRect(GetParent(hwnd), nullptr, FALSE);
            break;
        case WM_PAINT: {
            const LRESULT result = DefSubclassProc(hwnd, msg, wparam, lparam);
            if (!self->cue_.empty() && GetWindowTextLengthW(hwnd) == 0 && GetFocus() != hwnd) {
                self->paint_cue();
            }
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, proc, id);
            self->hwnd_ = nullptr;
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

SaveChoice ask_to_save(HWND owner, Lang lang, const std::string& question) {
    const std::wstring title = win::widen(text(lang, Str::SettingsTitle));
    const std::wstring main = win::widen(question);
    const std::wstring save = win::widen(text(lang, Str::PhpSave));
    const std::wstring dont = win::widen(text(lang, Str::PhpDontSave));
    const std::array<TASKDIALOG_BUTTON, 2> buttons{{{IDYES, save.c_str()}, {IDNO, dont.c_str()}}};
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof config;
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW |
                     (is_rtl(lang) ? TDF_RTL_LAYOUT : 0);
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = title.c_str();
    config.pszMainInstruction = main.c_str();
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = IDYES;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) {
        // Without common controls 6 (it's in the manifest): the plain box.
        pressed = MessageBoxW(owner, main.c_str(), title.c_str(),
                              MB_YESNOCANCEL | MB_ICONQUESTION | (is_rtl(lang) ? MB_RTLREADING | MB_RIGHT : 0U));
    }
    if (pressed == IDYES) {
        return SaveChoice::Save;
    }
    return pressed == IDNO ? SaveChoice::DontSave : SaveChoice::Cancel;
}

}  // namespace floserver::ui

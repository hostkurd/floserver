#include "form_parts.h"

#include "access_provider.h"
#include "canvas.h"
#include "win_util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>
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
        set_accessible_name(hwnd_, {});  // a name given to it goes with it
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

namespace {

// A path the service gave, as Windows wants it, when it's an existing folder.
std::optional<std::wstring> folder_on_disk(const std::string& path) {
    if (path.empty()) {
        return std::nullopt;
    }
    std::wstring wide = win::widen(path);
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return std::nullopt;
    }
    return wide;
}

}  // namespace

std::optional<std::string> pick_folder(HWND owner, const std::string& title, const std::string& start) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
        dialog == nullptr) {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    const std::wstring wide_title = win::widen(title);
    dialog->SetTitle(wide_title.c_str());
    if (const auto folder = folder_on_disk(start)) {
        IShellItem* at = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder->c_str(), nullptr, IID_PPV_ARGS(&at))) && at != nullptr) {
            dialog->SetFolder(at);
            at->Release();
        }
    }
    std::optional<std::string> path;
    const HRESULT shown = dialog->Show(owner);
    if (SUCCEEDED(shown)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR chosen = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) && chosen != nullptr) {
                path = win::narrow(chosen);
                CoTaskMemFree(chosen);
            }
            item->Release();
        }
    } else if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        path = std::string();
    }
    dialog->Release();
    return path;
}

std::optional<std::string> pick_program(HWND owner, const std::string& title, const std::string& filter) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
        dialog == nullptr) {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    // The program itself, not a shortcut to it (a .lnk would be started as a file).
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NODEREFERENCELINKS | FOS_NOCHANGEDIR);
    const std::wstring wide_title = win::widen(title);
    dialog->SetTitle(wide_title.c_str());
    const std::wstring name = win::widen(filter) + L" (*.exe)";
    const COMDLG_FILTERSPEC types[] = {{name.c_str(), L"*.exe"}};
    dialog->SetFileTypes(1, types);
    std::optional<std::string> path;
    const HRESULT shown = dialog->Show(owner);
    if (SUCCEEDED(shown)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR chosen = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) && chosen != nullptr) {
                path = win::narrow(chosen);
                CoTaskMemFree(chosen);
            }
            item->Release();
        }
    } else if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        path = std::string();
    }
    dialog->Release();
    return path;
}

bool explore_folder(HWND owner, const std::string& path) {
    const auto folder = folder_on_disk(path);
    return folder && reinterpret_cast<INT_PTR>(
                         ShellExecuteW(owner, L"explore", folder->c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
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


namespace {

// The remove question's controls.
constexpr int kRemoveButton = 100;
constexpr int kTitleText = 101;
constexpr int kBodyText = 102;
constexpr int kDropBox = 103;
constexpr int kTrashBox = 104;
constexpr int kFolderText = 105;
constexpr int kNoteText = 106;

struct RemoveDialog {
    std::wstring window_title;
    std::wstring title;
    std::wstring body;
    std::wstring remove;
    std::wstring cancel;
    std::wstring drop;
    std::wstring trash;
    std::wstring folder;
    std::wstring note;
    bool dropped = false;
    bool trashed = false;
    HFONT bold = nullptr;
};

INT_PTR CALLBACK remove_dialog_proc(HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* d = win::int_to_ptr<RemoveDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            d = win::int_to_ptr<RemoveDialog*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            SetWindowTextW(dialog, d->window_title.c_str());
            SetDlgItemTextW(dialog, kTitleText, d->title.c_str());
            SetDlgItemTextW(dialog, kBodyText, d->body.c_str());
            SetDlgItemTextW(dialog, kDropBox, d->drop.c_str());
            SetDlgItemTextW(dialog, kTrashBox, d->trash.c_str());
            SetDlgItemTextW(dialog, kFolderText, d->folder.c_str());
            SetDlgItemTextW(dialog, kNoteText, d->note.c_str());
            SetDlgItemTextW(dialog, kRemoveButton, d->remove.c_str());
            SetDlgItemTextW(dialog, IDCANCEL, d->cancel.c_str());
            // The question in bold, a little larger, as a task dialog's main line.
            LOGFONTW lf{};
            if (const auto font = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
                font != nullptr && GetObjectW(font, sizeof lf, &lf) != 0) {
                lf.lfWeight = FW_SEMIBOLD;
                lf.lfHeight = lf.lfHeight * 4 / 3;
                d->bold = CreateFontIndirectW(&lf);
                SendDlgItemMessageW(dialog, kTitleText, WM_SETFONT, reinterpret_cast<WPARAM>(d->bold), FALSE);
            }
            // Cancel is the default: Enter and Esc both keep everything.
            SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
            SetFocus(GetDlgItem(dialog, IDCANCEL));
            return FALSE;
        }
        case WM_COMMAND:
            if (d != nullptr && (LOWORD(wparam) == kRemoveButton || LOWORD(wparam) == IDCANCEL)) {
                const bool remove = LOWORD(wparam) == kRemoveButton;
                d->dropped = remove && IsDlgButtonChecked(dialog, kDropBox) == BST_CHECKED;
                d->trashed = remove && IsDlgButtonChecked(dialog, kTrashBox) == BST_CHECKED;
                EndDialog(dialog, remove ? 1 : 0);
                return TRUE;
            }
            break;
        case WM_DESTROY:
            if (d != nullptr && d->bold != nullptr) {
                DeleteObject(d->bold);
                d->bold = nullptr;
            }
            break;
        default:
            break;
    }
    (void)lparam;
    return FALSE;
}

}  // namespace

bool ask_remove_project(HWND owner, Lang lang, const RemoveQuestion& question, bool& drop, bool& trash) {
    drop = false;
    trash = false;
    RemoveDialog d;
    d.window_title = win::widen(question.window_title);
    d.title = win::widen(question.title);
    d.body = win::widen(question.text);
    d.remove = win::widen(question.remove);
    d.cancel = win::widen(question.cancel);
    d.drop = win::widen(question.drop);
    d.trash = win::widen(question.trash);
    d.folder = win::widen(question.folder);
    d.note = win::widen(question.trash_note);
    // In dialog units: the question, what happens to the folder, the boxes
    // (each only when it applies, both unticked), then Remove and Cancel.
    constexpr short kWidth = 280;
    constexpr short kMargin = 10;
    constexpr short kInner = kWidth - 2 * kMargin;
    const bool rtl = is_rtl(lang);
    short y = kMargin;
    std::vector<std::tuple<DWORD, short, short, short, short, WORD, WORD>> items;
    items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 24, kTitleText, 0x0082);
    y += 28;
    items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 18, kBodyText, 0x0082);
    y += 22;
    if (!d.drop.empty()) {
        items.emplace_back(BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP | WS_TABSTOP, kMargin, y, kInner, 20, kDropBox,
                           0x0080);
        y += 24;
    }
    if (!d.trash.empty()) {
        items.emplace_back(BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP | WS_TABSTOP, kMargin, y, kInner, 20, kTrashBox,
                           0x0080);
        y += 20;
        // Which folder, under its box; a long path breaks anywhere.
        items.emplace_back(SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, kMargin + 12, y, kInner - 12, 26, kFolderText,
                           0x0082);
        y += 30;
    } else if (!d.note.empty()) {
        // Why the folder isn't offered (its drive has no Recycle Bin).
        items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 18, kNoteText, 0x0082);
        y += 22;
    }
    y += 6;
    constexpr short kButtonW = 60;
    constexpr short kButtonH = 14;
    items.emplace_back(BS_PUSHBUTTON | WS_TABSTOP, kWidth - kMargin - 2 * kButtonW - 6, y, kButtonW, kButtonH,
                       kRemoveButton, 0x0080);
    items.emplace_back(BS_DEFPUSHBUTTON | WS_TABSTOP, kWidth - kMargin - kButtonW, y, kButtonW, kButtonH, IDCANCEL,
                       0x0080);
    y += kButtonH + kMargin;
    // Mirrored in Arabic and Kurdish: the boxes, the text and the buttons from the right.
    DialogTemplate dialog(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU,
                          rtl ? WS_EX_LAYOUTRTL | WS_EX_RTLREADING : 0U, kWidth, y);
    for (const auto& [style, x, iy, cx, cy, id, kind] : items) {
        dialog.add(style, x, iy, cx, cy, id, kind);
    }
    const INT_PTR answer =
        DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dialog.get(), owner, remove_dialog_proc,
                                reinterpret_cast<LPARAM>(&d));
    if (answer != 1) {
        return false;  // Cancel, Esc, closed, or no dialog at all: nothing is removed
    }
    drop = d.dropped;
    trash = d.trashed;
    return true;
}

namespace {

// Remove FloServer…'s controls.
constexpr int kUninstallButton = 200;
constexpr int kUninstallTitle = 201;
constexpr int kUninstallText = 202;
constexpr int kUninstallAlso = 203;
constexpr int kRuntimesBox = 204;
constexpr int kRuntimesNote = 205;
constexpr int kDatabasesBox = 206;
constexpr int kDatabasesNote = 207;
constexpr int kProjectsBox = 208;
constexpr int kProjectsNote = 209;
// The sizes are counted: the notes are worded again.
constexpr UINT kSizesMessage = WM_APP + 50;

// Shared by the dialog and the thread counting the sizes, which may finish
// after the dialog has gone: it posts only while `dialog` is set.
struct SizesBox {
    std::mutex mutex;
    HWND dialog = nullptr;
    std::optional<UninstallSizes> sizes;
};

struct UninstallDialog {
    Lang lang = Lang::En;
    std::string projects_dir;
    UninstallQuestion question;
    std::function<UninstallSizes()> count;
    std::shared_ptr<SizesBox> box = std::make_shared<SizesBox>();
    UninstallTicks ticks;
    HFONT bold = nullptr;
};

// The notes under the boxes, and the same as each box's help for screen
// readers (said after its name and state, so the size and "This can't be
// undone." are heard on the box itself).
void word_notes(HWND dialog, const UninstallQuestion& q) {
    for (const auto& [box, note, words] : {std::tuple{kRuntimesBox, kRuntimesNote, &q.runtimes_note},
                                           std::tuple{kDatabasesBox, kDatabasesNote, &q.databases_note},
                                           std::tuple{kProjectsBox, kProjectsNote, &q.projects_note}}) {
        if (HWND control = GetDlgItem(dialog, box); control != nullptr) {
            SetDlgItemTextW(dialog, note, win::widen(*words).c_str());
            set_accessible_help(control, *words);
        }
    }
}

// Remove FloServer, in red: it can't be taken back. In high contrast the
// system's own button colours, as every other button there.
void draw_destructive(const DRAWITEMSTRUCT& item, bool rtl) {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof contrast;
    const bool high = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) != FALSE &&
                      (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    RECT r = item.rcItem;
    COLORREF text_color = RGB(0xFF, 0xFF, 0xFF);
    if (high) {
        DrawFrameControl(item.hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pressed ? DFCS_PUSHED : 0U));
        text_color = GetSysColor(COLOR_BTNTEXT);
    } else {
        // Windows' own red for a destructive action, darker while pressed.
        const win::UniqueBrush fill(CreateSolidBrush(pressed ? RGB(0x9E, 0x22, 0x16) : RGB(0xC4, 0x2B, 0x1C)));
        FillRect(item.hDC, &r, fill.get());
    }
    wchar_t label[128] = {};
    const int n = GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    const int old_mode = SetBkMode(item.hDC, TRANSPARENT);
    const COLORREF old_color = SetTextColor(item.hDC, text_color);
    DrawTextW(item.hDC, label, n, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | (rtl ? UINT{DT_RTLREADING} : 0U));
    SetTextColor(item.hDC, old_color);
    SetBkMode(item.hDC, old_mode);
    if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0) {
        RECT ring = item.rcItem;
        InflateRect(&ring, -3, -3);
        DrawFocusRect(item.hDC, &ring);
    }
}

INT_PTR CALLBACK uninstall_dialog_proc(HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* d = win::int_to_ptr<UninstallDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            d = win::int_to_ptr<UninstallDialog*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            const UninstallQuestion& q = d->question;
            SetWindowTextW(dialog, win::widen(q.window_title).c_str());
            for (const auto& [id, words] : {std::pair{kUninstallTitle, &q.title}, std::pair{kUninstallText, &q.text},
                                            std::pair{kUninstallAlso, &q.also_delete},
                                            std::pair{kRuntimesBox, &q.runtimes}, std::pair{kDatabasesBox, &q.databases},
                                            std::pair{kProjectsBox, &q.projects},
                                            std::pair{kUninstallButton, &q.remove}, std::pair{IDCANCEL, &q.cancel}}) {
                SetDlgItemTextW(dialog, id, win::widen(*words).c_str());
            }
            word_notes(dialog, q);
            CheckDlgButton(dialog, kRuntimesBox, d->ticks.runtimes ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog, kDatabasesBox, d->ticks.databases ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog, kProjectsBox, d->ticks.projects ? BST_CHECKED : BST_UNCHECKED);
            // The question in bold, a little larger, as a task dialog's main line.
            LOGFONTW lf{};
            if (const auto font = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
                font != nullptr && GetObjectW(font, sizeof lf, &lf) != 0) {
                lf.lfWeight = FW_SEMIBOLD;
                lf.lfHeight = lf.lfHeight * 4 / 3;
                d->bold = CreateFontIndirectW(&lf);
                SendDlgItemMessageW(dialog, kUninstallTitle, WM_SETFONT, reinterpret_cast<WPARAM>(d->bold), FALSE);
            }
            // The sizes, counted off this thread ("…" until they're known).
            {
                const std::lock_guard lock(d->box->mutex);
                d->box->dialog = dialog;
            }
            try {
                std::thread([box = d->box, count = d->count] {
                    UninstallSizes sizes;
                    try {
                        sizes = count();
                    } catch (const std::exception&) {
                        return;  // left as "…"
                    }
                    const std::lock_guard lock(box->mutex);
                    box->sizes = sizes;
                    if (box->dialog != nullptr) {
                        PostMessageW(box->dialog, kSizesMessage, 0, 0);
                    }
                }).detach();
            } catch (const std::system_error&) {
                // No thread: the sizes stay "…"; the question still works.
            }
            // Cancel is the default: Enter and Esc both keep everything.
            SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
            SetFocus(GetDlgItem(dialog, IDCANCEL));
            return FALSE;
        }
        case kSizesMessage:
            if (d != nullptr) {
                std::optional<UninstallSizes> sizes;
                {
                    const std::lock_guard lock(d->box->mutex);
                    sizes = d->box->sizes;
                }
                if (sizes) {
                    d->question = uninstall_question(d->lang, d->projects_dir, *sizes);
                    word_notes(dialog, d->question);
                }
            }
            return TRUE;
        case WM_DRAWITEM: {
            const auto* item = win::int_to_ptr<const DRAWITEMSTRUCT*>(lparam);
            if (d != nullptr && item != nullptr && item->CtlID == kUninstallButton) {
                draw_destructive(*item, is_rtl(d->lang));
                return TRUE;
            }
            break;
        }
        case WM_COMMAND:
            if (d != nullptr && (LOWORD(wparam) == kUninstallButton || LOWORD(wparam) == IDCANCEL)) {
                const bool remove = LOWORD(wparam) == kUninstallButton;
                if (remove) {
                    d->ticks.runtimes = IsDlgButtonChecked(dialog, kRuntimesBox) == BST_CHECKED;
                    d->ticks.databases = IsDlgButtonChecked(dialog, kDatabasesBox) == BST_CHECKED;
                    d->ticks.projects = IsDlgButtonChecked(dialog, kProjectsBox) == BST_CHECKED;
                }
                EndDialog(dialog, remove ? 1 : 0);
                return TRUE;
            }
            break;
        case WM_DESTROY:
            if (d != nullptr) {
                {
                    const std::lock_guard lock(d->box->mutex);
                    d->box->dialog = nullptr;  // a count finishing now posts nothing
                }
                for (const int id : {kRuntimesBox, kDatabasesBox, kProjectsBox}) {
                    set_accessible_help(GetDlgItem(dialog, id), {});  // the help given goes with it
                }
                if (d->bold != nullptr) {
                    DeleteObject(d->bold);
                    d->bold = nullptr;
                }
            }
            break;
        default:
            break;
    }
    return FALSE;
}

}  // namespace

bool ask_remove_floserver(HWND owner, Lang lang, const std::string& projects_dir,
                          std::function<UninstallSizes()> count, UninstallTicks& ticks) {
    UninstallDialog d;
    d.lang = lang;
    d.projects_dir = projects_dir;
    d.question = uninstall_question(lang, projects_dir, {});
    d.count = std::move(count);
    d.ticks = ticks;
    d.ticks.projects = d.ticks.projects && !projects_dir.empty();
    // In dialog units: the question, what happens, then "Also delete" and a
    // box for each, its note under it, then Remove FloServer and Cancel.
    constexpr short kWidth = 300;
    constexpr short kMargin = 10;
    constexpr short kInner = kWidth - 2 * kMargin;
    constexpr short kIndent = 12;
    constexpr WORD kButton = 0x0080;
    constexpr WORD kStatic = 0x0082;
    constexpr DWORD kBox = BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP | WS_TABSTOP;
    constexpr DWORD kNote = SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL;
    short y = kMargin;
    std::vector<std::tuple<DWORD, short, short, short, short, WORD, WORD>> items;
    items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 24, kUninstallTitle, kStatic);
    y += 28;
    items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 28, kUninstallText, kStatic);
    y += 34;
    items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 10, kUninstallAlso, kStatic);
    y += 14;
    items.emplace_back(kBox, kMargin, y, kInner, 10, kRuntimesBox, kButton);
    y += 12;
    items.emplace_back(kNote, kMargin + kIndent, y, kInner - kIndent, 18, kRuntimesNote, kStatic);
    y += 22;
    items.emplace_back(kBox, kMargin, y, kInner, 10, kDatabasesBox, kButton);
    y += 12;
    items.emplace_back(kNote, kMargin + kIndent, y, kInner - kIndent, 18, kDatabasesNote, kStatic);
    y += 22;
    if (!projects_dir.empty()) {
        items.emplace_back(kBox, kMargin, y, kInner, 10, kProjectsBox, kButton);
        y += 12;
        // The folder's path, and that projects added from elsewhere stay; a long path breaks anywhere.
        items.emplace_back(kNote, kMargin + kIndent, y, kInner - kIndent, 26, kProjectsNote, kStatic);
        y += 30;
    }
    y += 6;
    constexpr short kRemoveW = 84;
    constexpr short kCancelW = 60;
    constexpr short kButtonH = 14;
    // Drawn here, in red; still a push button to the keyboard and to screen readers.
    items.emplace_back(BS_OWNERDRAW | WS_TABSTOP, kWidth - kMargin - kCancelW - 6 - kRemoveW, y, kRemoveW, kButtonH,
                       kUninstallButton, kButton);
    items.emplace_back(BS_DEFPUSHBUTTON | WS_TABSTOP, kWidth - kMargin - kCancelW, y, kCancelW, kButtonH, IDCANCEL,
                       kButton);
    y += kButtonH + kMargin;
    // Mirrored in Arabic and Kurdish: the boxes, the text and the buttons from the right.
    DialogTemplate dialog(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU,
                          is_rtl(lang) ? WS_EX_LAYOUTRTL | WS_EX_RTLREADING : 0U, kWidth, y);
    for (const auto& [style, x, iy, cx, cy, id, kind] : items) {
        dialog.add(style, x, iy, cx, cy, id, kind);
    }
    const INT_PTR answer = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dialog.get(), owner,
                                                   uninstall_dialog_proc, reinterpret_cast<LPARAM>(&d));
    if (answer != 1) {
        return false;  // Cancel, Esc, closed, or no dialog at all: nothing is removed
    }
    ticks = d.ticks;
    return true;
}

namespace {

// The Rename dialog's controls.
constexpr int kRenameButton = 110;
constexpr int kRenameTitle = 111;
constexpr int kNameLabel = 112;
constexpr int kNameEdit = 113;
constexpr int kNameHelp = 114;
constexpr int kFolderBox = 115;
constexpr int kFolderNote = 116;
constexpr int kDatabaseNote = 117;

struct RenameDialog {
    std::function<RenameQuestion(const std::string&)> ask;
    RenameQuestion first;
    std::wstring current;
    std::string name;
    bool folder = false;
    bool problem = false;  // the help says what's wrong
    HFONT bold = nullptr;
};

std::string dialog_text(HWND dialog, int id) {
    HWND control = GetDlgItem(dialog, id);
    const int n = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(control, text.data(), n + 1);
    text.resize(static_cast<std::size_t>(n));
    return win::narrow(text);
}

// The words for what's typed now: the help (or what's wrong), the folder's
// new name, and whether Rename can be pressed.
RenameQuestion sync_rename(HWND dialog, RenameDialog& d) {
    const RenameQuestion q = d.ask(dialog_text(dialog, kNameEdit));
    SetDlgItemTextW(dialog, kNameHelp, win::widen(q.help).c_str());
    SetDlgItemTextW(dialog, kFolderNote, win::widen(q.folder_note).c_str());
    set_accessible_help(GetDlgItem(dialog, kNameEdit), q.help);
    set_accessible_help(GetDlgItem(dialog, kFolderBox), q.folder_note);
    EnableWindow(GetDlgItem(dialog, kRenameButton), q.can_rename ? TRUE : FALSE);
    if (q.problem != d.problem) {
        d.problem = q.problem;
        InvalidateRect(GetDlgItem(dialog, kNameHelp), nullptr, TRUE);  // its colour
    }
    return q;
}

bool high_contrast() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof hc;
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) != FALSE && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

INT_PTR CALLBACK rename_dialog_proc(HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* d = win::int_to_ptr<RenameDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            d = win::int_to_ptr<RenameDialog*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            const RenameQuestion& q = d->first;
            SetWindowTextW(dialog, win::widen(q.window_title).c_str());
            SetDlgItemTextW(dialog, kRenameTitle, win::widen(q.title).c_str());
            SetDlgItemTextW(dialog, kNameLabel, win::widen(q.label).c_str());
            SetDlgItemTextW(dialog, kFolderBox, win::widen(q.folder).c_str());
            SetDlgItemTextW(dialog, kDatabaseNote, win::widen(q.database_note).c_str());
            SetDlgItemTextW(dialog, kRenameButton, win::widen(q.rename).c_str());
            SetDlgItemTextW(dialog, IDCANCEL, win::widen(q.cancel).c_str());
            HWND edit = GetDlgItem(dialog, kNameEdit);
            SendMessageW(edit, EM_LIMITTEXT, 63, 0);  // a project name's most
            SetWindowTextW(edit, d->current.c_str());
            // Its label is a static before it; said as its name all the same, as New project's box.
            set_accessible_name(edit, q.label);
            (void)sync_rename(dialog, *d);
            // The title in bold, a little larger, as the remove question's.
            LOGFONTW lf{};
            if (const auto font = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
                font != nullptr && GetObjectW(font, sizeof lf, &lf) != 0) {
                lf.lfWeight = FW_SEMIBOLD;
                lf.lfHeight = lf.lfHeight * 4 / 3;
                d->bold = CreateFontIndirectW(&lf);
                SendDlgItemMessageW(dialog, kRenameTitle, WM_SETFONT, reinterpret_cast<WPARAM>(d->bold), FALSE);
            }
            // Rename is the default (Enter, once the name can be used); Esc is Cancel.
            SendMessageW(dialog, DM_SETDEFID, kRenameButton, 0);
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            return FALSE;
        }
        case WM_CTLCOLORSTATIC: {
            // What's wrong with the name in the error colour; the database
            // note grey. High contrast keeps the system's colours.
            const auto id = GetDlgCtrlID(win::int_to_ptr<HWND>(static_cast<INT_PTR>(lparam)));
            if (d == nullptr || high_contrast() || (id != kNameHelp && id != kDatabaseNote) ||
                (id == kNameHelp && !d->problem)) {
                break;
            }
            auto* dc = win::int_to_ptr<HDC>(static_cast<INT_PTR>(wparam));
            const Rgb ink = id == kNameHelp ? palette(false).problem.text : palette(false).text_subtle;
            SetTextColor(dc, colorref(ink));
            SetBkColor(dc, GetSysColor(COLOR_3DFACE));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
        }
        case WM_COMMAND:
            if (d == nullptr) {
                break;
            }
            if (LOWORD(wparam) == kNameEdit && HIWORD(wparam) == EN_CHANGE) {
                (void)sync_rename(dialog, *d);
                return TRUE;
            }
            if (LOWORD(wparam) == kRenameButton) {
                // Asked again: Enter reaches here whatever the button's state.
                if (!sync_rename(dialog, *d).can_rename) {
                    MessageBeep(MB_OK);
                    SetFocus(GetDlgItem(dialog, kNameEdit));
                    return TRUE;
                }
                d->name = dialog_text(dialog, kNameEdit);
                d->folder = IsDlgButtonChecked(dialog, kFolderBox) == BST_CHECKED;
                EndDialog(dialog, 1);
                return TRUE;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                EndDialog(dialog, 0);
                return TRUE;
            }
            break;
        case WM_DESTROY:
            if (d != nullptr) {
                set_accessible_name(GetDlgItem(dialog, kNameEdit), {});
                set_accessible_help(GetDlgItem(dialog, kNameEdit), {});
                set_accessible_help(GetDlgItem(dialog, kFolderBox), {});
                if (d->bold != nullptr) {
                    DeleteObject(d->bold);
                    d->bold = nullptr;
                }
            }
            break;
        default:
            break;
    }
    return FALSE;
}

}  // namespace

bool ask_rename_project(HWND owner, Lang lang, const std::string& current,
                        const std::function<RenameQuestion(const std::string&)>& question, std::string& new_name,
                        bool& rename_folder) {
    new_name.clear();
    rename_folder = false;
    RenameDialog d;
    d.ask = question;
    d.first = question(current);
    d.current = win::widen(current);
    // In dialog units, as the remove question: the title, the box with its
    // label and help, the folder's box and note (when it has a folder), the
    // database note (when it has one), then Rename and Cancel.
    constexpr short kWidth = 280;
    constexpr short kMargin = 10;
    constexpr short kInner = kWidth - 2 * kMargin;
    const bool rtl = is_rtl(lang);
    short y = kMargin;
    std::vector<std::tuple<DWORD, short, short, short, short, WORD, WORD>> items;
    items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 24, kRenameTitle, 0x0082);
    y += 28;
    items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 10, kNameLabel, 0x0082);
    y += 11;
    // a–z as it's typed, as New project's name box.
    items.emplace_back(ES_LEFT | ES_AUTOHSCROLL | ES_LOWERCASE | WS_BORDER | WS_TABSTOP, kMargin, y, kInner, 14,
                       kNameEdit, 0x0081);
    y += 17;
    items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 20, kNameHelp, 0x0082);
    y += 24;
    if (!d.first.folder.empty()) {
        items.emplace_back(BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP | WS_TABSTOP, kMargin, y, kInner, 12, kFolderBox,
                           0x0080);
        y += 14;
        // The folder and its new name, under the box; a long path breaks anywhere.
        items.emplace_back(SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, kMargin + 12, y, kInner - 12, 26, kFolderNote,
                           0x0082);
        y += 30;
    }
    if (!d.first.database_note.empty()) {
        items.emplace_back(SS_LEFT | SS_NOPREFIX, kMargin, y, kInner, 18, kDatabaseNote, 0x0082);
        y += 22;
    }
    y += 6;
    constexpr short kButtonW = 60;
    constexpr short kButtonH = 14;
    items.emplace_back(BS_DEFPUSHBUTTON | WS_TABSTOP, kWidth - kMargin - 2 * kButtonW - 6, y, kButtonW, kButtonH,
                       kRenameButton, 0x0080);
    items.emplace_back(BS_PUSHBUTTON | WS_TABSTOP, kWidth - kMargin - kButtonW, y, kButtonW, kButtonH, IDCANCEL,
                       0x0080);
    y += kButtonH + kMargin;
    // Mirrored in Arabic and Kurdish, as the remove question.
    DialogTemplate dialog(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU,
                          rtl ? WS_EX_LAYOUTRTL | WS_EX_RTLREADING : 0U, kWidth, y);
    for (const auto& [style, x, iy, cx, cy, id, kind] : items) {
        dialog.add(style, x, iy, cx, cy, id, kind);
    }
    const INT_PTR answer = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dialog.get(), owner,
                                                   rename_dialog_proc, reinterpret_cast<LPARAM>(&d));
    if (answer != 1) {
        return false;  // Cancel, Esc, closed, or no dialog at all: nothing changes
    }
    new_name = d.name;
    rename_folder = d.folder;
    return true;
}

}  // namespace floserver::ui

#include "backup_dialogs.h"

#include "access_provider.h"
#include "dialog_template.h"
#include "model/view_model.h"
#include "win_util.h"

#include "floserver/core/backup.h"
#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/service_client.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

namespace floserver::ui {

namespace {

using nlohmann::json;

std::wstring w(std::string_view s) { return win::widen(s); }

// The dialogs' controls.
constexpr WORD kTitle = 101;
constexpr WORD kFileLabel = 102;
constexpr WORD kFileEdit = 103;
constexpr WORD kFileButton = 104;  // Change… or Choose…
constexpr WORD kIncludeLabel = 105;
constexpr WORD kDatabaseBox = 106;
constexpr WORD kDatabaseNote = 107;
constexpr WORD kLeaveLabel = 108;
constexpr WORD kTotal = 109;
constexpr WORD kDescribed = 110;
constexpr WORD kNameLabel = 111;
constexpr WORD kNameEdit = 112;
constexpr WORD kWhere = 113;
constexpr WORD kNameProblem = 114;
constexpr WORD kInstallBox = 115;
constexpr WORD kFilesBox = 116;
constexpr WORD kFilesNote = 117;
constexpr WORD kNote = 118;
constexpr WORD kLine = 119;
constexpr WORD kBar = 120;
constexpr WORD kForeignNote = 121;
constexpr WORD kFolderBox = 130;   // + i
constexpr WORD kFolderNote = 150;  // + i
constexpr WORD kConfirm = IDOK;

// In dialog units.
constexpr short kWidth = 300;
constexpr short kMargin = 10;
constexpr short kInner = kWidth - 2 * kMargin;
constexpr short kButtonW = 70;
constexpr short kButtonH = 14;
constexpr short kIndent = 12;

constexpr UINT kJobChanged = WM_APP + 30;

// Red for a destructive button (Restore), as Windows 11's own.
constexpr COLORREF kDanger = RGB(0xC4, 0x2B, 0x1C);
constexpr COLORREF kDangerPressed = RGB(0x9E, 0x22, 0x16);

// The items of one dialog, laid out top to bottom.
class Layout {
public:
    void add(DWORD style, short height, WORD id, WORD kind, short indent = 0, short gap = 4) {
        items_.emplace_back(style, static_cast<short>(kMargin + indent), y_, static_cast<short>(kInner - indent),
                            height, id, kind);
        y_ = static_cast<short>(y_ + height + gap);
    }
    // A box and a button at its end on one line (Save as + Change…).
    void add_with_button(WORD edit, WORD button, DWORD edit_style) {
        constexpr short kGap = 6;
        items_.emplace_back(edit_style | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kMargin, y_,
                            static_cast<short>(kInner - kButtonW - kGap), kButtonH, edit, DialogTemplate::kEdit);
        items_.emplace_back(BS_PUSHBUTTON | WS_TABSTOP, static_cast<short>(kWidth - kMargin - kButtonW), y_, kButtonW,
                            kButtonH, button, DialogTemplate::kButton);
        y_ = static_cast<short>(y_ + kButtonH + 6);
    }
    void space(short dy) { y_ = static_cast<short>(y_ + dy); }
    // The two buttons at the end of the last line, the confirming one first.
    void buttons(DWORD confirm_style, DWORD cancel_style) {
        space(4);
        items_.emplace_back(confirm_style | WS_TABSTOP, static_cast<short>(kWidth - kMargin - 2 * kButtonW - 6), y_,
                            kButtonW, kButtonH, kConfirm, DialogTemplate::kButton);
        items_.emplace_back(cancel_style | WS_TABSTOP, static_cast<short>(kWidth - kMargin - kButtonW), y_, kButtonW,
                            kButtonH, static_cast<WORD>(IDCANCEL), DialogTemplate::kButton);
        y_ = static_cast<short>(y_ + kButtonH + kMargin);
    }
    // A progress bar (a class named in full).
    void bar(WORD id) {
        bar_ = {kMargin, y_, kInner, 10, id};
        y_ = static_cast<short>(y_ + 16);
    }
    [[nodiscard]] INT_PTR run(HWND owner, Lang lang, DLGPROC proc, LPARAM param) const {
        DialogTemplate dialog(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU,
                              is_rtl(lang) ? WS_EX_LAYOUTRTL | WS_EX_RTLREADING : 0U, kWidth, y_);
        for (const auto& [style, x, iy, cx, cy, id, kind] : items_) {
            dialog.add(style, x, iy, cx, cy, id, kind);
        }
        if (bar_) {
            const auto& [x, iy, cx, cy, id] = *bar_;
            dialog.add(0, x, iy, cx, cy, id, PROGRESS_CLASSW);
        }
        return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dialog.get(), owner, proc, param);
    }

private:
    short y_ = kMargin;
    std::vector<std::tuple<DWORD, short, short, short, short, WORD, WORD>> items_;
    std::optional<std::tuple<short, short, short, short, WORD>> bar_;
};

// What every dialog here does when it opens: its words, its title in bold
// (a task dialog's main line), and where the keys start.
struct Common {
    std::wstring window_title;
    std::vector<std::pair<WORD, std::wstring>> texts;
    HFONT bold = nullptr;
    WORD default_id = kConfirm;
    WORD focus_id = kConfirm;
    bool rtl = false;

    void open(HWND dialog) {
        SetWindowTextW(dialog, window_title.c_str());
        for (const auto& [id, text] : texts) {
            SetDlgItemTextW(dialog, id, text.c_str());
        }
        LOGFONTW lf{};
        if (const auto font = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
            font != nullptr && GetObjectW(font, sizeof lf, &lf) != 0) {
            lf.lfWeight = FW_SEMIBOLD;
            lf.lfHeight = lf.lfHeight * 4 / 3;
            bold = CreateFontIndirectW(&lf);
            SendDlgItemMessageW(dialog, kTitle, WM_SETFONT, reinterpret_cast<WPARAM>(bold), FALSE);
        }
        SendMessageW(dialog, DM_SETDEFID, default_id, 0);
        SetFocus(GetDlgItem(dialog, focus_id));
    }
    void close() {
        if (bold != nullptr) {
            DeleteObject(bold);
            bold = nullptr;
        }
    }
};

std::string item_text(HWND dialog, int id) {
    HWND item = GetDlgItem(dialog, id);
    const int n = GetWindowTextLengthW(item);
    std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(item, text.data(), n + 1);
    text.resize(static_cast<std::size_t>(n));
    return win::narrow(text);
}

void set_item(HWND dialog, int id, const std::string& text) { SetDlgItemTextW(dialog, id, w(text).c_str()); }

bool ticked(HWND dialog, int id) { return IsDlgButtonChecked(dialog, id) == BST_CHECKED; }

void show_item(HWND dialog, int id, bool shown) { ShowWindow(GetDlgItem(dialog, id), shown ? SW_SHOW : SW_HIDE); }

// A full path on this computer or a share: "C:\..." or "\\server\...".
bool full_path(const std::string& p) {
    return (p.size() >= 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) || p.starts_with("\\\\");
}

std::string folder_of(const std::string& file) {
    const std::size_t slash = file.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : file.substr(0, slash);
}

std::string name_of(const std::string& file) {
    const std::size_t slash = file.find_last_of("\\/");
    return slash == std::string::npos ? file : file.substr(slash + 1);
}

// Opens `dialog` in `folder` when it's an existing one.
void start_in(IFileDialog* dialog, const std::string& folder) {
    if (folder.empty()) {
        return;
    }
    const std::wstring wide = w(folder);
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return;
    }
    IShellItem* at = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_PPV_ARGS(&at))) && at != nullptr) {
        dialog->SetFolder(at);
        at->Release();
    }
}

std::optional<std::string> dialog_result(IFileDialog* dialog) {
    std::optional<std::string> path;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
        PWSTR chosen = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &chosen)) && chosen != nullptr) {
            path = win::narrow(chosen);
            CoTaskMemFree(chosen);
        }
        item->Release();
    }
    return path;
}

// Windows' save or open picker for a .zip; the file, or nothing when cancelled.
std::optional<std::string> pick_zip(HWND owner, Lang lang, bool save, const std::string& current) {
    IFileDialog* dialog = nullptr;
    if (save) {
        IFileSaveDialog* made = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&made)))) {
            dialog = made;
        }
    } else {
        IFileOpenDialog* made = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&made)))) {
            dialog = made;
        }
    }
    if (dialog == nullptr) {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    options |= save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
    dialog->SetOptions(options);
    const std::wstring title = w(text(lang, save ? Str::BackupSaveTitle : Str::FromBackupPick));
    dialog->SetTitle(title.c_str());
    const std::wstring filter = w(text(lang, Str::BackupZipFiles)) + L" (*.zip)";
    const COMDLG_FILTERSPEC types[] = {{filter.c_str(), L"*.zip"}};
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"zip");
    if (save) {
        start_in(dialog, folder_of(current));
        const std::wstring name = w(name_of(current));
        dialog->SetFileName(name.c_str());
    } else {
        start_in(dialog, current);
    }
    std::optional<std::string> path;
    if (SUCCEEDED(dialog->Show(owner))) {
        path = dialog_result(dialog);
    }
    dialog->Release();
    return path;
}

// A chosen backup, read from the zip on disk: what it is, or why it can't be used.
struct Inspected {
    std::optional<BackupSummary> summary;
    std::string error;
};
Inspected inspect(const std::string& file) {
    Inspected out;
    const auto info = core::read_backup_info(core::path_from_utf8(file), out.error);
    out.error = core::shown_text(out.error);  // it may quote a name from the zip
    if (info) {
        const auto data = core::data_dir();
        out.summary = BackupSummary{info->name, info->date, info->versions, info->database, info->left_out,
                                    data && core::made_here(*info, *data)};
    }
    return out;
}

// ------------------------------------------------------------------ back up

struct BackupDialog {
    Common common;
    const BackupPlan* plan = nullptr;
    Lang lang = Lang::En;
    BackupChoice choice;
    std::vector<std::wstring> notes;  // kept alive: screen readers' help for each box
};

std::vector<bool> leave_out_ticks(HWND dialog, const BackupPlan& plan) {
    std::vector<bool> out;
    for (std::size_t i = 0; i < plan.folders.size(); ++i) {
        out.push_back(ticked(dialog, kFolderBox + static_cast<int>(i)));
    }
    return out;
}

void update_total(HWND dialog, const BackupDialog& d) {
    set_item(dialog, kTotal,
             backup_total_text(*d.plan, ticked(dialog, kDatabaseBox), leave_out_ticks(dialog, *d.plan), d.lang));
}

INT_PTR CALLBACK backup_proc(HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* d = win::int_to_ptr<BackupDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            d = win::int_to_ptr<BackupDialog*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            d->common.open(dialog);
            const BackupForm form = backup_form(*d->plan, d->lang);
            if (d->plan->database) {
                CheckDlgButton(dialog, kDatabaseBox, BST_CHECKED);
                set_accessible_help(GetDlgItem(dialog, kDatabaseBox), form.database_detail);
            }
            for (std::size_t i = 0; i < d->plan->folders.size(); ++i) {
                const int box = kFolderBox + static_cast<int>(i);
                CheckDlgButton(dialog, box, BST_CHECKED);
                set_accessible_help(GetDlgItem(dialog, box), form.folders[i].second);
            }
            update_total(dialog, *d);
            return FALSE;
        }
        case WM_COMMAND: {
            if (d == nullptr) {
                break;
            }
            const auto id = LOWORD(wparam);
            if (id == kFileButton) {
                if (const auto file = pick_zip(dialog, d->lang, true, item_text(dialog, kFileEdit))) {
                    set_item(dialog, kFileEdit, *file);
                }
                return TRUE;
            }
            if (id == kDatabaseBox || (id >= kFolderBox && id < kFolderBox + 20)) {
                update_total(dialog, *d);
                return TRUE;
            }
            if (id == kConfirm) {
                const std::string file = with_zip_extension(item_text(dialog, kFileEdit));
                if (!full_path(file)) {
                    // A name alone: the picker says where.
                    if (const auto picked = pick_zip(dialog, d->lang, true, file)) {
                        set_item(dialog, kFileEdit, *picked);
                    }
                    SetFocus(GetDlgItem(dialog, kFileEdit));
                    return TRUE;
                }
                d->choice.file = file;
                d->choice.database = d->plan->database.has_value() && ticked(dialog, kDatabaseBox);
                d->choice.leave_out = leave_out_ticks(dialog, *d->plan);
                EndDialog(dialog, 1);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dialog, 0);
                return TRUE;
            }
            break;
        }
        case WM_DESTROY:
            if (d != nullptr) {
                d->common.close();
            }
            break;
        default:
            break;
    }
    return FALSE;
}

// ------------------------------------------------------------------ new project from a backup

struct FromBackupDialog {
    Common common;
    Lang lang = Lang::En;
    const FromBackupSetup* setup = nullptr;
    std::optional<BackupSummary> summary;
    FromBackupChoice choice;
    bool setting_name = false;
};

void from_backup_check_name(HWND dialog, FromBackupDialog& d) {
    const std::string name = item_text(dialog, kNameEdit);
    const bool usable = is_project_name(name) && !(d.setup->name_taken && d.setup->name_taken(name));
    set_item(dialog, kWhere, name.empty() ? std::string() : from_backup_where(d.setup->projects_dir, name, d.lang));
    set_item(dialog, kNameProblem, usable || name.empty() ? std::string() : std::string(text(d.lang, Str::WizardNameRule)));
    EnableWindow(GetDlgItem(dialog, kConfirm), usable && d.summary ? TRUE : FALSE);
}

void from_backup_load(HWND dialog, FromBackupDialog& d, const std::string& file) {
    set_item(dialog, kFileEdit, file);
    const Inspected found = inspect(file);
    d.summary = found.summary;
    d.choice.file = file;
    if (!found.summary) {
        set_item(dialog, kDescribed, fill(d.lang, Str::BackupNotUsable, {found.error}));
        show_item(dialog, kDatabaseNote, false);
        show_item(dialog, kInstallBox, false);
        show_item(dialog, kForeignNote, false);
        EnableWindow(GetDlgItem(dialog, kConfirm), FALSE);
        return;
    }
    set_item(dialog, kDescribed, describe_backup(*found.summary, d.lang));
    d.setting_name = true;
    set_item(dialog, kNameEdit, core::copy_name(found.summary->name, d.setup->name_taken));
    d.setting_name = false;
    show_item(dialog, kDatabaseNote, found.summary->database);
    show_item(dialog, kInstallBox, !found.summary->left_out.empty());
    // Ticked for a backup made here; one from elsewhere would run its
    // packages' own scripts, so the user ticks it (the note says why).
    CheckDlgButton(dialog, kInstallBox, found.summary->made_here ? BST_CHECKED : BST_UNCHECKED);
    show_item(dialog, kForeignNote, !found.summary->left_out.empty() && !found.summary->made_here);
    from_backup_check_name(dialog, d);
}

INT_PTR CALLBACK from_backup_proc(HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* d = win::int_to_ptr<FromBackupDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG:
            d = win::int_to_ptr<FromBackupDialog*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            d->common.open(dialog);
            SendDlgItemMessageW(dialog, kNameEdit, EM_LIMITTEXT, 63, 0);  // a project name's most
            if (!d->setup->file.empty()) {
                from_backup_load(dialog, *d, d->setup->file);
                SetFocus(GetDlgItem(dialog, kNameEdit));
            } else {
                show_item(dialog, kDatabaseNote, false);
                show_item(dialog, kInstallBox, false);
                show_item(dialog, kForeignNote, false);
                EnableWindow(GetDlgItem(dialog, kConfirm), FALSE);
                SetFocus(GetDlgItem(dialog, kFileButton));
            }
            return FALSE;
        case WM_COMMAND: {
            if (d == nullptr) {
                break;
            }
            const auto id = LOWORD(wparam);
            if (id == kNameEdit && HIWORD(wparam) == EN_CHANGE && !d->setting_name) {
                from_backup_check_name(dialog, *d);
                return TRUE;
            }
            if (id == kFileButton) {
                if (const auto file = pick_zip(dialog, d->lang, false, backups_folder())) {
                    from_backup_load(dialog, *d, *file);
                }
                return TRUE;
            }
            if (id == kConfirm) {
                const std::string name = item_text(dialog, kNameEdit);
                if (!d->summary || !is_project_name(name)) {
                    return TRUE;
                }
                d->choice.name = name;
                d->choice.install = !d->summary->left_out.empty() && ticked(dialog, kInstallBox);
                EndDialog(dialog, 1);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dialog, 0);
                return TRUE;
            }
            break;
        }
        case WM_DESTROY:
            if (d != nullptr) {
                d->common.close();
            }
            break;
        default:
            break;
    }
    return FALSE;
}

// ------------------------------------------------------------------ restore

struct RestoreDialog {
    Common common;
    Lang lang = Lang::En;
    std::string files_note;
    std::string database_note;
    bool has_database = false;
    std::string start;
    std::optional<BackupSummary> summary;
    RestoreChoice choice;
};

void restore_update(HWND dialog, const RestoreDialog& d) {
    const bool database = d.summary && d.summary->database && d.has_database;
    EnableWindow(GetDlgItem(dialog, kDatabaseBox), database ? TRUE : FALSE);
    if (!database) {
        CheckDlgButton(dialog, kDatabaseBox, BST_UNCHECKED);
    }
    const bool files = ticked(dialog, kFilesBox);
    EnableWindow(GetDlgItem(dialog, kInstallBox), files && d.summary && !d.summary->left_out.empty() ? TRUE : FALSE);
    const bool something = files || ticked(dialog, kDatabaseBox);
    EnableWindow(GetDlgItem(dialog, kConfirm), d.summary && something ? TRUE : FALSE);
}

void restore_load(HWND dialog, RestoreDialog& d, const std::string& file) {
    set_item(dialog, kFileEdit, file);
    const Inspected found = inspect(file);
    d.summary = found.summary;
    d.choice.file = file;
    set_item(dialog, kDescribed, found.summary ? restore_backed_up(*found.summary, d.lang)
                                               : fill(d.lang, Str::BackupNotUsable, {found.error}));
    if (found.summary && found.summary->database && d.has_database) {
        CheckDlgButton(dialog, kDatabaseBox, BST_CHECKED);
    }
    const bool made_here = found.summary && found.summary->made_here;
    CheckDlgButton(dialog, kInstallBox, made_here ? BST_CHECKED : BST_UNCHECKED);
    show_item(dialog, kForeignNote, found.summary && !made_here && !found.summary->left_out.empty());
    restore_update(dialog, d);
}

// Restore in red, white words: what can't be undone looks it.
void draw_danger_button(const DRAWITEMSTRUCT& item, bool rtl) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const COLORREF fill = disabled ? GetSysColor(COLOR_BTNFACE) : pressed ? kDangerPressed : kDanger;
    const COLORREF ink = disabled ? GetSysColor(COLOR_GRAYTEXT) : RGB(0xFF, 0xFF, 0xFF);
    RECT r = item.rcItem;
    const win::UniqueBrush brush(CreateSolidBrush(fill));
    FillRect(item.hDC, &r, brush.get());
    const win::UniqueBrush edge(CreateSolidBrush(disabled ? GetSysColor(COLOR_GRAYTEXT) : kDangerPressed));
    FrameRect(item.hDC, &r, edge.get());
    const int n = GetWindowTextLengthW(item.hwndItem);
    std::wstring label(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(item.hwndItem, label.data(), n + 1);
    label.resize(static_cast<std::size_t>(n));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, ink);
    DrawTextW(item.hDC, label.c_str(), n, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | (rtl ? UINT{DT_RTLREADING} : 0U));
    if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0) {
        InflateRect(&r, -3, -3);
        SetTextColor(item.hDC, RGB(0, 0, 0));
        DrawFocusRect(item.hDC, &r);
    }
}

INT_PTR CALLBACK restore_proc(HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* d = win::int_to_ptr<RestoreDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG:
            d = win::int_to_ptr<RestoreDialog*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            d->common.open(dialog);
            CheckDlgButton(dialog, kFilesBox, BST_CHECKED);
            show_item(dialog, kForeignNote, false);
            // What each box does, said with it.
            set_accessible_help(GetDlgItem(dialog, kFilesBox), d->files_note);
            if (HWND box = GetDlgItem(dialog, kDatabaseBox); box != nullptr) {
                set_accessible_help(box, d->database_note);
            }
            restore_update(dialog, *d);
            return FALSE;
        case WM_DRAWITEM: {
            const auto* item = win::int_to_ptr<const DRAWITEMSTRUCT*>(lparam);
            if (d != nullptr && item != nullptr && item->CtlID == kConfirm) {
                draw_danger_button(*item, d->common.rtl);
                SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            if (d == nullptr) {
                break;
            }
            const auto id = LOWORD(wparam);
            if (id == kFileButton) {
                if (const auto file = pick_zip(dialog, d->lang, false, d->start)) {
                    restore_load(dialog, *d, *file);
                }
                return TRUE;
            }
            if (id == kFilesBox || id == kDatabaseBox) {
                restore_update(dialog, *d);
                return TRUE;
            }
            // Enter goes to the default button, Cancel, even with the keys on
            // Restore (an owner-drawn button is never the default): Enter
            // there means Restore, as Space does.
            const bool enter_on_restore = id == IDCANCEL && GetFocus() == GetDlgItem(dialog, kConfirm) &&
                                          (GetKeyState(VK_RETURN) & 0x8000) != 0;
            if (id == kConfirm || enter_on_restore) {
                if (!d->summary) {
                    return TRUE;
                }
                d->choice.files = ticked(dialog, kFilesBox);
                d->choice.database = ticked(dialog, kDatabaseBox);
                d->choice.install = d->choice.files && ticked(dialog, kInstallBox);
                if (!d->choice.files && !d->choice.database) {
                    return TRUE;
                }
                EndDialog(dialog, 1);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(dialog, 0);
                return TRUE;
            }
            break;
        }
        case WM_DESTROY:
            if (d != nullptr) {
                d->common.close();
            }
            break;
        default:
            break;
    }
    return FALSE;
}

// ------------------------------------------------------------------ the job

// Shared by the dialog and the thread that follows the job.
struct JobBox {
    std::mutex mutex;
    HWND dialog = nullptr;  // null once it's gone
    json job;               // backup.get's latest
    std::string error;
    bool ended = false;
    std::atomic<bool> cancel{false};
    std::atomic<bool> abandon{false};  // the dialog couldn't be shown: stop following
};

void follow_job(const std::shared_ptr<JobBox>& box, const std::string& method, const json& params) {
    const auto post = [&box](bool ended) {
        const std::lock_guard lock(box->mutex);
        box->ended = box->ended || ended;
        if (box->dialog != nullptr) {
            PostMessageW(box->dialog, kJobChanged, 0, 0);
        }
    };
    const auto fail = [&](const std::string& why) {
        {
            const std::lock_guard lock(box->mutex);
            box->error = why;
        }
        post(true);
    };
    std::string error;
    auto client = core::ServiceClient::connect(core::ipc::default_instance(), error);
    if (!client) {
        fail(error.empty() ? "FloServer isn't running" : error);
        return;
    }
    const auto started = client->call(method, params);
    if (!started || !started->ok || !started->result.contains("job") || !started->result.at("job").is_number_integer()) {
        fail(started ? started->error_message : "FloServer stopped answering");
        return;
    }
    const auto id = started->result.at("job").get<std::int64_t>();
    bool cancel_sent = false;
    while (!box->abandon) {
        if (box->cancel && !cancel_sent) {
            (void)client->call("backup.cancel", {{"id", id}});
            cancel_sent = true;
        }
        const auto reply = client->call("backup.get", {{"id", id}});
        if (!reply || !reply->ok || !reply->result.is_object()) {
            fail(reply ? reply->error_message : "FloServer stopped answering");
            return;
        }
        const bool ended = reply->result.value("state", std::string()) != "running";
        {
            const std::lock_guard lock(box->mutex);
            box->job = reply->result;
        }
        post(ended);
        if (ended) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

struct JobDialog {
    Common common;
    Lang lang = Lang::En;
    std::shared_ptr<JobBox> box;
    bool cancel_pressed = false;
    bool marquee = false;
};

void job_update(HWND dialog, JobDialog& d) {
    json job;
    bool ended = false;
    {
        const std::lock_guard lock(d.box->mutex);
        job = d.box->job;
        ended = d.box->ended;
    }
    if (ended) {
        EndDialog(dialog, 1);
        return;
    }
    if (!job.is_object()) {
        return;
    }
    set_item(dialog, kLine, backup_progress_text(job, d.lang));
    HWND bar = GetDlgItem(dialog, kBar);
    const auto percent = backup_progress_percent(job);
    if (percent) {
        if (d.marquee) {
            SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
            SetWindowLongPtrW(bar, GWL_STYLE, GetWindowLongPtrW(bar, GWL_STYLE) & ~LONG_PTR{PBS_MARQUEE});
            d.marquee = false;
        }
        SendMessageW(bar, PBM_SETPOS, static_cast<WPARAM>(*percent), 0);
    } else if (!d.marquee) {
        SetWindowLongPtrW(bar, GWL_STYLE, GetWindowLongPtrW(bar, GWL_STYLE) | LONG_PTR{PBS_MARQUEE});
        SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
        d.marquee = true;
    }
    // Once it's replacing things it can't be stopped: the button says so.
    const bool cancellable = job.value("cancellable", true);
    HWND cancel = GetDlgItem(dialog, IDCANCEL);
    if (!cancellable) {
        SetWindowTextW(cancel, w(text(d.lang, Str::BackupStillRunning)).c_str());
    }
    EnableWindow(cancel, cancellable && !d.cancel_pressed ? TRUE : FALSE);
}

INT_PTR CALLBACK job_proc(HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* d = win::int_to_ptr<JobDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG:
            d = win::int_to_ptr<JobDialog*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            d->common.open(dialog);
            {
                const std::lock_guard lock(d->box->mutex);
                d->box->dialog = dialog;
            }
            SendDlgItemMessageW(dialog, kBar, PBM_SETRANGE32, 0, 100);
            job_update(dialog, *d);
            return FALSE;
        case kJobChanged:
            if (d != nullptr) {
                job_update(dialog, *d);
            }
            return TRUE;
        case WM_COMMAND:
            // Cancel, Esc and the close box all ask the service to stop; the
            // dialog stays until the job says it has.
            if (d != nullptr && LOWORD(wparam) == IDCANCEL) {
                d->cancel_pressed = true;
                d->box->cancel = true;
                EnableWindow(GetDlgItem(dialog, IDCANCEL), FALSE);
                return TRUE;
            }
            break;
        case WM_DESTROY:
            if (d != nullptr) {
                const std::lock_guard lock(d->box->mutex);
                d->box->dialog = nullptr;
                d->common.close();
            }
            break;
        default:
            break;
    }
    return FALSE;
}

}  // namespace

std::string backups_folder() {
    const auto dir = core::default_backups_dir();
    return dir ? core::path_to_utf8(*dir) : std::string();
}

void show_file_in_folder(const std::string& file) {
    const std::wstring wide = w(file);
    PIDLIST_ABSOLUTE item = nullptr;
    if (SUCCEEDED(SHParseDisplayName(wide.c_str(), nullptr, &item, 0, nullptr)) && item != nullptr) {
        (void)SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        CoTaskMemFree(item);
    }
}

std::optional<BackupChoice> ask_backup(HWND owner, Lang lang, const BackupPlan& plan) {
    const BackupForm form = backup_form(plan, lang);
    BackupDialog d;
    d.plan = &plan;
    d.lang = lang;
    d.common.rtl = is_rtl(lang);
    d.common.window_title = w(form.title);
    Layout layout;
    const auto text_item = [&](WORD id, const std::string& words, short height, short indent = 0, short gap = 4) {
        layout.add(SS_LEFT | SS_NOPREFIX, height, id, DialogTemplate::kStatic, indent, gap);
        d.common.texts.emplace_back(id, w(words));
    };
    text_item(kTitle, form.title, 14, 0, 8);
    text_item(kFileLabel, form.save_as, 9, 0, 2);
    layout.add_with_button(kFileEdit, kFileButton, 0);
    d.common.texts.emplace_back(kFileEdit, w(plan.default_file.empty()
                                                  ? backups_folder() + "\\" + plan.name + ".zip"
                                                  : plan.default_file));
    d.common.texts.emplace_back(kFileButton, w(form.change));
    if (!form.database.empty()) {
        text_item(kIncludeLabel, form.include, 9, 0, 3);
        layout.add(BS_AUTOCHECKBOX | WS_TABSTOP, 10, kDatabaseBox, DialogTemplate::kButton, 0, 1);
        d.common.texts.emplace_back(kDatabaseBox, w(form.database));
        text_item(kDatabaseNote, form.database_detail, 9, kIndent, 6);
    }
    if (!form.leave_out.empty()) {
        text_item(kLeaveLabel, form.leave_out, 9, 0, 3);
        for (std::size_t i = 0; i < form.folders.size(); ++i) {
            const auto box = static_cast<WORD>(kFolderBox + i);
            layout.add(BS_AUTOCHECKBOX | WS_TABSTOP, 10, box, DialogTemplate::kButton, 0, 1);
            d.common.texts.emplace_back(box, w(form.folders[i].first));
            text_item(static_cast<WORD>(kFolderNote + i), form.folders[i].second, 9, kIndent, 4);
        }
        layout.space(2);
    }
    text_item(kTotal, {}, 9, 0, 4);
    layout.buttons(BS_DEFPUSHBUTTON, BS_PUSHBUTTON);
    d.common.texts.emplace_back(kConfirm, w(form.back_up));
    d.common.texts.emplace_back(static_cast<WORD>(IDCANCEL), w(form.cancel));
    d.common.default_id = kConfirm;
    d.common.focus_id = kConfirm;
    if (layout.run(owner, lang, backup_proc, reinterpret_cast<LPARAM>(&d)) != 1) {
        return std::nullopt;
    }
    return d.choice;
}

std::optional<FromBackupChoice> ask_from_backup(HWND owner, Lang lang, const FromBackupSetup& setup) {
    const FromBackupForm form = from_backup_form(lang);
    FromBackupDialog d;
    d.lang = lang;
    d.setup = &setup;
    d.common.rtl = is_rtl(lang);
    d.common.window_title = w(form.title);
    Layout layout;
    const auto text_item = [&](WORD id, const std::string& words, short height, short indent = 0, short gap = 4) {
        layout.add(SS_LEFT | SS_NOPREFIX, height, id, DialogTemplate::kStatic, indent, gap);
        d.common.texts.emplace_back(id, w(words));
    };
    text_item(kTitle, form.title, 14, 0, 8);
    text_item(kFileLabel, form.backup, 9, 0, 2);
    layout.add_with_button(kFileEdit, kFileButton, ES_READONLY);
    d.common.texts.emplace_back(kFileButton, w(form.choose));
    text_item(kDescribed, {}, 18, 0, 6);
    text_item(kNameLabel, form.name, 9, 0, 2);
    layout.add(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_LOWERCASE, kButtonH, kNameEdit, DialogTemplate::kEdit, 0, 3);
    text_item(kNameProblem, {}, 9, 0, 2);
    text_item(kWhere, {}, 18, 0, 4);
    text_item(kDatabaseNote, form.database_note, 18, 0, 4);
    layout.add(BS_AUTOCHECKBOX | WS_TABSTOP, 10, kInstallBox, DialogTemplate::kButton, 0, 1);
    d.common.texts.emplace_back(kInstallBox, w(form.install));
    text_item(kForeignNote, form.foreign, 18, kIndent, 4);
    layout.buttons(BS_DEFPUSHBUTTON, BS_PUSHBUTTON);
    d.common.texts.emplace_back(kConfirm, w(form.create));
    d.common.texts.emplace_back(static_cast<WORD>(IDCANCEL), w(form.cancel));
    d.common.default_id = kConfirm;
    d.common.focus_id = kFileButton;
    if (layout.run(owner, lang, from_backup_proc, reinterpret_cast<LPARAM>(&d)) != 1) {
        return std::nullopt;
    }
    return d.choice;
}

std::optional<RestoreChoice> ask_restore(HWND owner, Lang lang, const std::string& project, bool has_database,
                                         const std::string& start) {
    const RestoreForm form = restore_form(project, has_database, lang);
    RestoreDialog d;
    d.lang = lang;
    d.has_database = has_database;
    d.start = start;
    d.files_note = form.files_note;
    d.database_note = form.database_note;
    d.common.rtl = is_rtl(lang);
    d.common.window_title = w(form.title);
    Layout layout;
    const auto text_item = [&](WORD id, const std::string& words, short height, short indent = 0, short gap = 4) {
        layout.add(SS_LEFT | SS_NOPREFIX, height, id, DialogTemplate::kStatic, indent, gap);
        d.common.texts.emplace_back(id, w(words));
    };
    text_item(kTitle, form.title, 14, 0, 8);
    text_item(kFileLabel, form.backup, 9, 0, 2);
    layout.add_with_button(kFileEdit, kFileButton, ES_READONLY);
    d.common.texts.emplace_back(kFileButton, w(form.choose));
    text_item(kDescribed, {}, 9, 0, 8);
    layout.add(BS_AUTOCHECKBOX | WS_TABSTOP, 10, kFilesBox, DialogTemplate::kButton, 0, 1);
    d.common.texts.emplace_back(kFilesBox, w(form.files));
    text_item(kFilesNote, form.files_note, 9, kIndent, 5);
    if (!form.database.empty()) {
        layout.add(BS_AUTOCHECKBOX | WS_TABSTOP, 10, kDatabaseBox, DialogTemplate::kButton, 0, 1);
        d.common.texts.emplace_back(kDatabaseBox, w(form.database));
        text_item(kDatabaseNote, form.database_note, 9, kIndent, 5);
    }
    layout.add(BS_AUTOCHECKBOX | WS_TABSTOP, 10, kInstallBox, DialogTemplate::kButton, 0, 1);
    d.common.texts.emplace_back(kInstallBox, w(form.install));
    text_item(kForeignNote, form.foreign, 18, kIndent, 6);
    text_item(kNote, form.note, 9, 0, 4);
    // Restore in red; Cancel is the default and has the keys: Enter and Esc keep everything.
    layout.buttons(BS_OWNERDRAW, BS_DEFPUSHBUTTON);
    d.common.texts.emplace_back(kConfirm, w(form.restore));
    d.common.texts.emplace_back(static_cast<WORD>(IDCANCEL), w(form.cancel));
    d.common.default_id = static_cast<WORD>(IDCANCEL);
    d.common.focus_id = static_cast<WORD>(IDCANCEL);
    if (layout.run(owner, lang, restore_proc, reinterpret_cast<LPARAM>(&d)) != 1) {
        return std::nullopt;
    }
    return d.choice;
}

JobResult run_backup_job(HWND owner, Lang lang, const std::string& kind, const std::string& project,
                         const std::string& method, const json& params) {
    JobDialog d;
    d.lang = lang;
    d.box = std::make_shared<JobBox>();
    d.common.rtl = is_rtl(lang);
    const std::string title = backup_job_title(kind, project, lang);
    d.common.window_title = w(title);
    Layout layout;
    layout.add(SS_LEFT | SS_NOPREFIX, 14, kTitle, DialogTemplate::kStatic, 0, 8);
    d.common.texts.emplace_back(kTitle, w(title));
    layout.add(SS_LEFT | SS_NOPREFIX, 18, kLine, DialogTemplate::kStatic, 0, 4);
    layout.bar(kBar);
    // Wide enough for "It can't be stopped now" once it's replacing things.
    constexpr short kCancelW = 120;
    layout.add(BS_PUSHBUTTON | WS_TABSTOP, kButtonH, static_cast<WORD>(IDCANCEL), DialogTemplate::kButton,
               static_cast<short>(kInner - kCancelW), kMargin);
    d.common.texts.emplace_back(static_cast<WORD>(IDCANCEL), w(text(lang, Str::WizardCancel)));
    d.common.default_id = static_cast<WORD>(IDCANCEL);
    d.common.focus_id = static_cast<WORD>(IDCANCEL);
    std::thread follower;
    try {
        follower = std::thread([box = d.box, method, params] { follow_job(box, method, params); });
    } catch (const std::system_error&) {
        return {nullptr, "FloServer couldn't follow the job"};
    }
    const INT_PTR shown = layout.run(owner, lang, job_proc, reinterpret_cast<LPARAM>(&d));
    if (shown != 1) {
        d.box->abandon = true;  // no dialog: the job goes on in the service, unfollowed
    }
    follower.join();
    const std::lock_guard lock(d.box->mutex);
    return {d.box->job, d.box->error};
}

}  // namespace floserver::ui

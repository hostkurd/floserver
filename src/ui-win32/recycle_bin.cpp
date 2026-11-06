#include "recycle_bin.h"

#include "win_util.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifndef FOFX_RECYCLEONDELETE
#define FOFX_RECYCLEONDELETE 0x00080000
#endif
#ifndef FOF_WANTNUKEWARNING
#define FOF_WANTNUKEWARNING 0x4000
#endif

namespace floserver::ui {

namespace {

std::wstring w(std::string_view s) { return win::widen(s); }

// Where a folder really is (junctions above it followed), "\\?\C:\..."; empty when Windows won't say.
std::string real_path(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::wstring out(MAX_PATH, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, out.data(), static_cast<DWORD>(out.size()), VOLUME_NAME_DOS);
    if (n >= out.size()) {
        out.resize(n + 1);
        n = GetFinalPathNameByHandleW(h, out.data(), static_cast<DWORD>(out.size()), VOLUME_NAME_DOS);
    }
    CloseHandle(h);
    if (n == 0 || n >= out.size()) {
        return {};
    }
    out.resize(n);
    return win::narrow(out);
}

std::string hresult_text(HRESULT hr) {
    LPWSTR buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = n > 0 && buffer != nullptr ? std::wstring(buffer, n) : std::wstring();
    LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    if (text.empty()) {
        char hex[16] = {};
        (void)std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(hr));
        return std::string("Windows error ") + hex;
    }
    return win::narrow(text);
}

// COM interfaces have no virtual destructor (Release is the way out); GCC warns all the same.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
// What Windows did with each item it deleted: put it in the Recycle Bin
// (it names the new item there) or deleted it for good. A COM object on
// the move's stack; only IFileOperation holds it, while it runs.
class DeleteWatch final : public IFileOperationProgressSink {
public:
    DeleteWatch() = default;
    virtual ~DeleteWatch() = default;
    DeleteWatch(const DeleteWatch&) = delete;
    DeleteWatch& operator=(const DeleteWatch&) = delete;
    DeleteWatch(DeleteWatch&&) = delete;
    DeleteWatch& operator=(DeleteWatch&&) = delete;

    bool recycled = false;
    bool deleted = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IFileOperationProgressSink)) {
            *out = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return refs_ > 0 ? --refs_ : 0; }  // never deleted: on the stack

    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD /*flags*/, IShellItem* /*item*/, HRESULT result,
                                             IShellItem* created) override {
        if (SUCCEEDED(result)) {
            // Only an item that went to the Recycle Bin has a new one there.
            (created != nullptr ? recycled : deleted) = true;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT /*result*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

private:
    ULONG refs_ = 1;
};
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// The folder's drive is a fixed one that has a Recycle Bin.
bool has_recycle_bin(const std::wstring& path) {
    std::wstring root(MAX_PATH + 1, L'\0');
    if (GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size())) == FALSE) {
        return false;
    }
    root.resize(wcsnlen(root.c_str(), root.size()));
    if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
        return false;  // removable and network drives have none: Windows deletes for good
    }
    SHQUERYRBINFO info{};
    info.cbSize = sizeof info;
    return SUCCEEDED(SHQueryRecycleBinW(root.c_str(), &info));
}

bool folder_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// The move itself, on its own thread (COM single-threaded there, as the shell wants).
RecycleBin::Result recycle(HWND owner, const std::string& project, bool dropped, const std::string& folder,
                           const FolderGuard& guard, const std::string& refused, const std::string& missing) {
    RecycleBin::Result result{project, dropped, FolderOutcome::Stayed, {}};
    const std::wstring path = w(folder);
    if (!folder_exists(path)) {
        result.reason = missing;
        return result;
    }
    // Checked again just before: the folder, the guard and the links may have changed since the question.
    if (folder_refusal_now(folder, real_folder_guard(guard)) != FolderRefusal::None) {
        result.reason = refused;
        return result;
    }
    IFileOperation* operation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation));
    if (FAILED(hr) || operation == nullptr) {
        result.reason = hresult_text(hr);
        return result;
    }
    operation->SetOwnerWindow(owner);
    // The Recycle Bin, or Windows' own question when it can't take it (too
    // big, or "remove files immediately" set for the drive); no
    // FOF_NOCONFIRMATION, which would answer that question with a permanent delete.
    hr = operation->SetOperationFlags(FOFX_RECYCLEONDELETE | FOF_WANTNUKEWARNING | FOF_NOCONFIRMMKDIR);
    DeleteWatch watch;
    DWORD cookie = 0;
    const bool watching = SUCCEEDED(hr) && SUCCEEDED(operation->Advise(&watch, &cookie));
    IShellItem* item = nullptr;
    if (SUCCEEDED(hr)) {
        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    }
    if (SUCCEEDED(hr) && item != nullptr) {
        hr = operation->DeleteItem(item, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = operation->PerformOperations();
    }
    BOOL aborted = FALSE;
    (void)operation->GetAnyOperationsAborted(&aborted);
    if (watching) {
        (void)operation->Unadvise(cookie);
    }
    if (item != nullptr) {
        item->Release();
    }
    operation->Release();
    if (!folder_exists(path)) {
        // Said as Windows reported it: in the Recycle Bin only when it named the item there.
        if (watch.deleted) {
            result.outcome = FolderOutcome::Deleted;
        } else if (watch.recycled) {
            result.outcome = FolderOutcome::Recycled;
        } else {
            result.outcome = FolderOutcome::Gone;
        }
    } else if (aborted != FALSE || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        result.outcome = FolderOutcome::Cancelled;
    } else {
        result.reason = FAILED(hr) ? hresult_text(hr) : hresult_text(HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY));
    }
    return result;
}

}  // namespace

bool folder_has_recycle_bin(const std::wstring& path) { return has_recycle_bin(path); }

FolderGuard real_folder_guard(const FolderGuard& guard) {
    FolderGuard out = guard;
    for (auto* list : {&out.kept, &out.kept_with_contents, &out.other_projects}) {
        const std::vector<std::string> given = *list;
        for (const std::string& p : given) {
            if (const std::string real = real_path(w(p)); !real.empty()) {
                list->push_back(real);
            }
        }
    }
    return out;
}

FolderRefusal folder_refusal_now(const std::string& folder, const FolderGuard& real_guard) {
    const std::wstring path = w(folder);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return FolderRefusal::NotFullPath;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return FolderRefusal::Link;  // a junction or a link: what it leads to isn't the project's to bin
    }
    const std::string real = real_path(path);
    if (real.empty()) {
        return FolderRefusal::NotFullPath;
    }
    return folder_removal_refusal(folder, real, real_guard);
}

FolderOffer folder_offer_now(const std::string& folder, const FolderGuard& guard) {
    if (folder_refusal_now(folder, real_folder_guard(guard)) != FolderRefusal::None) {
        return FolderOffer::None;
    }
    return has_recycle_bin(w(folder)) ? FolderOffer::Offer : FolderOffer::NoRecycleBin;
}

void RecycleBin::retarget(HWND window, UINT message) {
    const std::lock_guard lock(shared_->mutex);
    shared_->window = window;
    shared_->message = message;
}

void RecycleBin::start(const std::string& project, bool database_dropped, const std::string& folder,
                       const FolderGuard& guard, const std::string& refused, const std::string& missing) {
    HWND owner = nullptr;
    {
        const std::lock_guard lock(shared_->mutex);
        owner = shared_->window;
        ++shared_->running;
    }
    const auto finish = [](const std::shared_ptr<Shared>& to, Result result) {
        const std::lock_guard lock(to->mutex);
        --to->running;
        to->done.push_back(std::move(result));
        if (to->window != nullptr) {
            PostMessageW(to->window, to->message, 0, 0);
        }
    };
    try {
        std::thread([shared = shared_, owner, project, database_dropped, folder, guard, refused, missing, finish] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            Result result = recycle(owner, project, database_dropped, folder, guard, refused, missing);
            if (SUCCEEDED(com)) {
                CoUninitialize();
            }
            finish(shared, std::move(result));
        }).detach();
    } catch (const std::system_error& e) {
        finish(shared_, {project, database_dropped, FolderOutcome::Stayed, e.what()});
    }
}

std::vector<RecycleBin::Result> RecycleBin::take() {
    const std::lock_guard lock(shared_->mutex);
    return std::exchange(shared_->done, {});
}

bool RecycleBin::busy() const {
    const std::lock_guard lock(shared_->mutex);
    return shared_->running > 0;
}

void RecycleBin::check(const std::string& project, const std::string& folder, const FolderGuard& guard) {
    const auto finish = [](const std::shared_ptr<Shared>& to, Check answer) {
        const std::lock_guard lock(to->mutex);
        to->checks.push_back(std::move(answer));
        if (to->window != nullptr) {
            PostMessageW(to->window, to->message, 0, 0);
        }
    };
    try {
        std::thread([shared = shared_, project, folder, guard, finish] {
            finish(shared, {project, folder, folder_offer_now(folder, guard)});
        }).detach();
    } catch (const std::system_error&) {
        finish(shared_, {project, folder, FolderOffer::None});  // not offered: the project is still removed if asked
    }
}

RecycleBin::Result recycle_folder_now(HWND owner, const std::string& folder, const FolderGuard& guard,
                                      const std::string& refused, const std::string& missing) {
    return recycle(owner, {}, false, folder, guard, refused, missing);
}

std::vector<RecycleBin::Check> RecycleBin::take_checks() {
    const std::lock_guard lock(shared_->mutex);
    return std::exchange(shared_->checks, {});
}

}  // namespace floserver::ui

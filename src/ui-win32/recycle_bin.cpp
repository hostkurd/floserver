#include "recycle_bin.h"

#include "win_util.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifndef FOFX_RECYCLEONDELETE
#define FOFX_RECYCLEONDELETE 0x00080000
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
    // The Recycle Bin, or Windows' own question when it can't take it; no
    // FOF_NOCONFIRMATION, which would answer that question with a permanent delete.
    hr = operation->SetOperationFlags(FOFX_RECYCLEONDELETE | FOF_NOCONFIRMMKDIR);
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
    if (item != nullptr) {
        item->Release();
    }
    operation->Release();
    if (!folder_exists(path)) {
        result.outcome = FolderOutcome::Recycled;
    } else if (aborted != FALSE || hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        result.outcome = FolderOutcome::Cancelled;
    } else {
        result.reason = FAILED(hr) ? hresult_text(hr) : hresult_text(HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY));
    }
    return result;
}

}  // namespace

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
    }
    const auto finish = [](const std::shared_ptr<Shared>& to, Result result) {
        const std::lock_guard lock(to->mutex);
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

}  // namespace floserver::ui

#include "uninstall.h"

#include "recycle_bin.h"
#include "win_util.h"

#include "floserver/core/data_folder.h"
#include "floserver/core/encoding.h"
#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/service_client.h"

#include <msi.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <shellapi.h>

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace floserver::ui {

namespace fs = std::filesystem;

namespace {

// As `flo untrust` waits: starting the service and its first answer can take a while.
constexpr auto kServiceTimeout = std::chrono::seconds(30);
// The sizes stop counting after this (a huge runtimes folder on a slow disk).
constexpr auto kCountFor = std::chrono::minutes(2);
// Deleting stops counting what it frees after this; it still deletes everything.
constexpr auto kCleanCountFor = std::chrono::minutes(10);
// How long the clean-up waits for the service to be gone before it deletes anything.
constexpr auto kStopFor = std::chrono::seconds(60);

// The service, started first when it isn't running (as the CLI's connect_or_start).
std::optional<core::ServiceClient> connect_or_start(std::string& error) {
    const std::string instance = core::ipc::default_instance();
    if (auto client = core::ServiceClient::connect(instance, error, kServiceTimeout)) {
        return client;
    }
    if (!error.empty()) {
        return std::nullopt;  // there, but it can't be reached: starting another wouldn't help
    }
    if (core::start_service(instance, error) == core::StartResult::Failed) {
        return std::nullopt;
    }
    return core::ServiceClient::connect(instance, error, kServiceTimeout);
}

std::string reply_error(const std::optional<core::protocol::Reply>& reply, const std::string& lost) {
    if (!reply) {
        return lost;
    }
    return reply->error_message.empty() ? reply->error_code : reply->error_message;
}

SkippedItem skipped_item(const core::CleanSkip& s) { return {s.path, s.why, s.reason, s.inside}; }

}  // namespace

std::string installed_product_code() {
    const std::wstring upgrade = win::widen(kUpgradeCode);
    // 38 characters and the end: a product code as Windows Installer writes it.
    std::array<wchar_t, 39> product{};
    for (DWORD i = 0;; ++i) {
        const UINT result = MsiEnumRelatedProductsW(upgrade.c_str(), 0, i, product.data());
        if (result != ERROR_SUCCESS) {
            return {};  // ERROR_NO_MORE_ITEMS: not installed from the MSI
        }
        std::string code = win::narrow(product.data());
        // Only one that's really installed for this computer or this user.
        INSTALLSTATE state = MsiQueryProductStateW(product.data());
        if (is_product_code(code) && (state == INSTALLSTATE_DEFAULT || state == INSTALLSTATE_LOCAL)) {
            return code;
        }
    }
}

bool start_msi_uninstall(HWND owner, const std::string& product_code, std::string& error) {
    const std::string args = msiexec_uninstall_args(product_code);
    if (args.empty()) {
        error = "not a product code: " + product_code;
        return false;
    }
    // Windows' own msiexec, by its full path: never one found elsewhere first.
    std::array<wchar_t, MAX_PATH> system{};
    const UINT n = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (n == 0 || n >= system.size()) {
        error = core::error_text({static_cast<int>(GetLastError()), std::system_category()});
        return false;
    }
    const std::wstring program = std::wstring(system.data(), n) + L"\\msiexec.exe";
    const std::wstring params = win::widen(args);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpFile = program.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info) == FALSE) {
        error = core::error_text({static_cast<int>(GetLastError()), std::system_category()});
        return false;
    }
    return true;
}

UninstallSizes count_uninstall_sizes() {
    UninstallSizes sizes;
    const auto data = core::data_dir();
    if (!data) {
        sizes.runtimes = 0;
        sizes.databases = 0;
        return sizes;
    }
    const auto until = std::chrono::steady_clock::now() + kCountFor;
    sizes.runtimes = core::total_bytes(core::uninstall_items(*data, true, false, until));
    sizes.databases = core::total_bytes(core::uninstall_items(*data, false, true, until));
    return sizes;
}

void Uninstaller::retarget(HWND window, UINT message) {
    const std::lock_guard lock(shared_->mutex);
    shared_->window = window;
    shared_->message = message;
}

void Uninstaller::run(Step step, std::function<Answer()> work) {
    const auto finish = [](const std::shared_ptr<Shared>& to, Answer answer) {
        const std::lock_guard lock(to->mutex);
        to->done.push_back(std::move(answer));
        if (to->window != nullptr) {
            PostMessageW(to->window, to->message, 0, 0);
        }
    };
    try {
        std::thread([shared = shared_, step, work = std::move(work), finish] {
            Answer answer;
            try {
                answer = work();
            } catch (const std::exception& e) {
                answer.error = e.what();
            }
            answer.step = step;
            finish(shared, std::move(answer));
        }).detach();
    } catch (const std::system_error& e) {
        Answer answer;
        answer.step = step;
        answer.error = e.what();
        finish(shared_, std::move(answer));
    }
}

namespace {

// Whether a process runs floserverd.exe, or a program from `runtimes`
// (MariaDB, nginx, php-cgi, Node... the service started them). Compared as
// Windows compares paths: without case.
bool something_runs(const fs::path& runtimes) {
    std::wstring prefix = runtimes.wstring();
    if (!prefix.empty() && prefix.back() != L'\\') {
        prefix += L'\\';
    }
    CharLowerBuffW(prefix.data(), static_cast<DWORD>(prefix.size()));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return true;  // can't tell: assume it runs, delete nothing
    }
    bool found = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Process32FirstW(snap, &entry); more != FALSE && !found; more = Process32NextW(snap, &entry)) {
        std::wstring name = entry.szExeFile;
        CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
        if (name == L"floserverd.exe") {
            found = true;
            break;
        }
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr) {
            continue;  // another user's or a system process: not ours
        }
        std::array<wchar_t, 32768> image{};
        DWORD length = static_cast<DWORD>(image.size());
        if (QueryFullProcessImageNameW(process, 0, image.data(), &length) != FALSE) {
            std::wstring path(image.data(), length);
            CharLowerBuffW(path.data(), static_cast<DWORD>(path.size()));
            found = path.starts_with(prefix);
        }
        CloseHandle(process);
    }
    CloseHandle(snap);
    return found;
}

}  // namespace

bool wait_until_nothing_runs(const fs::path& runtimes, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (something_runs(runtimes)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return true;
}

void Uninstaller::read_ca(const std::string& lost, const std::string& unreadable) {
    run(Step::ReadCa, [lost, unreadable] {
        Answer answer;
        std::string error;
        auto client = connect_or_start(error);
        if (!client) {
            answer.error = error.empty() ? lost : error;
            return answer;
        }
        const auto status = client->call("certs.status");
        if (status && !status->ok && status->error_code == "ca_damaged") {
            // Its files go with certs.remove; without the certificate there's
            // nothing to look for in Windows' store (as `flo untrust` does).
            return answer;
        }
        if (!status || !status->ok) {
            answer.error = reply_error(status, lost);
            return answer;
        }
        const nlohmann::json& ca = status->result.contains("ca") ? status->result["ca"] : nlohmann::json();
        if (!ca.is_object()) {
            return answer;  // none made yet: nothing in Windows' store
        }
        const auto der = core::from_base64(ca.contains("der") && ca["der"].is_string() ? ca["der"].get<std::string>()
                                                                                         : std::string());
        if (!der || der->empty()) {
            answer.error = unreadable;
            return answer;
        }
        answer.ca_der = *der;
        return answer;
    });
}

void Uninstaller::remove_ca(const std::string& lost) {
    run(Step::RemoveCa, [lost] {
        Answer answer;
        std::string error;
        auto client = connect_or_start(error);
        if (!client) {
            answer.error = error.empty() ? lost : error;
            return answer;
        }
        const auto removed = client->call("certs.remove");
        if (!removed || !removed->ok) {
            answer.error = reply_error(removed, lost);
        }
        return answer;
    });
}

void Uninstaller::clean(const UninstallTicks& ticks, const std::string& confirmed_projects_dir,
                        const FolderGuard& base, const std::string& no_recycle_bin, const std::string& refused,
                        const std::string& missing) {
    HWND owner = nullptr;
    {
        const std::lock_guard lock(shared_->mutex);
        owner = shared_->window;
    }
    run(Step::Clean, [ticks, confirmed_projects_dir, base, no_recycle_bin, refused, missing, owner] {
        Answer answer;
        const auto data = core::data_dir();
        if (!data) {
            answer.error = "Windows gives no LOCALAPPDATA folder";
            return answer;
        }
        // The worker asked it to quit and waited a few seconds; stopping
        // every project (MariaDB above all) can take longer. Its channel
        // closes before it has stopped what it runs, so "gone" means no
        // floserverd and nothing running from FloServer's runtimes folder.
        // Nothing is deleted while any of it may still have files open.
        std::string stop_error;
        (void)core::stop_service(core::ipc::default_instance(), stop_error, kStopFor);
        if (!wait_until_nothing_runs(*data / "runtimes", kStopFor)) {
            answer.stopped = false;
            answer.error = stop_error.empty() ? std::string("floserverd or one of its runtimes is still running")
                                              : stop_error;
            return answer;
        }
        // Read now, with the service stopped: config.json as it was left.
        const UninstallFolders folders = uninstall_folders(*data);
        if (!folders.readable) {
            answer.error = "config.json can't be read, so FloServer can't tell which folders hold projects; nothing was deleted";
            return answer;
        }
        core::CleanOptions options;
        for (const std::string& dir : folders.project_dirs) {
            options.protected_dirs.push_back(core::path_from_utf8(dir));
            options.protected_kinds.emplace_back();
        }
        if (!folders.projects_dir.empty()) {
            options.protected_dirs.push_back(core::path_from_utf8(folders.projects_dir));
            options.protected_kinds.emplace_back("projects");
        }
        if (!folders.shared_dir.empty()) {
            options.protected_dirs.push_back(core::path_from_utf8(folders.shared_dir));
            options.protected_kinds.emplace_back("www");
        }
        if (ticks.runtimes || ticks.databases) {
            const auto items = core::uninstall_items(*data, ticks.runtimes, ticks.databases,
                                                     std::chrono::steady_clock::now() + kCleanCountFor);
            const core::CleanResult result = core::remove_uninstall_items(*data, items, options);
            for (const core::CleanSkip& s : result.skipped_items) {
                answer.report.skipped.push_back(skipped_item(s));
            }
        }
        std::error_code ec;
        if (ticks.projects && !folders.projects_dir.empty() &&
            comparable_path(folders.projects_dir) != comparable_path(confirmed_projects_dir)) {
            // config.json names another projects folder than the one asked about.
            answer.report.projects = FolderOutcome::Stayed;
            answer.report.projects_reason = refused;
        } else if (ticks.projects && !folders.projects_dir.empty() &&
                   fs::is_directory(core::path_from_utf8(folders.projects_dir), ec) &&
                   !folder_has_recycle_bin(win::widen(folders.projects_dir))) {
            // The question said "moved to the Recycle Bin": on a drive with
            // none, Windows would delete it for good instead.
            answer.report.projects = FolderOutcome::Stayed;
            answer.report.projects_reason = no_recycle_bin;
        } else if (ticks.projects && !folders.projects_dir.empty() &&
                   fs::is_directory(core::path_from_utf8(folders.projects_dir), ec)) {
            // The shell's move wants a single-threaded apartment on its thread.
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            const RecycleBin::Result moved = recycle_folder_now(owner, folders.projects_dir,
                                                                projects_folder_guard(base, folders), refused, missing);
            if (SUCCEEDED(com)) {
                CoUninitialize();
            }
            answer.report.projects = moved.outcome;
            answer.report.projects_reason = moved.reason;
            if (moved.outcome == FolderOutcome::Recycled) {
                // Its projects leave the list with it (the service is stopped:
                // nothing else writes config.json now). A list that can't be
                // saved is said, and the rest goes on.
                std::string config_error;
                const fs::path file = core::app_config_file(*data);
                if (auto config = core::load_app_config(file, config_error)) {
                    if (forget_projects_inside(*config, folders.projects_dir) > 0 &&
                        !core::save_app_config(file, *config, config_error)) {
                        answer.report.skipped.push_back({"config.json", "unsaved", config_error, {}});
                    }
                } else {
                    answer.report.skipped.push_back({"config.json", "unreadable", config_error, {}});
                }
            }
        }
        answer.product_code = installed_product_code();
        return answer;
    });
}

std::vector<Uninstaller::Answer> Uninstaller::take() {
    const std::lock_guard lock(shared_->mutex);
    return std::exchange(shared_->done, {});
}

}  // namespace floserver::ui

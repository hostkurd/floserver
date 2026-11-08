// Remove FloServer (D21; screens D25, approved with their mockups 6 Nov).
// Windows' Settings -> Apps -> FloServer -> Uninstall (FloServer.exe
// --uninstall) and Settings > General > Remove FloServer… both open the
// Uninstall window from floserver-setup.dll. Its Remove runs the steps
// uninstall.h lists from this window, because it owns the service's link
// (the ServiceWorker stops the service, as Quit does); each step is shown in
// the Uninstall window as it starts. A step that fails before anything is
// deleted stops there and says why, and FloServer goes on as it was.
// Windows' certificate question is the only one asked on this thread;
// everything slow runs on the Uninstaller's threads.
//
// At the end this window copies FloServer.exe (and its DLLs) into %TEMP%
// and quits; the copy (FloServer.exe --remove-program) shows the same
// window at its last step while Windows removes the program, then Done.

#include "main_window.h"

#include "apply_update.h"
#include "model/uninstall.h"

#include "floserver/core/build_info.h"
#include "floserver/core/paths.h"
#include "floserver/core/trust_store.h"

#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace floserver::ui {

namespace {

std::wstring w(std::string_view s) { return win::widen(s); }

// The Uninstall window is English only (D21, D25), as the setup is.
constexpr Lang kSetupLang = Lang::En;

enum RemoveStep : int { kStepCertificate = 0, kStepStopping = 1, kStepCleaning = 2 };

// Said after a stop that came before anything was deleted.
constexpr std::string_view kNothingDeleted = "Nothing was deleted. FloServer goes on as it was.";

}  // namespace

void __stdcall MainWindow::uninstall_remove_pressed(void* context, unsigned ticks) {
    // Not from inside the Uninstall window's own message: posted to this one.
    // The boxes are kept here; the message only says Remove was pressed, so
    // nothing posted from elsewhere can choose what's deleted.
    auto* self = static_cast<MainWindow*>(context);
    self->uninstall_pending_ticks_ = ticks;
    self->uninstall_remove_pressed_ = true;  // the message alone starts nothing
    PostMessageW(self->hwnd_, kUninstallRemoveMessage, 0, 0);
}

void __stdcall MainWindow::uninstall_window_closed(void* context) {
    auto* self = static_cast<MainWindow*>(context);
    PostMessageW(self->hwnd_, kUninstallClosedMessage, 0, 0);
}

void MainWindow::open_uninstall(bool quit_on_cancel) {
    if (uninstall_window_ != nullptr) {
        setup_dll_->forward(uninstall_window_);
        return;
    }
    if (quitting_ || !worker_) {
        return;
    }
    std::string error;
    setup_dll_ = setup_dll(error);
    if (setup_dll_ == nullptr) {
        const ModalScope modal(*this);  // the message boxes run their own message loop
        // Without its screens, Windows' own uninstall of the MSI still
        // removes the program (FloServer's entry in Settings -> Apps leads
        // here, so this is the way left there).
        const std::string product = installed_product_code();
        if (product.empty()) {
            const std::wstring message = L"FloServer's uninstall screens can't be opened (" + w(error) + L").";
            MessageBoxW(nullptr, message.c_str(), L"FloServer", MB_OK | MB_ICONWARNING);
        } else if (asking_ || (settings_ && settings_->asking()) || recycle_bin_.busy()) {
            // Windows' uninstall would close FloServer mid-question: later.
            const std::wstring message = L"FloServer's uninstall screens can't be opened (" + w(error) +
                                         L"). Finish what's open in FloServer's window, then try again.";
            MessageBoxW(nullptr, message.c_str(), L"FloServer", MB_OK | MB_ICONWARNING);
        } else {
            const std::wstring message =
                L"FloServer's uninstall screens can't be opened (" + w(error) +
                L").\n\nRemove FloServer with Windows' standard uninstall instead? Your projects, databases and "
                L"downloaded runtimes stay on this computer.";
            if (MessageBoxW(nullptr, message.c_str(), L"FloServer", MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                std::string start_error;
                if (start_msi_uninstall(nullptr, product, start_error)) {
                    quit();  // Windows Installer finds nothing of FloServer running
                    return;
                }
                const std::wstring failed = w(fill(kSetupLang, Str::UninstallStartFailed, {start_error}));
                MessageBoxW(nullptr, failed.c_str(), L"FloServer", MB_OK | MB_ICONWARNING);
            }
        }
        if (quit_on_cancel) {
            quit();
        }
        return;
    }
    const std::wstring version = w(core::kVersion);
    FloUninstallHost host{};
    host.size = sizeof host;
    host.context = this;
    host.remove = &MainWindow::uninstall_remove_pressed;
    host.closed = &MainWindow::uninstall_window_closed;
    host.version = version.c_str();
    uninstall_window_ = setup_dll_->open(&host, SW_SHOWNORMAL);
    if (uninstall_window_ == nullptr) {
        if (quit_on_cancel) {
            quit();
        }
        return;
    }
    quit_after_uninstall_ = quit_on_cancel;
    // The projects folder the box offers: config.json, read here (a small
    // file) so the box shows it at once; the sizes are counted on a thread.
    const auto data = core::data_dir();
    uninstall_offer_dir_ = data ? uninstall_folders(*data).projects_dir : std::string();
    push_uninstall_choices(UninstallSizes{});
    {
        const std::lock_guard lock(uninstall_sizes_->mutex);
        uninstall_sizes_->sizes.reset();
    }
    std::thread([box = uninstall_sizes_, hwnd = hwnd_] {
        const UninstallSizes sizes = count_uninstall_sizes();
        {
            const std::lock_guard lock(box->mutex);
            box->sizes = sizes;
        }
        PostMessageW(hwnd, kUninstallSizesMessage, 0, 0);  // a window gone by then ignores it
    }).detach();
}

void MainWindow::push_uninstall_choices(const UninstallSizes& sizes) {
    if (uninstall_window_ == nullptr) {
        return;
    }
    const UninstallQuestion q = uninstall_question(kSetupLang, uninstall_offer_dir_, sizes);
    const std::vector<UninstallChoiceText> texts = uninstall_choices(q, UninstallTicks{});
    std::vector<std::wstring> words;
    words.reserve(texts.size() * 2);
    for (const UninstallChoiceText& t : texts) {
        words.push_back(w(t.label));
        words.push_back(w(t.note));
    }
    std::vector<FloUninstallChoice> choices;
    for (std::size_t i = 0; i < texts.size(); ++i) {
        choices.push_back({texts[i].bit, words[2 * i].c_str(), words[(2 * i) + 1].c_str(), texts[i].checked ? 1 : 0});
    }
    setup_dll_->choices(uninstall_window_, choices.data(), static_cast<std::uint32_t>(choices.size()));
}

void MainWindow::on_uninstall_window_closed() {
    if (uninstall_window_ == nullptr) {
        return;
    }
    setup_dll_->close(uninstall_window_);
    uninstall_window_ = nullptr;
    if (uninstall_finished_) {
        // The clean-up ended with a message: the service is already stopped.
        DestroyWindow(hwnd_);
        return;
    }
    // Started only for it (Windows' Uninstall) and nothing is running now: done.
    if (quit_after_uninstall_ && !uninstalling_ && IsWindowVisible(hwnd_) == FALSE) {
        quit();
    }
}

bool MainWindow::dialog_message(MSG& msg) {
    return uninstall_window_ != nullptr && setup_dll_->dialog_message(uninstall_window_, &msg) != 0;
}

void MainWindow::start_uninstall(const UninstallTicks& ticks) {
    if (uninstalling_ || uninstall_window_ == nullptr) {
        return;
    }
    // Every early stop says so: the Uninstall window is on its Removing page by now.
    if (quitting_ || !worker_) {
        uninstall_failed("FloServer is closing.", kNothingDeleted);
        return;
    }
    if (asking_ || (settings_ && settings_->asking())) {
        uninstall_failed("A question is open in FloServer's window. Answer it, then try again.", kNothingDeleted);
        return;
    }
    // Windows' progress and question for a project's folder belong to this
    // window; the projects folder's move would wait behind it.
    if (recycle_bin_.busy()) {
        uninstall_failed(std::string(text(kSetupLang, Str::UninstallWait)), kNothingDeleted);
        return;
    }
    uninstalling_ = true;
    uninstall_ticks_ = ticks;
    // The projects folder as the box showed it: the clean-up moves that one or none.
    uninstall_projects_dir_ = uninstall_offer_dir_;
    // Settings goes (nothing in General waits for a Save).
    if (settings_) {
        settings_->destroy_now();
        settings_placement_ = settings_->placement();
        settings_.reset();
    }
    setup_dll_->step(uninstall_window_, kStepCertificate);
    uninstaller_.read_ca(std::string(text(kSetupLang, Str::ConnectionLost)),
                         std::string(text(kSetupLang, Str::PortsCertUnreadable)));
}

void MainWindow::uninstall_failed(const std::string& message, std::string_view note) {
    uninstalling_ = false;
    if (uninstall_window_ != nullptr) {
        std::string words = message;
        if (!note.empty()) {
            words += "\n\n";
            words += note;
        }
        setup_dll_->message(uninstall_window_, L"FloServer wasn't removed", w(words).c_str(), 0);
    } else {
        show_window();
        show_notice(message, true);
    }
}

void MainWindow::on_uninstall() {
    for (const Uninstaller::Answer& answer : uninstaller_.take()) {
        if (!uninstalling_) {
            continue;  // a late answer after a stop: nothing more to do
        }
        switch (answer.step) {
            case Uninstaller::Step::ReadCa: {
                if (!answer.error.empty()) {
                    uninstall_failed(fill(kSetupLang, Str::UninstallFailed, {answer.error}), kNothingDeleted);
                    break;
                }
                if (!answer.ca_der.empty()) {
                    // Windows shows its own question here and waits for the
                    // answer; as with the panel's questions, nothing else
                    // is started meanwhile.
                    bool cancelled = false;
                    std::string error;
                    asking_ = true;
                    const bool removed = core::untrust_ca(answer.ca_der, cancelled, error);
                    asking_ = false;
                    modal_closed();
                    if (!removed) {
                        // "No" already says nothing was changed.
                        uninstall_failed(cancelled ? std::string(text(kSetupLang, Str::UninstallCaNo))
                                                   : fill(kSetupLang, Str::UninstallFailed, {error}),
                                         cancelled ? std::string_view() : kNothingDeleted);
                        break;
                    }
                }
                uninstaller_.remove_ca(std::string(text(kSetupLang, Str::ConnectionLost)));
                break;
            }
            case Uninstaller::Step::RemoveCa:
                if (!answer.error.empty()) {
                    uninstall_failed(fill(kSetupLang, Str::UninstallFailed, {answer.error}),
                                     "FloServer's certificate was already taken out of Windows' trusted list; "
                                     "nothing else was deleted. Turn HTTPS on again in Settings > Ports & HTTPS.");
                    break;
                }
                // Stopped the way Quit stops it: the worker hangs up first (the
                // service waits for its clients to leave), then waits for it to
                // exit; its quit_done comes to on_snapshot(), then uninstall_stopped().
                if (uninstall_window_ != nullptr) {
                    setup_dll_->step(uninstall_window_, kStepStopping);
                }
                worker_->quit_service();
                break;
            case Uninstaller::Step::Clean:
                uninstall_done(answer);
                return;  // the window is gone
        }
    }
}

void MainWindow::uninstall_stopped(const std::string& error) {
    // The worker waited a few seconds; when the service is still stopping
    // its projects (`error`), the clean-up's thread waits longer itself and
    // deletes nothing unless it's gone.
    (void)error;
    if (uninstall_window_ != nullptr) {
        setup_dll_->step(uninstall_window_, kStepCleaning);
    }
    uninstaller_.clean(uninstall_ticks_, uninstall_projects_dir_, windows_folder_guard(),
                       std::string(text(kSetupLang, Str::RemoveNoRecycleBin)),
                       std::string(text(kSetupLang, Str::RemoveFolderRefused)),
                       std::string(text(kSetupLang, Str::ProgramMissing)));
}

bool MainWindow::hand_over_removal(const Uninstaller::Answer& answer, std::string& error) {
    RemoveProgramPlan plan;
    plan.product = answer.product_code;
    plan.parent = GetCurrentProcessId();  // the copy waits until this process has gone
    POINT at{};
    if (setup_dll_->place(uninstall_window_, &at) != 0) {
        plan.x = at.x;
        plan.y = at.y;
    }
    plan.deleted = uninstall_deleted_text(uninstall_ticks_, !uninstall_projects_dir_.empty());
    const auto data = core::data_dir();
    const std::string data_dir = data ? core::path_to_utf8(data->lexically_normal().make_preferred()) : std::string();
    const std::string runtimes_dir =
        data ? core::path_to_utf8(core::runtimes_dir(*data).lexically_normal().make_preferred()) : std::string();
    plan.kept = uninstall_kept_lines(uninstall_ticks_, data_dir, runtimes_dir, uninstall_projects_dir_);
    plan.problems = uninstall_problems(answer.report, kSetupLang);
    if (!uninstall_ticks_.databases || !uninstall_ticks_.runtimes) {
        plan.data_dir = data_dir;
    }
    std::filesystem::path copy;
    if (!copy_floserver_to_temp(copy, error)) {
        return false;
    }
    const std::filesystem::path plan_file = copy.parent_path() / L"remove-program.json";
    if (!core::write_file_atomically(plan_file, remove_program_json(plan), error)) {
        return false;
    }
    std::wstring command = w(remove_program_command(core::path_to_utf8(copy), core::path_to_utf8(plan_file)));
    if (command.empty()) {
        error = core::path_to_utf8(copy);
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const std::wstring dir = copy.parent_path().wstring();
    if (CreateProcessW(copy.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi) ==
        FALSE) {
        error = core::error_text({static_cast<int>(GetLastError()), std::system_category()});
        return false;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void MainWindow::uninstall_done(const Uninstaller::Answer& answer) {
    const bool installed = answer.error.empty() && !answer.product_code.empty();
    std::wstring heading;
    std::string message;
    bool done = false;
    if (!answer.stopped) {
        // Still running: its files may be in use, so nothing was deleted.
        heading = L"FloServer wasn't removed";
        message = fill(kSetupLang, Str::UninstallStopFailed, {answer.error});
    } else if (!answer.error.empty()) {
        heading = L"FloServer wasn't removed";
        message = fill(kSetupLang, Str::UninstallFailed, {answer.error});
    } else if (!installed) {
        // A copy that wasn't installed by the MSI (built by hand): its data is
        // cleaned up; its program files stay where they are.
        heading = L"FloServer's data was cleaned up";
        message = uninstall_done_text(answer.report, false, kSetupLang);
        done = true;
    } else {
        std::string error;
        if (hand_over_removal(answer, error)) {
            // The copy shows the same window from here on.
            if (uninstall_window_ != nullptr) {
                setup_dll_->close(uninstall_window_);
                uninstall_window_ = nullptr;
            }
        } else {
            // Windows' own uninstall instead, with its usual window.
            std::string start_error;
            if (start_msi_uninstall(hwnd_, answer.product_code, start_error)) {
                if (uninstall_window_ != nullptr) {
                    setup_dll_->close(uninstall_window_);
                    uninstall_window_ = nullptr;
                }
            } else {
                heading = L"FloServer's program wasn't removed";
                message = fill(kSetupLang, Str::UninstallStartFailed, {start_error});
            }
        }
    }
    // The service is stopped and the worker with it: FloServer quits, so
    // Windows Installer finds nothing of it running. A message stays on the
    // Uninstall window until it's closed (on_uninstall_window_closed quits then).
    uninstalling_ = false;
    if (!message.empty() && uninstall_window_ != nullptr) {
        setup_dll_->message(uninstall_window_, heading.c_str(), w(message).c_str(), done ? 1 : 0);
        quit_after_uninstall_ = true;
        uninstall_finished_ = true;
        ShowWindow(hwnd_, SW_HIDE);
        quitting_ = true;
        return;
    }
    quitting_ = true;
    DestroyWindow(hwnd_);
}

}  // namespace floserver::ui

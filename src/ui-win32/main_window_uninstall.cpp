// Remove FloServer… (Settings > General, D21, approved with its mockup):
// the steps uninstall.h lists, run from FloServer's window because it owns
// the service's link (the ServiceWorker stops the service, as Quit does) and
// quits at the end. Each step is said in a toast as it starts; a step that
// fails before anything is deleted stops there and says why, and FloServer
// goes on as it was. Windows' certificate question is the only one asked on
// this thread; everything slow runs on the Uninstaller's threads.

#include "main_window.h"

#include "floserver/core/trust_store.h"

#include <string>
#include <string_view>
#include <utility>

namespace floserver::ui {

namespace {

std::wstring w(std::string_view s) { return win::widen(s); }

}  // namespace

void MainWindow::start_uninstall(const UninstallTicks& ticks) {
    if (uninstalling_ || quitting_ || !worker_ || asking_ || (settings_ && settings_->asking())) {
        return;
    }
    // Windows' progress and question for a project's folder belong to this
    // window; the projects folder's move would wait behind it.
    if (recycle_bin_.busy()) {
        show_window();
        show_notice(std::string(text(lang_, Str::UninstallWait)), true);
        return;
    }
    uninstalling_ = true;
    uninstall_ticks_ = ticks;
    // Settings goes (nothing in General waits for a Save); what happens is said here.
    if (settings_) {
        settings_->destroy_now();
        settings_placement_ = settings_->placement();
        settings_.reset();
    }
    show_window();
    show_notice(std::string(text(lang_, Str::UninstallRemovingCa)), false);
    uninstaller_.read_ca(std::string(text(lang_, Str::ConnectionLost)),
                         std::string(text(lang_, Str::PortsCertUnreadable)));
}

void MainWindow::uninstall_failed(const std::string& message) {
    uninstalling_ = false;
    show_window();
    show_notice(message, true);
}

void MainWindow::on_uninstall() {
    for (const Uninstaller::Answer& answer : uninstaller_.take()) {
        if (!uninstalling_) {
            continue;  // a late answer after a stop: nothing more to do
        }
        switch (answer.step) {
            case Uninstaller::Step::ReadCa: {
                if (!answer.error.empty()) {
                    uninstall_failed(fill(lang_, Str::UninstallFailed, {answer.error}));
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
                    if (!removed) {
                        uninstall_failed(cancelled ? std::string(text(lang_, Str::UninstallCaNo))
                                                   : fill(lang_, Str::UninstallFailed, {error}));
                        break;
                    }
                }
                uninstaller_.remove_ca(std::string(text(lang_, Str::ConnectionLost)));
                break;
            }
            case Uninstaller::Step::RemoveCa:
                if (!answer.error.empty()) {
                    uninstall_failed(fill(lang_, Str::UninstallFailed, {answer.error}));
                    break;
                }
                // Stopped the way Quit stops it: the worker hangs up first (the
                // service waits for its clients to leave), then waits for it to
                // exit; its quit_done comes to on_snapshot(), then uninstall_stopped().
                show_notice(std::string(text(lang_, Str::UninstallStopping)), false);
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
    show_notice(std::string(text(lang_, Str::UninstallCleaning)), false);
    uninstaller_.clean(uninstall_ticks_, windows_folder_guard(), std::string(text(lang_, Str::RemoveFolderRefused)),
                       std::string(text(lang_, Str::ProgramMissing)));
}

void MainWindow::uninstall_done(const Uninstaller::Answer& answer) {
    const UINT rtl = rtl_ ? MB_RTLREADING | MB_RIGHT : 0U;
    const bool installed = answer.error.empty() && !answer.product_code.empty();
    const bool problems = !answer.error.empty() || !uninstall_problems(answer.report, lang_).empty();
    std::string message;
    if (!answer.stopped) {
        // Still running: its files may be in use, so nothing was deleted.
        message = fill(lang_, Str::UninstallStopFailed, {answer.error});
    } else if (!answer.error.empty()) {
        message = fill(lang_, Str::UninstallFailed, {answer.error});
    } else if (!installed || problems) {
        message = uninstall_done_text(answer.report, installed, lang_);
    }
    if (!message.empty()) {
        // A box of its own: FloServer's window goes next, and a toast with it.
        const UINT icon = problems ? UINT{MB_ICONWARNING} : UINT{MB_ICONINFORMATION};
        MessageBoxW(hwnd_, w(message).c_str(), L"FloServer", MB_OK | icon | rtl);
    }
    if (installed) {
        std::string error;
        if (!start_msi_uninstall(hwnd_, answer.product_code, error)) {
            const std::wstring failed = w(fill(lang_, Str::UninstallStartFailed, {error}));
            MessageBoxW(hwnd_, failed.c_str(), L"FloServer", MB_OK | MB_ICONWARNING | rtl);
        }
    }
    // The service is stopped and the worker with it: FloServer quits, so
    // Windows Installer finds nothing of it running.
    uninstalling_ = false;
    quitting_ = true;
    DestroyWindow(hwnd_);
}

}  // namespace floserver::ui

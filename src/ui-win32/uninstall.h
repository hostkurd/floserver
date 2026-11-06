#pragma once

// Remove FloServer… (Settings > General, D21): finding FloServer's MSI,
// starting Windows' own uninstall, and the slow steps on threads of their
// own so the window keeps painting. FloServer's window runs the steps in
// order (main_window_uninstall.cpp):
//
//   1. read_ca: the certificate authority from the service (certs.status),
//      started first when it isn't running, as `flo untrust` does;
//   2. the window takes it out of Windows' store (Windows asks the user;
//      a no stops everything, nothing changed);
//   3. remove_ca: certs.remove, the CA's and the projects' certificate files;
//   4. the window's ServiceWorker stops the service (and hangs up, which the
//      service waits for before it exits);
//   5. clean: waits until the service is gone (up to a minute; nothing is
//      deleted otherwise), deletes what was ticked (data_folder.h's
//      uninstall_items), moves the projects folder to the Recycle Bin, then
//      finds which MSI is installed;
//   6. the window starts msiexec /x {product} and quits, or says the program
//      files were left (a copy that wasn't installed by the MSI).

#include "model/folder_removal.h"
#include "model/uninstall.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace floserver::ui {

// The product code ("{...}") of the FloServer installed from its MSI (found
// by kUpgradeCode); empty when there's none, as for a copy built by hand.
[[nodiscard]] std::string installed_product_code();

// Windows' uninstall with its usual window: msiexec /x {product_code}.
// False with `error` when it couldn't be started.
bool start_msi_uninstall(HWND owner, const std::string& product_code, std::string& error);

// The sizes beside the question's boxes, counted on the calling thread (it
// walks the folders: never the window's).
[[nodiscard]] UninstallSizes count_uninstall_sizes();

// Waits until no floserverd runs and nothing runs from `runtimes`; false if
// something still does after `limit` (then nothing may be deleted).
[[nodiscard]] bool wait_until_nothing_runs(const std::filesystem::path& runtimes, std::chrono::milliseconds limit);

class Uninstaller {
public:
    enum class Step : std::uint8_t { ReadCa, RemoveCa, Clean };
    struct Answer {
        Step step = Step::ReadCa;
        std::string error;                 // empty: the step was done
        std::vector<std::uint8_t> ca_der;  // ReadCa: the CA to take out of Windows' store; empty: none
        UninstallReport report;            // Clean
        std::string product_code;          // Clean: empty when FloServer wasn't installed from its MSI
        bool stopped = true;               // Clean: false when the service was still running (`error`)
    };

    // When a step is done, `message` is posted to the window retarget() last named.
    void retarget(HWND window, UINT message);
    // `lost` and `unreadable` are the window's words for a connection that
    // broke and a certificate that can't be read.
    void read_ca(const std::string& lost, const std::string& unreadable);
    void remove_ca(const std::string& lost);
    // `base`: Windows' folders and the data folder (projects_folder_guard()
    // adds the rest on the thread, from config.json). `refused` and
    // `missing` as for RecycleBin::start.
    // `confirmed_projects_dir`: the projects folder as the user saw it in the
    // question; a different one now (config.json changed) stays.
    void clean(const UninstallTicks& ticks, const std::string& confirmed_projects_dir, const FolderGuard& base,
               const std::string& no_recycle_bin, const std::string& refused,
               const std::string& missing);
    // The steps finished since the last call.
    [[nodiscard]] std::vector<Answer> take();

private:
    struct Shared {
        std::mutex mutex;
        HWND window = nullptr;
        UINT message = 0;
        std::vector<Answer> done;
    };
    void run(Step step, std::function<Answer()> work);
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
};

}  // namespace floserver::ui

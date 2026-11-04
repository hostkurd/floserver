#pragma once

// A removed project's folder to the Recycle Bin (approved 4 Nov): checked
// against model/folder_removal.h with what Windows says about it (a junction
// or link at the top, where it really is), then moved with the shell's own
// IFileOperation on a thread of its own, so Windows shows its progress and
// asks about a folder too big for the Recycle Bin, and the window keeps
// painting. Never a permanent delete from here: when Windows would delete it
// for good, it asks the user itself.

#include "model/folder_removal.h"
#include "model/project_menu.h"

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace floserver::ui {

// The guard's folders as written, and again where each really is.
[[nodiscard]] FolderGuard real_folder_guard(const FolderGuard& guard);

// Why `folder` may not go to the Recycle Bin: the model's rules on the
// folder and its real path, plus Link for a junction or link at the top
// (and NotFullPath when it isn't an existing folder).
[[nodiscard]] FolderRefusal folder_refusal_now(const std::string& folder, const FolderGuard& real_guard);

// Whether the remove question offers the folder: the rules let it go, and
// its drive is a fixed one with a Recycle Bin (else Windows would delete it
// for good). Looks at the disk: never on the window's thread (RecycleBin::check).
[[nodiscard]] FolderOffer folder_offer_now(const std::string& folder, const FolderGuard& guard);

class RecycleBin {
public:
    struct Result {
        std::string project;
        bool database_dropped = false;
        FolderOutcome outcome = FolderOutcome::Stayed;
        std::string reason;  // Windows' words, when it stayed
    };

    // When a move is done, `message` is posted to the window retarget() last named.
    void retarget(HWND window, UINT message);
    // Checks the folder again (`guard`: real_folder_guard() is applied on
    // the thread) and moves it. `refused` and `missing` are the reasons
    // said when the rules refuse it or it isn't there, in the window's language.
    void start(const std::string& project, bool database_dropped, const std::string& folder, const FolderGuard& guard,
               const std::string& refused, const std::string& missing);
    // The moves finished since the last call.
    [[nodiscard]] std::vector<Result> take();
    // A move is running: quitting waits for it (Windows' question needs its owner).
    [[nodiscard]] bool busy() const;

    // Whether a project's folder may be offered, worked out on a thread of
    // its own (a share that's offline mustn't freeze the window); the answer
    // comes with the same message, from take_checks().
    struct Check {
        std::string project;
        std::string folder;
        FolderOffer offer = FolderOffer::None;
    };
    void check(const std::string& project, const std::string& folder, const FolderGuard& guard);
    [[nodiscard]] std::vector<Check> take_checks();

private:
    struct Shared {
        std::mutex mutex;
        HWND window = nullptr;
        UINT message = 0;
        std::vector<Result> done;
        std::vector<Check> checks;
        int running = 0;  // moves not finished
    };
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
};

}  // namespace floserver::ui

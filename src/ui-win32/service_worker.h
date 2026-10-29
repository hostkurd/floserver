#pragma once

// The window's link to floserverd (D2). One background thread connects
// (starting the service the way the CLI does), asks for projects.list,
// status and web.status every 2 seconds, and sends the user's changes. The
// UI thread never waits on the pipe: the worker keeps the latest snapshot and
// posts a message to the window when it changes.

#include "model/ui_strings.h"
#include "model/view_model.h"

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace floserver::ui {

enum class Link : std::uint8_t {
    Connecting,  // first try
    Starting,    // start_service is running
    Connected,
    NotRunning,  // the service isn't running; the user can start it
    Failed,      // it couldn't start or be reached; `error` says why
    Lost,        // it stopped answering; trying again
    Stopping,    // Quit: stop_service is running
};

struct Snapshot {
    Link link = Link::Connecting;
    std::string error;  // Failed: the reason
    bool has_list = false;
    std::vector<ProjectRow> rows;
    FooterText footer;
    // Taken once by the UI:
    std::vector<std::string> settled;  // projects whose On/Off change finished
    std::string change_error;          // "Couldn't change shop: ..."
    bool quit_done = false;
    std::string quit_error;
};

class ServiceWorker {
public:
    // Posts `message` to `window` when there's a new snapshot.
    ServiceWorker(HWND window, UINT message, Lang lang);
    ~ServiceWorker();
    ServiceWorker(const ServiceWorker&) = delete;
    ServiceWorker& operator=(const ServiceWorker&) = delete;
    ServiceWorker(ServiceWorker&&) = delete;
    ServiceWorker& operator=(ServiceWorker&&) = delete;

    void set_on(const std::string& project, bool on);
    void start_service();  // after NotRunning or Failed
    void refresh();
    void quit_service();   // stops floserverd; quit_done follows

    // The latest snapshot; the one-shot parts are handed over only once.
    [[nodiscard]] Snapshot take();

private:
    struct Command {
        enum class Kind : std::uint8_t { SetOn, Start, Refresh, Quit } kind;
        std::string project;
        bool on = false;
    };

    void push(Command command);
    void run(const std::stop_token& stop);
    void publish();

    HWND window_;
    UINT message_;
    Lang lang_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Command> commands_;
    Snapshot latest_;
    bool posted_ = false;
    std::jthread thread_;  // last: it starts after everything above exists
};

}  // namespace floserver::ui

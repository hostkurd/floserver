#pragma once

// The window's link to floserverd (D2). One background thread connects
// (starting the service the way the CLI does), asks for projects.list,
// status, web.status and databases.list every 2 seconds, and sends the
// user's changes. While a project's panel is open it also asks for that
// project's log. The UI thread never waits on the pipe: the worker keeps the
// latest snapshot and posts a message to the window when it changes.

#include "model/ui_strings.h"
#include "model/view_model.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
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

// The open panel's project, as the worker last saw it.
struct PanelData {
    std::string project;        // empty: no panel
    bool has_log = false;
    std::vector<LogLine> log;
    bool flocms = false;        // composer.json says FloCMS (D14)
    bool studio = false;        // and FloCMS Studio is installed
};

struct Snapshot {
    Link link = Link::Connecting;
    std::string error;  // Failed: the reason
    bool has_list = false;
    std::vector<ProjectRow> rows;
    FooterText footer;
    Installed installed;
    nlohmann::json databases;   // databases.list's result, or null
    PanelData panel;
    // Taken once by the UI:
    std::vector<std::string> settled;  // change keys whose command finished (see ServiceWorker)
    std::string change_error;          // "Couldn't change shop: ..."
    std::string clipboard;             // .env lines to put on the clipboard
    std::string added;                 // a project just added from the window
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

    // Each change reports a key in Snapshot::settled when it's done:
    // on_key(), keep_key(), use_key().
    void set_on(const std::string& project, bool on);
    void set_keep_running(const std::string& project, bool keep);
    void use(const std::string& project, const std::string& key, const std::optional<std::string>& value);
    void copy_env(const std::string& project);
    void open_in_studio(const std::string& project);
    // Adds a folder as a project (projects.add) and turns it on.
    void add_folder(const std::string& path);
    // The project whose log is fetched; empty for none.
    void select(const std::string& project);
    void start_service();  // after NotRunning or Failed
    void refresh();
    void quit_service();   // stops floserverd; quit_done follows

    [[nodiscard]] static std::string on_key(const std::string& project) { return "on\n" + project; }
    [[nodiscard]] static std::string keep_key(const std::string& project) { return "keep\n" + project; }
    [[nodiscard]] static std::string use_key(const std::string& project, const std::string& key) {
        return "use\n" + project + "\n" + key;
    }

    // The latest snapshot; the one-shot parts are handed over only once.
    [[nodiscard]] Snapshot take();

private:
    struct Command {
        enum class Kind : std::uint8_t {
            SetOn,
            SetKeep,
            Use,
            CopyEnv,
            OpenStudio,
            AddFolder,
            Select,
            Start,
            Refresh,
            Quit
        } kind;
        std::string project;
        bool flag = false;
        std::string key;
        std::optional<std::string> value;
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

#pragma once

// The window's link to floserverd (D2). One background thread connects
// (starting the service the way the CLI does), asks for projects.list,
// status, web.status, databases.list and shared.get (without the www
// folder's folders: nothing reads it in the background) every 2 seconds,
// and sends the user's changes. While a project's panel is open it also asks for that
// project's log. The UI thread never waits on the pipe: the worker keeps the
// latest snapshot and posts a message to the window when it changes.

#include "model/runtimes_page.h"
#include "model/ui_strings.h"
#include "model/view_model.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
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
    bool has_installed = false;  // runtimes.list has answered at least once
    nlohmann::json databases;   // databases.list's result, or null
    PanelData panel;
    // Projects with installs running from the window: the progress line.
    std::map<std::string, std::string> downloads;
    // Every install the worker follows, for Settings' rows (D20).
    std::vector<RuntimeJob> runtime_jobs;
    std::string projects_dir;  // projects.list's, or empty
    SharedSites shared;        // Shared sites (D19): shared.get's answer
    // New project (D9): the templates, once asked for.
    bool templates_loaded = false;
    std::string templates_error;
    std::vector<TemplateCard> templates;
    // The project being made: create.get's answer.
    struct Create {
        std::int64_t job = 0;
        std::string state;  // running, done, failed, cancelled; empty before the first answer
        std::string step;
        std::vector<std::string> log;
        std::string project;
        std::string error;
    } create;
    // New project: the defaults from Settings (D20), once asked for.
    bool defaults_loaded = false;
    Defaults defaults;
    // Taken once by the UI:
    // Settings (D20): runtimes.list's full answer, or why it couldn't be had.
    std::optional<nlohmann::json> runtimes;
    std::string runtimes_error;
    bool runtimes_refresh = false;  // the answer to "Check for updates"
    std::string settings_error;     // a change in Settings failed
    std::vector<std::string> settled;  // change keys whose command finished (see ServiceWorker)
    // Settings > PHP and the project panel's PHP part (D20): answers in the
    // order they came. `to` is kPhpSettings, or the project's name for the panel.
    struct PhpAnswer {
        std::string to;
        std::string series;  // the series asked for (php.get, php.set); empty for projects.php
        std::optional<nlohmann::json> reply;  // php.get's or php.set's; projects.php's (the project)
        std::string error;                    // the service's reason, or the connection's
        bool save = false;                    // the answer to a save
    };
    std::vector<PhpAnswer> php;
    std::string custom_ini;        // php.custom_ini's path, to open in Notepad
    std::string custom_ini_error;
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
    // Quietly: nothing is said when Studio isn't installed (after a FloCMS template).
    void open_in_studio(const std::string& project, bool quietly = false);
    // Adds a folder as a project (projects.add) and turns it on.
    void add_folder(const std::string& path);
    // Installs what a project needs (runtimes.install) and follows the jobs.
    // Shared sites' installs go under kSharedKey.
    void install(const std::string& project, const std::vector<Download>& downloads);
    // Shared sites (D19): sends shared.set with `changes`; `key` is reported
    // in settled when it's done (on_key(kSharedKey) or use_key(kSharedKey, ...)).
    void set_shared(const nlohmann::json& changes, const std::string& key);
    // Puts shared sites' database login on the clipboard as .env lines (shared.login).
    void copy_shared_env();
    // Settings (D20). runtimes.list's full answer: the list in hand or on
    // disk (no network), or a fresh one when `refresh`. Handed over once.
    void list_runtimes(bool refresh);
    // While Settings is open, a finished install, remove or default change
    // makes the worker ask runtimes.list again (cached). Never a timer.
    void watch_runtimes(bool on);
    // runtimes.remove; settled reports remove_key(name, folder).
    void remove_runtime(const std::string& name, const std::string& folder);
    // runtimes.default; nothing clears it. settled reports default_key(name).
    void set_default(const std::string& name, const std::optional<std::string>& series);
    void cancel_job(std::int64_t id);
    // Settings > PHP (D20): php.get for one series ("8.5"), answered to `to`
    // (kPhpSettings or the panel's project); php.set with only what changed;
    // php.custom_ini, whose path comes back to be opened.
    void php_get(const std::string& series, const std::string& to);
    void php_set(const nlohmann::json& params);
    void php_custom_ini(const std::string& series);
    // The panel's PHP part: projects.php, answered to the project.
    void project_php(const std::string& project, const nlohmann::json& params);
    // New project: runtimes.list (cached) once, for its defaults.
    void load_defaults();
    // Asks for the templates (templates.list).
    void load_templates();
    // Makes a project from a template (projects.create) and follows the job.
    void create_project(const std::string& template_id, const std::string& name, const std::string& parent,
                        const std::map<std::string, std::optional<std::string>>& versions);
    // The project whose log is fetched; empty for none.
    void select(const std::string& project);
    void start_service();  // after NotRunning or Failed
    void refresh();
    void quit_service();   // stops floserverd; quit_done follows

    // Shared sites' name wherever a project's goes; project names can't have '_'.
    static constexpr const char* kSharedKey = "_shared";
    // Settings' answers in Snapshot::php; project names can't have '_'.
    static constexpr const char* kPhpSettings = "_settings";

    [[nodiscard]] static std::string on_key(const std::string& project) { return "on\n" + project; }
    [[nodiscard]] static std::string keep_key(const std::string& project) { return "keep\n" + project; }
    [[nodiscard]] static std::string install_key(const std::string& project) { return "install\n" + project; }
    [[nodiscard]] static std::string remove_key(const std::string& name, const std::string& folder) {
        return "remove-runtime\n" + name + "\n" + folder;
    }
    [[nodiscard]] static std::string default_key(const std::string& name) { return "default\n" + name; }
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
            Install,
            SetShared,
            CopySharedEnv,
            LoadTemplates,
            CreateProject,
            ListRuntimes,
            WatchRuntimes,
            RemoveRuntime,
            SetDefault,
            CancelJob,
            PhpGet,
            PhpSet,
            PhpCustomIni,
            ProjectPhp,
            LoadDefaults,
            Select,
            Start,
            Refresh,
            Quit
        } kind;
        std::string project;
        bool flag = false;
        std::string key;
        std::optional<std::string> value;
        std::vector<Download> downloads;
        nlohmann::json params;  // CreateProject: projects.create's parameters; SetShared: the changes; php.*
    };

    // "Check for updates" (runtimes.list {"refresh": true}) can take as long
    // as the network does. It runs on its own short-lived thread and pipe
    // connection, never on the 2 s polling path, so the main window keeps
    // updating during a slow or offline fetch. The thread is detached and
    // shares only this box; take() hands its answer over.
    struct RefreshBox {
        std::mutex mutex;
        bool running = false;
        bool done = false;
        std::optional<nlohmann::json> reply;
        std::string error;
    };

    void push(Command command);
    void run(const std::stop_token& stop);
    void publish();
    void start_refresh();

    HWND window_;
    UINT message_;
    Lang lang_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Command> commands_;
    Snapshot latest_;
    bool posted_ = false;
    std::shared_ptr<RefreshBox> refresh_ = std::make_shared<RefreshBox>();
    std::jthread thread_;  // last: it starts after everything above exists
};

}  // namespace floserver::ui

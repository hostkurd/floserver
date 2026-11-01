#include "service_worker.h"

#include "floserver/core/integration.h"
#include "floserver/core/ipc.h"
#include "floserver/core/mariadb.h"
#include "floserver/core/paths.h"
#include "floserver/core/service_client.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace floserver::ui {

namespace {

namespace core = floserver::core;
using namespace std::chrono_literals;

constexpr auto kPollEvery = 2s;
// What's installed changes only with an install: asked every few polls.
constexpr int kInstalledEvery = 5;
constexpr std::size_t kLogLines = 60;
// A request that gets no answer in this time counts as a lost connection.
// projects.set waits until nginx serves the change, so it's generous; the
// window never waits for it, only this thread does.
constexpr auto kRequestTimeout = 30s;
// phpmyadmin.login waits for MariaDB to start; its first start sets up the
// data folder too (the service allows 90 s for each).
constexpr auto kPhpMyAdminTimeout = 200s;

// A string member of a reply, or empty.
std::string string_field(const nlohmann::json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return {};
}

// Zeroes a string's bytes before it's freed (it may hold a password).
void wipe(std::string& s) {
    if (!s.empty()) {
        SecureZeroMemory(s.data(), s.size());
    }
}

// The same for every string inside a reply.
void wipe_strings(nlohmann::json& j) {
    if (j.is_string()) {
        wipe(j.get_ref<std::string&>());
    } else if (j.is_structured()) {
        for (auto& item : j) {
            wipe_strings(item);
        }
    }
}

// ms since 1970 (UTC) -> local hours, minutes, seconds.
void local_time(std::int64_t ms, int& h, int& m, int& s) {
    constexpr std::int64_t kEpochDifference = 116444736000000000LL;  // 1601 -> 1970, in 100 ns
    const std::int64_t ticks = ms * 10000 + kEpochDifference;
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(ticks) & 0xFFFFFFFFU);
    ft.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(ticks) >> 32U);
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (FileTimeToSystemTime(&ft, &utc) == FALSE || SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local) == FALSE) {
        h = m = s = 0;
        return;
    }
    h = local.wHour;
    m = local.wMinute;
    s = local.wSecond;
}

}  // namespace

ServiceWorker::ServiceWorker(HWND window, UINT message, Lang lang)
    : window_(window), message_(message), lang_(lang), thread_([this](const std::stop_token& st) { run(st); }) {}

ServiceWorker::~ServiceWorker() {
    thread_.request_stop();
    wake_.notify_all();
}

void ServiceWorker::retarget(HWND window, Lang lang) {
    lang_ = lang;
    {
        const std::lock_guard lock(mutex_);
        window_ = window;
        // A message posted to the old window may have gone with it: post again.
        posted_ = false;
        publish();
    }
    // The rows, footer and errors in hand were worded in the old language:
    // asked again now rather than at the next 2 s poll.
    refresh();
}

void ServiceWorker::push(Command command) {
    {
        const std::lock_guard lock(mutex_);
        commands_.push_back(std::move(command));
    }
    wake_.notify_all();
}

void ServiceWorker::set_on(const std::string& project, bool on) {
    push({Command::Kind::SetOn, project, on, {}, std::nullopt, {}, {}});
}
void ServiceWorker::set_keep_running(const std::string& project, bool keep) {
    push({Command::Kind::SetKeep, project, keep, {}, std::nullopt, {}, {}});
}
void ServiceWorker::use(const std::string& project, const std::string& key, const std::optional<std::string>& value) {
    push({Command::Kind::Use, project, false, key, value, {}, {}});
}
void ServiceWorker::copy_env(const std::string& project) {
    push({Command::Kind::CopyEnv, project, false, {}, std::nullopt, {}, {}});
}
void ServiceWorker::open_in_studio(const std::string& project, bool quietly) {
    push({Command::Kind::OpenStudio, project, quietly, {}, std::nullopt, {}, {}});
}
void ServiceWorker::add_folder(const std::string& path) {
    push({Command::Kind::AddFolder, path, false, {}, std::nullopt, {}, {}});
}
void ServiceWorker::install(const std::string& project, const std::vector<Download>& downloads) {
    push({Command::Kind::Install, project, false, {}, std::nullopt, downloads, {}});
}
void ServiceWorker::set_shared(const nlohmann::json& changes, const std::string& key) {
    push({Command::Kind::SetShared, {}, false, key, std::nullopt, {}, changes});
}
void ServiceWorker::copy_shared_env() {
    push({Command::Kind::CopySharedEnv, {}, false, {}, std::nullopt, {}, {}});
}
void ServiceWorker::load_templates() {
    {
        const std::lock_guard lock(mutex_);
        latest_.templates_loaded = false;  // "Loading…" until the answer
        latest_.templates_error.clear();
    }
    push({Command::Kind::LoadTemplates, {}, false, {}, std::nullopt, {}, {}});
}
void ServiceWorker::create_project(const std::string& template_id, const std::string& name, const std::string& parent,
                                   const std::map<std::string, std::optional<std::string>>& versions) {
    nlohmann::json params = {{"template", template_id}, {"name", name}, {"parent", parent}, {"versions", nlohmann::json::object()}};
    for (const auto& [key, value] : versions) {
        params["versions"][key] = value ? nlohmann::json(*value) : nlohmann::json(nullptr);
    }
    {
        // The last project's answer goes now, so the window never mistakes it for this one's.
        const std::lock_guard lock(mutex_);
        latest_.create = {};
    }
    push({Command::Kind::CreateProject, name, false, {}, std::nullopt, {}, std::move(params)});
}
void ServiceWorker::list_runtimes(bool refresh) {
    if (refresh) {
        start_refresh();  // off the polling path: see RefreshBox
        return;
    }
    // Cached is quick: the service copies the list in hand under its lock,
    // or reads the kept file, and never waits on a fetch that's running
    // (get_list() keeps its lock released during the download).
    push({Command::Kind::ListRuntimes, {}, false, {}, std::nullopt, {}, {}});
}

void ServiceWorker::start_refresh() {
    {
        const std::lock_guard lock(refresh_->mutex);
        if (refresh_->running) {
            return;  // one check at a time; its answer is on its way
        }
        refresh_->running = true;
        refresh_->done = false;
    }
    std::thread([box = refresh_, window = window_, message = message_, lost = std::string(text(lang_, Str::ConnectionLost))] {
        std::optional<nlohmann::json> reply;
        std::string error;
        try {
            const std::string instance = core::ipc::default_instance();
            if (auto client = core::ServiceClient::connect(instance, error, kRequestTimeout)) {
                if (auto got = client->call("runtimes.list", {{"refresh", true}}); !got) {
                    error = lost;
                } else if (!got->ok) {
                    error = got->error_message;
                } else {
                    reply = std::move(got->result);
                }
            } else if (error.empty()) {
                error = lost;  // not running: the main window says so and offers to start it
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        {
            const std::lock_guard lock(box->mutex);
            box->reply = std::move(reply);
            box->error = std::move(error);
            box->done = true;
            box->running = false;
        }
        // Gone windows ignore it; the box lives as long as this thread needs it.
        PostMessageW(window, message, 0, 0);
    }).detach();
}
void ServiceWorker::watch_runtimes(bool on) { push({Command::Kind::WatchRuntimes, {}, on, {}, std::nullopt, {}, {}}); }
void ServiceWorker::remove_runtime(const std::string& name, const std::string& folder) {
    push({Command::Kind::RemoveRuntime, name, false, folder, std::nullopt, {}, {}});
}
void ServiceWorker::set_default(const std::string& name, const std::optional<std::string>& series) {
    push({Command::Kind::SetDefault, name, false, {}, series, {}, {}});
}
void ServiceWorker::cancel_job(std::int64_t id) {
    push({Command::Kind::CancelJob, {}, false, {}, std::nullopt, {}, nlohmann::json(id)});
}
void ServiceWorker::php_get(const std::string& series, const std::string& to) {
    push({Command::Kind::PhpGet, to, false, series, std::nullopt, {}, {}});
}
void ServiceWorker::php_set(const nlohmann::json& params) {
    push({Command::Kind::PhpSet, kPhpSettings, false, {}, std::nullopt, {}, params});
}
void ServiceWorker::php_custom_ini(const std::string& series, bool final) {
    push({Command::Kind::PhpCustomIni, {}, final, series, std::nullopt, {}, {}});
}
void ServiceWorker::web_settings(const std::string& what, const nlohmann::json& params) {
    push({Command::Kind::WebSettings, {}, false, what, std::nullopt, {}, params});
}
void ServiceWorker::project_php(const std::string& project, const nlohmann::json& params) {
    push({Command::Kind::ProjectPhp, project, false, {}, std::nullopt, {}, params});
}
void ServiceWorker::database_call(const std::string& method, const nlohmann::json& params, const std::string& purpose) {
    if (method == "phpmyadmin.login") {
        start_side_call(method, params, purpose);  // off the polling path: see SideBox
        return;
    }
    // Settings > Folders: folders.get counts the data folder and
    // folders.clean deletes files; seconds, maybe. Off the polling path too.
    if (method == "folders.get" || method == "folders.clean") {
        start_side_call(method, params, method);
        return;
    }
    push({Command::Kind::DatabaseCall, purpose, false, method, std::nullopt, {}, params});
}

void ServiceWorker::start_side_call(const std::string& method, const nlohmann::json& params, const std::string& purpose) {
    {
        const std::lock_guard lock(side_->mutex);
        if (!side_->running.insert(purpose).second) {
            return;  // one at a time; its answer is on its way
        }
    }
    std::thread([box = side_, window = window_, message = message_, method, params, purpose,
                 lost = std::string(text(lang_, Str::ConnectionLost))] {
        Snapshot::DbAnswer answer{method, string_field(params, "series"), purpose, std::nullopt, {}};
        try {
            std::string error;
            const std::string instance = core::ipc::default_instance();
            if (auto client = core::ServiceClient::connect(instance, error, kPhpMyAdminTimeout)) {
                if (auto got = client->call(method, params); !got) {
                    answer.error = lost;
                } else if (!got->ok) {
                    answer.error = got->error_message;
                } else {
                    answer.reply = std::move(got->result);
                }
            } else {
                answer.error = error.empty() ? lost : error;
            }
        } catch (const std::exception& e) {
            answer.error = e.what();
        }
        {
            const std::lock_guard lock(box->mutex);
            box->running.erase(purpose);
            box->answers.push_back(std::move(answer));
        }
        // Gone windows ignore it; the box lives as long as this thread needs it.
        PostMessageW(window, message, 0, 0);
    }).detach();
}
void ServiceWorker::load_defaults() {
    {
        const std::lock_guard lock(mutex_);
        latest_.defaults_loaded = false;
    }
    push({Command::Kind::LoadDefaults, {}, false, {}, std::nullopt, {}, {}});
}
void ServiceWorker::select(const std::string& project) {
    push({Command::Kind::Select, project, false, {}, std::nullopt, {}, {}});
}
void ServiceWorker::start_service() { push({Command::Kind::Start, {}, false, {}, std::nullopt, {}, {}}); }
void ServiceWorker::refresh() { push({Command::Kind::Refresh, {}, false, {}, std::nullopt, {}, {}}); }
void ServiceWorker::quit_service() { push({Command::Kind::Quit, {}, false, {}, std::nullopt, {}, {}}); }

Snapshot ServiceWorker::take() {
    const std::lock_guard lock(mutex_);
    posted_ = false;
    // The clipboard text holds a password: moved, never copied.
    std::string clipboard = std::move(latest_.clipboard);
    wipe(latest_.clipboard);
    latest_.clipboard.clear();
    // Settings > Database's answers may hold root's password: moved too.
    std::vector<Snapshot::DbAnswer> db = std::move(latest_.db);
    latest_.db.clear();
    Snapshot out = latest_;
    out.clipboard = std::move(clipboard);
    out.db = std::move(db);
    {
        const std::lock_guard side_lock(side_->mutex);
        for (Snapshot::DbAnswer& answer : side_->answers) {
            out.db.push_back(std::move(answer));
        }
        side_->answers.clear();
    }
    {
        const std::lock_guard refresh_lock(refresh_->mutex);
        // A cached answer waiting in this snapshot goes first; the check's follows.
        if (refresh_->done && !out.runtimes) {
            refresh_->done = false;
            out.runtimes_refresh = true;
            out.runtimes = std::move(refresh_->reply);
            out.runtimes_error = std::move(refresh_->error);
            refresh_->reply.reset();
            refresh_->error.clear();
        }
    }
    latest_.settled.clear();
    latest_.change_error.clear();
    latest_.added.clear();
    latest_.runtimes.reset();
    latest_.runtimes_error.clear();
    latest_.runtimes_refresh = false;
    latest_.settings_error.clear();
    latest_.php.clear();
    latest_.web_settings.clear();
    latest_.custom_ini.clear();
    latest_.custom_ini_error.clear();
    latest_.custom_ini_final = false;
    latest_.quit_done = false;
    latest_.quit_error.clear();
    return out;
}

// Called with mutex_ held.
void ServiceWorker::publish() {
    if (!posted_) {
        posted_ = PostMessageW(window_, message_, 0, 0) != FALSE;
    }
}

void ServiceWorker::run(const std::stop_token& stop) {
    const std::string instance = core::ipc::default_instance();
    std::optional<core::ServiceClient> client;
    bool may_start = true;  // at launch, the way the CLI does; later only when the user asks
    bool first = true;
    int polls = 0;
    bool need_installed = true;
    std::string selected;
    PanelData panel;
    bool panel_checked = false;  // flocms/studio looked up for `selected`
    // Installs started from the window, followed until they finish.
    // The service hands out one job for the same runtime while it runs, so
    // several projects can wait on one job.
    struct Job {
        std::int64_t id = 0;
        std::set<std::string> projects;
        std::string label;  // "PHP 8.2"
    };
    std::vector<Job> jobs;
    std::int64_t create_job = 0;  // the project being made, followed until it ends
    bool watching = false;        // Settings is open (D20)

    const auto set_link = [this](Link link, std::string error = {}) {
        const std::lock_guard lock(mutex_);
        latest_.link = link;
        latest_.error = std::move(error);
        publish();
    };

    const auto connect = [&]() -> std::optional<core::ServiceClient> {
        std::string error;
        if (auto c = core::ServiceClient::connect(instance, error, kRequestTimeout)) {
            may_start = false;
            return c;
        }
        if (!error.empty()) {
            set_link(Link::Failed, error);
            return std::nullopt;
        }
        if (!may_start) {
            set_link(Link::NotRunning);
            return std::nullopt;
        }
        may_start = false;
        set_link(Link::Starting);
        if (core::start_service(instance, error) == core::StartResult::Failed) {
            set_link(Link::Failed, error);
            return std::nullopt;
        }
        auto c = core::ServiceClient::connect(instance, error, kRequestTimeout);
        if (!c) {
            set_link(Link::Failed, error.empty() ? "it started but can't be reached" : error);
        }
        return c;
    };

    while (!stop.stop_requested()) {
        std::deque<Command> batch;
        {
            std::unique_lock lock(mutex_);
            if (!first) {
                wake_.wait_for(lock, stop, kPollEvery, [this] { return !commands_.empty(); });
            }
            if (stop.stop_requested()) {
                return;
            }
            batch.swap(commands_);
        }
        first = false;

        bool quit = false;
        for (const Command& c : batch) {
            quit = quit || c.kind == Command::Kind::Quit;
            may_start = may_start || c.kind == Command::Kind::Start;
            if (c.kind == Command::Kind::Select && c.project != selected) {
                selected = c.project;
                panel = PanelData{};
                panel.project = selected;
                panel_checked = false;
                need_installed = true;
            }
        }
        if (quit) {
            client.reset();
            set_link(Link::Stopping);
            std::string error;
            const bool stopped = core::stop_service(instance, error);
            const std::lock_guard lock(mutex_);
            latest_.quit_done = true;
            latest_.quit_error = stopped ? std::string() : error;
            publish();
            return;
        }

        if (!client) {
            client = connect();
            // Job ids belong to the service that gave them; a new one starts again at 1.
            if (client && !jobs.empty()) {
                jobs.clear();
                need_installed = true;
            }
            // A new service doesn't know the project being made.
            if (client && create_job != 0) {
                Snapshot::Create made;
                made.job = create_job;
                made.state = "failed";
                made.error = std::string(text(lang_, Str::ConnectionLost));
                create_job = 0;
                const std::lock_guard lock(mutex_);
                latest_.create = std::move(made);
            }
        }

        const std::string lost(text(lang_, Str::ConnectionLost));
        std::vector<std::string> settled;
        std::string change_error;
        // Settings (D20): its messages stay in its own window.
        std::string settings_error;
        std::optional<nlohmann::json> runtimes;
        std::string runtimes_error;
        bool refetch_runtimes = false;  // a change Settings shows is done: ask again
        std::optional<Defaults> defaults;
        std::vector<Snapshot::PhpAnswer> php_answers;
        std::vector<Snapshot::WebSettingsAnswer> web_answers;
        std::vector<Snapshot::DbAnswer> db_answers;
        std::string custom_ini;
        std::string custom_ini_error;
        bool custom_ini_final = false;
        // One php.* or projects.php request, answered to `to`.
        const auto php_call = [&](const std::string& to, const std::string& series, const char* method,
                                  const nlohmann::json& params, bool save) {
            Snapshot::PhpAnswer answer{to, series, std::nullopt, {}, save};
            if (!client) {
                answer.error = lost;
            } else if (auto got = client->call(method, params); !got) {
                client.reset();
                answer.error = lost;
            } else if (!got->ok) {
                answer.error = got->error_message;
            } else {
                answer.reply = std::move(got->result);
            }
            php_answers.push_back(std::move(answer));
        };
        const auto list_runtimes_now = [&](bool refresh) {
            if (!client) {
                runtimes_error = lost;
                return;
            }
            const auto got = client->call("runtimes.list", refresh ? nlohmann::json{{"refresh", true}}
                                                                   : nlohmann::json{{"cached", true}});
            if (!got) {
                client.reset();
                runtimes_error = lost;
            } else if (!got->ok) {
                runtimes_error = got->error_message;
            } else {
                runtimes = got->result;
                runtimes_error.clear();
            }
        };
        std::string clipboard;
        std::string added;
        // "Couldn't change <project>: <reason>". The project name keeps its
        // own direction; the service's reason is English (left to right).
        auto change_failed = [this](const std::string& project, const std::string& reason, bool english) {
            const bool rtl = is_rtl(lang_);
            return fill_pattern(text(lang_, Str::ChangeFailed), false,
                                {rtl ? isolate_auto(project) : project, rtl && english ? isolate_ltr(reason) : reason});
        };
        const std::string shared_name(text(lang_, Str::SharedSites));  // "Couldn't change Shared sites: ..."
        // One request for a change: false when the connection is gone.
        const auto send = [&](const std::string& project, const char* method, const nlohmann::json& params,
                              std::optional<core::protocol::Reply>& reply) {
            if (!client) {
                change_error = change_failed(project, lost, false);  // never sent: the user is told why
                return false;
            }
            reply = client->call(method, params);
            if (!reply) {
                client.reset();
                change_error = change_failed(project, lost, false);
                return false;
            }
            if (!reply->ok) {
                change_error = change_failed(project, reply->error_message, true);
                return false;
            }
            return true;
        };
        for (Command& c : batch) {
            std::optional<core::protocol::Reply> reply;
            switch (c.kind) {
                case Command::Kind::SetOn:
                    settled.push_back(on_key(c.project));
                    (void)send(c.project, "projects.set", {{"name", c.project}, {"on", c.flag}}, reply);
                    break;
                case Command::Kind::SetKeep:
                    settled.push_back(keep_key(c.project));
                    (void)send(c.project, "projects.set", {{"name", c.project}, {"keep_running", c.flag}}, reply);
                    break;
                case Command::Kind::Use: {
                    settled.push_back(use_key(c.project, c.key));
                    nlohmann::json params = {{"name", c.project}, {"key", c.key}, {"value", nullptr}};
                    if (c.value) {
                        params["value"] = *c.value;
                    }
                    if (send(c.project, "projects.use", params, reply)) {
                        need_installed = true;
                    }
                    break;
                }
                case Command::Kind::CopyEnv:
                    // The login is made the first time; the reply holds the
                    // password, so it lives only until it's on the clipboard.
                    if (send(c.project, "databases.login", {{"name", c.project}}, reply)) {
                        wipe(clipboard);
                        clipboard = env_text(reply->result);
                    }
                    if (reply) {
                        wipe_strings(reply->result);
                        reply->result = nlohmann::json();
                    }
                    break;
                case Command::Kind::OpenStudio: {
                    std::optional<core::protocol::Reply> list;
                    std::string error;
                    const auto program = core::find_studio();
                    if (!program) {
                        if (!c.flag) {
                            change_error = fill(lang_, Str::OpenFailed, {"FloCMS Studio"});
                        }
                    } else if (send(c.project, "projects.list", {}, list)) {
                        for (const auto& p : list->result.value("projects", nlohmann::json::array())) {
                            if (p.value("name", std::string()) == c.project) {
                                const auto dir = core::path_from_utf8(p.value("path", std::string()));
                                if (!core::open_in_studio(*program, dir, error)) {
                                    change_error = fill(lang_, Str::OpenFailed, {"FloCMS Studio"}) + ": " +
                                                   (is_rtl(lang_) ? isolate_ltr(error) : error);
                                }
                            }
                        }
                    }
                    break;
                }
                case Command::Kind::AddFolder: {
                    // The folder's own name for the message; the path for the service.
                    std::string shown = core::path_to_utf8(core::path_from_utf8(c.project).filename());
                    if (shown.empty()) {
                        shown = c.project;  // a drive or share root
                    }
                    const auto failed = [&](const std::string& reason, bool english) {
                        const bool rtl = is_rtl(lang_);
                        change_error = fill_pattern(text(lang_, Str::AddFailed), false,
                                                    {rtl ? isolate_auto(shown) : shown,
                                                     rtl && english ? isolate_ltr(reason) : reason});
                    };
                    if (!client) {
                        failed(lost, false);
                        break;
                    }
                    reply = client->call("projects.add", {{"path", c.project}});
                    if (!reply) {
                        client.reset();
                        failed(lost, false);
                    } else if (!reply->ok) {
                        failed(reply->error_message, true);
                    } else {
                        const std::string name = reply->result.is_object() && reply->result.contains("name") &&
                                                         reply->result["name"].is_string()
                                                     ? reply->result["name"].get<std::string>()
                                                     : std::string();
                        if (name.empty()) {
                            failed("the service's reply had no project name", true);
                            break;
                        }
                        // Turned on at once; if that fails it stays Off and change_error says why.
                        std::optional<core::protocol::Reply> on;
                        (void)send(name, "projects.set", {{"name", name}, {"on", true}}, on);
                        added = name;
                    }
                    break;
                }
                case Command::Kind::Install: {
                    settled.push_back(install_key(c.project));
                    std::string& install_error = is_settings_key(c.project) ? settings_error : change_error;
                    for (const Download& d : c.downloads) {
                        const std::string label = runtime_label(d.name) + (d.version.empty() ? "" : " " + d.version);
                        nlohmann::json params = {{"name", d.name}};
                        if (!d.version.empty()) {
                            params["version"] = d.version;
                        }
                        std::optional<core::protocol::Reply> started;
                        if (!client) {
                            install_error = fill(lang_, Str::DownloadFailed, {label, lost});
                            break;
                        }
                        started = client->call("runtimes.install", params);
                        if (!started) {
                            client.reset();
                            install_error = fill(lang_, Str::DownloadFailed, {label, lost});
                            break;
                        }
                        if (!started->ok || !started->result.contains("job") ||
                            !started->result["job"].is_number_integer()) {
                            const std::string why = started->ok ? std::string("no job") : started->error_message;
                            install_error = fill(lang_, Str::DownloadFailed, {label, why});
                            continue;
                        }
                        const auto id = started->result["job"].get<std::int64_t>();
                        const auto same = std::find_if(jobs.begin(), jobs.end(), [id](const Job& j) { return j.id == id; });
                        if (same != jobs.end()) {
                            same->projects.insert(c.project);
                        } else {
                            jobs.push_back({id, {c.project}, label});
                        }
                    }
                    break;
                }
                case Command::Kind::SetShared: {
                    settled.push_back(c.key);
                    nlohmann::json params = c.params;
                    params["folders"] = false;  // the www folder isn't read for the window
                    (void)send(shared_name, "shared.set", params, reply);
                    break;
                }
                case Command::Kind::CopySharedEnv:
                    // As CopyEnv: the reply holds the password, kept only until
                    // it's on the clipboard. FloCMS's keys (the default when a
                    // project's .env doesn't say).
                    if (send(shared_name, "shared.login", nlohmann::json::object(), reply)) {
                        core::DatabaseLogin login;
                        const auto& r = reply->result;
                        if (r.is_object() && r.contains("port") && r["port"].is_number_unsigned() &&
                            r["port"].get<std::uint64_t>() <= 0xFFFFU) {
                            login.port = static_cast<core::Port>(r["port"].get<std::uint64_t>());
                        }
                        login.database = string_field(r, "database");
                        login.user = string_field(r, "user");
                        login.password = string_field(r, "password");
                        auto values = core::env_values(core::EnvStyle::FloCms, login);
                        nlohmann::json env = {{"env", nlohmann::json::array()}};
                        for (auto& [key, value] : values) {
                            env["env"].push_back({key, value});
                            wipe(value);
                        }
                        wipe(clipboard);
                        clipboard = env_text(env);
                        wipe_strings(env);
                        wipe(login.password);
                    }
                    if (reply) {
                        wipe_strings(reply->result);
                        reply->result = nlohmann::json();
                    }
                    break;
                case Command::Kind::LoadTemplates: {
                    std::optional<core::protocol::Reply> got;
                    std::string why;
                    std::vector<TemplateCard> cards;
                    if (!client) {
                        why = lost;
                    } else if (got = client->call("templates.list"); !got) {
                        client.reset();
                        why = lost;
                    } else if (!got->ok) {
                        why = got->error_message;
                    } else {
                        cards = cards_from_list(got->result, lang_);
                    }
                    const std::lock_guard lock(mutex_);
                    latest_.templates_loaded = true;
                    latest_.templates_error = why;
                    if (why.empty()) {
                        latest_.templates = std::move(cards);  // a failed ask keeps the cards there were
                    }
                    break;
                }
                case Command::Kind::CreateProject: {
                    Snapshot::Create made;
                    if (!client) {
                        made.state = "failed";
                        made.error = lost;
                    } else if (reply = client->call("projects.create", c.params); !reply) {
                        client.reset();
                        made.state = "failed";
                        made.error = lost;
                    } else if (!reply->ok || !reply->result.contains("job") || !reply->result["job"].is_number_integer()) {
                        made.state = "failed";
                        made.error = reply->ok ? std::string("no job") : reply->error_message;
                    } else {
                        made.job = reply->result["job"].get<std::int64_t>();
                        create_job = made.job;
                    }
                    const std::lock_guard lock(mutex_);
                    latest_.create = std::move(made);
                    break;
                }
                case Command::Kind::ListRuntimes:
                    list_runtimes_now(false);  // cached only; a refresh has its own thread
                    break;
                case Command::Kind::WatchRuntimes:
                    watching = c.flag;
                    break;
                case Command::Kind::RemoveRuntime: {
                    settled.push_back(remove_key(c.project, c.key));
                    const std::string label = runtime_label(c.project) + " " + c.key;
                    const bool rtl = is_rtl(lang_);
                    if (!client) {
                        settings_error = fill(lang_, Str::SettingsRemoveFailed, {label, lost});
                    } else if (reply = client->call("runtimes.remove", {{"name", c.project}, {"version", c.key}}); !reply) {
                        client.reset();
                        settings_error = fill(lang_, Str::SettingsRemoveFailed, {label, lost});
                    } else if (!reply->ok) {
                        // The service's words, such as who still uses it (in_use).
                        settings_error = fill_pattern(text(lang_, Str::SettingsRemoveFailed), false,
                                                      {rtl ? isolate_ltr(label) : label,
                                                       rtl ? isolate_ltr(reply->error_message) : reply->error_message});
                    } else {
                        need_installed = true;
                        refetch_runtimes = refetch_runtimes || watching;
                    }
                    break;
                }
                case Command::Kind::SetDefault: {
                    settled.push_back(default_key(c.project));
                    const nlohmann::json params = {{"name", c.project},
                                                   {"series", c.value ? nlohmann::json(*c.value) : nlohmann::json(nullptr)}};
                    if (!client) {
                        settings_error = fill(lang_, Str::SettingsDefaultFailed, {lost});
                    } else if (reply = client->call("runtimes.default", params); !reply) {
                        client.reset();
                        settings_error = fill(lang_, Str::SettingsDefaultFailed, {lost});
                    } else if (!reply->ok) {
                        settings_error = fill(lang_, Str::SettingsDefaultFailed, {reply->error_message});
                    } else {
                        refetch_runtimes = refetch_runtimes || watching;
                    }
                    break;
                }
                case Command::Kind::CancelJob:
                    if (client && c.params.is_number_integer()) {
                        if (reply = client->call("jobs.cancel", {{"id", c.params.get<std::int64_t>()}}); !reply) {
                            client.reset();
                        }
                    }
                    break;
                case Command::Kind::PhpGet:
                    php_call(c.project, c.key, "php.get", {{"series", c.key}}, false);
                    break;
                case Command::Kind::PhpSet:
                    php_call(c.project, string_field(c.params, "series"), "php.set", c.params, true);
                    break;
                case Command::Kind::ProjectPhp:
                    php_call(c.project, {}, "projects.php", c.params, true);
                    break;
                case Command::Kind::WebSettings: {
                    Snapshot::WebSettingsAnswer answer{c.key, std::nullopt, {}};
                    if (client && c.key == "reload") {
                        // The CA was trusted or removed here: sites switch now.
                        if (const auto reloaded = client->call("web.reload", nlohmann::json::object()); !reloaded) {
                            client.reset();
                        }
                    }
                    const char* method = c.key == "set" ? "web.settings.set"
                                         : c.key == "ca" ? "certs.ensure_ca"
                                                         : "web.settings.get";
                    if (!client) {
                        answer.error = lost;
                    } else if (auto got = client->call(method, c.key == "set" ? c.params : nlohmann::json::object());
                               !got) {
                        client.reset();
                        answer.error = lost;
                    } else if (!got->ok) {
                        answer.error = got->error_message;
                    } else {
                        answer.reply = std::move(got->result);
                    }
                    web_answers.push_back(std::move(answer));
                    break;
                }
                case Command::Kind::PhpCustomIni:
                    custom_ini_final = c.flag;
                    if (!client) {
                        custom_ini_error = lost;
                    } else if (reply = client->call(c.flag ? "php.final_ini" : "php.custom_ini", {{"series", c.key}});
                               !reply) {
                        client.reset();
                        custom_ini_error = lost;
                    } else if (!reply->ok) {
                        custom_ini_error = reply->error_message;
                    } else {
                        custom_ini = string_field(reply->result, "path");
                        if (custom_ini.empty()) {
                            custom_ini_error = "the service's reply had no path";
                        }
                    }
                    break;
                case Command::Kind::DatabaseCall: {
                    Snapshot::DbAnswer answer{c.key, string_field(c.params, "series"), c.project, std::nullopt, {}};
                    if (!client) {
                        answer.error = lost;
                    } else if (auto got = client->call(c.key, c.params); !got) {
                        client.reset();
                        answer.error = lost;
                    } else if (!got->ok) {
                        answer.error = got->error_message;
                    } else {
                        answer.reply = std::move(got->result);
                    }
                    db_answers.push_back(std::move(answer));
                    break;
                }
                case Command::Kind::LoadDefaults: {
                    // The wizard waits for nothing: without an answer it keeps today's picks.
                    std::optional<core::protocol::Reply> got;
                    if (client) {
                        got = client->call("runtimes.list", {{"cached", true}});
                        if (!got) {
                            client.reset();
                        }
                    }
                    defaults = got && got->ok ? defaults_from_list(got->result) : Defaults{};
                    break;
                }
                case Command::Kind::Select:
                case Command::Kind::Start:
                case Command::Kind::Refresh:
                case Command::Kind::Quit:
                    break;
            }
        }

        std::optional<core::protocol::Reply> list;
        std::optional<core::protocol::Reply> status;
        std::optional<core::protocol::Reply> web;
        std::optional<core::protocol::Reply> databases;
        std::optional<core::protocol::Reply> installed;
        std::optional<core::protocol::Reply> log;
        std::optional<core::protocol::Reply> shared;
        if (client) {
            list = client->call("projects.list");
            status = list ? client->call("status") : std::nullopt;
            web = status ? client->call("web.status") : std::nullopt;
            databases = web ? client->call("databases.list") : std::nullopt;
            if (databases && (need_installed || polls % kInstalledEvery == 0)) {
                installed = client->call("runtimes.list", {{"installed_only", true}});
                need_installed = !installed;
            }
            if (databases && !selected.empty()) {
                log = client->call("projects.log", {{"name", selected}, {"lines", kLogLines}});
            }
            // Shared sites' settings and state, never their folders.
            shared = databases ? client->call("shared.get", {{"folders", false}}) : std::nullopt;
            if (!databases || !shared) {
                client.reset();
            }
            ++polls;
        }
        // The installs' progress; a finished one makes the window ask what's installed.
        std::map<std::string, std::string> downloads;
        std::vector<RuntimeJob> runtime_jobs;
        if (client && !jobs.empty()) {
            std::vector<Job> running;
            bool any_done = false;
            for (std::size_t k = 0; k < jobs.size(); ++k) {
                const Job& j = jobs[k];
                const auto got = client->call("jobs.get", {{"id", j.id}});
                if (!got) {
                    client.reset();
                    running.insert(running.end(), jobs.begin() + static_cast<std::ptrdiff_t>(k), jobs.end());
                    break;
                }
                if (!got->ok || !got->result.is_object()) {
                    need_installed = true;  // forgotten by a restarted service: ask what's there
                    continue;
                }
                const std::string state = string_field(got->result, "state");
                if (state == "done") {
                    any_done = true;
                    need_installed = true;
                } else if (state == "failed") {
                    // Settings' own installs say so in Settings; the rest in the main window.
                    const bool settings_only = std::all_of(j.projects.begin(), j.projects.end(), is_settings_key);
                    (settings_only ? settings_error : change_error) =
                        fill(lang_, Str::DownloadFailed, {j.label, string_field(got->result, "message")});
                } else if (state != "cancelled") {
                    if (auto job = runtime_job(got->result)) {
                        runtime_jobs.push_back(std::move(*job));
                    }
                    std::string progress = job_progress_text(got->result, lang_);
                    if (progress.empty()) {
                        progress = fill(lang_, Str::Installing, {j.label});
                    }
                    for (const std::string& project : j.projects) {
                        downloads.try_emplace(project, progress);
                    }
                    running.push_back(j);
                }
            }
            jobs = std::move(running);
            refetch_runtimes = refetch_runtimes || (any_done && watching);
            if (any_done && client) {
                // Served at once rather than at the service's next look (5 s),
                // and the list asked again, so a finished project doesn't show
                // its Download button once more with the old answers.
                (void)client->call("web.reload");
                if (auto fresh = client->call("runtimes.list", {{"installed_only", true}}); fresh && fresh->ok) {
                    installed = std::move(fresh);
                    need_installed = false;
                }
                if (auto fresh = client->call("projects.list"); fresh) {
                    list = std::move(fresh);
                }
                // Shared sites too, or their Download button comes back once.
                if (shared) {
                    if (auto fresh = client->call("shared.get", {{"folders", false}}); fresh) {
                        shared = std::move(fresh);
                    }
                }
            }
        }
        // Settings shows who uses what: asked again once a change is done.
        if (refetch_runtimes && client && !runtimes) {
            list_runtimes_now(false);
        }
        // The project being made.
        std::optional<Snapshot::Create> create;
        if (client && create_job != 0) {
            if (const auto got = client->call("create.get", {{"id", create_job}}); !got) {
                client.reset();
            } else if (got->ok && got->result.is_object()) {
                Snapshot::Create made;
                made.job = create_job;
                made.state = string_field(got->result, "state");
                made.step = string_field(got->result, "step");
                made.project = string_field(got->result, "project");
                made.error = string_field(got->result, "error");
                if (got->result.contains("log") && got->result["log"].is_array()) {
                    for (const auto& line : got->result["log"]) {
                        if (line.is_string()) {
                            made.log.push_back(line.get<std::string>());
                        }
                    }
                }
                if (made.state != "running") {
                    create_job = 0;  // finished: the last answer stays on screen
                    need_installed = true;
                    if (auto fresh = client->call("projects.list"); fresh) {
                        list = std::move(fresh);
                    }
                }
                create = std::move(made);
            } else {
                // Forgotten (a restarted service): the window must not wait forever.
                Snapshot::Create made;
                made.job = create_job;
                made.state = "failed";
                made.error = got->ok ? std::string("the service's answer was unreadable") : got->error_message;
                create = std::move(made);
                create_job = 0;
            }
        }
        // Whether the open project is FloCMS (D14), read from its folder
        // here, off the UI thread, when it's first shown.
        if (list && list->ok && !selected.empty() && !panel_checked) {
            panel_checked = true;
            for (const auto& p : list->result.value("projects", nlohmann::json::array())) {
                if (p.value("name", std::string()) == selected) {
                    panel.flocms = core::is_flocms_project(core::path_from_utf8(p.value("path", std::string())));
                    panel.studio = panel.flocms && core::find_studio().has_value();
                }
            }
        }
        if (log && log->ok) {
            panel.has_log = true;
            panel.log = log_lines(log->result, local_time);
        }

        const std::lock_guard lock(mutex_);
        latest_.settled.insert(latest_.settled.end(), settled.begin(), settled.end());
        if (!change_error.empty()) {
            latest_.change_error = change_error;
        }
        if (!clipboard.empty()) {
            latest_.clipboard = std::move(clipboard);
        }
        // The UI may not have taken an earlier one yet: that one stays.
        if (!added.empty() && latest_.added.empty()) {
            latest_.added = added;
        }
        latest_.panel = panel;
        latest_.downloads = std::move(downloads);
        latest_.runtime_jobs = std::move(runtime_jobs);
        if (!settings_error.empty()) {
            latest_.settings_error = settings_error;
        }
        if (runtimes || !runtimes_error.empty()) {
            latest_.runtimes = std::move(runtimes);
            latest_.runtimes_error = runtimes_error;
        }
        for (auto& answer : php_answers) {
            latest_.php.push_back(std::move(answer));
        }
        for (auto& answer : web_answers) {
            latest_.web_settings.push_back(std::move(answer));
        }
        for (auto& answer : db_answers) {
            latest_.db.push_back(std::move(answer));
        }
        if (!custom_ini.empty() || !custom_ini_error.empty()) {
            latest_.custom_ini = std::move(custom_ini);
            latest_.custom_ini_error = std::move(custom_ini_error);
            latest_.custom_ini_final = custom_ini_final;
        }
        if (defaults) {
            latest_.defaults = std::move(*defaults);
            latest_.defaults_loaded = true;
        }
        if (create) {
            latest_.create = std::move(*create);
        }
        if (databases) {
            latest_.link = Link::Connected;
            latest_.error = list->ok ? std::string() : list->error_message;
            latest_.has_list = list->ok;
            latest_.rows = list->ok ? rows_from_list(list->result, lang_) : std::vector<ProjectRow>{};
            latest_.projects_dir = list->ok ? string_field(list->result, "projects_dir") : std::string();
            latest_.footer = footer_text(latest_.rows, status->ok ? &status->result : nullptr,
                                         web->ok ? &web->result : nullptr, lang_);
            latest_.databases = databases->ok ? databases->result : nlohmann::json();
            // An older service doesn't know shared.get: the row isn't shown.
            latest_.shared = shared && shared->ok ? shared_from_reply(shared->result) : SharedSites{};
            if (installed && installed->ok) {
                latest_.installed = installed_from_list(installed->result);
                latest_.has_installed = true;
            }
        } else if (latest_.link == Link::Connected) {
            // It was answering a moment ago: keep the last list on screen.
            latest_.link = Link::Lost;
        }
        publish();
    }
}

}  // namespace floserver::ui

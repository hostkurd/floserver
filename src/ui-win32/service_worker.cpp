#include "service_worker.h"

#include "floserver/core/integration.h"
#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/service_client.h"

#include <chrono>
#include <optional>
#include <utility>

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

void ServiceWorker::push(Command command) {
    {
        const std::lock_guard lock(mutex_);
        commands_.push_back(std::move(command));
    }
    wake_.notify_all();
}

void ServiceWorker::set_on(const std::string& project, bool on) {
    push({Command::Kind::SetOn, project, on, {}, std::nullopt});
}
void ServiceWorker::set_keep_running(const std::string& project, bool keep) {
    push({Command::Kind::SetKeep, project, keep, {}, std::nullopt});
}
void ServiceWorker::use(const std::string& project, const std::string& key, const std::optional<std::string>& value) {
    push({Command::Kind::Use, project, false, key, value});
}
void ServiceWorker::copy_env(const std::string& project) {
    push({Command::Kind::CopyEnv, project, false, {}, std::nullopt});
}
void ServiceWorker::open_in_studio(const std::string& project) {
    push({Command::Kind::OpenStudio, project, false, {}, std::nullopt});
}
void ServiceWorker::add_folder(const std::string& path) {
    push({Command::Kind::AddFolder, path, false, {}, std::nullopt});
}
void ServiceWorker::select(const std::string& project) {
    push({Command::Kind::Select, project, false, {}, std::nullopt});
}
void ServiceWorker::start_service() { push({Command::Kind::Start, {}, false, {}, std::nullopt}); }
void ServiceWorker::refresh() { push({Command::Kind::Refresh, {}, false, {}, std::nullopt}); }
void ServiceWorker::quit_service() { push({Command::Kind::Quit, {}, false, {}, std::nullopt}); }

Snapshot ServiceWorker::take() {
    const std::lock_guard lock(mutex_);
    posted_ = false;
    // The clipboard text holds a password: moved, never copied.
    std::string clipboard = std::move(latest_.clipboard);
    wipe(latest_.clipboard);
    latest_.clipboard.clear();
    Snapshot out = latest_;
    out.clipboard = std::move(clipboard);
    latest_.settled.clear();
    latest_.change_error.clear();
    latest_.added.clear();
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
        }

        std::vector<std::string> settled;
        std::string change_error;
        std::string clipboard;
        std::string added;
        // "Couldn't change <project>: <reason>". The project name keeps its
        // own direction; the service's reason is English (left to right).
        auto change_failed = [this](const std::string& project, const std::string& reason, bool english) {
            const bool rtl = is_rtl(lang_);
            return fill_pattern(text(lang_, Str::ChangeFailed), false,
                                {rtl ? isolate_auto(project) : project, rtl && english ? isolate_ltr(reason) : reason});
        };
        const std::string lost(text(lang_, Str::ConnectionLost));
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
                        change_error = fill(lang_, Str::OpenFailed, {"FloCMS Studio"});
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
            if (!databases) {
                client.reset();
            }
            ++polls;
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
        if (databases) {
            latest_.link = Link::Connected;
            latest_.error = list->ok ? std::string() : list->error_message;
            latest_.has_list = list->ok;
            latest_.rows = list->ok ? rows_from_list(list->result, lang_) : std::vector<ProjectRow>{};
            latest_.footer = footer_text(latest_.rows, status->ok ? &status->result : nullptr,
                                         web->ok ? &web->result : nullptr, lang_);
            latest_.databases = databases->ok ? databases->result : nlohmann::json();
            if (installed && installed->ok) {
                latest_.installed = installed_from_list(installed->result);
            }
        } else if (latest_.link == Link::Connected) {
            // It was answering a moment ago: keep the last list on screen.
            latest_.link = Link::Lost;
        }
        publish();
    }
}

}  // namespace floserver::ui

#include "service_worker.h"

#include "floserver/core/ipc.h"
#include "floserver/core/service_client.h"

#include <chrono>
#include <optional>
#include <utility>

namespace floserver::ui {

namespace {

namespace core = floserver::core;
using namespace std::chrono_literals;

constexpr auto kPollEvery = 2s;
// A request that gets no answer in this time counts as a lost connection.
// projects.set waits until nginx serves the change, so it's generous; the
// window never waits for it, only this thread does.
constexpr auto kRequestTimeout = 30s;

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
    push({Command::Kind::SetOn, project, on});
}

void ServiceWorker::start_service() { push({Command::Kind::Start, {}, false}); }
void ServiceWorker::refresh() { push({Command::Kind::Refresh, {}, false}); }
void ServiceWorker::quit_service() { push({Command::Kind::Quit, {}, false}); }

Snapshot ServiceWorker::take() {
    const std::lock_guard lock(mutex_);
    posted_ = false;
    Snapshot out = latest_;
    latest_.settled.clear();
    latest_.change_error.clear();
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
        // "Couldn't change <project>: <reason>". The project name keeps its
        // own direction; the service's reason is English (left to right).
        auto change_failed = [this](const std::string& project, const std::string& reason, bool english) {
            const bool rtl = is_rtl(lang_);
            return fill_pattern(text(lang_, Str::ChangeFailed), false,
                                {rtl ? isolate_auto(project) : project, rtl && english ? isolate_ltr(reason) : reason});
        };
        for (const Command& c : batch) {
            if (c.kind != Command::Kind::SetOn) {
                continue;
            }
            settled.push_back(c.project);
            if (!client) {
                // Never sent: the switch goes back, and the user is told why.
                change_error = change_failed(c.project, std::string(text(lang_, Str::ConnectionLost)), false);
                continue;
            }
            const auto reply = client->call("projects.set", {{"name", c.project}, {"on", c.on}});
            if (!reply) {
                client.reset();
                change_error = change_failed(c.project, std::string(text(lang_, Str::ConnectionLost)), false);
            } else if (!reply->ok) {
                change_error = change_failed(c.project, reply->error_message, true);
            }
        }

        std::optional<core::protocol::Reply> list;
        std::optional<core::protocol::Reply> status;
        std::optional<core::protocol::Reply> web;
        if (client) {
            list = client->call("projects.list");
            status = list ? client->call("status") : std::nullopt;
            web = status ? client->call("web.status") : std::nullopt;
            if (!web) {
                client.reset();
            }
        }

        const std::lock_guard lock(mutex_);
        latest_.settled.insert(latest_.settled.end(), settled.begin(), settled.end());
        if (!change_error.empty()) {
            latest_.change_error = change_error;
        }
        if (web) {
            latest_.link = Link::Connected;
            latest_.error = list->ok ? std::string() : list->error_message;
            latest_.has_list = list->ok;
            latest_.rows = list->ok ? rows_from_list(list->result, lang_) : std::vector<ProjectRow>{};
            latest_.footer = footer_text(latest_.rows, status->ok ? &status->result : nullptr,
                                         web->ok ? &web->result : nullptr, lang_);
        } else if (latest_.link == Link::Connected) {
            // It was answering a moment ago: keep the last list on screen.
            latest_.link = Link::Lost;
        }
        publish();
    }
}

}  // namespace floserver::ui

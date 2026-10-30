#pragma once

// The service's runtime work (D5, D8): the runtime list, installs and
// removals. Kept in core, apart from floserverd, so tests can run it with a
// fake transport and their own signing key.
//
// Installs are jobs: the request returns a job id at once and the work runs
// on one background thread, one install at a time. Callers poll job() for
// progress (the channel is request/reply, and a 95 MB download takes longer
// than any request may).

#include "floserver/core/downloader.h"
#include "floserver/core/manifest.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace floserver::core {

struct RuntimeServiceConfig {
    std::filesystem::path data_dir;
    std::string platform;
    std::vector<TrustedKey> keys;
    ManifestSource source;
    // Visual C++ runtime DLLs to copy into each install; empty for none.
    std::filesystem::path vc_runtime_dir;
    // The clock and the wait between attempts; tests replace them.
    std::function<std::chrono::sys_seconds()> now;
    std::function<void(std::chrono::milliseconds)> sleep;
    std::chrono::milliseconds retry_delay{3000};
    // A list fetched less than this long ago is used without asking again.
    std::chrono::minutes list_max_age{60};
    // The templates used when no signed list with templates can be had
    // (offline, first run): a JSON array, normally kBuiltInTemplatesJson.
    std::string builtin_templates_json = "[]";
};

// Why a request was refused; the codes go out on the channel.
struct ServiceRefusal {
    std::string code;     // "bad_params", "downloads_unavailable", "not_installed"...
    std::string message;  // plain English
};

class RuntimeService {
public:
    // `transport` may be null where there's none yet (D3): installs are then refused.
    RuntimeService(RuntimeServiceConfig config, std::shared_ptr<HttpTransport> transport);
    RuntimeService(const RuntimeService&) = delete;
    RuntimeService& operator=(const RuntimeService&) = delete;
    RuntimeService(RuntimeService&&) = delete;
    RuntimeService& operator=(RuntimeService&&) = delete;
    // Cancels the running install and waits for the worker to finish.
    ~RuntimeService();

    // Cancels every job and any list fetch without waiting. Safe from any
    // thread; for a process about to exit.
    void stop();

    // Installed runtimes and the ones the list offers for this platform.
    // With `refresh`, the list is fetched again even when it's fresh. With
    // `cached_only` (and no `refresh`), nothing goes online: the list in
    // hand however old, else the last signed list kept on disk.
    [[nodiscard]] nlohmann::json list(bool refresh, bool installed_only = false, bool cached_only = false);

    // The project templates (D9): the signed list's, fetching it when there's
    // no fresh copy. Offline (or with `fetch` false, so a project job never
    // waits on the network at quit) the last signed list kept on disk is used,
    // else the templates built into FloServer. Nothing (with `error`) only
    // when none of those has any.
    [[nodiscard]] std::optional<std::vector<ProjectTemplate>> templates(std::string& error, bool fetch = true);

    // Queues an install of the newest `name` matching `version` ("8.5",
    // "8.5.12", or empty for the newest). Returns the job id; the same request
    // while one is still waiting or running returns that job's id.
    [[nodiscard]] std::optional<std::int64_t> install(const std::string& name, const std::string& version,
                                                      ServiceRefusal& refusal);

    // A job's state, or nothing for an unknown id:
    //   {"id", "kind", "name", "requested", "version", "state", "received", "total", "message"}
    // state: queued, checking, downloading, installing, done, failed, cancelled.
    [[nodiscard]] std::optional<nlohmann::json> job(std::int64_t id) const;

    // Asks a waiting or running job to stop. False for an unknown or finished job.
    bool cancel(std::int64_t id);

    // Removes an installed version: `version` is the folder name ("8.5.12").
    [[nodiscard]] bool remove(const std::string& name, const std::string& version, ServiceRefusal& refusal);

private:
    struct Job;
    struct CachedList {
        Manifest manifest;
        std::string key_id;
        std::vector<std::string> warnings;
        std::chrono::steady_clock::time_point fetched;
    };

    // The list, fetched again when missing, stale or asked to. Nothing (with
    // `error`) when it can't be had.
    std::optional<CachedList> get_list(bool refresh, std::string& error);
    // The templates without the network: the kept list's, else the built-in ones.
    std::optional<std::vector<ProjectTemplate>> offline_templates(std::string& error);
    // The kept list's bytes opened and checked (an expired one still opens).
    [[nodiscard]] std::optional<Manifest> open_kept_list(const std::optional<std::string>& list,
                                                         const std::optional<std::string>& sig) const;
    [[nodiscard]] std::filesystem::path runtimes_root() const;
    void worker();
    void run(Job& job);
    void set_state(Job& job, const char* state, std::string message = {});
    [[nodiscard]] static nlohmann::json describe(const Job& job);

    RuntimeServiceConfig config_;
    std::shared_ptr<HttpTransport> transport_;

    std::mutex list_mutex_;  // guards list_ and fetching_; not held during network I/O
    std::condition_variable list_changed_;
    std::optional<CachedList> list_;
    bool fetching_ = false;
    std::mutex offline_mutex_;  // guards offline_
    std::optional<std::vector<ProjectTemplate>> offline_;
    std::atomic<bool> stop_requested_{false};

    mutable std::mutex jobs_mutex_;
    std::condition_variable jobs_changed_;
    std::map<std::int64_t, std::shared_ptr<Job>> jobs_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::int64_t next_job_ = 1;
    bool stopping_ = false;
    bool worker_started_ = false;
    std::thread worker_;
};

}  // namespace floserver::core

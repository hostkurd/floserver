#pragma once

// App updates (Q5, approved 5 Nov): the service's side. The signed runtime
// list (manifest.h) may name the newest Windows app ("app"). Once a little
// after the service starts, then every 24 hours, and whenever the window asks
// (updates.check), the list is read again; when it offers a newer version
// than this one (kVersion), its MSI is downloaded in the background to
// <data folder>/updates/ by the runtimes' own downloader: a .part file while
// it arrives, kept only at the list's exact size and SHA-256. The ready file
// stays for the window's "Update now" (the window installs it, never the
// service); any other file there is stale and goes.
//
// A cancelled download isn't started again by itself: only the next daily
// check or updates.check starts it. A check that can't read the list keeps
// an update that's ready: it was checked against a signed list already. An
// installer found in the folder at start is trusted only once a check has
// read the list again (and it matches that list's size and SHA-256).
//
// Kept in core, apart from floserverd, so tests run it with a fake
// transport and their own list.

#include "floserver/core/downloader.h"
#include "floserver/core/http.h"
#include "floserver/core/manifest.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace floserver::core {

struct UpdateServiceConfig {
    std::filesystem::path data_dir;
    std::string current_version;  // kVersion
    std::string platform{kAppPlatform};
    // Reads the signed list again (RuntimeService::fresh_list); nothing, with
    // `error`, when it can't be had.
    std::function<std::optional<Manifest>(std::string& error)> fetch_list;
    // The clock for last_checked; tests replace it.
    std::function<std::chrono::sys_seconds()> now;
    // The first check after start(), then the next ones.
    std::chrono::milliseconds first_delay{std::chrono::minutes{1}};
    std::chrono::milliseconds every{std::chrono::hours{24}};
};

class UpdateService {
public:
    // `transport` may be null where there's none yet (D3): status() then
    // says updates aren't available here, and nothing is ever checked.
    UpdateService(UpdateServiceConfig config, std::shared_ptr<HttpTransport> transport);
    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;
    UpdateService(UpdateService&&) = delete;
    UpdateService& operator=(UpdateService&&) = delete;
    // Stops a download and waits for the thread.
    ~UpdateService();

    // Starts the daily checks (the first one after `first_delay`).
    void start();
    // Stops a download and the checks without waiting. Safe from any thread.
    void stop();

    // updates.status:
    //   {"state": "idle" | "checking" | "downloading" | "ready" | "failed",
    //    "available": bool,              // false: no updates on this platform
    //    "current": "0.9.0",
    //    "version"?, "size"?, "sha256"?, "notes_url"?   // the newer app the list offers
    //    "done"?,                        // bytes downloaded (downloading, ready)
    //    "file"?,                        // the checked MSI (ready)
    //    "last_checked"?: "2026-11-05T09:12:00Z",       // the last check that read the list
    //    "error"?}                       // failed: why, plain English
    [[nodiscard]] nlohmann::json status() const;

    // updates.check: a check now (and the download, when there's a newer
    // version). False with `error` when updates aren't available here.
    bool check(std::string& error);

    // updates.cancel: stops the download. False when none is running.
    bool cancel();

    // The installer's name in the updates folder: FloServer-<version>-x64.msi.
    [[nodiscard]] static std::string file_name(const std::string& version);

private:
    enum class State : std::uint8_t { Idle, Checking, Downloading, Ready, Failed };

    void run();
    void check_now();
    // Deletes everything in the updates folder but `keep` (an empty name keeps nothing).
    void remove_stale(const std::string& keep) const;
    void set_failed(std::string message);

    UpdateServiceConfig config_;
    std::shared_ptr<HttpTransport> transport_;

    mutable std::mutex mutex_;  // guards everything below but the thread
    std::condition_variable wake_;
    State state_ = State::Idle;
    std::optional<AppRelease> offered_;  // the newer app the last check found
    std::uint64_t done_ = 0;
    std::filesystem::path file_;  // ready: the checked MSI
    std::optional<std::chrono::sys_seconds> last_checked_;
    std::string error_;
    bool check_wanted_ = false;
    bool stopping_ = false;
    std::atomic<bool> cancel_{false};
    std::thread thread_;
};

}  // namespace floserver::core

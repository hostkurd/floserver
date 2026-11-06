#include "floserver/core/update_service.h"

#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/version.h"

#include <array>
#include <cstdio>
#include <exception>
#include <system_error>
#include <utility>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::chrono::sys_seconds system_now() {
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

std::string utc_text(std::chrono::sys_seconds t) {
    const auto days = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day ymd{days};
    const std::chrono::hh_mm_ss hms{t - days};
    std::array<char, 32> text{};
    const int n = std::snprintf(text.data(), text.size(), "%04d-%02u-%02uT%02d:%02d:%02dZ",
                                static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                                static_cast<unsigned>(ymd.day()), static_cast<int>(hms.hours().count()),
                                static_cast<int>(hms.minutes().count()),
                                static_cast<int>(hms.seconds().count()));
    return {text.data(), n > 0 ? static_cast<std::size_t>(n) : 0};
}

const char* state_name(int state) {
    static constexpr std::array<const char*, 5> kNames = {"idle", "checking", "downloading", "ready", "failed"};
    return kNames.at(static_cast<std::size_t>(state));
}

}  // namespace

UpdateService::UpdateService(UpdateServiceConfig config, std::shared_ptr<HttpTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {
    if (!config_.now) {
        config_.now = system_now;
    }
    // No download can be running yet: one cut off mid-way goes.
    std::error_code ec;
    for (fs::directory_iterator it(updates_dir(config_.data_dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".part") {
            std::error_code ignored;
            fs::remove(it->path(), ignored);
        }
    }
}

UpdateService::~UpdateService() {
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::string UpdateService::file_name(const std::string& version) { return "FloServer-" + version + "-x64.msi"; }

void UpdateService::start() {
    if (!transport_ || !config_.fetch_list) {
        return;
    }
    const std::lock_guard lock(mutex_);
    if (thread_.joinable() || stopping_) {
        return;
    }
    thread_ = std::thread([this] { run(); });
}

void UpdateService::stop() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        cancel_ = true;
    }
    wake_.notify_all();
}

bool UpdateService::check(std::string& error) {
    if (!transport_ || !config_.fetch_list) {
        error = "updates aren't available on this platform yet";
        return false;
    }
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) {
            error = "FloServer is stopping";
            return false;
        }
        // One already on its way answers this one too.
        if (state_ != State::Checking && state_ != State::Downloading) {
            check_wanted_ = true;
        }
    }
    wake_.notify_all();
    return true;
}

bool UpdateService::cancel() {
    const std::lock_guard lock(mutex_);
    if (state_ != State::Downloading) {
        return false;
    }
    cancel_ = true;
    return true;
}

json UpdateService::status() const {
    const std::lock_guard lock(mutex_);
    json out = {{"state", state_name(static_cast<int>(state_))},
                {"available", transport_ != nullptr && config_.fetch_list != nullptr},
                {"current", config_.current_version}};
    if (offered_) {
        out["version"] = offered_->version;
        out["size"] = offered_->size;
        out["sha256"] = to_hex(offered_->sha256);
        if (!offered_->notes_url.empty()) {
            out["notes_url"] = offered_->notes_url;
        }
        if (state_ == State::Downloading || state_ == State::Ready) {
            out["done"] = done_;
        }
        if (state_ == State::Ready) {
            out["file"] = path_to_utf8(file_);
        }
    }
    if (last_checked_) {
        out["last_checked"] = utc_text(*last_checked_);
    }
    if (!transport_ || !config_.fetch_list) {
        out["state"] = "failed";
        out["error"] = "updates aren't available on this platform yet";
    } else if (state_ == State::Failed) {
        out["error"] = error_;
    }
    return out;
}

void UpdateService::run() {
    auto next = std::chrono::steady_clock::now() + config_.first_delay;
    std::unique_lock lock(mutex_);
    while (true) {
        wake_.wait_until(lock, next, [this] { return stopping_ || check_wanted_; });
        if (stopping_) {
            return;
        }
        // Asked or due: the next daily check counts from this one.
        check_wanted_ = false;
        next = std::chrono::steady_clock::now() + config_.every;
        lock.unlock();
        try {
            check_now();
        } catch (const std::exception& e) {
            set_failed(std::string("unexpected error: ") + e.what());
        } catch (...) {
            set_failed("unexpected error");
        }
        lock.lock();
    }
}

void UpdateService::set_failed(std::string message) {
    const std::lock_guard lock(mutex_);
    state_ = State::Failed;
    error_ = std::move(message);
    done_ = 0;
    file_.clear();
}

void UpdateService::remove_stale(const std::string& keep) const {
    std::error_code ec;
    for (fs::directory_iterator it(updates_dir(config_.data_dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (keep.empty() || it->path().filename() != path_from_utf8(keep)) {
            std::error_code ignored;
            fs::remove_all(it->path(), ignored);
        }
    }
}

void UpdateService::check_now() {
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        state_ = State::Checking;
        error_.clear();
        cancel_ = false;
    }
    std::string error;
    const std::optional<Manifest> list = config_.fetch_list(error);
    if (!list) {
        set_failed(error.empty() ? std::string("the runtime list can't be read") : error);
        return;
    }
    const auto current = Version::parse(config_.current_version);
    std::optional<AppRelease> app = list->app;
    const auto offered = app ? Version::parse(app->version) : std::nullopt;
    const bool newer = app && app->platform == config_.platform && current && offered && *offered > *current;
    if (!newer) {
        // Nothing newer: an installer kept from before is this version's or older.
        remove_stale({});
        const std::lock_guard lock(mutex_);
        last_checked_ = config_.now();
        state_ = State::Idle;
        offered_.reset();
        done_ = 0;
        file_.clear();
        return;
    }
    const std::string name = file_name(app->version);
    remove_stale(name);
    const fs::path target = updates_dir(config_.data_dir) / path_from_utf8(name);
    {
        const std::lock_guard lock(mutex_);
        last_checked_ = config_.now();
        offered_ = *app;
        state_ = State::Downloading;
        done_ = 0;
        file_.clear();
    }
    DownloadOptions options;
    options.progress = [this](const DownloadProgress& p) {
        const std::lock_guard lock(mutex_);
        done_ = p.received;
        return !cancel_.load();
    };
    options.cancel = &cancel_;
    // A file already there at the list's size and hash is kept as it is.
    const DownloadResult downloaded = download_file(*transport_, app_download_entry(*app), target, options);
    const std::lock_guard lock(mutex_);
    if (downloaded.cancelled || cancel_) {
        // The .part went with it; nothing starts again until the next check.
        state_ = State::Idle;
        done_ = 0;
        return;
    }
    if (!downloaded.ok) {
        state_ = State::Failed;
        error_ = downloaded.message;
        done_ = 0;
        return;
    }
    state_ = State::Ready;
    done_ = app->size;
    file_ = target;
}

}  // namespace floserver::core

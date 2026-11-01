#pragma once

// Makes new projects from templates (D9), one job each, on their own
// threads. A job:
//   1. checks the name and that the folder is new (or empty),
//   2. installs every runtime the project will need that isn't installed,
//   3. makes the folder, copies the template's runtime files (WordPress),
//      writes its starter files and runs its steps (composer create-project,
//      npm ...) with the project's own runtimes, showing each command and
//      what it prints,
//   4. writes floserver.json, adds the project and turns it on,
//   5. fills in the database settings (.env, wp-config.php).
// What the service needs from its other parts comes in as hooks, so the
// rules here are tested without a network or a database.

#include "floserver/core/mariadb.h"
#include "floserver/core/runtime_service.h"
#include "floserver/core/templates.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
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

struct TemplateHooks {
    // The template with this id from the current list.
    std::function<std::optional<ProjectTemplate>(const std::string& id, std::string& error)> find_template;
    // Installs `name` matching `version` ("" = newest) unless it's there,
    // and waits; false (with `error`) when it can't. `progress` gets lines
    // such as "Downloading PHP 8.5.12… 42%" while it runs.
    std::function<bool(const std::string& name, const std::string& version, const std::atomic<bool>& cancel,
                       const std::function<void(const std::string&)>& progress, std::string& error)>
        install;
    // Registers the folder as a project, turned on; its name, or nothing.
    std::function<std::optional<std::string>(const std::filesystem::path& dir, const std::string& name,
                                             std::string& error)>
        add_project;
    // The project's database login (made the first time).
    std::function<std::optional<DatabaseLogin>(const std::string& project, std::string& error)> database_login;
};

struct CreateRequest {
    std::string template_id;
    std::string name;                    // the project's and the folder's name
    std::filesystem::path parent;        // the folder it's made in (absolute, existing)
    // Changes to the template's floserver.json: "php", "node", "python",
    // "database", "web"; nothing removes the key.
    std::map<std::string, std::optional<std::string>> versions;
};

class TemplateService {
public:
    TemplateService(std::filesystem::path data_dir, TemplateHooks hooks);
    ~TemplateService();
    TemplateService(const TemplateService&) = delete;
    TemplateService& operator=(const TemplateService&) = delete;
    TemplateService(TemplateService&&) = delete;
    TemplateService& operator=(TemplateService&&) = delete;

    // Starts a job; its id, or nothing (with `refusal`) when the request is
    // refused before anything is made.
    [[nodiscard]] std::optional<std::int64_t> create(const CreateRequest& request, ServiceRefusal& refusal);

    // {"id", "state": "running" | "done" | "failed" | "cancelled", "step": "...",
    //  "log": ["$ composer create-project ...", "..."], "project"?: "...",
    //  "folder"?: "...", "error"?: "..."}
    [[nodiscard]] std::optional<nlohmann::json> job(std::int64_t id) const;
    bool cancel(std::int64_t id);
    // Whether a project is being made (its work folder is in use).
    [[nodiscard]] bool busy() const;
    // Cancels every job and waits for them.
    void stop();

    // The most lines a job keeps (the oldest go first).
    static constexpr std::size_t kLogLines = 400;

private:
    struct Job;
    void run(const std::shared_ptr<Job>& job, const CreateRequest& request);

    std::filesystem::path data_dir_;
    TemplateHooks hooks_;
    mutable std::mutex mutex_;
    std::map<std::int64_t, std::shared_ptr<Job>> jobs_;  // each holds its thread
    std::int64_t next_id_ = 1;
    bool stopping_ = false;
};

// wp-config-sample.php with the database login and new keys and salts, or
// nothing when the sample doesn't have the lines WordPress has always had.
[[nodiscard]] std::optional<std::string> make_wp_config(const std::string& sample, const DatabaseLogin& login);

}  // namespace floserver::core

#include "floserver/core/template_service.h"

#include "floserver/core/phpmyadmin.h"

#include "floserver/core/child_process.h"
#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/secret_store.h"
#include "floserver/core/shim.h"
#include "floserver/core/site_plan.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <thread>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr auto kStepTimeLimit = 20min;
constexpr std::size_t kMaxRunningJobs = 2;

const char* runtime_title(const std::string& name) {
    if (name == "php") {
        return "PHP";
    }
    if (name == "mariadb") {
        return "MariaDB";
    }
    if (name == "node") {
        return "Node.js";
    }
    if (name == "python") {
        return "Python";
    }
    if (name == "composer") {
        return "Composer";
    }
    if (name == "apache") {
        return "Apache";
    }
    if (name == "wordpress") {
        return "WordPress";
    }
    return nullptr;
}

std::string titled(const std::string& name, const std::string& version) {
    const char* t = runtime_title(name);
    std::string out = t != nullptr ? t : name;
    if (!version.empty()) {
        out.append(" ").append(version);
    }
    return out;
}

// A word in a shown command: quoted when it has a space.
std::string shown_arg(const std::string& a) { return a.find(' ') == std::string::npos ? a : "\"" + a + "\""; }

// A new project's folder must not be there, or be a real, empty folder: a
// link or junction there would send every file somewhere else.
bool folder_is_free(const fs::path& dir) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(dir, ec);
    if (status.type() == fs::file_type::not_found) {
        return true;
    }
    return !ec && status.type() == fs::file_type::directory && fs::is_empty(dir, ec) && !ec;
}

// The same folder, by Windows' rules on Windows (case doesn't count).
bool same_folder(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
               return lower(x) == lower(y);
           });
#else
    return a == b;
#endif
}

constexpr std::size_t kMaxPending = std::size_t{8} * 1024;
constexpr std::size_t kMaxLine = 1000;
constexpr std::size_t kKeptFinishedJobs = 8;

// Copies a runtime's files into the new project, leaving out FloServer's marker.
bool copy_runtime(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path rel = it->path().lexically_relative(from);
        if (rel == fs::path(std::string(kRuntimeMarker))) {
            continue;
        }
        const fs::path target = to / rel;
        if (it->is_symlink(ec)) {
            continue;  // never in our installs; never followed
        }
        if (it->is_directory(ec)) {
            fs::create_directories(target, ec);
        } else if (it->is_regular_file(ec)) {
            fs::create_directories(target.parent_path(), ec);
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            break;
        }
    }
    if (ec) {
        error = "couldn't copy the files: " + error_text(ec);
        return false;
    }
    return true;
}

// 64 letters and digits (about 380 bits), safe inside a PHP single-quoted string.
std::string random_salt() { return generate_password(64).value_or(std::string()); }

}  // namespace

std::optional<std::string> make_wp_config(const std::string& sample, const DatabaseLogin& login) {
    std::string text = sample;
    const auto replace_once = [&text](const std::string& from, const std::string& to) {
        const std::size_t at = text.find(from);
        if (at == std::string::npos) {
            return false;
        }
        text.replace(at, from.size(), to);
        return true;
    };
    if (!replace_once("'database_name_here'", "'" + login.database + "'") ||
        !replace_once("'username_here'", "'" + login.user + "'") ||
        !replace_once("'password_here'", "'" + login.password + "'") ||
        !replace_once("'DB_HOST', 'localhost'", "'DB_HOST', '127.0.0.1:" + std::to_string(login.port) + "'")) {
        return std::nullopt;
    }
    int salts = 0;
    while (true) {
        const std::string salt = random_salt();
        if (salt.empty()) {
            return std::nullopt;
        }
        if (!replace_once("'put your unique phrase here'", "'" + salt + "'")) {
            break;
        }
        ++salts;
    }
    if (salts == 0) {
        return std::nullopt;
    }
    return text;
}

struct TemplateService::Job {
    std::int64_t id = 0;
    std::thread thread;
    std::atomic<bool> cancel{false};
    mutable std::mutex mutex;
    std::string state = "running";
    std::string step;
    std::deque<std::string> log;
    std::string project;
    std::string folder;
    std::string error;

    void add_line(const std::string& line) {
        const std::lock_guard lock(mutex);
        log.push_back(line);
        while (log.size() > kLogLines) {
            log.pop_front();
        }
    }
    void set_step(const std::string& s) {
        const std::lock_guard lock(mutex);
        step = s;
        log.push_back(s);
        while (log.size() > kLogLines) {
            log.pop_front();
        }
    }
    void finish(const std::string& final_state, const std::string& message) {
        const std::lock_guard lock(mutex);
        state = final_state;
        error = message;
    }
};

TemplateService::TemplateService(fs::path data_dir, TemplateHooks hooks)
    : data_dir_(std::move(data_dir)), hooks_(std::move(hooks)) {}

TemplateService::~TemplateService() { stop(); }

std::optional<std::int64_t> TemplateService::create(const CreateRequest& request, ServiceRefusal& refusal) {
    if (const auto problem = project_name_problem(request.name)) {
        refusal = {"bad_name", "the name " + request.name + " can't be used: " + *problem};
        return std::nullopt;
    }
    if (reserved_project_name(request.name)) {
        refusal = {"bad_name", request.name + ".localhost is kept for phpMyAdmin; choose another name"};
        return std::nullopt;
    }
    std::error_code ec;
    // Local folders only: a \\server\share path would make Windows sign in there.
    const std::string parent_text = path_to_utf8(request.parent);
    if (!request.parent.is_absolute() || parent_text.starts_with("\\\\") || parent_text.starts_with("//") ||
        !fs::is_directory(request.parent, ec)) {
        refusal = {"bad_folder", "the folder to make it in must be an existing full path on this computer"};
        return std::nullopt;
    }
    const fs::path dir = request.parent / path_from_utf8(request.name);
    if (!folder_is_free(dir)) {
        refusal = {"folder_exists", path_to_utf8(dir) + " already exists and isn't an empty folder"};
        return std::nullopt;
    }
    for (const auto& [key, value] : request.versions) {
        if (key != "php" && key != "node" && key != "python" && key != "database" && key != "web") {
            refusal = {"bad_params", "versions take php, node, python, database and web"};
            return std::nullopt;
        }
    }
    const std::lock_guard lock(mutex_);
    if (stopping_) {
        refusal = {"stopping", "FloServer is stopping"};
        return std::nullopt;
    }
    if (cleaning_) {
        refusal = {"busy", "FloServer is removing leftover downloads; try again in a moment"};
        return std::nullopt;
    }
    const std::string folder = path_to_utf8(dir);
    std::size_t running = 0;
    std::vector<std::int64_t> finished;
    for (const auto& [id, j] : jobs_) {
        const std::lock_guard job_lock(j->mutex);
        if (j->state == "running") {
            ++running;
            if (same_folder(j->folder, folder)) {
                refusal = {"folder_exists", folder + " is already being made"};
                return std::nullopt;
            }
        } else {
            finished.push_back(id);
        }
    }
    if (running >= kMaxRunningJobs) {
        refusal = {"busy", "a new project is still being made; try again when it's done"};
        return std::nullopt;
    }
    // Finished jobs beyond the last few go; their threads have ended.
    while (finished.size() > kKeptFinishedJobs) {
        const auto it = jobs_.find(finished.front());
        if (it->second->thread.joinable()) {
            it->second->thread.join();
        }
        jobs_.erase(it);
        finished.erase(finished.begin());
    }
    auto job = std::make_shared<Job>();
    job->id = next_id_++;
    job->folder = folder;
    try {
        job->thread = std::thread([this, job, request] {
            try {
                run(job, request);
            } catch (const std::exception& e) {
                job->finish("failed", std::string("something went wrong: ") + e.what());
            } catch (...) {
                job->finish("failed", "something went wrong");
            }
        });
    } catch (const std::system_error&) {
        refusal = {"busy", "FloServer couldn't start the job; try again"};
        return std::nullopt;
    }
    jobs_[job->id] = job;
    return job->id;
}

bool TemplateService::begin_cleanup() {
    const std::lock_guard lock(mutex_);
    const bool running = std::any_of(jobs_.begin(), jobs_.end(), [](const auto& entry) {
        const std::lock_guard job_lock(entry.second->mutex);
        return entry.second->state == "running";
    });
    if (running || cleaning_) {
        return false;
    }
    cleaning_ = true;
    return true;
}

void TemplateService::end_cleanup() {
    const std::lock_guard lock(mutex_);
    cleaning_ = false;
}

bool TemplateService::busy() const {
    const std::lock_guard lock(mutex_);
    return std::any_of(jobs_.begin(), jobs_.end(), [](const auto& entry) {
        const std::lock_guard job_lock(entry.second->mutex);
        return entry.second->state == "running";
    });
}

std::optional<json> TemplateService::job(std::int64_t id) const {
    std::shared_ptr<Job> j;
    {
        const std::lock_guard lock(mutex_);
        const auto it = jobs_.find(id);
        if (it == jobs_.end()) {
            return std::nullopt;
        }
        j = it->second;
    }
    const std::lock_guard lock(j->mutex);
    json out = {{"id", j->id}, {"state", j->state}, {"step", j->step}, {"log", j->log}, {"folder", j->folder}};
    if (!j->project.empty()) {
        out["project"] = j->project;
    }
    if (!j->error.empty()) {
        out["error"] = j->error;
    }
    return out;
}

bool TemplateService::cancel(std::int64_t id) {
    const std::lock_guard lock(mutex_);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        return false;
    }
    const std::lock_guard job_lock(it->second->mutex);
    if (it->second->state != "running") {
        return false;
    }
    it->second->cancel = true;
    return true;
}

void TemplateService::stop() {
    std::vector<std::shared_ptr<Job>> jobs;
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& [id, j] : jobs_) {
            j->cancel = true;
            jobs.push_back(j);
        }
    }
    for (const auto& j : jobs) {
        if (j->thread.joinable()) {
            j->thread.join();
        }
    }
}

void TemplateService::run(const std::shared_ptr<Job>& job, const CreateRequest& request) {
    const auto fail = [&job](const std::string& message) { job->finish("failed", message); };
    const auto cancelled = [&job]() {
        if (job->cancel) {
            job->finish("cancelled", "cancelled");
            return true;
        }
        return false;
    };

    std::string error;
    const auto found = hooks_.find_template ? hooks_.find_template(request.template_id, error) : std::nullopt;
    if (!found) {
        fail(error.empty() ? "there's no template called " + request.template_id : error);
        return;
    }
    const ProjectTemplate& t = *found;

    // 1. floserver.json: the template's, with the user's versions.
    std::string config_text = t.config.dump(2) + "\n";
    for (const auto& [key, value] : request.versions) {
        const auto parsed_key = parse_config_key(key);
        if (!parsed_key) {
            continue;
        }
        const auto next = set_project_config_value(config_text, *parsed_key, key, value, error);
        if (!next) {
            fail(error);
            return;
        }
        config_text = *next;
    }
    const ConfigResult config = parse_project_config(config_text);
    if (!config.config) {
        fail("the project's settings can't be used");
        return;
    }
    const ProjectConfig& pc = *config.config;

    // 2. Every runtime it will need. A PHP project without a version gets the
    // newest; a version that isn't installed is downloaded.
    const std::vector<TemplateNeed> needs = template_runtime_needs(t, pc);
    for (const TemplateNeed& n : needs) {
        if (cancelled()) {
            return;
        }
        if (best_installed(list_installed(runtimes_dir(data_dir_)), n.name, n.version) != nullptr) {
            continue;
        }
        job->set_step("Installing " + titled(n.name, n.version) + "…");
        if (!hooks_.install || !hooks_.install(n.name, n.version, job->cancel,
                                               [&job](const std::string& line) {
                                                   const std::lock_guard lock(job->mutex);
                                                   job->step = line;
                                               },
                                               error)) {
            if (!cancelled()) {
                fail("couldn't install " + titled(n.name, n.version) + ": " + error);
            }
            return;
        }
    }

    // 3. The folder and its files.
    const fs::path dir = request.parent / path_from_utf8(request.name);
    const std::string dir_text = path_to_utf8(dir);
    std::error_code ec;
    if (!folder_is_free(dir)) {
        fail(dir_text + " already exists and isn't an empty folder");
        return;
    }
    job->set_step("Making " + dir_text);
    (void)fs::create_directory(dir, ec);
    if (ec || !folder_is_free(dir)) {
        fail("couldn't make " + dir_text + (ec ? ": " + error_text(ec) : std::string()));
        return;
    }
    std::string left_note = " The folder ";
    left_note.append(dir_text).append(" is left as it is.");
    // A failure once the folder exists: the message, then that note.
    const auto fail_left = [&](std::string message, const char* end = ".") {
        message.append(end).append(left_note);
        fail(message);
    };
    if (!t.copy.empty()) {
        std::string wanted;
        for (const TemplateNeed& n : needs) {
            if (n.name == t.copy) {
                wanted = n.version;
            }
        }
        const auto installed = list_installed(runtimes_dir(data_dir_));
        const InstalledRuntime* source = best_installed(installed, t.copy, wanted);
        if (source == nullptr) {
            fail_left(titled(t.copy, wanted) + " isn't installed", ".");
            return;
        }
        job->set_step("Copying " + titled(source->name, source->version) + "…");
        if (!copy_runtime(source->dir, dir, error)) {
            fail_left(error);
            return;
        }
    }
    for (const TemplateFile& f : t.files) {
        if (!write_file_atomically(dir / path_from_utf8(f.path), f.text, error)) {
            fail_left(error);
            return;
        }
        job->add_line("Wrote " + f.path);
    }

    // 4. The steps, with the project's own runtimes. The shims pick them from
    // a floserver.json, so a work folder holds one while the project folder
    // may still have to be empty (composer create-project wants that).
    const fs::path work = data_dir_ / "templates" / ("job-" + std::to_string(job->id));
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    // The work folder goes however the job ends.
    struct RemoveWork {
        fs::path path;
        RemoveWork(const RemoveWork&) = delete;
        RemoveWork& operator=(const RemoveWork&) = delete;
        RemoveWork(RemoveWork&&) = delete;
        RemoveWork& operator=(RemoveWork&&) = delete;
        explicit RemoveWork(fs::path p) : path(std::move(p)) {}
        ~RemoveWork() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } const remove_work(work);
    if (!t.steps.empty() && !write_file_atomically(work / path_from_utf8(kProjectConfigFileName), config_text, error)) {
        fail_left(error);
        return;
    }
    for (std::size_t i = 0; i < t.steps.size(); ++i) {
        if (cancelled()) {
            return;
        }
        const TemplateStep& step = t.steps[i];
        const auto target = resolve_shim(step.tool, work, data_dir_, error);
        if (!target) {
            fail_left(step.tool + ": " + error, "");
            return;
        }
        const std::vector<std::string> args = fill_step_args(step, dir_text, request.name);
        std::string shown = "$ " + step.tool;
        for (const std::string& a : args) {
            shown += " " + shown_arg(a);
        }
        job->set_step(shown);

        ChildOptions options;
        options.program = target->program;
        options.args = target->args;
        options.args.insert(options.args.end(), args.begin(), args.end());
        options.working_dir = dir;
        options.env = target->env;
        options.env.emplace_back("COMPOSER_NO_INTERACTION", "1");
        options.env.emplace_back("NO_COLOR", "1");
        options.env.emplace_back("npm_config_color", "false");
        options.output_log = work / ("step-" + std::to_string(i + 1) + ".log");
        auto child = ChildProcess::start(options, error);
        if (!child) {
            fail_left(step.tool + " didn't start: " + error, "");
            return;
        }
        // Its output, line by line, as it comes.
        std::ifstream out;
        std::string pending;
        const auto pump = [&]() {
            if (!out.is_open()) {
                out.open(options.output_log, std::ios::binary);
            }
            if (!out.is_open()) {
                return;
            }
            out.clear();
            std::array<char, 4096> buffer{};
            while (out.read(buffer.data(), buffer.size()) || out.gcount() > 0) {
                pending.append(buffer.data(), static_cast<std::size_t>(out.gcount()));
                if (out.eof()) {
                    break;
                }
            }
            // A spinner redrawn with \r and no newline: keep only its last state.
            if (pending.size() > kMaxPending && pending.find('\n') == std::string::npos) {
                const std::size_t cr = pending.rfind('\r');
                pending.erase(0, cr == std::string::npos ? pending.size() - kMaxPending : cr + 1);
            }
            std::size_t nl = 0;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, std::min(nl, kMaxPending));
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                // A progress line redrawn with \r keeps only its last state.
                if (const std::size_t cr = line.rfind('\r'); cr != std::string::npos) {
                    line.erase(0, cr + 1);
                }
                if (line.size() > kMaxLine) {
                    line.resize(kMaxLine);
                    line += "...";
                }
                if (!line.empty()) {
                    job->add_line(line);  // replies replace bad UTF-8 when they are written
                }
            }
        };
        const auto started = std::chrono::steady_clock::now();
        std::optional<int> code;
        while (!(code = child->wait(250ms))) {
            pump();
            if (job->cancel) {
                child->kill();
                std::string message = "cancelled.";
                message.append(left_note);
                job->finish("cancelled", message);
                return;
            }
            if (std::chrono::steady_clock::now() - started > kStepTimeLimit) {
                child->kill();
                fail_left(step.tool + " ran for 20 minutes without finishing and was stopped", ".");
                return;
            }
        }
        pump();
        if (!pending.empty()) {
            job->add_line(pending.substr(0, kMaxLine));
            pending.clear();
        }
        if (*code != 0) {
            fail_left(step.tool + " ended with exit code " + std::to_string(*code));
            return;
        }
    }

    // 5. floserver.json: the template's keys over whatever the steps wrote.
    const fs::path config_file = dir / path_from_utf8(kProjectConfigFileName);
    if (fs::exists(config_file, ec)) {
        // Kept in a variable: items() of a temporary would outlive it.
        const nlohmann::ordered_json wanted = nlohmann::ordered_json::parse(config_text, nullptr, false);
        for (const auto& [key, value] : wanted.items()) {
            const auto parsed_key = parse_config_key(key);
            if (parsed_key && value.is_string() &&
                !set_project_config_file_value(dir, *parsed_key, key, value.get<std::string>(), error)) {
                fail_left(error);
                return;
            }
        }
    } else if (!write_file_atomically(config_file, config_text, error)) {
        fail_left(error);
        return;
    }

    // 6. The project itself.
    if (cancelled()) {
        return;
    }
    job->set_step("Adding the project…");
    const auto name = hooks_.add_project ? hooks_.add_project(dir, request.name, error) : std::nullopt;
    if (!name) {
        fail_left("couldn't add the project: " + error, "");
        return;
    }
    {
        const std::lock_guard lock(job->mutex);
        job->project = *name;
    }

    // 7. Its database settings.
    if (cancelled()) {
        return;
    }
    if (pc.database && (t.env || t.wordpress)) {
        job->set_step("Setting up the database…");
        const auto login = hooks_.database_login ? hooks_.database_login(*name, error) : std::nullopt;
        if (!login) {
            fail("the project was made, but its database couldn't be set up: " + error);
            return;
        }
        if (t.env) {
            std::string text = read_small_file(dir / ".env", std::size_t{1024} * 1024).value_or("");
            if (text.empty()) {
                text = read_small_file(dir / ".env.example", std::size_t{1024} * 1024).value_or("");
            }
            text = update_env_text(text, env_values(env_style(dir), *login));
            if (!write_private_file_atomically(dir / ".env", text, error)) {
                fail("the project was made, but .env couldn't be written: " + error);
                return;
            }
            job->add_line("Wrote the database lines into .env");
        }
        if (t.wordpress) {
            const auto sample = read_small_file(dir / "wp-config-sample.php", std::size_t{1024} * 1024);
            const auto wp = sample ? make_wp_config(*sample, *login) : std::nullopt;
            if (!wp || !write_private_file_atomically(dir / "wp-config.php", *wp, error)) {
                fail("the project was made, but wp-config.php couldn't be written" +
                     (error.empty() ? std::string() : ": " + error));
                return;
            }
            job->add_line("Wrote wp-config.php");
        }
    }
    job->set_step("Done");
    job->finish("done", {});
}

}  // namespace floserver::core

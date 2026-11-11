#include "floserver/core/project_service.h"

#include "floserver/core/shared_sites.h"

#include "floserver/core/database_service.h"  // kFirstInternalDatabasePort
#include "floserver/core/mailpit.h"
#include "floserver/core/mariadb.h"
#include "floserver/core/mariadb_settings.h"
#include "floserver/core/paths.h"
#include "floserver/core/php_pool.h"
#include "floserver/core/php_settings.h"
#include "floserver/core/process.h"
#include "floserver/core/site_plan.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <array>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>
#else
#include <fcntl.h>

#include <cerrno>
#include <cstdio>
#endif

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const char* state_name(SiteState s) {
    switch (s) {
        case SiteState::Off:
            return "off";
        case SiteState::Ready:
            return "ready";
        case SiteState::NeedsDownload:
            return "needs_download";
        case SiteState::Problem:
            return "problem";
    }
    return "problem";
}

json site_json(const Project& project, const PlannedSite& site, const WebPlan& plan) {
    json out = {{"name", project.name},
                {"path", path_to_utf8(project.dir)},
                {"on", project.on},
                {"keep_running", project.keep_running},
                {"state", state_name(site.state)},
                {"missing", site.missing},
                {"database_name", project_database_name(project)}};
    if (site.pool) {
        out["php"] = plan.pools.at(*site.pool).version;
    }
    if (site.apache) {
        out["web"] = "apache";
    }
    if (site.static_site) {
        out["static"] = true;
    }
    if (site.php_files) {
        out["php_files"] = true;  // PHP is off and there are .php files: they're sent as they are
    }
    if (site.app) {
        const AppSpec& app = plan.apps.at(*site.app);
        out[app.kind == AppKind::Python ? "python" : "node"] = app.version;
        if (!app.node_version.empty()) {
            out["node"] = app.node_version;
        }
    }
    if (!site.problem.empty()) {
        out["problem"] = site.problem;
    }
    if (site.database) {
        out["database"] = site.database->family + "-" + site.database->version;
    }
    // As floserver.json says it: the window's version choices start here.
    if (site.config) {
        const ProjectConfig& c = *site.config;
        json wants = {{"web", c.web == WebServer::Apache ? "apache" : "nginx"}};
        if (c.php) {
            wants["php"] = *c.php;
        } else if (c.php_choice == PhpChoice::Off) {
            wants["php"] = false;
        } else if (c.php_choice == PhpChoice::Newest) {
            wants["php"] = std::string(kPhpNewest);
        }
        if (c.node) {
            wants["node"] = *c.node;
        }
        if (c.python) {
            wants["python"] = *c.python;
        }
        if (c.database) {
            wants["database"] = c.database->family + "-" + c.database->version;
        }
        // The project's PHP changes (D20), as the file lists them.
        if (!c.extensions.empty() || !c.extensions_off.empty()) {
            json list = c.extensions;
            for (const std::string& e : c.extensions_off) {
                list.push_back("-" + e);
            }
            wants["extensions"] = std::move(list);
        }
        if (!c.php_ini.empty()) {
            wants["php_ini"] = c.php_ini;
        }
        out["wants"] = std::move(wants);
    }
    return out;
}

WebPlan plan_for(const AppConfig& config, const fs::path& data_dir) {
    return plan_sites(config, read_project_config_file, list_installed(runtimes_dir(data_dir)), std::nullopt);
}

// A project that is Off is still described with what it would need, so the
// window can show "Needs download" before it's turned on.
json describe_all(const AppConfig& config, const fs::path& data_dir) {
    AppConfig all_on = config;
    for (Project& p : all_on.projects) {
        p.on = true;
    }
    const WebPlan plan = plan_for(all_on, data_dir);
    json list = json::array();
    for (std::size_t i = 0; i < config.projects.size(); ++i) {
        json item = site_json(config.projects[i], plan.sites.at(i), plan);
        if (!config.projects[i].on) {
            item["state"] = "off";
        }
        list.push_back(std::move(item));
    }
    return list;
}

}  // namespace

ProjectService::ProjectService(fs::path data_dir) : data_dir_(std::move(data_dir)) {}

std::optional<AppConfig> ProjectService::load(ServiceRefusal& refusal) const {
    std::string error;
    auto config = load_app_config(app_config_file(data_dir_), error);
    if (!config) {
        refusal = {"config_unreadable", "FloServer's settings file can't be used: " + error};
    }
    return config;
}

bool ProjectService::save(const AppConfig& config, ServiceRefusal& refusal) const {
    std::string error;
    if (!save_app_config(app_config_file(data_dir_), config, error)) {
        refusal = {"config_unwritable", "FloServer couldn't save its settings: " + error};
        return false;
    }
    return true;
}

std::optional<AppConfig> ProjectService::config(ServiceRefusal& refusal) const {
    const std::lock_guard lock(mutex_);
    return load(refusal);
}

std::optional<AppConfig> ProjectService::change_config(const std::function<bool(AppConfig&, ServiceRefusal&)>& change,
                                                       ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config || !change(*config, refusal) || !save(*config, refusal)) {
        return std::nullopt;
    }
    return config;
}

json ProjectService::describe(const AppConfig& config, const std::string& name) const {
    const json all = describe_all(config, data_dir_);
    for (const json& item : all) {
        if (item["name"] == name) {
            return item;
        }
    }
    return json::object();
}

std::optional<json> ProjectService::list(ServiceRefusal& refusal) const {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    const auto dir = config->projects_dir ? config->projects_dir : default_projects_dir();
    return json{{"projects", describe_all(*config, data_dir_)},
                {"projects_dir", dir ? json(path_to_utf8(*dir)) : json(nullptr)}};
}

namespace {

// The folder's text as compared: '/' separators, and on Windows any case.
std::string folder_key(const fs::path& p) {
    const std::u8string generic = p.lexically_normal().generic_u8string();
    std::string text(generic.begin(), generic.end());
    while (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
#ifdef _WIN32
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
#endif
    return text;
}

bool same_or_inside(const fs::path& child, const fs::path& parent) {
    const std::string c = folder_key(child);
    const std::string p = folder_key(parent);
    return c == p || (c.size() > p.size() && c.starts_with(p) && (p.ends_with("/") || c[p.size()] == '/'));
}

// The projects folder as it's kept: as the file system names it when it's
// there (real_folder()); when it isn't yet (it's made when the first project
// goes there, or Open is pressed), its nearest existing folder so named with
// the rest as typed. Refused: a network share, a file.
std::optional<fs::path> projects_folder_to_keep(const fs::path& dir, std::string& error) {
    const std::string text = path_to_utf8(dir);
    if (text.starts_with("\\\\") || text.starts_with("//")) {
        error = text + " is on a network share; FloServer projects must be on a local drive";
        return std::nullopt;
    }
#ifdef _WIN32
    const std::wstring root = dir.root_name().native();
    if (root.size() != 2 || root[1] != L':') {
        error = text + " is on a network share; FloServer projects must be on a local drive";
        return std::nullopt;
    }
#endif
    std::error_code ec;
    const fs::path normal = dir.lexically_normal();
    if (fs::exists(normal, ec)) {
        return real_folder(normal, error);  // a file is refused there ("there's no folder")
    }
    // The nearest folder that's there, and the parts below it still to make.
    fs::path at = normal;
    std::vector<fs::path> rest;
    while (!at.empty() && at != at.root_path() && !fs::exists(at, ec)) {
        rest.push_back(at.filename());
        at = at.parent_path();
    }
    if (!fs::exists(at, ec)) {
        // Walked up to the root and nothing's there: a drive that isn't ("Z:").
        const std::string drive = path_to_utf8(normal.root_name());
        error = drive.empty() ? "there's no folder " + path_to_utf8(at) : "there's no drive " + drive;
        return std::nullopt;
    }
    if (!fs::is_directory(at, ec)) {
        error = path_to_utf8(at) + " is a file, not a folder";
        return std::nullopt;
    }
    const auto real = real_folder(at, error);
    if (!real) {
        return std::nullopt;
    }
    fs::path out = *real;
    for (const fs::path& part : std::ranges::reverse_view(rest)) {
        out /= part;
    }
    return out;
}

json describe_projects_folder(const AppConfig& config) {
    const auto fallback = default_projects_dir();
    const auto dir = config.projects_dir ? config.projects_dir : fallback;
    return {{"projects_dir", dir ? json(path_to_utf8(*dir)) : json(nullptr)},
            {"default_projects_dir", fallback ? json(path_to_utf8(*fallback)) : json(nullptr)},
            {"custom", config.projects_dir.has_value()}};
}

}  // namespace

std::optional<json> ProjectService::projects_folder(ServiceRefusal& refusal) const {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    return describe_projects_folder(*config);
}

std::optional<json> ProjectService::set_projects_folder(const std::optional<fs::path>& dir, ServiceRefusal& refusal) {
    std::optional<fs::path> chosen;
    if (dir) {
        if (!dir->is_absolute()) {
            refusal = {"bad_params", "the projects folder must be a full path"};
            return std::nullopt;
        }
        std::string error;
        chosen = projects_folder_to_keep(*dir, error);
        if (!chosen) {
            refusal = {"bad_folder", error};
            return std::nullopt;
        }
        std::string ignored;
        const auto real_data = real_folder(data_dir_, ignored);  // its long names, as `chosen` has them
        if (same_or_inside(*chosen, data_dir_) || (real_data && same_or_inside(*chosen, *real_data))) {
            refusal = {"bad_folder", "the projects folder can't be inside FloServer's own data folder"};
            return std::nullopt;
        }
        // The default's own path is kept as the default (it follows the user's folder).
        if (const auto fallback = default_projects_dir(); fallback && folder_key(*fallback) == folder_key(*chosen)) {
            chosen.reset();
        }
    }
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    config->projects_dir = chosen;
    if (!save(*config, refusal)) {
        return std::nullopt;
    }
    return describe_projects_folder(*config);
}

std::optional<json> ProjectService::add(const fs::path& dir, const std::string& name, ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    std::string error;
    const auto added = add_project(*config, dir, name, error);
    if (!added) {
        refusal = {"bad_params", error};
        return std::nullopt;
    }
    if (!save(*config, refusal)) {
        return std::nullopt;
    }
    return describe(*config, added->name);
}

bool ProjectService::remove(const std::string& name, ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return false;
    }
    if (!remove_project(*config, name)) {
        refusal = {"unknown_project", "there's no project called " + name};
        return false;
    }
    return save(*config, refusal);
}

namespace {

// A folder renamed only when nothing is at `to`, never replacing it.
std::error_code rename_folder_no_replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    // Without MOVEFILE_REPLACE_EXISTING it refuses a `to` that's there;
    // "My-Shop" -> "my-shop" (the same folder) is allowed.
    if (MoveFileExW(from.c_str(), to.c_str(), 0) == FALSE) {
        return {static_cast<int>(GetLastError()), std::system_category()};
    }
    return {};
#else
#ifdef RENAME_NOREPLACE
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
        return {};
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return {errno, std::generic_category()};
    }
    // A file system without it: checked again right before.
#endif
    std::error_code ec;
    if (fs::exists(to, ec) || fs::is_symlink(to, ec)) {
        return std::make_error_code(std::errc::file_exists);
    }
    fs::rename(from, to, ec);
    return ec;
#endif
}

bool is_exists_error(const std::error_code& ec) {
#ifdef _WIN32
    if (ec.category() == std::system_category() &&
        (ec.value() == ERROR_ALREADY_EXISTS || ec.value() == ERROR_FILE_EXISTS)) {
        return true;
    }
#endif
    return ec == std::errc::file_exists || ec == std::errc::directory_not_empty;
}

// The user's own folders and Windows' (the same ones Remove never moves to
// the Recycle Bin, model/folder_removal.h): `kept` may hold a project's
// folder but never be renamed with it; `trees` (Windows', AppData) have no
// project folder renamed inside them either.
struct KnownFolders {
    std::vector<fs::path> kept;
    std::vector<fs::path> trees;
};

KnownFolders known_folders() {
    KnownFolders out;
#ifdef _WIN32
    const auto known = [](const KNOWNFOLDERID& id, std::vector<fs::path>& into) {
        PWSTR found = nullptr;
        // KF_FLAG_DONT_VERIFY: a folder that isn't there is still named, never made.
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &found)) && found != nullptr) {
            into.emplace_back(found);
        }
        CoTaskMemFree(found);
    };
    for (const KNOWNFOLDERID& id :
         {FOLDERID_Profile, FOLDERID_UserProfiles, FOLDERID_Public, FOLDERID_Desktop, FOLDERID_Documents,
          FOLDERID_Downloads, FOLDERID_Pictures, FOLDERID_Music, FOLDERID_Videos, FOLDERID_SavedGames, FOLDERID_Links,
          FOLDERID_Favorites, FOLDERID_Contacts, FOLDERID_SavedSearches, FOLDERID_Objects3D, FOLDERID_SkyDrive}) {
        known(id, out.kept);
    }
    for (const char* name : {"OneDrive", "OneDriveConsumer", "OneDriveCommercial"}) {
        if (const auto dir = get_env(name); dir && !dir->empty()) {
            out.kept.push_back(path_from_utf8(*dir));
        }
    }
    for (const KNOWNFOLDERID& id :
         {FOLDERID_Windows, FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86, FOLDERID_ProgramFilesCommon,
          FOLDERID_ProgramFilesCommonX86, FOLDERID_ProgramData, FOLDERID_RoamingAppData, FOLDERID_LocalAppData,
          FOLDERID_LocalAppDataLow}) {
        known(id, out.trees);
    }
#else
    if (const auto home = get_env("HOME"); home && !home->empty()) {
        const fs::path h = path_from_utf8(*home);
        out.kept.push_back(h);
        for (const char* name : {"Desktop", "Documents", "Downloads", "Pictures", "Music", "Videos", "Public"}) {
            out.kept.push_back(h / name);
        }
        out.trees.push_back(h / ".config");
        out.trees.push_back(h / ".local");
    }
    for (const char* dir : {"/bin", "/boot", "/etc", "/lib", "/opt", "/sbin", "/usr", "/var", "/System", "/Library"}) {
        out.trees.emplace_back(dir);
    }
#endif
    return out;
}

// Why the project's folder can't be renamed (D22), or nothing: Remove's
// rules for the Recycle Bin (model/folder_removal.h) on the folder and on
// where it really is.
std::optional<ServiceRefusal> folder_rename_problem(const AppConfig& config, const Project& project,
                                                    const fs::path& data_dir) {
    const fs::path& dir = project.dir;
    const std::string shown = path_to_utf8(dir);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return ServiceRefusal{"bad_folder", "there's no folder " + shown + " any more"};
    }
    // A folder reached through a link or junction: its target would be renamed.
    std::string error;
    const auto real = real_folder(dir, error);
    if (!real) {
        return ServiceRefusal{"bad_folder", error};
    }
    if (folder_key(*real) != folder_key(dir) || fs::is_symlink(dir, ec)) {
        return ServiceRefusal{"bad_folder", shown + " is reached through a link (it's really " + path_to_utf8(*real) +
                                                "), so FloServer doesn't rename it"};
    }
    if (dir == dir.root_path() || !dir.has_parent_path() || dir.parent_path() == dir) {
        return ServiceRefusal{"bad_folder", shown + " is a drive's top folder, which can't be renamed"};
    }
    const auto refused = [&shown](const std::string& why) {
        return ServiceRefusal{"bad_folder", shown + " " + why + ", so FloServer doesn't rename it"};
    };
    const KnownFolders known = known_folders();
    // Never renamed themselves, nor a folder holding one.
    std::vector<fs::path> kept = known.kept;
    if (const auto projects = config.projects_dir ? config.projects_dir : default_projects_dir()) {
        kept.push_back(*projects);
    }
    std::string ignored;
    if (const auto shared = read_shared_config(config, ignored)) {
        if (const auto www = shared_dir(*shared)) {
            kept.push_back(*www);
        }
    }
    for (const fs::path& k : kept) {
        if (same_or_inside(k, dir)) {
            return refused(folder_key(k) == folder_key(dir) ? "is one of your own folders"
                                                            : "holds " + path_to_utf8(k));
        }
    }
    // Nor anything inside or holding one: FloServer's data folder (as given
    // and as the file system names it), Windows' own.
    std::vector<fs::path> trees = known.trees;
    trees.push_back(data_dir);
    if (const auto real_data = real_folder(data_dir, ignored)) {
        trees.push_back(*real_data);
    }
    for (const fs::path& t : trees) {
        if (same_or_inside(dir, t) || same_or_inside(t, dir)) {
            return refused(same_or_inside(t, dir) ? "holds " + path_to_utf8(t) : "is inside " + path_to_utf8(t));
        }
    }
    if (folder_key(dir).find("/$recycle.bin") != std::string::npos) {
        return refused("is in a Recycle Bin");
    }
    // What would be left behind at the old path, or lose its own.
    for (const Project& other : config.projects) {
        if (other.name != project.name && (same_or_inside(other.dir, dir) || same_or_inside(dir, other.dir))) {
            return refused(same_or_inside(other.dir, dir) ? "holds " + other.name + "'s folder too"
                                                          : "is inside " + other.name + "'s folder");
        }
    }
    return std::nullopt;
}

#ifdef _WIN32
bool same_name_any_case(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
               return lower(x) == lower(y);
           });
}
#endif

}  // namespace

std::optional<fs::path> rename_project_folder(const AppConfig& config, const Project& project,
                                              const std::string& new_name, const fs::path& data_dir,
                                              const std::function<bool(const fs::path&, ServiceRefusal&)>& save,
                                              ServiceRefusal& refusal) {
    if (auto problem = folder_rename_problem(config, project, data_dir)) {
        refusal = std::move(*problem);
        return std::nullopt;
    }
    const fs::path& dir = project.dir;
    const std::string shown = path_to_utf8(dir);
    const std::string old_name = path_to_utf8(dir.filename());
    if (old_name == new_name) {
        return save(dir, refusal) ? std::optional(dir) : std::nullopt;  // already called that
    }
    const fs::path target = dir.parent_path() / path_from_utf8(new_name);
    // Windows: "My-Shop" -> "my-shop" is the same folder, only its case changes.
#ifdef _WIN32
    const bool case_only = same_name_any_case(old_name, new_name);
#else
    const bool case_only = false;
#endif
    std::error_code ec;
    if (!case_only && (fs::exists(target, ec) || fs::is_symlink(target, ec))) {
        refusal = {"folder_exists", "there's already a folder " + path_to_utf8(target)};
        return std::nullopt;
    }
    // A server FloServer just stopped there may take a moment to end, and
    // Windows' indexer or a virus scanner may hold a file briefly.
    for (int attempt = 0;; ++attempt) {
        ec = rename_folder_no_replace(dir, target);
        if (!ec || is_exists_error(ec) || attempt >= 30) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (ec) {
        refusal = is_exists_error(ec)
                      ? ServiceRefusal{"folder_exists", "there's already a folder " + path_to_utf8(target)}
                      : ServiceRefusal{"folder_in_use", shown + " can't be renamed: " + error_text(ec) +
                                                            ". Close editors and terminals that use it, then try again"};
        return std::nullopt;
    }
    std::string ignored;
    fs::path kept = real_folder(target, ignored).value_or(target);
    if (save(kept, refusal)) {
        return kept;
    }
    // config.json still has the old folder: it goes back there.
    if (const std::error_code back = rename_folder_no_replace(kept, dir)) {
        refusal.message += ". The folder couldn't be renamed back (" + error_text(back) + "): it's now " +
                           path_to_utf8(kept);
    }
    return std::nullopt;
}

std::optional<AppConfig> ProjectService::check_rename(const std::string& name, const std::string& new_name,
                                                      ServiceRefusal& refusal) const {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    if (find_project(*config, name) == nullptr) {
        refusal = {"unknown_project", "there's no project called " + name};
        return std::nullopt;
    }
    std::string error;
    if (rename_project(*config, name, new_name, error) == nullptr) {
        refusal = {"bad_params", error};
        return std::nullopt;
    }
    return config;
}

std::optional<json> ProjectService::rename(const std::string& name, const std::string& new_name, bool rename_folder,
                                           ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    const Project* before = find_project(*config, name);
    if (before == nullptr) {
        refusal = {"unknown_project", "there's no project called " + name};
        return std::nullopt;
    }
    const std::string old_database = project_database_name(*before);
    AppConfig renamed = *config;
    std::string error;
    Project* project = rename_project(renamed, name, new_name, error);
    if (project == nullptr) {
        refusal = {"bad_params", error};
        return std::nullopt;
    }
    const fs::path old_dir = project->dir;
    if (rename_folder) {
        const auto dir = rename_project_folder(*config, *before, new_name, data_dir_,
                                               [&](const fs::path& moved, ServiceRefusal& why) {
                                                   project->dir = moved;
                                                   return save(renamed, why);
                                               },
                                               refusal);
        if (!dir) {
            return std::nullopt;
        }
        project->dir = *dir;
    } else if (!save(renamed, refusal)) {
        return std::nullopt;
    }
    json out = describe(renamed, new_name);
    out["old_name"] = name;
    out["old_path"] = path_to_utf8(old_dir);
    out["old_database_name"] = old_database;
    return out;
}

std::optional<json> ProjectService::set(const std::string& name, std::optional<bool> on,
                                        std::optional<bool> keep_running, ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    Project* project = find_project(*config, name);
    if (project == nullptr) {
        refusal = {"unknown_project", "there's no project called " + name};
        return std::nullopt;
    }
    if (on) {
        project->on = *on;
    }
    if (keep_running) {
        project->keep_running = *keep_running;
    }
    if (!save(*config, refusal)) {
        return std::nullopt;
    }
    return describe(*config, name);
}

std::optional<json> ProjectService::use(const std::string& name, const std::string& key,
                                        const std::optional<std::string>& value, ServiceRefusal& refusal) {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    const auto parsed_key = parse_config_key(key);
    if (!parsed_key || key == "mariadb" || key == "mysql") {
        refusal = {"bad_params", "the key is php, node, python, database or web"};
        return std::nullopt;
    }
    const Project* project = find_project(*config, name);
    if (project == nullptr) {
        refusal = {"unknown_project", "there's no project called " + name};
        return std::nullopt;
    }
    std::string error;
    if (!set_project_config_file_value(project->dir, *parsed_key, key, value, error)) {
        refusal = {"config_refused", error};
        return std::nullopt;
    }
    return describe(*config, name);
}

std::optional<fs::path> ProjectService::folder(const std::string& name, ServiceRefusal& refusal) const {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    for (const Project& p : config->projects) {
        if (p.name == name) {
            return p.dir;
        }
    }
    refusal = {"unknown_project", "there's no project called " + name};
    return std::nullopt;
}

std::optional<json> ProjectService::describe_shared(const AppConfig& config, ServiceRefusal& refusal,
                                                    bool folders) const {
    std::string error;
    const auto shared = read_shared_config(config, error);
    if (!shared) {
        refusal = {"config_unreadable", "FloServer's settings for shared sites can't be used: " + error};
        return std::nullopt;
    }
    json out = {{"on", shared->on}, {"web", shared->web == WebServer::Apache ? "apache" : "nginx"}};
    const auto dir = shared_dir(*shared);
    out["dir"] = dir ? json(path_to_utf8(*dir)) : json(nullptr);
    if (const auto d = default_shared_dir()) {
        out["default_dir"] = path_to_utf8(*d);
    }
    if (shared->php) {
        out["php"] = *shared->php;
    }
    if (shared->database) {
        out["database"] = shared->database->family + "-" + shared->database->version;
    }
    if (!folders) {
        return out;  // the www folder isn't read
    }
    json list = json::array();
    if (dir) {
        for (const SharedFolder& f : scan_shared_folders(*dir, config)) {
            json j = {{"name", f.name}, {"kind", shared_kind_name(f.kind)}, {"host", f.host_name}};
            if (f.clash) {
                j["clash"] = *f.clash;
            }
            list.push_back(std::move(j));
        }
    }
    out["folders"] = std::move(list);
    return out;
}

namespace {

constexpr std::array<std::string_view, 5> kDefaultRuntimes = {"php", "mariadb", "mysql", "node", "python"};

// config.json's "defaults", keeping only what this version understands.
json read_defaults(const AppConfig& config) {
    json out = json::object();
    // A copy (it's a few keys): GCC's -O3 null-dereference check trips on
    // iterators into nlohmann::json.
    const json stored = config.other.is_object() && config.other.contains("defaults") ? config.other.at("defaults")
                                                                                      : json();
    if (!stored.is_object()) {
        return out;
    }
    for (const std::string_view name : kDefaultRuntimes) {
        const std::string key(name);
        if (stored.contains(key) && stored.at(key).is_string() && Version::parse(stored.at(key).get<std::string>())) {
            out[key] = stored.at(key);
        }
    }
    return out;
}

}  // namespace

std::optional<json> ProjectService::runtime_users(const std::vector<InstalledRuntime>& installed,
                                                  ServiceRefusal& refusal) const {
    AppConfig config;
    {
        const std::lock_guard lock(mutex_);
        auto loaded = load(refusal);
        if (!loaded) {
            return std::nullopt;
        }
        config = std::move(*loaded);
    }
    // The floserver.json files are read without the lock: a slow disk
    // doesn't hold up other requests.
    json out = json::object();
    for (auto& [key, who] : floserver::core::runtime_users(config, read_project_config_file, installed, std::nullopt)) {
        out[key] = who;
    }
    return out;
}

std::optional<json> ProjectService::defaults(ServiceRefusal& refusal) const {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    return read_defaults(*config);
}

std::optional<json> ProjectService::set_default(const std::string& runtime, const std::optional<std::string>& series,
                                                ServiceRefusal& refusal) {
    if (std::find(kDefaultRuntimes.begin(), kDefaultRuntimes.end(), runtime) == kDefaultRuntimes.end()) {
        refusal = {"bad_params", "a default can be set for php, mariadb, mysql, node or python, not \"" + runtime + "\""};
        return std::nullopt;
    }
    if (series && !Version::parse(*series)) {
        refusal = {"bad_params", "\"" + *series + "\" isn't a version such as 8.5"};
        return std::nullopt;
    }
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    json& stored = config->other["defaults"];
    if (!stored.is_object()) {
        stored = json::object();  // unusable before: replaced
    }
    if (series) {
        stored[runtime] = *series;
    } else {
        stored.erase(runtime);
    }
    if (stored.empty()) {
        config->other.erase("defaults");
    }
    if (!save(*config, refusal)) {
        return std::nullopt;
    }
    return read_defaults(*config);
}

std::optional<json> ProjectService::shared(ServiceRefusal& refusal, bool folders) const {
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    return describe_shared(*config, refusal, folders);
}

std::optional<json> ProjectService::set_shared(const json& changes, ServiceRefusal& refusal, bool folders) {
    const std::lock_guard lock(mutex_);
    auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    std::string error;
    auto shared = read_shared_config(*config, error);
    if (!shared) {
        refusal = {"config_unreadable", "FloServer's settings for shared sites can't be used: " + error};
        return std::nullopt;
    }
    if (!changes.is_object()) {
        refusal = {"bad_params", "the changes must be an object"};
        return std::nullopt;
    }
    for (const auto& [key, value] : changes.items()) {
        if (key == "on") {
            if (!value.is_boolean()) {
                refusal = {"bad_params", "on must be true or false"};
                return std::nullopt;
            }
            shared->on = value.get<bool>();
        } else if (key == "dir") {
            if (value.is_null()) {
                shared->dir.reset();
                continue;
            }
            if (!value.is_string()) {
                refusal = {"bad_params", "dir must be a folder"};
                return std::nullopt;
            }
            const auto real = real_folder(path_from_utf8(value.get<std::string>()), error);
            if (!real) {
                refusal = {"bad_params", error};
                return std::nullopt;
            }
            shared->dir = *real;
        } else if (key == "php" || key == "web" || key == "database") {
            if (value.is_null() && key != "web") {
                if (key == "php") {
                    shared->php.reset();
                } else {
                    shared->database.reset();
                }
                continue;
            }
            // Checked by floserver.json's own rules.
            const ConfigResult parsed = parse_project_config(json{{key, value}}.dump());
            if (!parsed.config) {
                const std::string why = parsed.errors.empty() ? std::string("isn't valid") : parsed.errors.front().message;
                std::string message = key;
                message.append(" ").append(why);
                refusal = {"bad_params", std::move(message)};
                return std::nullopt;
            }
            if (key == "php" && php_off(*parsed.config)) {
                refusal = {"bad_params", "php can't be false: shared sites always have PHP"};
                return std::nullopt;
            }
            if (key == "php") {
                shared->php = parsed.config->php;  // "newest": none named, the newest
            } else if (key == "web") {
                shared->web = parsed.config->web;
            } else {
                shared->database = parsed.config->database;
            }
        } else {
            refusal = {"bad_params", "unknown setting " + key + " (on, dir, php, web, database)"};
            return std::nullopt;
        }
    }
    // The default www folder is made when shared sites are turned on.
    if (shared->on && !shared->dir) {
        if (const auto d = default_shared_dir()) {
            std::error_code ec;
            fs::create_directories(*d, ec);
            if (ec) {
                refusal = {"folder_unwritable", "can't make " + path_to_utf8(*d) + ": " + error_text(ec)};
                return std::nullopt;
            }
        }
    }
    write_shared_config(*config, *shared);
    if (!save(*config, refusal)) {
        return std::nullopt;
    }
    return describe_shared(*config, refusal, folders);
}

namespace {

// "8.5" from a request: two numbers, nothing else.
std::optional<std::string> request_series(const std::string& series, ServiceRefusal& refusal) {
    const auto v = Version::parse(series);
    if (!v || v->parts().size() != 2) {
        refusal = {"bad_params", "\"" + series + "\" isn't a PHP series such as 8.5"};
        return std::nullopt;
    }
    return v->to_string();
}

}  // namespace

std::optional<json> ProjectService::php_get(const std::string& series_text, ServiceRefusal& refusal) const {
    const auto series = request_series(series_text, refusal);
    if (!series) {
        return std::nullopt;
    }
    AppConfig config;
    {
        const std::lock_guard lock(mutex_);
        auto loaded = load(refusal);
        if (!loaded) {
            return std::nullopt;
        }
        config = std::move(*loaded);
    }
    std::vector<std::string> problems;
    const PhpSettings all = read_php_settings(config, &problems);
    const auto found = all.find(*series);
    const PhpSeriesSettings settings = found != all.end() ? found->second : PhpSeriesSettings{};
    const auto installed = list_installed(runtimes_dir(data_dir_));
    const InstalledRuntime* php = best_installed(installed, "php", *series);

    json out = {{"series", *series}, {"version", php != nullptr ? json(php->version) : json(nullptr)}};

    // Extensions: on Windows the ones the install has (ext/php_*.dll); with
    // builds that come with their extensions, the base list as it is.
    const auto& base = base_php_extensions();
    const auto available = php != nullptr ? available_php_extensions(php->dir) : std::nullopt;
    out["extensions_fixed"] = !available.has_value();
    json list = json::array();
    const auto is_base = [&base](const std::string& e) { return std::find(base.begin(), base.end(), e) != base.end(); };
    const auto on = [&](const std::string& e) {
        const auto it = settings.extensions.find(e);
        return it != settings.extensions.end() ? it->second : is_base(e);
    };
    const auto has = [&available](const std::string& e) {
        return !available || std::find(available->begin(), available->end(), e) != available->end();
    };
    for (const std::string& e : base) {
        json item = {{"name", e}, {"on", available ? on(e) : true}, {"base", true}};
        if (!has(e)) {
            item["missing"] = true;
        }
        list.push_back(std::move(item));
    }
    if (available) {
        std::set<std::string> others(available->begin(), available->end());
        for (const auto& [e, want] : settings.extensions) {
            if (want) {
                others.insert(e);  // turned on, even if this install lacks it: shown as missing
            }
        }
        for (const std::string& e : others) {
            if (is_base(e) || e == "opcache") {
                continue;
            }
            json item = {{"name", e}, {"on", on(e)}, {"base", false}};
            if (!has(e)) {
                item["missing"] = true;
            }
            list.push_back(std::move(item));
        }
    }
    out["extensions"] = std::move(list);

    json ini = json::object();
    for (const auto& [k, v] : php_ini_defaults()) {
        ini[k] = v;
    }
    for (const auto& [k, v] : settings.ini) {
        ini[k] = v;
    }
    out["ini"] = std::move(ini);
    out["changed"] = settings.ini;

    const fs::path custom = php_custom_ini_file(data_dir_, *series);
    std::error_code ec;
    out["custom_ini"] = path_to_utf8(custom);
    out["custom_ini_exists"] = fs::is_regular_file(custom, ec);

    // Who's on this series: the users of every install of it.
    std::set<std::string> users;
    const auto by_runtime = floserver::core::runtime_users(config, read_project_config_file, installed, std::nullopt);
    for (const InstalledRuntime& r : installed) {
        if (r.name == "php" && php_series_of(r.version) == *series) {
            const auto it = by_runtime.find("php/" + path_to_utf8(r.dir.filename()));
            if (it != by_runtime.end()) {
                users.insert(it->second.begin(), it->second.end());
            }
        }
    }
    out["used_by"] = users;
    if (!problems.empty()) {
        out["problems"] = problems;
    }
    // The names PHP knows, so the window can warn about one it would ignore.
    if (php != nullptr) {
        const std::string dir = path_to_utf8(php->dir);
        bool failed_before = false;
        {
            const std::lock_guard lock(known_mutex_);
            failed_before = known_failed_.contains(dir);
        }
        if (!failed_before) {
            const fs::path cache = php_custom_ini_file(data_dir_, *series).parent_path() /
                                   path_from_utf8("known-ini-" + path_to_utf8(php->dir.filename()) + ".json");
            std::string error;
            bool timed_out = false;
            if (const auto known = known_php_ini_names(php->dir, cache, error, timed_out)) {
                out["known_ini"] = *known;
            } else if (!timed_out) {
                // PHP itself can't list them: not asked again while the service runs.
                // A run that was stopped for taking too long is tried next time.
                const std::lock_guard lock(known_mutex_);
                known_failed_.insert(dir);
            }
        }
    }
    return out;
}

std::optional<json> ProjectService::php_set(const std::string& series_text, const json& extensions, const json& ini,
                                            ServiceRefusal& refusal) {
    const auto series = request_series(series_text, refusal);
    if (!series) {
        return std::nullopt;
    }
    if (!extensions.is_null() && !extensions.is_object()) {
        refusal = {"bad_params", R"(extensions is an object such as {"bcmath": true})"};
        return std::nullopt;
    }
    if (!ini.is_null() && !ini.is_object()) {
        refusal = {"bad_params", R"(ini is an object such as {"memory_limit": "1G"})"};
        return std::nullopt;
    }
    {
        const std::lock_guard lock(mutex_);
        auto config = load(refusal);
        if (!config) {
            return std::nullopt;
        }
        const PhpSettings all = read_php_settings(*config);
        const auto found = all.find(*series);
        PhpSeriesSettings settings = found != all.end() ? found->second : PhpSeriesSettings{};
        const auto& base = base_php_extensions();
        if (extensions.is_object()) {
            for (const auto& [name, value] : extensions.items()) {
                if (!is_php_extension_name(name)) {
                    refusal = {"bad_params", "\"" + name + "\" isn't an extension name (a-z, 0-9, _)"};
                    return std::nullopt;
                }
                if (name == "opcache") {
                    refusal = {"bad_params", "opcache is always on"};
                    return std::nullopt;
                }
                if (!value.is_null() && !value.is_boolean()) {
                    refusal = {"bad_params", "extension " + name + " is true, false or null"};
                    return std::nullopt;
                }
                const bool is_base = std::find(base.begin(), base.end(), name) != base.end();
                // Only changes from the base list are kept.
                if (value.is_null() || value.get<bool>() == is_base) {
                    settings.extensions.erase(name);
                } else {
                    settings.extensions[name] = value.get<bool>();
                }
            }
        }
        if (ini.is_object()) {
            for (const auto& [key, value] : ini.items()) {
                if (const auto problem = php_ini_key_problem(key)) {
                    refusal = {"bad_params", *problem};
                    return std::nullopt;
                }
                if (value.is_null()) {
                    settings.ini.erase(key);
                    continue;
                }
                if (!value.is_string()) {
                    refusal = {"bad_params", key + " is text, or null for the default"};
                    return std::nullopt;
                }
                if (const auto problem = php_ini_value_problem(value.get<std::string>())) {
                    refusal = {"bad_params", key + ": " + *problem};
                    return std::nullopt;
                }
                settings.ini[key] = value.get<std::string>();
            }
        }
        write_php_series(*config, *series, settings);
        if (!save(*config, refusal)) {
            return std::nullopt;
        }
    }
    return php_get(*series, refusal);
}

std::optional<fs::path> ProjectService::php_custom_ini(const std::string& series_text, ServiceRefusal& refusal) const {
    const auto series = request_series(series_text, refusal);
    if (!series) {
        return std::nullopt;
    }
    std::string error;
    if (!ensure_php_custom_ini(data_dir_, *series, error)) {
        refusal = {"folder_unwritable", error};
        return std::nullopt;
    }
    return php_custom_ini_file(data_dir_, *series);
}

std::optional<fs::path> ProjectService::php_final_ini(const std::string& series_text, ServiceRefusal& refusal) const {
    const auto series = request_series(series_text, refusal);
    if (!series) {
        return std::nullopt;
    }
    AppConfig config;
    {
        const std::lock_guard lock(mutex_);
        auto loaded = load(refusal);
        if (!loaded) {
            return std::nullopt;
        }
        config = std::move(*loaded);
    }
    const auto installed = list_installed(runtimes_dir(data_dir_));
    const InstalledRuntime* php = best_installed(installed, "php", *series);
    if (php == nullptr) {
        refusal = {"not_installed", "PHP " + *series + " isn't installed"};
        return std::nullopt;
    }
    const PhpSettings all = read_php_settings(config);
    const auto found = all.find(*series);
    PhpIniSettings settings = effective_php_settings(
        *series, found != all.end() ? &found->second : nullptr, read_php_custom_ini(data_dir_, *series), nullptr);
    settings.mail_ini = mail_php_ini(config, installed);  // as the pools have it (D27)
    // Each pool logs to its own file; the real path is in the pool's copy.
    const PhpIniResult made = make_php_ini(php->dir, settings, fs::path("(each project's own log)"));
    const std::string text = "; What PHP " + php->version +
                             " uses for shared sites and for projects without PHP changes of their own.\n"
                             "; Written by FloServer when you ask to see it: edits here are lost and do nothing.\n"
                             "; Change values in Settings > PHP, or add your own lines to custom.ini beside this file.\n"
                             "; A project's own changes (its floserver.json) come after all of this.\n\n" +
                             made.text;
    fs::path file = php_custom_ini_file(data_dir_, *series).parent_path() / "final-php.ini";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    // Read-only, so Notepad says so on Save; made writable again to replace it.
    fs::permissions(file, fs::perms::owner_write, fs::perm_options::add, ec);
    std::string error;
    if (!write_file_atomically(file, text, error)) {
        refusal = {"folder_unwritable", error};
        return std::nullopt;
    }
    fs::permissions(file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove, ec);
    return file;
}

namespace {

std::optional<std::string> request_database_series(const std::string& series, std::string_view family,
                                                   ServiceRefusal& refusal) {
    if (!mysql_protocol_family(family)) {
        refusal = {"bad_params", "\"" + std::string(family) + "\" isn't a database FloServer runs (mariadb or mysql)"};
        return std::nullopt;
    }
    auto out = mariadb_request_series(series);
    if (!out) {
        refusal = {"bad_params", "\"" + series + "\" isn't a " + database_family_title(family) + " series such as " +
                                     (family == kMySqlFamily ? "8.4" : "11.4")};
    }
    return out;
}

// The newest install of a MariaDB (or `family`'s) series, or nothing.
const InstalledRuntime* newest_mariadb(const std::vector<InstalledRuntime>& installed, std::string_view family,
                                       const std::string& series) {
    const InstalledRuntime* r = best_installed(installed, std::string(family), series);
    return r != nullptr && mariadb_series(r->version) == series ? r : nullptr;
}

}  // namespace

std::optional<json> ProjectService::database_get(const std::string& series_text, ServiceRefusal& refusal,
                                                 std::string_view family) const {
    const auto series = request_database_series(series_text, family, refusal);
    if (!series) {
        return std::nullopt;
    }
    AppConfig config;
    {
        const std::lock_guard lock(mutex_);
        auto loaded = load(refusal);
        if (!loaded) {
            return std::nullopt;
        }
        config = std::move(*loaded);
    }
    std::vector<std::string> problems;
    const MariaDbSettings all = read_mariadb_settings(config, &problems, family);
    const auto found = all.find(*series);
    const MariaDbSeriesSettings settings = found != all.end() ? found->second : MariaDbSeriesSettings{};
    const auto installed = list_installed(runtimes_dir(data_dir_));
    const InstalledRuntime* db = newest_mariadb(installed, family, *series);

    json out = {{"family", family},
                {"series", *series},
                {"version", db != nullptr ? json(db->version) : json(nullptr)},
                {"port", settings.port ? json(*settings.port) : json(nullptr)},
                {"keep_running", settings.keep_running}};
    json ini = json::object();
    for (const auto& [k, v] : mariadb_ini_defaults(family)) {
        ini[k] = v;
    }
    for (const auto& [k, v] : settings.ini) {
        ini[k] = v;
    }
    out["ini"] = std::move(ini);
    out["changed"] = settings.ini;

    const fs::path custom = mariadb_custom_ini_file(data_dir_, *series, family);
    std::error_code ec;
    out["custom_ini"] = path_to_utf8(custom);
    out["custom_ini_exists"] = fs::is_regular_file(custom, ec);
    const FilteredCustomIni filtered = filter_mariadb_custom_ini(read_mariadb_custom_ini(data_dir_, *series, family));
    if (!filtered.problems.empty()) {
        out["custom_problems"] = filtered.problems;
    }
    out["custom_keys"] = filtered.keys;

    // Who's on this series: the users of every install of it, and the
    // projects among them set to keep running.
    std::set<std::string> users;
    const auto by_runtime = floserver::core::runtime_users(config, read_project_config_file, installed, std::nullopt);
    for (const InstalledRuntime& r : installed) {
        if (r.name == family && mariadb_series(r.version) == *series) {
            const auto it = by_runtime.find(std::string(family) + "/" + path_to_utf8(r.dir.filename()));
            if (it != by_runtime.end()) {
                users.insert(it->second.begin(), it->second.end());
            }
        }
    }
    json kept = json::array();
    for (const Project& p : config.projects) {
        if (p.keep_running && users.contains(p.name)) {
            kept.push_back(p.name);
        }
    }
    out["used_by"] = users;
    out["kept_by"] = std::move(kept);
    if (!problems.empty()) {
        out["problems"] = problems;
    }
    // The names MariaDB (MySQL) knows, so the window can warn about one it would refuse.
    if (db != nullptr) {
        const MariaDbFiles files = mariadb_files(db->dir, db->version, data_dir_, family);
        const std::string dir = std::string(family) + "\n" + path_to_utf8(db->dir);
        const fs::path cache = custom.parent_path() /
                               path_from_utf8("known-options-" + path_to_utf8(db->dir.filename()) + ".json");
        const auto now = std::chrono::steady_clock::now();
        bool skip = false;
        {
            const std::lock_guard lock(known_mutex_);
            const auto timed_out = known_timed_out_.find(dir);
            // A list kept on disk is always read; mariadbd is only run again
            // 10 minutes after a run that took too long, never after one that failed.
            skip = !fs::is_regular_file(cache, ec) &&
                   (known_failed_.contains(dir) ||
                    (timed_out != known_timed_out_.end() && now - timed_out->second < std::chrono::minutes(10)));
        }
        if (!skip) {
            std::string error;
            bool timed_out = false;
            if (const auto known = known_mariadb_options(files.server, cache, error, timed_out)) {
                out["known_options"] = *known;
            } else {
                const std::lock_guard lock(known_mutex_);
                if (timed_out) {
                    known_timed_out_[dir] = now;
                } else {
                    known_failed_.insert(dir);
                }
            }
        }
    }
    return out;
}

std::optional<json> ProjectService::database_set(const std::string& series_text, const json& ini, const json* port,
                                                 const json* keep_running, bool& changed_ini, ServiceRefusal& refusal,
                                                 std::string_view family) {
    changed_ini = false;
    const auto series = request_database_series(series_text, family, refusal);
    if (!series) {
        return std::nullopt;
    }
    if (!ini.is_null() && !ini.is_object()) {
        refusal = {"bad_params", R"(ini is an object such as {"max_connections": "300"})"};
        return std::nullopt;
    }
    if (port != nullptr && !port->is_null() &&
        (!port->is_number_integer() || port->get<std::int64_t>() < 1024 || port->get<std::int64_t>() > 65535)) {
        refusal = {"bad_params", "port: a number from 1024 to 65535"};
        return std::nullopt;
    }
    if (keep_running != nullptr && !keep_running->is_boolean()) {
        refusal = {"bad_params", "keep_running is true or false"};
        return std::nullopt;
    }
    {
        const std::lock_guard lock(mutex_);
        auto config = load(refusal);
        if (!config) {
            return std::nullopt;
        }
        const MariaDbSettings all = read_mariadb_settings(*config, nullptr, family);
        const auto found = all.find(*series);
        MariaDbSeriesSettings settings = found != all.end() ? found->second : MariaDbSeriesSettings{};
        const auto before = settings.ini;
        const std::string own_key = database_server_key(family, *series);
        if (ini.is_object()) {
            for (const auto& [key, value] : ini.items()) {
                if (const auto problem = mariadb_option_problem(key)) {
                    refusal = {"bad_params", *problem};
                    return std::nullopt;
                }
                const std::string name = canonical_mariadb_option(key);
                if (value.is_null()) {
                    settings.ini.erase(name);
                    continue;
                }
                if (!value.is_string()) {
                    refusal = {"bad_params", key + " is text, or null for the default"};
                    return std::nullopt;
                }
                const std::string text = value.get<std::string>();
                if (const auto problem = mariadb_value_problem(text)) {
                    refusal = {"bad_params", key + ": " + *problem};
                    return std::nullopt;
                }
                if (text.empty()) {
                    settings.ini.erase(name);
                } else {
                    settings.ini[name] = text;
                }
            }
        }
        if (port != nullptr) {
            std::optional<std::uint16_t> wanted;
            if (!port->is_null()) {
                wanted = static_cast<std::uint16_t>(port->get<std::int64_t>());
            }
            if (wanted && wanted != settings.port) {
                // Not another series' (chosen, or kept in its folder), and not FloServer's internal ones.
                if (*wanted >= kFirstInternalDatabasePort && *wanted < kFirstInternalDatabasePort + 100) {
                    refusal = {"bad_params", "port " + std::to_string(*wanted) + " is one FloServer uses inside (33100-33199)"};
                    return std::nullopt;
                }
                // Every family's servers: MariaDB's and MySQL's share one set of ports (D28).
                std::set<std::string> chosen_keys;
                for (const std::string& f : database_families()) {
                    if (!mysql_protocol_family(f)) {
                        continue;
                    }
                    for (const auto& [other, s] : read_mariadb_settings(*config, nullptr, f)) {
                        const std::string key = database_server_key(f, other);
                        chosen_keys.insert(key);
                        if (key != own_key && s.port == wanted) {
                            refusal = {"port_taken", "port " + std::to_string(*wanted) + " is " +
                                                         database_family_title(f) + " " + other + "'s"};
                            return std::nullopt;
                        }
                    }
                }
                const fs::path data = data_dir_ / "data";
                std::error_code ec;
                for (fs::directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec)) {
                    const std::string name = path_to_utf8(it->path().filename());
                    const auto dash = name.find('-');
                    if (dash == std::string::npos || !mysql_protocol_family(std::string_view(name).substr(0, dash)) ||
                        name == own_key || chosen_keys.contains(name)) {
                        continue;
                    }
                    const auto saved = read_small_file(it->path() / "port", 32);
                    if (saved && saved->substr(0, saved->find_first_of("\r\n")) == std::to_string(*wanted)) {
                        refusal = {"port_taken", "port " + std::to_string(*wanted) + " is " +
                                                     database_family_title(name.substr(0, dash)) + " " +
                                                     name.substr(dash + 1) + "'s"};
                        return std::nullopt;
                    }
                }
                const auto own = read_small_file(data / path_from_utf8(own_key) / "port", 32);
                const bool current = own && own->substr(0, own->find_first_of("\r\n")) == std::to_string(*wanted);
                if (!current && !can_listen_on_loopback(*wanted)) {
                    refusal = {"port_taken", "port " + std::to_string(*wanted) + " is in use by another program"};
                    return std::nullopt;
                }
            }
            settings.port = wanted;
        }
        if (keep_running != nullptr) {
            settings.keep_running = keep_running->get<bool>();
        }
        changed_ini = settings.ini != before;
        write_mariadb_series(*config, *series, settings, family);
        if (!save(*config, refusal)) {
            return std::nullopt;
        }
    }
    return database_get(*series, refusal, family);
}

std::optional<fs::path> ProjectService::database_custom_ini(const std::string& series_text, ServiceRefusal& refusal,
                                                            std::string_view family) const {
    const auto series = request_database_series(series_text, family, refusal);
    if (!series) {
        return std::nullopt;
    }
    std::string error;
    if (!ensure_mariadb_custom_ini(data_dir_, *series, error, family)) {
        refusal = {"folder_unwritable", error};
        return std::nullopt;
    }
    return mariadb_custom_ini_file(data_dir_, *series, family);
}

std::optional<json> ProjectService::project_php(const std::string& name, const json& extensions, const json& ini,
                                                ServiceRefusal& refusal) {
    std::map<std::string, std::optional<std::string>> ext_changes;
    std::map<std::string, std::optional<std::string>> ini_changes;
    const auto read = [&refusal](const json& in, const char* what,
                                 std::map<std::string, std::optional<std::string>>& out) {
        if (in.is_null()) {
            return true;
        }
        if (!in.is_object()) {
            refusal = {"bad_params", std::string(what) + " must be an object"};
            return false;
        }
        for (const auto& [key, value] : in.items()) {
            if (value.is_null()) {
                out[key] = std::nullopt;
            } else if (value.is_string()) {
                out[key] = value.get<std::string>();
            } else {
                refusal = {"bad_params", std::string(what) + " " + key + " is text, or null for the series' setting"};
                return false;
            }
        }
        return true;
    };
    if (!read(extensions, "extensions", ext_changes) || !read(ini, "ini", ini_changes)) {
        return std::nullopt;
    }
    const std::lock_guard lock(mutex_);
    const auto config = load(refusal);
    if (!config) {
        return std::nullopt;
    }
    const Project* project = find_project(*config, name);
    if (project == nullptr) {
        refusal = {"unknown_project", "there's no project called " + name};
        return std::nullopt;
    }
    std::string error;
    if (!set_project_php_settings_file(project->dir, ext_changes, ini_changes, error)) {
        refusal = {"config_refused", error};
        return std::nullopt;
    }
    return describe(*config, name);
}

}  // namespace floserver::core

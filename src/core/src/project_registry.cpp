#include "floserver/core/project_registry.h"

#include "floserver/core/mariadb.h"
#include "floserver/core/paths.h"
#include "floserver/core/phpmyadmin.h"
#include "floserver/core/process.h"

#include <algorithm>
#include <set>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::size_t kMaxConfigBytes = std::size_t{1024} * 1024;

bool is_name_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }

std::string generic_utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return {u.begin(), u.end()};
}

// True when `a` and `b` are the same folder: the file system's own answer when
// both exist (case, short names and links on Windows), else the same text.
bool same_folder(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::equivalent(a, b, ec) && !ec) {
        return true;
    }
    return a.lexically_normal() == b.lexically_normal();
}

std::optional<Project> read_project(const json& item, std::size_t index, std::string& error) {
    const std::string where = "projects[" + std::to_string(index) + "]";
    if (!item.is_object() || !item.contains("name") || !item["name"].is_string() || !item.contains("path") ||
        !item["path"].is_string()) {
        error = where + R"( needs a "name" and a "path")";
        return std::nullopt;
    }
    Project p;
    p.name = item["name"].get<std::string>();
    if (const auto problem = project_name_problem(p.name)) {
        error = where + ": " + *problem;
        return std::nullopt;
    }
    p.dir = path_from_utf8(item["path"].get<std::string>());
    if (!p.dir.is_absolute()) {
        error = where + ": the path must be a full path";
        return std::nullopt;
    }
    p.dir = p.dir.lexically_normal();
    for (const char* key : {"on", "keep_running"}) {
        if (item.contains(key) && !item[key].is_boolean()) {
            error = where + ": " + key + " must be true or false";
            return std::nullopt;
        }
    }
    p.on = item.value("on", false);
    p.keep_running = item.value("keep_running", false);
    if (item.contains("database_name")) {
        // As project_database_name() makes them: a-z, 0-9 and '_'.
        const json& db = item["database_name"];
        const auto usable = [](const std::string& n) {
            return !n.empty() && n.size() <= kMaxProjectNameLength && std::all_of(n.begin(), n.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            });
        };
        if (!db.is_string() || !usable(db.get<std::string>())) {
            error = where + ": database_name may use only a-z, 0-9 and _";
            return std::nullopt;
        }
        p.database_name = db.get<std::string>();
    }
    p.other = item;
    for (const char* key : {"name", "path", "on", "keep_running", "database_name"}) {
        p.other.erase(key);
    }
    return p;
}

}  // namespace

fs::path app_config_file(const fs::path& data) { return data / "config.json"; }

std::optional<fs::path> default_projects_dir() {
#ifdef _WIN32
    const char* home_var = "USERPROFILE";
#else
    const char* home_var = "HOME";
#endif
    const auto home = get_env(home_var);
    if (!home || home->empty()) {
        return std::nullopt;
    }
    const fs::path p = path_from_utf8(*home);
    if (!p.is_absolute()) {
        return std::nullopt;
    }
    return p / "FloServer" / "projects";
}

std::optional<std::string> project_name_problem(std::string_view name) {
    if (name.empty()) {
        return "the name is empty";
    }
    if (name.size() > kMaxProjectNameLength) {
        return "the name is longer than 63 characters";
    }
    if (!std::all_of(name.begin(), name.end(), is_name_char)) {
        return "the name may use only a-z, 0-9 and - (it becomes <name>.localhost)";
    }
    if (name.front() == '-' || name.back() == '-') {
        return "the name can't start or end with -";
    }
    // "xn--" starts an internationalised name, which browsers show differently.
    if (name.starts_with("xn--")) {
        return "the name can't start with xn--";
    }
    return std::nullopt;
}

std::string project_name_from(std::string_view folder_name) {
    std::string out;
    bool dash = false;
    for (const char raw : folder_name) {
        char c = raw;
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && !out.empty()) {
                out += '-';
            }
            dash = false;
            out += c;
            if (out.size() >= kMaxProjectNameLength) {
                break;
            }
        } else {
            dash = true;  // spaces, '_', '.', and every byte of a non-ASCII letter
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    if (out.empty() || project_name_problem(out)) {
        return "project";
    }
    return out;
}

std::optional<AppConfig> load_app_config(const fs::path& file, std::string& error) {
    std::error_code ec;
    if (!fs::exists(file, ec) && !ec) {
        return AppConfig{};
    }
    const auto text = read_small_file(file, kMaxConfigBytes);
    if (!text) {
        error = "can't read " + path_to_utf8(file);
        return std::nullopt;
    }
    json doc = json::parse(*text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        error = path_to_utf8(file) + " is not valid JSON";
        return std::nullopt;
    }

    AppConfig config;
    if (doc.contains("projects_dir")) {
        if (!doc["projects_dir"].is_string()) {
            error = path_to_utf8(file) + ": projects_dir must be a path";
            return std::nullopt;
        }
        const fs::path dir = path_from_utf8(doc["projects_dir"].get<std::string>());
        if (!dir.is_absolute()) {
            error = path_to_utf8(file) + ": projects_dir must be a full path";
            return std::nullopt;
        }
        config.projects_dir = dir.lexically_normal();
    }
    if (doc.contains("projects")) {
        const json& list = doc["projects"];
        if (!list.is_array() || list.size() > kMaxProjects) {
            error = path_to_utf8(file) + ": projects must be a list of at most " + std::to_string(kMaxProjects);
            return std::nullopt;
        }
        std::set<std::string, std::less<>> names;
        for (std::size_t i = 0; i < list.size(); ++i) {
            std::string why;
            auto project = read_project(list[i], i, why);
            if (!project) {
                error = path_to_utf8(file) + ": " + why;
                return std::nullopt;
            }
            if (!names.insert(project->name).second) {
                error = path_to_utf8(file) + ": the name " + project->name + " is used twice";
                return std::nullopt;
            }
            config.projects.push_back(std::move(*project));
        }
    }
    doc.erase("projects_dir");
    doc.erase("projects");
    config.other = std::move(doc);
    return config;
}

std::string write_app_config(const AppConfig& config) {
    json doc = config.other.is_object() ? config.other : json::object();
    if (config.projects_dir) {
        doc["projects_dir"] = generic_utf8(*config.projects_dir);
    }
    json list = json::array();
    for (const Project& p : config.projects) {
        json item = p.other.is_object() ? p.other : json::object();
        item["name"] = p.name;
        item["path"] = generic_utf8(p.dir);
        item["on"] = p.on;
        item["keep_running"] = p.keep_running;
        if (!p.database_name.empty()) {
            item["database_name"] = p.database_name;
        }
        list.push_back(std::move(item));
    }
    doc["projects"] = std::move(list);
    // A folder name that isn't valid UTF-8 can't reach here (paths come from
    // path_to_utf8), but never throw on the way out.
    return doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

bool save_app_config(const fs::path& file, const AppConfig& config, std::string& error) {
    return write_file_atomically(file, write_app_config(config), error);
}

Project* find_project(AppConfig& config, std::string_view name) {
    const auto it = std::find_if(config.projects.begin(), config.projects.end(),
                                 [name](const Project& p) { return p.name == name; });
    return it == config.projects.end() ? nullptr : &*it;
}

const Project* find_project(const AppConfig& config, std::string_view name) {
    const auto it = std::find_if(config.projects.begin(), config.projects.end(),
                                 [name](const Project& p) { return p.name == name; });
    return it == config.projects.end() ? nullptr : &*it;
}

const Project* project_for_folder(const AppConfig& config, const fs::path& dir) {
    const auto it = std::find_if(config.projects.begin(), config.projects.end(),
                                 [&dir](const Project& p) { return same_folder(p.dir, dir); });
    return it == config.projects.end() ? nullptr : &*it;
}

std::optional<fs::path> real_folder(const fs::path& dir, std::string& error) {
    if (!dir.is_absolute()) {
        error = "the folder must be a full path";
        return std::nullopt;
    }
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        error = "there's no folder " + path_to_utf8(dir);
        return std::nullopt;
    }
    fs::path real = fs::canonical(dir, ec);
    if (ec) {
        error = "can't use " + path_to_utf8(dir) + ": " + error_text(ec);
        return std::nullopt;
    }
#ifdef _WIN32
    // Some standard libraries return the long-path form "\\?\C:\...".
    std::wstring text = real.native();
    if (text.starts_with(LR"(\\?\)") && text.size() > 6 && text[5] == L':') {
        real = fs::path(text.substr(4));
    }
    const std::wstring root = real.root_name().native();
    if (root.size() != 2 || root[1] != L':') {
        error = path_to_utf8(dir) + " is on a network share; FloServer projects must be on a local drive";
        return std::nullopt;
    }
#endif
    return real;
}

std::optional<Project> add_project(AppConfig& config, const fs::path& dir, std::string_view name,
                                   std::string& error) {
    const auto real = real_folder(dir, error);
    if (!real) {
        return std::nullopt;
    }
    if (config.projects.size() >= kMaxProjects) {
        error = "FloServer can keep at most " + std::to_string(kMaxProjects) + " projects";
        return std::nullopt;
    }
    if (const Project* existing = project_for_folder(config, *real)) {
        error = path_to_utf8(*real) + " is already the project " + existing->name;
        return std::nullopt;
    }

    const fs::path& folder = *real;  // canonical: no trailing separator

    std::string chosen(name);
    if (chosen.empty()) {
        const std::string base = project_name_from(path_to_utf8(folder.filename()));
        chosen = base;
        // A renamed project may keep the database a name would get (D22).
        const auto taken = [&config](const std::string& n) {
            return find_project(config, n) != nullptr || reserved_project_name(n) ||
                   project_with_database(config, project_database_name(n)) != nullptr;
        };
        for (int n = 2; taken(chosen); ++n) {
            const std::string suffix = "-" + std::to_string(n);
            chosen = base.substr(0, kMaxProjectNameLength - suffix.size());
            while (!chosen.empty() && chosen.back() == '-') {
                chosen.pop_back();
            }
            chosen += suffix;
        }
    } else if (const auto problem = project_name_problem(chosen)) {
        error = *problem;
        return std::nullopt;
    } else if (find_project(config, chosen) != nullptr) {
        error = "there's already a project called " + chosen;
        return std::nullopt;
    } else if (reserved_project_name(chosen)) {
        error = chosen + ".localhost is kept for phpMyAdmin; choose another name";
        return std::nullopt;
    } else if (const Project* other = project_with_database(config, project_database_name(chosen))) {
        error = "the database " + project_database_name(chosen) + " is " + other->name + "'s; choose another name";
        return std::nullopt;
    }

    Project p{chosen, folder, false, false, json::object()};
    config.projects.push_back(p);
    return p;
}

bool remove_project(AppConfig& config, std::string_view name) {
    const auto it = std::find_if(config.projects.begin(), config.projects.end(),
                                 [name](const Project& p) { return p.name == name; });
    if (it == config.projects.end()) {
        return false;
    }
    config.projects.erase(it);
    return true;
}

const Project* project_with_database(const AppConfig& config, std::string_view database, std::string_view except) {
    const auto it = std::find_if(config.projects.begin(), config.projects.end(), [&](const Project& p) {
        return p.name != except && project_database_name(p) == database;
    });
    return it == config.projects.end() ? nullptr : &*it;
}

Project* rename_project(AppConfig& config, std::string_view name, std::string_view new_name, std::string& error) {
    Project* project = find_project(config, name);
    if (project == nullptr) {
        error = "there's no project called " + std::string(name);
        return nullptr;
    }
    const std::string chosen(new_name);
    if (chosen == name) {
        error = "the project is already called " + chosen;
        return nullptr;
    }
    if (const auto problem = project_name_problem(chosen)) {
        error = *problem;
        return nullptr;
    }
    if (find_project(config, chosen) != nullptr) {
        error = "there's already a project called " + chosen;
        return nullptr;
    }
    if (reserved_project_name(chosen)) {
        error = chosen + ".localhost is kept for phpMyAdmin; choose another name";
        return nullptr;
    }
    // The database keeps its name; one FloServer never made (it refuses
    // reserved names) has nothing to keep, so it follows the new name.
    std::string database = project_database_name(*project);
    if (reserved_database_name(database)) {
        database = project_database_name(chosen);
    }
    if (const Project* other = project_with_database(config, database, name)) {
        error = "the database " + database + " is " + other->name + "'s; choose another name";
        return nullptr;
    }
    project->name = chosen;
    // Written down only while it differs from what the name gives.
    project->database_name = database == project_database_name(chosen) ? std::string() : database;
    return project;
}

}  // namespace floserver::core

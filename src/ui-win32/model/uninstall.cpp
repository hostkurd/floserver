#include "model/uninstall.h"

#include "floserver/core/paths.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/shared_sites.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

namespace floserver::ui {

namespace fs = std::filesystem;

namespace {

constexpr unsigned kRuntimesBit = 1U;
constexpr unsigned kDatabasesBit = 2U;
constexpr unsigned kProjectsBit = 4U;

// As Windows writes it ('\' on Windows), for the question and the guard.
std::string shown_path(const fs::path& p) {
    fs::path out = p.lexically_normal();
    out.make_preferred();
    return core::path_to_utf8(out);
}

}  // namespace

unsigned uninstall_ticks_bits(const UninstallTicks& ticks) {
    return (ticks.runtimes ? kRuntimesBit : 0U) | (ticks.databases ? kDatabasesBit : 0U) |
           (ticks.projects ? kProjectsBit : 0U);
}

UninstallTicks uninstall_ticks_from(unsigned bits) {
    return {(bits & kRuntimesBit) != 0, (bits & kDatabasesBit) != 0, (bits & kProjectsBit) != 0};
}

UninstallFolders uninstall_folders(const fs::path& data) {
    UninstallFolders out;
    std::string error;
    const auto config = core::load_app_config(core::app_config_file(data), error);
    if (!config) {
        // A config.json that can't be read: which projects were added from
        // elsewhere isn't known, so the projects folder isn't offered at all,
        // and the clean-up deletes nothing (no project can be protected).
        out.readable = false;
        return out;
    }
    if (const auto dir = config->projects_dir ? config->projects_dir : core::default_projects_dir()) {
        out.projects_dir = shown_path(*dir);
    }
    for (const core::Project& p : config->projects) {
        if (!p.dir.empty()) {
            out.project_dirs.push_back(shown_path(p.dir));
        }
    }
    if (const auto shared = core::read_shared_config(*config, error)) {
        if (const auto dir = core::shared_dir(*shared)) {
            out.shared_dir = shown_path(*dir);
        }
    } else if (const auto dir = core::default_shared_dir()) {
        out.shared_dir = shown_path(*dir);  // kept all the same: the default is where it would be
    }
    return out;
}

FolderGuard projects_folder_guard(FolderGuard base, const UninstallFolders& folders) {
    const std::string projects = comparable_path(folders.projects_dir);
    if (!folders.shared_dir.empty()) {
        base.kept.push_back(folders.shared_dir);
    }
    base.other_projects.clear();
    for (const std::string& dir : folders.project_dirs) {
        // Inside the projects folder: it goes with it. Anywhere else (or not
        // a path that can be compared): never touched, nor anything holding it.
        if (!projects.empty() && same_or_inside(comparable_path(dir), projects)) {
            continue;
        }
        base.other_projects.push_back(dir);
    }
    return base;
}

UninstallQuestion uninstall_question(Lang lang, const std::string& projects_dir, const UninstallSizes& sizes) {
    const auto size = [](const std::optional<std::uint64_t>& bytes) {
        return bytes ? disk_size_text(*bytes) : std::string("…");
    };
    UninstallQuestion q;
    q.window_title = "FloServer";
    q.title = std::string(text(lang, Str::UninstallTitle));
    q.text = std::string(text(lang, Str::UninstallText));
    q.also_delete = std::string(text(lang, Str::UninstallAlsoDelete));
    q.runtimes = std::string(text(lang, Str::UninstallRuntimes));
    q.runtimes_note = fill(lang, Str::UninstallRuntimesNote, {size(sizes.runtimes)});
    q.databases = std::string(text(lang, Str::UninstallDatabases));
    q.databases_note = fill(lang, Str::UninstallDatabasesNote, {size(sizes.databases)});
    if (!projects_dir.empty()) {
        q.projects = std::string(text(lang, Str::UninstallProjects));
        q.projects_note = fill(lang, Str::UninstallProjectsNote, {projects_dir});
    }
    q.remove = std::string(text(lang, Str::UninstallConfirm));
    q.cancel = std::string(text(lang, Str::WizardCancel));
    return q;
}

std::vector<std::string> uninstall_problems(const UninstallReport& report, Lang lang) {
    std::vector<std::string> out;
    if (!report.skipped.empty()) {
        // The list isolates its paths itself.
        out.push_back(fill_pattern(text(lang, Str::UninstallLeft), false, {skipped_list_text(report.skipped, lang)}));
    }
    switch (report.projects) {
        case FolderOutcome::Stayed:
            out.push_back(fill(lang, Str::UninstallProjectsStayed, {report.projects_reason}));
            break;
        case FolderOutcome::Cancelled:
            out.emplace_back(text(lang, Str::UninstallProjectsCancelled));
            break;
        case FolderOutcome::Deleted:
            out.emplace_back(text(lang, Str::UninstallProjectsDeleted));
            break;
        case FolderOutcome::NotAsked:
        case FolderOutcome::Recycled:
        case FolderOutcome::Gone:
            break;
    }
    return out;
}

std::string uninstall_done_text(const UninstallReport& report, bool installed, Lang lang) {
    std::string out;
    for (const std::string& line : uninstall_problems(report, lang)) {
        out += line + "\n";
    }
    if (!out.empty()) {
        out += "\n";
    }
    out += text(lang, installed ? Str::UninstallNext : Str::UninstallDevBuild);
    return out;
}

bool is_product_code(std::string_view code) {
    // {8-4-4-4-12}, hex digits.
    constexpr std::string_view kShape = "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}";
    if (code.size() != kShape.size()) {
        return false;
    }
    for (std::size_t i = 0; i < code.size(); ++i) {
        const char want = kShape[i];
        const char c = code[i];
        if (want == 'x' ? std::isxdigit(static_cast<unsigned char>(c)) == 0 : c != want) {
            return false;
        }
    }
    return true;
}

std::string msiexec_uninstall_args(std::string_view product_code) {
    if (!is_product_code(product_code)) {
        return {};
    }
    return "/x " + std::string(product_code);
}

std::vector<UninstallChoiceText> uninstall_choices(const UninstallQuestion& q, const UninstallTicks& ticks) {
    std::vector<UninstallChoiceText> out;
    out.push_back({uninstall_ticks_bits({true, false, false}), q.runtimes, q.runtimes_note, ticks.runtimes});
    out.push_back({uninstall_ticks_bits({false, true, false}), q.databases, q.databases_note, ticks.databases});
    if (!q.projects.empty()) {
        out.push_back({uninstall_ticks_bits({false, false, true}), q.projects, q.projects_note, ticks.projects});
    }
    return out;
}

std::string remove_program_json(const RemoveProgramPlan& plan) {
    const nlohmann::json j = {{"product", plan.product}, {"x", plan.x},           {"y", plan.y},
                              {"deleted", plan.deleted}, {"kept", plan.kept},     {"problems", plan.problems},
                              {"data_dir", plan.data_dir}};
    return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::optional<RemoveProgramPlan> parse_remove_program_json(std::string_view text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) {
        return std::nullopt;
    }
    auto str = [&j](const char* key) -> std::optional<std::string> {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_string()) {
            return std::nullopt;
        }
        return it->get<std::string>();
    };
    auto list = [&j](const char* key) -> std::optional<std::vector<std::string>> {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_array()) {
            return std::nullopt;
        }
        std::vector<std::string> out;
        for (const auto& v : *it) {
            if (!v.is_string()) {
                return std::nullopt;
            }
            out.push_back(v.get<std::string>());
        }
        return out;
    };
    auto number = [&j](const char* key) -> std::optional<int> {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_number_integer()) {
            return std::nullopt;
        }
        return it->get<int>();
    };
    RemoveProgramPlan plan;
    const auto product = str("product");
    const auto deleted = str("deleted");
    const auto data_dir = str("data_dir");
    const auto kept = list("kept");
    const auto problems = list("problems");
    const auto x = number("x");
    const auto y = number("y");
    if (!product || !is_product_code(*product) || !deleted || !data_dir || !kept || !problems || !x || !y) {
        return std::nullopt;
    }
    plan.product = *product;
    plan.deleted = *deleted;
    plan.data_dir = *data_dir;
    plan.kept = *kept;
    plan.problems = *problems;
    plan.x = *x;
    plan.y = *y;
    return plan;
}

std::string uninstall_deleted_text(const UninstallTicks& ticks, bool projects_offered) {
    std::vector<std::string> gone;
    if (ticks.runtimes) {
        gone.emplace_back("downloaded runtimes and caches");
    }
    if (ticks.databases) {
        gone.emplace_back("databases");
    }
    if (ticks.projects && projects_offered) {
        gone.emplace_back("the projects folder (to the Recycle Bin)");
    }
    if (gone.empty()) {
        return "Your projects, databases and downloaded runtimes are still on this computer.";
    }
    std::string out;
    for (std::size_t i = 0; i < gone.size(); ++i) {
        if (i > 0) {
            out += i + 1 == gone.size() ? " and " : ", ";
        }
        out += gone[i];
    }
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out + " were deleted too.";
}

std::vector<std::string> uninstall_kept_lines(const UninstallTicks& ticks, const std::string& data_dir,
                                              const std::string& runtimes_dir, const std::string& projects_dir) {
    std::vector<std::string> out;
    if (!ticks.databases && !data_dir.empty()) {
        out.push_back("Databases: " + data_dir);
    }
    if (!ticks.projects && !projects_dir.empty()) {
        out.push_back("Projects: " + projects_dir);
    }
    if (!ticks.runtimes && !runtimes_dir.empty()) {
        out.push_back("Downloaded runtimes: " + runtimes_dir);
    }
    return out;
}

std::optional<std::string> parse_remove_program_args(const std::vector<std::string>& args) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == kRemoveProgramFlag && !args[i + 1].empty()) {
            return args[i + 1];
        }
    }
    return std::nullopt;
}

std::string remove_program_command(std::string_view exe, std::string_view plan_file) {
    const auto quotable = [](std::string_view s) {
        return !s.empty() && s.find_first_of("\"\r\n") == std::string_view::npos;
    };
    if (!quotable(exe) || !quotable(plan_file)) {
        return {};
    }
    return "\"" + std::string(exe) + "\" " + std::string(kRemoveProgramFlag) + " \"" + std::string(plan_file) + "\"";
}

}  // namespace floserver::ui

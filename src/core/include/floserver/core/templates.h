#pragma once

// Project templates (D9): data in the signed runtime list, so new ones ship
// without an app update. manifest.json carries them beside the runtimes:
//
//   "templates": [
//     {
//       "id": "flocms",                       // a-z 0-9 -, at most 32
//       "order": 20,                          // the cards' order on the New project screen
//       "title": {"en": "FloCMS", "ar": "...", "ku": "..."},        // en needed
//       "description": {"en": "A FloCMS site; opens in FloCMS Studio", ...},
//       "mark": "Flo", "mark_bg": "#0F6CBD", "mark_fg": "#FFFFFF",  // the card's badge
//       "chips": ["PHP", "MariaDB", "Composer"],
//       "config": {"php": "8.5", "database": "mariadb-11.4", "root": "public"},
//                                             // floserver.json defaults (its own rules)
//       "needs": ["composer"],                // more runtimes: "name" or "name version"
//       "copy": "wordpress",                  // a "needs" runtime whose files become the project
//       "files": [{"path": "public/index.php", "text": "<?php ..."}],
//       "steps": [{"tool": "composer", "args": ["create-project", "--no-interaction", "hostkurd/flocms", "{dir}"]}],
//       "env": true,                          // database lines into .env (FloCMS or Laravel keys)
//       "wordpress": true,                    // wp-config.php with the database and new salts
//       "studio": true                        // the window offers FloCMS Studio afterwards (D14)
//     }
//   ]
//
// Steps run the project's own runtimes through the shim rules (D10) with a
// fixed set of tools; {dir} is the new project folder and {name} its name.
// Nothing in a template runs a shell. A template that breaks a rule is
// skipped with a warning, like a runtime entry.

#include "floserver/core/project_config.h"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

struct TemplateFile {
    std::string path;  // relative, '/' separated, checked like zip entry names
    std::string text;
};

struct TemplateStep {
    std::string tool;               // composer, npm, npx, php, python, pip
    std::vector<std::string> args;  // {dir} and {name} are filled in
};

struct TemplateNeed {
    std::string name;     // a runtime name: "composer", "wordpress"
    std::string version;  // empty: the newest
};

struct ProjectTemplate {
    std::string id;
    int order = 0;
    std::map<std::string, std::string> title;        // language code -> text; "en" always there
    std::map<std::string, std::string> description;
    std::string mark;
    std::string mark_bg;  // "#RRGGBB"
    std::string mark_fg;
    std::vector<std::string> chips;
    nlohmann::ordered_json config = nlohmann::ordered_json::object();  // as written into floserver.json
    ProjectConfig parsed;                                               // the same, read by its rules
    std::vector<TemplateNeed> needs;
    std::string copy;
    std::vector<TemplateFile> files;
    std::vector<TemplateStep> steps;
    bool env = false;
    bool wordpress = false;
    bool studio = false;
};

// The tools a step may run.
[[nodiscard]] bool is_template_tool(std::string_view tool);

// One template from the list, or the reason it's skipped.
[[nodiscard]] std::optional<ProjectTemplate> parse_template(const nlohmann::json& j, std::string& problem);

// The "templates" array of a manifest, in card order; bad entries are
// skipped with a line in `warnings`.
[[nodiscard]] std::vector<ProjectTemplate> parse_templates(const nlohmann::json& list,
                                                           std::vector<std::string>& warnings);

// A template as the window shows it (templates.list): no files or steps.
[[nodiscard]] nlohmann::json template_summary(const ProjectTemplate& t);

// Every runtime a project from `t` needs, with `config` as its floserver.json
// (the template's own, or with the user's versions): its PHP (the newest
// when a PHP project names none), Node, Python, database, Apache, nginx,
// and the template's "needs". Versions are as written ("" for the newest).
[[nodiscard]] std::vector<TemplateNeed> template_runtime_needs(const ProjectTemplate& t, const ProjectConfig& config);

// A step's arguments with {dir} and {name} filled in.
[[nodiscard]] std::vector<std::string> fill_step_args(const TemplateStep& step, std::string_view dir,
                                                      std::string_view name);

}  // namespace floserver::core

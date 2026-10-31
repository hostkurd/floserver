#include "floserver/core/templates.h"

#include "floserver/core/version.h"

#include <algorithm>
#include <array>

namespace floserver::core {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxTemplates = 64;
constexpr std::size_t kMaxIdLength = 32;
constexpr std::size_t kMaxTextLength = 300;
constexpr std::size_t kMaxChips = 6;
constexpr std::size_t kMaxChipLength = 20;
constexpr std::size_t kMaxFiles = 32;
constexpr std::size_t kMaxFileBytes = std::size_t{64} * 1024;
constexpr std::size_t kMaxPathLength = 200;
constexpr std::size_t kMaxSteps = 8;
constexpr std::size_t kMaxArgs = 32;
constexpr std::size_t kMaxArgLength = 512;
constexpr std::size_t kMaxNeeds = 8;

constexpr std::array kTools = {std::string_view("composer"), std::string_view("npm"), std::string_view("npx"),
                               std::string_view("php"),      std::string_view("python"), std::string_view("pip")};
constexpr std::array kLanguages = {std::string_view("en"), std::string_view("ar"), std::string_view("ku")};

bool is_id(std::string_view s) {
    return !s.empty() && s.size() <= kMaxIdLength && s.front() != '-' && s.back() != '-' &&
           std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool is_hex_colour(std::string_view s) {
    return s.size() == 7 && s.front() == '#' && std::all_of(s.begin() + 1, s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
           });
}

bool printable_ascii(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c < 0x7F; });
}

// A starter file's path: relative, '/' separated, letters, digits, '.', '_'
// and '-' only, no "." or ".." parts, no part ending in a dot, and no
// Windows device names (the same file is written on every platform).
bool is_safe_path(std::string_view path) {
    if (path.empty() || path.size() > kMaxPathLength || path.front() == '/' || path.back() == '/') {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' ||
            !std::all_of(part.begin(), part.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                       c == '_' || c == '-';
            })) {
            return false;
        }
        std::string stem(part.substr(0, part.find('.')));
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; });
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
            (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '0' &&
             stem[3] <= '9')) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

// {"en": "...", "ar": "...", "ku": "..."}: English needed, the others optional.
bool read_texts(const json& j, const char* key, std::map<std::string, std::string>& out, std::string& problem) {
    if (!j.contains(key) || !j[key].is_object()) {
        problem = std::string("has no ") + key;
        return false;
    }
    for (const std::string_view lang : kLanguages) {
        const std::string code(lang);
        if (!j[key].contains(code)) {
            continue;
        }
        const json& v = j[key][code];
        if (!v.is_string() || v.get_ref<const std::string&>().empty() ||
            v.get_ref<const std::string&>().size() > kMaxTextLength) {
            problem = std::string("has a bad ") + key + " in " + code;
            return false;
        }
        out[code] = v.get<std::string>();
    }
    if (!out.contains("en")) {
        problem = std::string("has no English ") + key;
        return false;
    }
    return true;
}

}  // namespace

bool is_template_tool(std::string_view tool) {
    return std::find(kTools.begin(), kTools.end(), tool) != kTools.end();
}

std::optional<ProjectTemplate> parse_template(const json& j, std::string& problem) {
    if (!j.is_object()) {
        problem = "isn't an object";
        return std::nullopt;
    }
    ProjectTemplate t;
    if (!j.contains("id") || !j["id"].is_string() || !is_id(j["id"].get_ref<const std::string&>())) {
        problem = "has no usable id";
        return std::nullopt;
    }
    t.id = j["id"].get<std::string>();
    if (j.contains("order")) {
        if (!j["order"].is_number_integer() || j["order"].get<std::int64_t>() < 0 ||
            j["order"].get<std::int64_t>() > 10000) {
            problem = "has a bad order";
            return std::nullopt;
        }
        t.order = static_cast<int>(j["order"].get<std::int64_t>());
    }
    if (!read_texts(j, "title", t.title, problem) || !read_texts(j, "description", t.description, problem)) {
        return std::nullopt;
    }
    if (!j.contains("mark") || !j["mark"].is_string() || j["mark"].get_ref<const std::string&>().empty() ||
        j["mark"].get_ref<const std::string&>().size() > 4 || !printable_ascii(j["mark"].get_ref<const std::string&>())) {
        problem = "has no usable mark";
        return std::nullopt;
    }
    t.mark = j["mark"].get<std::string>();
    for (const char* key : {"mark_bg", "mark_fg"}) {
        if (!j.contains(key) || !j[key].is_string() || !is_hex_colour(j[key].get_ref<const std::string&>())) {
            problem = std::string("has no usable ") + key;
            return std::nullopt;
        }
    }
    t.mark_bg = j["mark_bg"].get<std::string>();
    t.mark_fg = j["mark_fg"].get<std::string>();
    if (j.contains("chips")) {
        if (!j["chips"].is_array() || j["chips"].size() > kMaxChips) {
            problem = "has bad chips";
            return std::nullopt;
        }
        for (const json& c : j["chips"]) {
            if (!c.is_string() || c.get_ref<const std::string&>().empty() ||
                c.get_ref<const std::string&>().size() > kMaxChipLength || !printable_ascii(c.get_ref<const std::string&>())) {
                problem = "has bad chips";
                return std::nullopt;
            }
            t.chips.push_back(c.get<std::string>());
        }
    }

    // The floserver.json it starts with, read by floserver.json's own rules.
    if (j.contains("config")) {
        if (!j["config"].is_object()) {
            problem = "has a config that isn't an object";
            return std::nullopt;
        }
        t.config = nlohmann::ordered_json::parse(j["config"].dump());
        const ConfigResult parsed = parse_project_config(j["config"].dump());
        if (!parsed.config || !parsed.warnings.empty()) {
            problem = "has a config floserver.json doesn't accept";
            return std::nullopt;
        }
        if (parsed.config->command) {
            problem = "has a config with a command";  // a template never sets one
            return std::nullopt;
        }
        t.parsed = *parsed.config;
    }

    if (j.contains("needs")) {
        if (!j["needs"].is_array() || j["needs"].size() > kMaxNeeds) {
            problem = "has bad needs";
            return std::nullopt;
        }
        for (const json& n : j["needs"]) {
            if (!n.is_string()) {
                problem = "has bad needs";
                return std::nullopt;
            }
            const auto& text = n.get_ref<const std::string&>();
            const std::size_t space = text.find(' ');
            TemplateNeed need{text.substr(0, space), space == std::string::npos ? std::string() : text.substr(space + 1)};
            if (!is_id(need.name) || (!need.version.empty() && !Version::parse(need.version))) {
                problem = "has a bad need \"" + text + "\"";
                return std::nullopt;
            }
            t.needs.push_back(std::move(need));
        }
    }
    if (j.contains("copy")) {
        if (!j["copy"].is_string() ||
            std::none_of(t.needs.begin(), t.needs.end(),
                         [&](const TemplateNeed& n) { return n.name == j["copy"].get_ref<const std::string&>(); })) {
            problem = "copies a runtime it doesn't need";
            return std::nullopt;
        }
        t.copy = j["copy"].get<std::string>();
    }

    if (j.contains("files")) {
        if (!j["files"].is_array() || j["files"].size() > kMaxFiles) {
            problem = "has bad files";
            return std::nullopt;
        }
        for (const json& f : j["files"]) {
            if (!f.is_object() || !f.contains("path") || !f["path"].is_string() || !f.contains("text") ||
                !f["text"].is_string() || !is_safe_path(f["path"].get_ref<const std::string&>()) ||
                f["text"].get_ref<const std::string&>().size() > kMaxFileBytes) {
                problem = "has a bad file";
                return std::nullopt;
            }
            t.files.push_back({f["path"].get<std::string>(), f["text"].get<std::string>()});
        }
    }

    if (j.contains("steps")) {
        if (!j["steps"].is_array() || j["steps"].size() > kMaxSteps) {
            problem = "has bad steps";
            return std::nullopt;
        }
        for (const json& s : j["steps"]) {
            if (!s.is_object() || !s.contains("tool") || !s["tool"].is_string() ||
                !is_template_tool(s["tool"].get_ref<const std::string&>()) || !s.contains("args") ||
                !s["args"].is_array() || s["args"].size() > kMaxArgs) {
                problem = "has a bad step";
                return std::nullopt;
            }
            TemplateStep step{s["tool"].get<std::string>(), {}};
            for (const json& a : s["args"]) {
                if (!a.is_string() || a.get_ref<const std::string&>().size() > kMaxArgLength ||
                    !printable_ascii(a.get_ref<const std::string&>())) {
                    problem = "has a bad step argument";
                    return std::nullopt;
                }
                step.args.push_back(a.get<std::string>());
            }
            t.steps.push_back(std::move(step));
        }
    }
    for (const char* key : {"env", "wordpress", "studio"}) {
        if (j.contains(key) && !j[key].is_boolean()) {
            problem = std::string("has a bad ") + key;
            return std::nullopt;
        }
    }
    t.env = j.value("env", false);
    t.wordpress = j.value("wordpress", false);
    t.studio = j.value("studio", false);
    if ((t.env || t.wordpress) && !t.parsed.database) {
        problem = "writes database settings but has no database";
        return std::nullopt;
    }
    return t;
}

std::vector<ProjectTemplate> parse_templates(const json& list, std::vector<std::string>& warnings) {
    std::vector<ProjectTemplate> out;
    if (!list.is_array()) {
        return out;
    }
    for (std::size_t i = 0; i < list.size() && i < kMaxTemplates; ++i) {
        std::string problem;
        auto t = parse_template(list[i], problem);
        if (!t) {
            warnings.push_back("template entry " + std::to_string(i) + " " + problem + " and was skipped");
            continue;
        }
        if (std::any_of(out.begin(), out.end(), [&](const ProjectTemplate& o) { return o.id == t->id; })) {
            warnings.push_back("template entry " + std::to_string(i) + " repeats the id " + t->id + " and was skipped");
            continue;
        }
        out.push_back(std::move(*t));
    }
    std::stable_sort(out.begin(), out.end(), [](const ProjectTemplate& a, const ProjectTemplate& b) { return a.order < b.order; });
    return out;
}

json template_summary(const ProjectTemplate& t) {
    json needs = json::array();
    for (const TemplateNeed& n : t.needs) {
        needs.push_back(n.version.empty() ? n.name : n.name + " " + n.version);
    }
    return {{"id", t.id},
            {"title", t.title},
            {"description", t.description},
            {"mark", t.mark},
            {"mark_bg", t.mark_bg},
            {"mark_fg", t.mark_fg},
            {"chips", t.chips},
            {"config", json::parse(t.config.dump())},
            {"needs", needs},
            {"studio", t.studio}};
}

std::vector<TemplateNeed> template_runtime_needs(const ProjectTemplate& t, const ProjectConfig& pc) {
    std::vector<TemplateNeed> needs;
    const auto need = [&needs](const std::string& name, const std::string& version) {
        if (std::none_of(needs.begin(), needs.end(), [&](const TemplateNeed& n) { return n.name == name; })) {
            needs.push_back({name, version});
        }
    };
    // PHP when it's asked for, or when the template is a PHP one whose version
    // was left to "the newest"; a static or Node/Python project, or one with
    // PHP off ("php": false), has none.
    if (php_asked(pc) || (!php_off(pc) && t.config.contains("php") && !pc.node && !pc.python)) {
        need("php", pc.php.value_or(""));
    }
    if (pc.node) {
        need("node", *pc.node);
    }
    if (pc.python) {
        need("python", *pc.python);
    }
    if (pc.database) {
        need(pc.database->family, pc.database->version);
    }
    if (pc.web == WebServer::Apache) {
        need("apache", "");
    }
    need("nginx", "");
    for (const TemplateNeed& n : t.needs) {
        need(n.name, n.version);
    }
    return needs;
}

std::vector<std::string> fill_step_args(const TemplateStep& step, std::string_view dir, std::string_view name) {
    std::vector<std::string> out;
    out.reserve(step.args.size());
    for (const std::string& a : step.args) {
        std::string filled;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a.compare(i, 5, "{dir}") == 0) {
                filled += dir;
                i += 4;
            } else if (a.compare(i, 6, "{name}") == 0) {
                filled += name;
                i += 5;
            } else {
                filled += a[i];
            }
        }
        out.push_back(std::move(filled));
    }
    return out;
}

}  // namespace floserver::core

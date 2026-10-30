#include "floserver/core/project_config.h"

#include "floserver/core/paths.h"
#include "floserver/core/version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <vector>

namespace floserver::core {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxExtensionLength = 32;
constexpr std::size_t kMaxCommandLength = 1000;

bool is_extension_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxExtensionLength) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// Windows device names open a device instead of a folder, with or without an
// extension ("nul", "CON.txt", "public/aux").
bool is_device_name(std::string_view part) {
    const std::string_view stem = part.substr(0, part.find('.'));
    if (stem.size() != 3 && stem.size() != 4) {
        return false;
    }
    std::string upper(stem);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    });
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
        return true;
    }
    return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) &&
           upper[3] >= '0' && upper[3] <= '9';
}

bool is_root_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.' || c == ' ' || c == '/';
}

// A document root must stay inside the project. The file is shared through git
// between Windows, macOS and Linux, so the check is plain text and the same
// everywhere, and it covers how Windows reads names: trailing dots and spaces
// are dropped (".. " is ".."), and device names are not folders. Only ASCII
// letters, digits, '_', '-', '.', spaces and '/' are allowed, which also keeps
// the value safe to write into nginx and Apache config files.
std::optional<std::string> normalise_root(std::string_view raw) {
    std::string text(raw);
    std::replace(text.begin(), text.end(), '\\', '/');
    if (!std::all_of(text.begin(), text.end(), is_root_char) || text.starts_with('/')) {
        return std::nullopt;
    }

    std::vector<std::string_view> kept;
    std::string_view rest(text);
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        if (part.empty() || part == ".") {
            continue;  // "public//x", "./public", "public/"
        }
        if (part == ".." || part.ends_with('.') || part.ends_with(' ') || part.starts_with(' ') ||
            is_device_name(part)) {
            return std::nullopt;
        }
        kept.push_back(part);
    }
    if (kept.empty()) {
        return ".";
    }
    std::string out;
    for (const std::string_view part : kept) {
        if (!out.empty()) {
            out += '/';
        }
        out += part;
    }
    return out;
}

void read_version(const json& doc, const char* key, std::optional<std::string>& out,
                  ConfigResult& result) {
    const auto it = doc.find(key);
    if (it == doc.end() || it->is_null()) {
        return;
    }
    if (!it->is_string()) {
        result.errors.push_back({key, R"(must be a version in quotes, such as "8.2")"});
        return;
    }
    const auto& text = it->get_ref<const std::string&>();
    const auto version = Version::parse(text);
    if (!version) {
        result.errors.push_back({key, "\"" + text + R"(" is not a version such as "8.2" or "8.2.27")"});
        return;
    }
    out = version->to_string();  // "08.2" is stored as "8.2"
}

void read_database(const json& doc, ProjectConfig& config, ConfigResult& result) {
    const auto it = doc.find("database");
    if (it == doc.end() || it->is_null()) {
        return;
    }
    if (!it->is_string()) {
        result.errors.push_back({"database", R"(must be text such as "mariadb-11.4")"});
        return;
    }
    const auto& text = it->get_ref<const std::string&>();
    const auto dash = text.find('-');
    std::string family = text.substr(0, dash);
    std::transform(family.begin(), family.end(), family.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (family != "mariadb") {
        // Q7: MariaDB only in 1.0. MySQL may come later through the manifest.
        result.errors.push_back({"database", "\"" + text +
                                                 R"(" isn't available; use MariaDB, such as "mariadb-11.4")"});
        return;
    }
    if (dash == std::string::npos) {
        result.errors.push_back({"database", "\"" + text + R"(" needs a version, such as "mariadb-11.4")"});
        return;
    }
    const auto version = Version::parse(std::string_view(text).substr(dash + 1));
    if (!version) {
        result.errors.push_back({"database", "\"" + text.substr(dash + 1) + R"(" is not a version such as "11.4")"});
        return;
    }
    DatabaseSpec spec{family, version->to_string()};
    config.database = std::move(spec);
}

void read_web(const json& doc, ProjectConfig& config, ConfigResult& result) {
    const auto it = doc.find("web");
    if (it == doc.end() || it->is_null()) {
        return;
    }
    if (it->is_string() && *it == "nginx") {
        config.web = WebServer::Nginx;
    } else if (it->is_string() && *it == "apache") {
        config.web = WebServer::Apache;
    } else {
        result.errors.push_back({"web", R"(must be "nginx" or "apache")"});
    }
}

void read_root(const json& doc, ProjectConfig& config, ConfigResult& result) {
    const auto it = doc.find("root");
    if (it == doc.end() || it->is_null()) {
        return;
    }
    if (!it->is_string()) {
        result.errors.push_back({"root", R"(must be a folder name in quotes, such as "public")"});
        return;
    }
    auto root = normalise_root(it->get_ref<const std::string&>());
    if (!root) {
        result.errors.push_back({"root", R"(must be a folder inside the project, such as "public" (letters, digits, _ - . and spaces))"});
        return;
    }
    config.root = std::move(*root);
}

void read_extensions(const json& doc, ProjectConfig& config, ConfigResult& result) {
    const auto it = doc.find("extensions");
    if (it == doc.end() || it->is_null()) {
        return;
    }
    if (!it->is_array()) {
        result.errors.push_back({"extensions", R"(must be a list, such as ["intl", "gd"])"});
        return;
    }
    std::set<std::string> seen;
    for (std::size_t i = 0; i < it->size(); ++i) {
        const json& item = (*it)[i];
        const std::string key = "extensions[" + std::to_string(i) + "]";
        if (!item.is_string() || !is_extension_name(item.get_ref<const std::string&>())) {
            result.errors.push_back({key, R"(must be an extension name such as "intl" (a-z, 0-9, _))"});
            continue;
        }
        const auto& name = item.get_ref<const std::string&>();
        if (!seen.insert(name).second) {
            result.warnings.push_back({key, "\"" + name + R"(" is listed twice)"});
            continue;
        }
        config.extensions.push_back(name);
    }
}

// A command to start the project's own server (D11), run by the system's
// shell in the project folder. One line, so a file can't hide a second
// command on a line the user doesn't see.
void read_command(const json& doc, ProjectConfig& config, ConfigResult& result) {
    const auto it = doc.find("command");
    if (it == doc.end() || it->is_null()) {
        return;
    }
    const auto bad = [](const std::string& s) {
        return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
    };
    if (!it->is_string() || it->get_ref<const std::string&>().empty() ||
        it->get_ref<const std::string&>().size() > kMaxCommandLength || bad(it->get_ref<const std::string&>())) {
        result.errors.push_back({"command", R"(must be one line of text, such as "npm run serve -- --port {port}")"});
        return;
    }
    config.command = it->get_ref<const std::string&>();
}

}  // namespace

ConfigResult parse_project_config(std::string_view json_text) {
    ConfigResult result;
    json doc;
    try {
        doc = json::parse(json_text);
    } catch (const json::parse_error& e) {
        result.errors.push_back({"", "is not valid JSON (near byte " + std::to_string(e.byte) + ")"});
        return result;
    }
    if (!doc.is_object()) {
        result.errors.push_back({"", "must be a JSON object: { ... }"});
        return result;
    }

    static const std::set<std::string, std::less<>> known = {"php",  "node",       "python", "database",
                                                             "web",  "root",       "extensions", "command"};
    for (const auto& item : doc.items()) {
        if (!known.contains(item.key())) {
            result.warnings.push_back({item.key(), "is not a FloServer setting and is ignored"});
        }
    }

    ProjectConfig config;
    read_version(doc, "php", config.php, result);
    read_version(doc, "node", config.node, result);
    read_version(doc, "python", config.python, result);
    read_database(doc, config, result);
    read_web(doc, config, result);
    read_root(doc, config, result);
    read_extensions(doc, config, result);
    read_command(doc, config, result);

    if (result.errors.empty()) {
        result.config = std::move(config);
    }
    return result;
}

std::string write_project_config(const ProjectConfig& config) {
    nlohmann::ordered_json doc = nlohmann::ordered_json::object();
    if (config.php) {
        doc["php"] = *config.php;
    }
    if (config.node) {
        doc["node"] = *config.node;
    }
    if (config.python) {
        doc["python"] = *config.python;
    }
    if (config.database) {
        doc["database"] = config.database->family + "-" + config.database->version;
    }
    if (config.web == WebServer::Apache) {
        doc["web"] = "apache";
    }
    if (config.root != ".") {
        doc["root"] = config.root;
    }
    if (!config.extensions.empty()) {
        doc["extensions"] = config.extensions;
    }
    if (config.command) {
        doc["command"] = *config.command;
    }
    return doc.dump(2) + "\n";
}

std::optional<std::filesystem::path> find_project_config(const std::filesystem::path& start) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::absolute(start, ec);
    if (ec) {
        return std::nullopt;
    }
    dir = dir.lexically_normal();
    while (true) {
        std::filesystem::path candidate = dir / kProjectConfigFileName;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
        std::filesystem::path parent = dir.parent_path();
        if (parent == dir || parent.empty()) {
            return std::nullopt;
        }
        dir = std::move(parent);
    }
}

std::optional<ConfigKey> parse_config_key(std::string_view name) {
    if (name == "php") {
        return ConfigKey::Php;
    }
    if (name == "node") {
        return ConfigKey::Node;
    }
    if (name == "python") {
        return ConfigKey::Python;
    }
    if (name == "database" || name == "mariadb") {
        return ConfigKey::Database;
    }
    if (name == "web") {
        return ConfigKey::Web;
    }
    return std::nullopt;
}

namespace {

const char* config_key_name(ConfigKey key) {
    switch (key) {
        case ConfigKey::Php:
            return "php";
        case ConfigKey::Node:
            return "node";
        case ConfigKey::Python:
            return "python";
        case ConfigKey::Database:
            return "database";
        case ConfigKey::Web:
            break;
    }
    return "web";
}

}  // namespace

std::optional<std::string> set_project_config_value(std::string_view text, ConfigKey key, std::string_view key_name,
                                                    const std::optional<std::string>& value, std::string& error) {
    nlohmann::ordered_json doc = nlohmann::ordered_json::object();
    if (text.find_first_not_of(" \t\r\n") != std::string_view::npos) {
        doc = nlohmann::ordered_json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) {
            error = "floserver.json isn't valid JSON; fix it first (flo check shows what's wrong)";
            return std::nullopt;
        }
    }
    const char* const name = config_key_name(key);
    std::optional<std::string> stored;
    switch (key) {
        case ConfigKey::Php:
        case ConfigKey::Node:
        case ConfigKey::Python:
            if (value) {
                const auto v = Version::parse(*value);
                if (!v) {
                    error = "\"" + *value + "\" is not a version such as 8.4 or 8.4.27";
                    return std::nullopt;
                }
                stored = v->to_string();
            }
            break;
        case ConfigKey::Database:
            if (value) {
                // `flo use mariadb 11.4` gives the version alone.
                if (key_name == "mariadb") {
                    const auto v = Version::parse(*value);
                    if (!v) {
                        error = "\"" + *value + "\" is not a version such as 11.4";
                        return std::nullopt;
                    }
                    stored = "mariadb-" + v->to_string();
                } else {
                    stored = *value;
                }
            }
            break;
        case ConfigKey::Web:
            if (value && *value == "apache") {
                stored = "apache";
            } else if (value && *value != "nginx") {
                error = "the web server is nginx or apache, not \"" + *value + "\"";
                return std::nullopt;
            }
            break;
    }
    if (stored) {
        doc[name] = *stored;
    } else {
        doc.erase(name);
    }
    std::string out = doc.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    const ConfigResult parsed = parse_project_config(out);
    if (!parsed.config) {
        const ConfigProblem& first = parsed.errors.empty() ? ConfigProblem{"", "can't be read"} : parsed.errors.front();
        error = "floserver.json: " + (first.key.empty() ? std::string() : first.key + " ") + first.message;
        return std::nullopt;
    }
    return out;
}

bool set_project_config_file_value(const std::filesystem::path& dir, ConfigKey key, std::string_view key_name,
                                   const std::optional<std::string>& value, std::string& error) {
    namespace fs = std::filesystem;
    constexpr std::size_t kMaxBytes = std::size_t{64} * 1024;
    const fs::path file = dir / kProjectConfigFileName;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(file, ec);
    std::string text;
    if (fs::exists(status)) {
        if (!fs::is_regular_file(status)) {
            error = "floserver.json isn't a plain file, so FloServer won't change it";
            return false;
        }
        const auto read = read_small_file(file, kMaxBytes);
        if (!read) {
            error = "floserver.json can't be read, or is larger than 64 KB";
            return false;
        }
        text = *read;
    }
    const auto out = set_project_config_value(text, key, key_name, value, error);
    return out && write_file_atomically(file, *out, error);
}

}  // namespace floserver::core

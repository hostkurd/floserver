#include "floserver/core/shared_sites.h"

#include "floserver/core/paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

bool is_label_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }

bool is_host_label(std::string_view s) {
    return !s.empty() && s.size() <= kMaxProjectNameLength && s.front() != '-' && s.back() != '-' &&
           !s.starts_with("xn--") && std::all_of(s.begin(), s.end(), is_label_char);
}

std::string html_escape(std::string_view s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

// A folder name as one URL path segment.
std::string url_segment(std::string_view s) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex.at(u >> 4U);
            out += kHex.at(u & 0xFU);
        }
    }
    return out;
}

bool exists_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

SharedKind kind_of(const fs::path& dir) {
    const bool index_php = exists_file(dir / "index.php");
    if (index_php) {
        return SharedKind::Php;
    }
    const bool index_html = exists_file(dir / "index.html") || exists_file(dir / "index.htm");
    std::error_code ec;
    bool php = false;
    bool any = false;
    int seen = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end && seen < 500; it.increment(ec), ++seen) {
        any = true;
        if (it->path().extension() == ".php") {
            php = true;
            break;
        }
    }
    if (php) {
        return SharedKind::Php;
    }
    if (index_html) {
        return SharedKind::Static;  // a built front end too (package.json beside it)
    }
    if (exists_file(dir / "package.json")) {
        return SharedKind::Node;
    }
    if (exists_file(dir / "requirements.txt") || exists_file(dir / "pyproject.toml") ||
        exists_file(dir / "manage.py")) {
        return SharedKind::Python;
    }
    return any ? SharedKind::Static : SharedKind::Empty;
}

}  // namespace

std::optional<fs::path> default_shared_dir() {
    const auto projects = default_projects_dir();  // <home>/FloServer/projects
    if (!projects) {
        return std::nullopt;
    }
    return projects->parent_path() / "www";
}

std::optional<fs::path> shared_dir(const SharedConfig& config) {
    return config.dir ? config.dir : default_shared_dir();
}

std::optional<SharedConfig> read_shared_config(const AppConfig& app, std::string& error) {
    SharedConfig out;
    if (!app.other.is_object() || !app.other.contains("shared")) {
        return out;
    }
    const json& doc = app.other["shared"];
    if (!doc.is_object()) {
        error = "\"shared\" must be an object";
        return std::nullopt;
    }
    if (doc.contains("on")) {
        if (!doc["on"].is_boolean()) {
            error = "shared.on must be true or false";
            return std::nullopt;
        }
        out.on = doc["on"].get<bool>();
    }
    if (doc.contains("dir")) {
        if (!doc["dir"].is_string()) {
            error = "shared.dir must be a path";
            return std::nullopt;
        }
        const fs::path dir = path_from_utf8(doc["dir"].get<std::string>());
        if (!dir.is_absolute()) {
            error = "shared.dir must be a full path";
            return std::nullopt;
        }
        out.dir = dir.lexically_normal();
    }
    // php, web and database are read by floserver.json's own rules.
    json as_project = json::object();
    for (const char* key : {"php", "web", "database"}) {
        if (doc.contains(key)) {
            as_project[key] = doc[key];
        }
    }
    const ConfigResult parsed = parse_project_config(as_project.dump());
    if (!parsed.config) {
        const ConfigProblem& first = parsed.errors.empty() ? ConfigProblem{"", "is unusable"} : parsed.errors.front();
        error = "shared." + (first.key.empty() ? std::string("settings") : first.key) + " " + first.message;
        return std::nullopt;
    }
    out.php = parsed.config->php;
    out.web = doc.contains("web") ? parsed.config->web : WebServer::Apache;
    out.database = parsed.config->database;
    return out;
}

void write_shared_config(AppConfig& app, const SharedConfig& shared) {
    if (!app.other.is_object()) {
        app.other = json::object();
    }
    json doc = json::object();
    doc["on"] = shared.on;
    if (shared.dir) {
        const std::u8string u = shared.dir->generic_u8string();
        doc["dir"] = std::string(u.begin(), u.end());
    }
    if (shared.php) {
        doc["php"] = *shared.php;
    }
    doc["web"] = shared.web == WebServer::Apache ? "apache" : "nginx";
    if (shared.database) {
        doc["database"] = shared.database->family + "-" + shared.database->version;
    }
    app.other["shared"] = std::move(doc);
}

ProjectConfig shared_project_config(const SharedConfig& shared, const std::string& php_version) {
    ProjectConfig pc;
    pc.php = php_version;
    pc.web = shared.web;
    pc.database = shared.database;
    return pc;
}

std::string_view shared_kind_name(SharedKind kind) {
    switch (kind) {
        case SharedKind::Php:
            return "php";
        case SharedKind::Static:
            return "static";
        case SharedKind::Node:
            return "node";
        case SharedKind::Python:
            return "python";
        case SharedKind::Empty:
            return "empty";
    }
    return "empty";
}

std::vector<SharedFolder> scan_shared_folders(const fs::path& dir, const AppConfig& app) {
    std::vector<SharedFolder> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_directory(type_ec) || type_ec) {
            continue;
        }
        const std::string name = path_to_utf8(it->path().filename());
        if (name.empty() || name.front() == '.') {
            continue;
        }
        SharedFolder f;
        f.name = name;
        f.kind = kind_of(it->path());
        f.host_name = is_host_label(name);
        if (f.host_name && find_project(app, name) != nullptr) {
            f.clash = name;
        }
        out.push_back(std::move(f));
        if (out.size() >= kMaxSharedFolders) {
            break;
        }
    }
    std::sort(out.begin(), out.end(), [](const SharedFolder& a, const SharedFolder& b) { return a.name < b.name; });
    return out;
}

std::string shared_port_suffix(const WebPorts& ports) {
    return ports.http == 80 ? std::string() : ":" + std::to_string(ports.http);
}

std::string shared_address(const WebPorts& ports) { return "http://localhost" + shared_port_suffix(ports); }

std::string shared_index_page(const std::vector<SharedFolder>& folders, const fs::path& dir,
                              const std::string& php_version, WebServer web, const std::string& base_url,
                              const std::string& host_suffix) {
    std::ostringstream o;
    o << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
      << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      << "<title>Shared sites</title>\n<style>\n"
      << ":root{color-scheme:light dark;--bg:#f7f7f8;--card:#fff;--line:#e1e1e1;--text:#1b1b1b;--muted:#5f5f5f;--link:#0f6cbd;--warn:#7a4a00}\n"
      << "@media (prefers-color-scheme:dark){:root{--bg:#1c1c1c;--card:#262626;--line:#3a3a3a;--text:#f0f0f0;--muted:#b0b0b0;--link:#6cb4ff;--warn:#f0c060}}\n"
      << "body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 'Segoe UI',system-ui,sans-serif}\n"
      << "main{max-width:860px;margin:0 auto;padding:28px 16px}\n"
      << "h1{margin:0;font-size:22px;font-weight:600}.sub{color:var(--muted);font-size:13px;margin:4px 0 16px}\n"
      << "ul{list-style:none;margin:0;padding:0;background:var(--card);border:1px solid var(--line);border-radius:8px;overflow:hidden}\n"
      << "li{display:flex;gap:12px;align-items:baseline;padding:11px 16px;border-bottom:1px solid var(--line);flex-wrap:wrap}\n"
      << "li:last-child{border-bottom:0}a{color:var(--link);text-decoration:none}a:hover{text-decoration:underline}\n"
      << ".name{font-weight:600;flex:1;min-width:160px}.note{color:var(--muted);font-size:12px}.warn{color:var(--warn);font-size:12px}\n"
      << "p.foot{color:var(--muted);font-size:12px;margin:12px 2px}\n</style>\n</head>\n<body>\n<main>\n"
      << "<h1>Shared sites</h1>\n<p class=\"sub\">" << html_escape(path_to_utf8(dir)) << " · PHP "
      << html_escape(php_version) << " · " << (web == WebServer::Apache ? "Apache" : "nginx")
      << " · served by FloServer</p>\n";
    if (folders.empty()) {
        o << "<p>The www folder is empty. Put a folder with an index.php or index.html in it and reload this page.</p>\n";
    } else {
        o << "<ul>\n";
        for (const SharedFolder& f : folders) {
            const std::string name = html_escape(f.name);
            o << R"(<li><a class="name" href=")" << base_url << "/" << url_segment(f.name) << R"(/">)" << name
              << "/</a>";
            if (f.kind == SharedKind::Node || f.kind == SharedKind::Python) {
                o << "<span class=\"warn\">" << (f.kind == SharedKind::Node ? "Node.js" : "Python")
                  << " app: make it a project in FloServer</span>";
            } else if (f.clash) {
                o << "<span class=\"note\">" << name << ".localhost is the project " << name << "</span>";
            } else if (f.host_name) {
                o << R"(<a class="note" href="http://)" << f.name << ".localhost" << host_suffix << R"(/">)" << name
                  << ".localhost" << html_escape(host_suffix) << "</a>";
            }
            o << "</li>\n";
        }
        o << "</ul>\n";
    }
    o << "<p class=\"foot\">Put your own index.php or index.html in the www folder to show it here instead.</p>\n"
      << "</main>\n</body>\n</html>\n";
    return o.str();
}

}  // namespace floserver::core

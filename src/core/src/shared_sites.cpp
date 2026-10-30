#include "floserver/core/shared_sites.h"

#include "floserver/core/paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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
    // Keys this version doesn't know stay.
    json doc = app.other.contains("shared") && app.other["shared"].is_object() ? app.other["shared"] : json::object();
    for (const char* key : {"dir", "php", "database"}) {
        doc.erase(key);
    }
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

std::string shared_address(const WebPorts& ports) {
    return ports.http == 80 ? std::string("http://localhost") : "http://localhost:" + std::to_string(ports.http);
}

namespace {

// A PHP single-quoted string literal.
std::string php_string(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\\' || c == '\'') {
            out += '\\';
        }
        out += c;
    }
    return out + "'";
}


constexpr std::string_view kListScript = R"FLOPHP(<?php
// Written by FloServer: the list of Shared sites (D19). It reads the www
// folder only when this page is opened; nothing else reads it.
$www = @WWW@;
$config = @CONFIG@;
$web = @WEB@;
header('Content-Type: text/html; charset=utf-8');
header('Cache-Control: no-store');
function fs_h($s) { return htmlspecialchars((string)$s, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8'); }
function fs_kind($dir) {
    if (is_file("$dir/index.php")) { return 'php'; }
    $names = @scandir($dir) ?: [];
    $count = 0;
    foreach ($names as $n) {
        if (++$count > 500) { break; }
        if (strcasecmp(pathinfo($n, PATHINFO_EXTENSION), 'php') === 0) { return 'php'; }
    }
    if (is_file("$dir/index.html") || is_file("$dir/index.htm")) { return 'static'; }
    if (is_file("$dir/package.json")) { return 'node'; }
    if (is_file("$dir/requirements.txt") || is_file("$dir/pyproject.toml") || is_file("$dir/manage.py")) { return 'python'; }
    return count($names) > 2 ? 'static' : 'empty';
}
$projects = [];
$c = json_decode((string)@file_get_contents($config), true);
if (is_array($c) && isset($c['projects']) && is_array($c['projects'])) {
    foreach ($c['projects'] as $p) {
        if (is_array($p) && isset($p['name']) && is_string($p['name'])) { $projects[$p['name']] = true; }
    }
}
$host = isset($_SERVER['HTTP_HOST']) ? (string)$_SERVER['HTTP_HOST'] : 'localhost';
$port = preg_match('/:(\d{1,5})$/', $host, $m) ? ':' . $m[1] : '';
$base = 'http://localhost' . $port;
$folders = [];
foreach ((@scandir($www) ?: []) as $n) {
    if ($n === '' || $n[0] === '.' || !is_dir("$www/$n")) { continue; }
    $folders[] = $n;
    if (count($folders) >= 1000) { break; }
}
sort($folders, SORT_STRING);
?><!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Shared sites</title>
<style>
:root{color-scheme:light dark;--bg:#f7f7f8;--card:#fff;--line:#e1e1e1;--text:#1b1b1b;--muted:#5f5f5f;--link:#0f6cbd;--warn:#7a4a00}
@media (prefers-color-scheme:dark){:root{--bg:#1c1c1c;--card:#262626;--line:#3a3a3a;--text:#f0f0f0;--muted:#b0b0b0;--link:#6cb4ff;--warn:#f0c060}}
body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 'Segoe UI',system-ui,sans-serif}
main{max-width:860px;margin:0 auto;padding:28px 16px}
h1{margin:0;font-size:22px;font-weight:600}.sub{color:var(--muted);font-size:13px;margin:4px 0 16px}
ul{list-style:none;margin:0;padding:0;background:var(--card);border:1px solid var(--line);border-radius:8px;overflow:hidden}
li{display:flex;gap:12px;align-items:baseline;padding:11px 16px;border-bottom:1px solid var(--line);flex-wrap:wrap}
li:last-child{border-bottom:0}a{color:var(--link);text-decoration:none}a:hover{text-decoration:underline}
.name{font-weight:600;flex:1;min-width:160px}.note{color:var(--muted);font-size:12px}.warn{color:var(--warn);font-size:12px}
p.foot{color:var(--muted);font-size:12px;margin:12px 2px}
</style>
</head>
<body>
<main>
<h1>Shared sites</h1>
<p class="sub"><?= fs_h($www) ?> · PHP <?= fs_h(PHP_VERSION) ?> · <?= fs_h($web) ?> · served by FloServer</p>
<?php if (!$folders): ?>
<p>The www folder is empty. Put a folder with an index.php or index.html in it and reload this page.</p>
<?php else: ?>
<ul>
<?php foreach ($folders as $n):
    $kind = fs_kind("$www/$n");
    $label = preg_match('/^(?!xn--)[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/D', $n) === 1; ?>
<li><a class="name" href="<?= fs_h($base . '/' . rawurlencode($n) . '/') ?>"><?= fs_h($n) ?>/</a>
<?php if ($kind === 'node' || $kind === 'python'): ?>
<span class="warn"><?= $kind === 'node' ? 'Node.js' : 'Python' ?> app: make it a project in FloServer</span>
<?php elseif ($label && isset($projects[$n])): ?>
<span class="note"><?= fs_h($n) ?>.localhost is the project <?= fs_h($n) ?></span>
<?php elseif ($label): ?>
<a class="note" href="<?= fs_h('http://' . $n . '.localhost' . $port . '/') ?>"><?= fs_h($n . '.localhost' . $port) ?></a>
<?php endif; ?>
</li>
<?php endforeach; ?>
</ul>
<?php endif; ?>
<p class="foot">Put your own index.php or index.html in the www folder to show it here instead.</p>
</main>
</body>
</html>
)FLOPHP";

}  // namespace

std::string shared_list_script(const fs::path& dir, const fs::path& config_json, WebServer web) {
    const auto text = [](const fs::path& p) {
        const std::u8string u = p.generic_u8string();
        return std::string(u.begin(), u.end());
    };
    // One pass over the template: an inserted value is never searched again.
    const std::array<std::pair<std::string_view, std::string>, 3> values = {{
        {"@WWW@", php_string(text(dir))},
        {"@CONFIG@", php_string(text(config_json))},
        {"@WEB@", php_string(web == WebServer::Apache ? "Apache" : "nginx")},
    }};
    std::string out;
    std::string_view rest = kListScript;
    while (!rest.empty()) {
        const auto at = rest.find('@');
        if (at == std::string_view::npos) {
            out += rest;
            break;
        }
        out += rest.substr(0, at);
        rest.remove_prefix(at);
        bool replaced = false;
        for (const auto& [key, value] : values) {
            if (rest.starts_with(key)) {
                out += value;
                rest.remove_prefix(key.size());
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            out += '@';
            rest.remove_prefix(1);
        }
    }
    return out;
}

}  // namespace floserver::core

#include "floserver/core/apache_settings.h"

#include "floserver/core/apache_config.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_service.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/site_plan.h"
#include "floserver/core/version.h"
#include "floserver/core/web_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <system_error>
#include <utility>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::size_t kMaxModuleName = 40;
constexpr std::size_t kMaxLine = 1000;
constexpr std::size_t kMaxCustomBytes = std::size_t{256} * 1024;

// FloServer's own config uses these (apache_config.cpp): always loaded.
struct Needed {
    std::string_view name;
    std::string_view why;
};
constexpr std::array<Needed, 8> kNeeded{{
    {"authz_core", "require"},      // Require all denied / granted
    {"mime", "types"},              // TypesConfig
    {"dir", "index"},               // DirectoryIndex
    {"alias", "hidden"},            // RedirectMatch 404 for dot files
    {"setenvif", "https"},          // HTTPS=on from nginx's X-Forwarded-Proto
    {"proxy", "fcgi"},              // PHP through the pools
    {"proxy_fcgi", "fcgi"},
    {"vhost_alias", "shared"},      // shared sites' <folder>.localhost
}};

// What .htaccess files commonly use: on unless turned off. With kNeeded, in
// the order the config has always loaded them.
constexpr std::array<std::string_view, 22> kBaseOrder{
    "authz_core", "authz_host", "authz_user", "authn_core", "authn_file", "auth_basic", "access_compat",
    "log_config", "mime",       "dir",        "alias",      "rewrite",    "env",        "setenvif",
    "headers",    "expires",    "filter",     "deflate",    "version",    "proxy",      "proxy_fcgi",
    "vhost_alias"};

// Modules that need others loaded first.
struct Needs {
    std::string_view name;
    std::array<std::string_view, 3> needs;
};
constexpr std::array<Needs, 38> kNeeds{{
    {"auth_basic", {"authn_core"}},
    {"auth_digest", {"authn_core"}},
    {"auth_form", {"authn_core", "session", "request"}},
    {"authn_anon", {"authn_core"}},
    {"authn_dbd", {"authn_core", "dbd"}},
    {"authn_dbm", {"authn_core"}},
    {"authn_file", {"authn_core"}},
    {"authn_socache", {"authn_core", "socache_shmcb"}},
    {"authnz_ldap", {"ldap", "authn_core"}},
    {"authz_dbd", {"dbd"}},
    {"deflate", {"filter"}},
    {"brotli", {"filter"}},
    {"cache_disk", {"cache"}},
    {"cache_socache", {"cache", "socache_shmcb"}},
    {"session_cookie", {"session"}},
    {"session_crypto", {"session"}},
    {"session_dbd", {"session", "dbd"}},
    {"dav_fs", {"dav"}},
    {"dav_lock", {"dav"}},
    {"proxy_ajp", {"proxy"}},
    {"proxy_balancer", {"proxy", "slotmem_shm", "lbmethod_byrequests"}},
    {"proxy_connect", {"proxy"}},
    {"proxy_express", {"proxy"}},
    {"proxy_fdpass", {"proxy"}},
    {"proxy_ftp", {"proxy"}},
    {"proxy_hcheck", {"proxy", "watchdog"}},
    {"proxy_html", {"proxy", "xml2enc"}},
    {"proxy_http", {"proxy"}},
    {"proxy_http2", {"proxy"}},
    {"proxy_scgi", {"proxy"}},
    {"proxy_uwsgi", {"proxy"}},
    {"proxy_wstunnel", {"proxy"}},
    {"lbmethod_bybusyness", {"proxy_balancer"}},
    {"lbmethod_byrequests", {"proxy_balancer"}},
    {"lbmethod_bytraffic", {"proxy_balancer"}},
    {"lbmethod_heartbeat", {"proxy_balancer", "heartmonitor"}},
    {"heartbeat", {"watchdog"}},
    {"heartmonitor", {"watchdog"}},
}};

// Modules that don't apply behind nginx: never loaded, and why.
struct Off {
    std::string_view name;
    std::string_view why;
};
constexpr std::array<Off, 6> kOff{{
    {"ssl", "https"},        // nginx handles HTTPS; Apache only hears nginx, on this computer
    {"md", "https"},         // certificates come from FloServer's own authority
    {"http2", "http2"},      // nginx speaks HTTP/2 to browsers, HTTP/1.1 to Apache
    {"mpm_prefork", "mpm"},  // FloServer chooses how Apache runs
    {"mpm_worker", "mpm"},
    {"suexec", "suexec"},    // runs scripts as other users
}};

const Needs* needs_of(std::string_view name) {
    for (const Needs& n : kNeeds) {
        if (n.name == name) {
            return &n;
        }
    }
    return nullptr;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trimmed(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t");
    return std::string(s.substr(first, last - first + 1));
}

// The first argument as Apache reads it: in quotes (either kind) or up to a space.
std::string first_argument(std::string_view args) {
    std::string a = trimmed(args);
    if (a.empty()) {
        return a;
    }
    if (a.front() == '"' || a.front() == '\'') {
        const auto end = a.find(a.front(), 1);
        return a.substr(1, end == std::string::npos ? std::string::npos : end - 1);
    }
    return a.substr(0, a.find_first_of(" \t"));
}

// A log file's path is FloServer's logs folder or below it.
bool inside(const fs::path& dir, std::string_view text) {
    if (text.empty() || text.find('$') != std::string_view::npos) {
        return false;
    }
    std::string s(text);
    std::replace(s.begin(), s.end(), '\\', '/');
    const fs::path p = path_from_utf8(s).lexically_normal();
    if (!p.is_absolute()) {
        return false;  // relative to ServerRoot: the Apache install's folder
    }
    std::string base = path_to_utf8(dir.lexically_normal());
    std::string full = path_to_utf8(p);
    std::replace(base.begin(), base.end(), '\\', '/');
    std::replace(full.begin(), full.end(), '\\', '/');
#ifdef _WIN32
    base = lower(base);
    full = lower(full);
#endif
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return full.size() > base.size() + 1 && full.starts_with(base) && full[base.size()] == '/' &&
           full.find("/../") == std::string::npos;
}

}  // namespace

bool is_apache_module_name(std::string_view name) {
    return !name.empty() && name.size() <= kMaxModuleName &&
           std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

ApacheModuleInfo apache_module_info(std::string_view name, bool posix) {
    ApacheModuleInfo info;
    if (const Needs* n = needs_of(name)) {
        for (const std::string_view need : n->needs) {
            if (!need.empty()) {
                info.needs.emplace_back(need);
            }
        }
    }
    if (posix && (name == "mpm_event" || name == "unixd")) {
        info.kind = ApacheModuleKind::Needed;
        info.why = "mpm";
        return info;
    }
    for (const Needed& n : kNeeded) {
        if (n.name == name) {
            info.kind = ApacheModuleKind::Needed;
            info.why = n.why;
            return info;
        }
    }
    for (const Off& o : kOff) {
        if (o.name == name) {
            info.kind = ApacheModuleKind::Off;
            info.why = o.why;
            return info;
        }
    }
    if (std::find(kBaseOrder.begin(), kBaseOrder.end(), name) != kBaseOrder.end()) {
        info.kind = ApacheModuleKind::Default;
    }
    return info;
}

ApacheSettings read_apache_settings(const AppConfig& app, std::vector<std::string>* problems) {
    ApacheSettings out;
    const auto problem = [problems](const std::string& what) {
        if (problems != nullptr) {
            problems->push_back("config.json: apache " + what);
        }
    };
    // A copy: GCC's -O3 null-dereference check trips on iterators into nlohmann::json.
    const json stored = app.other.is_object() && app.other.contains("apache") ? app.other.at("apache") : json();
    if (stored.is_null()) {
        return out;
    }
    if (!stored.is_object()) {
        problem(R"(must be an object such as {"modules": {"expires": true}})");
        return out;
    }
    if (stored.contains("version")) {
        const json& v = stored.at("version");
        if (v.is_string() && Version::parse(v.get<std::string>())) {
            out.version = v.get<std::string>();
        } else if (!v.is_null()) {
            problem("version must be a version such as \"2.4.69\"");
        }
    }
    if (stored.contains("modules")) {
        const json& modules = stored.at("modules");
        if (!modules.is_object()) {
            problem("modules must be an object of true or false");
        } else {
            for (const auto& [name, on] : modules.items()) {
                if (!is_apache_module_name(name) || !on.is_boolean()) {
                    problem("modules " + name + " is left out");
                    continue;
                }
                out.modules[name] = on.get<bool>();
            }
        }
    }
    return out;
}

void write_apache_settings(AppConfig& app, const ApacheSettings& settings) {
    if (!app.other.is_object()) {
        app.other = json::object();
    }
    json& entry = app.other["apache"];
    if (!entry.is_object()) {
        entry = json::object();
    }
    if (settings.version) {
        entry["version"] = *settings.version;
    } else {
        entry.erase("version");
    }
    if (settings.modules.empty()) {
        entry.erase("modules");
    } else {
        // The object as it was, so a key this version doesn't read survives.
        json& modules = entry["modules"];
        modules = json::object();
        for (const auto& [name, on] : settings.modules) {
            modules[name] = on;
        }
    }
    if (entry.empty()) {
        app.other.erase("apache");
    }
}

std::vector<std::string> apache_modules_in(const fs::path& modules_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(modules_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string file = lower(path_to_utf8(it->path().filename()));
        if (!file.starts_with("mod_") || !file.ends_with(".so")) {
            continue;
        }
        std::string name = file.substr(4, file.size() - 7);
        if (is_apache_module_name(name)) {
            out.push_back(std::move(name));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

ApacheModulePlan plan_apache_modules(const std::map<std::string, bool>& changes,
                                     const std::function<bool(const std::string&)>& present, bool posix) {
    ApacheModulePlan plan;
    std::set<std::string> loaded;
    std::set<std::string> visiting;
    const auto on_by_itself = [&](const std::string& name) {
        const ApacheModuleInfo info = apache_module_info(name, posix);
        if (info.kind == ApacheModuleKind::Needed) {
            return true;
        }
        if (info.kind == ApacheModuleKind::Off) {
            return false;
        }
        const auto it = changes.find(name);
        return it != changes.end() ? it->second : info.kind == ApacheModuleKind::Default;
    };
    // Loads `name` after what it needs; false when it (or a need) isn't there.
    std::function<bool(const std::string&, const std::string&)> add = [&](const std::string& name,
                                                                         const std::string& for_whom) -> bool {
        const ApacheModuleInfo info = apache_module_info(name, posix);
        if (info.kind == ApacheModuleKind::Off) {
            return false;
        }
        if (loaded.contains(name)) {
            if (!for_whom.empty() && !on_by_itself(name)) {
                plan.needed_by[name].push_back(for_whom);
            }
            return true;
        }
        if (visiting.contains(name)) {
            return true;  // a loop in the table: loaded once
        }
        if (info.kind != ApacheModuleKind::Needed && !present(name)) {
            return false;
        }
        visiting.insert(name);
        for (const std::string& need : info.needs) {
            if (!add(need, name)) {
                visiting.erase(name);
                return false;
            }
        }
        visiting.erase(name);
        loaded.insert(name);
        plan.load.push_back(name);
        if (!for_whom.empty() && !on_by_itself(name)) {
            plan.needed_by[name].push_back(for_whom);
        }
        return true;
    };
    for (const std::string_view base : kBaseOrder) {
        const std::string name(base);
        if (on_by_itself(name) && !add(name, {}) && apache_module_info(name, posix).kind != ApacheModuleKind::Default) {
            plan.missing.push_back(name);
        }
    }
    for (const auto& [name, on] : changes) {
        if (!on || loaded.contains(name) || !is_apache_module_name(name) ||
            apache_module_info(name, posix).kind == ApacheModuleKind::Off) {
            continue;
        }
        if (!add(name, {})) {
            plan.missing.push_back(name);
        }
    }
    return plan;
}

fs::path apache_custom_conf_file(const fs::path& data) { return data / "apache" / "custom.conf"; }

std::string apache_custom_conf_header() {
    return "# Your own httpd.conf lines for FloServer's shared Apache.\n"
           "# FloServer adds them at the end of the config it writes, so a line here wins.\n"
           "# Apache stays on this computer behind nginx: lines that would change where it listens, its\n"
           "# files or its modules (use the tick boxes in Settings > Apache) are left out, and so is\n"
           "# <VirtualHost> (Show the final httpd.conf says which). Comments here aren't copied.\n"
           "# Changes are used within a few seconds of saving this file, once Apache has checked them.\n\n";
}

bool ensure_apache_custom_conf(const fs::path& data, std::string& error) {
    const fs::path file = apache_custom_conf_file(data);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(file, ec))) {
        return true;
    }
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "can't make " + path_to_utf8(file.parent_path()) + ": " + error_text(ec);
        return false;
    }
    return write_file_atomically(file, apache_custom_conf_header(), error);
}

std::string read_apache_custom_conf(const fs::path& data) {
    const fs::path file = apache_custom_conf_file(data);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return {};
    }
    std::string text = read_small_file(file, kMaxCustomBytes).value_or(std::string());
    if (text == apache_custom_conf_header()) {
        return {};
    }
    return text;
}

std::optional<std::string> apache_directive_problem(std::string_view name_text, std::string_view args,
                                                    const fs::path& logs_dir) {
    std::string_view bare = name_text;
    const bool section = bare.starts_with("<");
    if (bare.starts_with("</")) {
        bare.remove_prefix(2);
    } else if (section) {
        bare.remove_prefix(1);
    }
    if (bare.ends_with(">")) {
        bare.remove_suffix(1);
    }
    const std::string name = lower(bare);
    if (name == "listen" || name == "serverroot" || name == "pidfile" || name == "mutex" ||
        name == "defaultruntimedir" || name == "scoreboardfile" || name == "user" || name == "group" ||
        name == "coredumpdirectory") {
        return std::string(bare) + " is FloServer's: Apache listens only on 127.0.0.1 behind nginx, with its own files";
    }
    if (name == "include" || name == "includeoptional") {
        return std::string(bare) + " can't be used: every line must be here, where FloServer can check it";
    }
    if (name == "loadmodule" || name == "loadfile") {
        return "modules are turned on with the tick boxes in Settings > Apache";
    }
    if (name == "virtualhost") {
        return "<VirtualHost> can't be used: each project's host is FloServer's, and nginx only sends Apache those";
    }
    if (name == "define" || name == "undefine") {
        return std::string(bare) + " can't be used: FloServer's config doesn't read variables";
    }
    if (name == "errorlog") {
        return "ErrorLog is FloServer's (logs/apache-error.log), shown in the project's Log tab";
    }
    if (name == "customlog" || name == "transferlog" || name == "globallog" || name == "forensiclog" ||
        name == "scriptlog") {
        const std::string file = first_argument(args);
        if (file.starts_with("|") || lower(file).starts_with("syslog")) {
            return std::string(bare) + " can't send the log to a program";
        }
        if (!inside(logs_dir, file)) {
            return std::string(bare) + " may only write in FloServer's logs folder (" + path_to_utf8(logs_dir) +
                   "), with the full path";
        }
        return std::nullopt;
    }
    if (name == "rewritemap" &&
        (lower(args).find("prg:") != std::string::npos || args.find("${") != std::string_view::npos)) {
        return "a RewriteMap can't run a program";
    }
    if (name == "extfilterdefine") {
        return "ExtFilterDefine can't be used: it runs a program";
    }
    return std::nullopt;
}

FilteredApacheConf filter_apache_custom_conf(std::string_view text, const fs::path& logs_dir) {
    FilteredApacheConf out;
    std::size_t pos = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;  // Notepad's "UTF-8 with BOM"
    std::size_t number = 0;
    // A refused section is left out up to its end: its lines would
    // otherwise apply to everything.
    std::string skipping;
    int depth = 0;
    std::size_t skip_from = 0;
    while (pos < text.size()) {
        // One line as Apache reads it: a '\' at the end joins the next line
        // (CRLF too), whose leading spaces are kept.
        const std::size_t first = number + 1;
        std::string line;
        bool more = true;
        while (more && pos < text.size()) {
            const auto end = text.find('\n', pos);
            std::string_view raw = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
            pos = end == std::string_view::npos ? text.size() : end + 1;
            ++number;
            if (raw.ends_with('\r')) {
                raw.remove_suffix(1);
            }
            more = raw.ends_with('\\');
            if (more) {
                raw.remove_suffix(1);
            }
            line.append(raw);
            if (line.size() > kMaxLine) {
                // Read on to the end of what Apache would join, unread.
                while (more && pos < text.size()) {
                    const auto e = text.find('\n', pos);
                    std::string_view r = text.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
                    pos = e == std::string_view::npos ? text.size() : e + 1;
                    ++number;
                    if (r.ends_with('\r')) {
                        r.remove_suffix(1);
                    }
                    more = r.ends_with('\\');
                }
                break;
            }
        }
        const std::string where = first == number ? "line " + std::to_string(first)
                                                  : "lines " + std::to_string(first) + "-" + std::to_string(number);
        const auto leave_out = [&](const std::string& why) {
            std::string problem = where;
            problem.append(": ").append(why);
            out.problems.push_back(std::move(problem));
            out.text += "# FloServer left out " + where + "\n";
        };
        if (line.size() > kMaxLine) {
            leave_out("the line is longer than 1000 characters");
            continue;
        }
        const std::string t = trimmed(line);
        if (t.empty() || t.front() == '#') {
            continue;  // comments stay in custom.conf
        }
        if (std::any_of(t.begin(), t.end(), [](char c) {
                const auto u = static_cast<unsigned char>(c);
                return (u < 0x20 && c != '\t') || u == 0x7f;
            })) {
            leave_out("the line has a control character");
            continue;
        }
        // The directive's name: Apache would also read one in quotes, or
        // made of ${VARIABLES}; only plain names are taken.
        const auto name_end = t.find_first_of(" \t>");
        std::string name = t.substr(0, name_end);
        std::string args = name_end == std::string::npos ? std::string() : t.substr(name_end);
        if (name_end != std::string::npos && t[name_end] == '>') {
            name += '>';
            args = t.substr(name_end + 1);
        }
        std::string_view bare = name;
        const bool closing = bare.starts_with("</");
        const bool opening = !closing && bare.starts_with("<");
        bare.remove_prefix(closing ? 2 : opening ? 1 : 0);
        if (bare.ends_with(">")) {
            bare.remove_suffix(1);
        }
        const std::string key = lower(bare);
        if (!skipping.empty()) {
            if (opening && key == skipping) {
                ++depth;
            } else if (closing && key == skipping && --depth == 0) {
                out.problems.push_back("lines " + std::to_string(skip_from) + "-" + std::to_string(number) +
                                       ": the <" + skipping + "> section is left out up to its end");
                skipping.clear();
            }
            continue;
        }
        const bool plain = !bare.empty() && std::isalpha(static_cast<unsigned char>(bare.front())) != 0 &&
                           std::all_of(bare.begin(), bare.end(), [](char c) {
                               return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
                           });
        if (!plain) {
            leave_out("a directive is written as its plain name (no quotes or ${...})");
            continue;
        }
        if (const auto problem = apache_directive_problem(name, args, logs_dir)) {
            leave_out(*problem);
            if (opening) {
                skipping = key;
                depth = 1;
                skip_from = first;
                out.text += "# FloServer left out the <" + std::string(bare) + "> section from line " +
                            std::to_string(first) + "\n";
            }
            continue;
        }
        out.text += t;
        out.text += "\n";
    }
    if (!skipping.empty()) {
        out.problems.push_back("lines " + std::to_string(skip_from) + "-" + std::to_string(number) + ": the <" +
                               skipping + "> section is left out (it has no end)");
    }
    return out;
}

// ------------------------------------------------------------------ service

ApacheSettingsService::ApacheSettingsService(fs::path data_dir, ProjectService& projects, Hooks hooks)
    : data_(std::move(data_dir)), projects_(projects), hooks_(std::move(hooks)) {}

namespace {

// Installed Apache versions, newest first.
std::vector<const InstalledRuntime*> apache_installs(const std::vector<InstalledRuntime>& installed) {
    std::vector<const InstalledRuntime*> out;
    for (const InstalledRuntime& r : installed) {
        if (r.name == "apache" && Version::parse(r.version)) {
            out.push_back(&r);
        }
    }
    std::sort(out.begin(), out.end(), [](const InstalledRuntime* a, const InstalledRuntime* b) {
        return *Version::parse(b->version) < *Version::parse(a->version);
    });
    return out;
}

const char* kind_name(ApacheModuleKind kind) {
    switch (kind) {
        case ApacheModuleKind::Needed:
            return "needed";
        case ApacheModuleKind::Default:
            return "default";
        case ApacheModuleKind::Optional:
            return "optional";
        case ApacheModuleKind::Off:
            return "off";
    }
    return "optional";
}

}  // namespace

std::optional<json> ApacheSettingsService::describe(const AppConfig& config, ServiceRefusal& refusal) const {
    (void)refusal;
    std::vector<std::string> problems;
    const ApacheSettings settings = read_apache_settings(config, &problems);
    const auto installed = list_installed(runtimes_dir(data_));
    const auto installs = apache_installs(installed);
    const InstalledRuntime* used = chosen_apache(installed, config);
    json out = {{"version", used != nullptr ? json(used->version) : json(nullptr)},
                {"chosen", settings.version ? json(*settings.version) : json(nullptr)},
                {"versions", json::array()},
                {"modules", json::array()},
                {"custom_conf", path_to_utf8(apache_custom_conf_file(data_))},
                {"used_by", json::array()},
                {"running", false}};
    for (const InstalledRuntime* r : installs) {
        out["versions"].push_back(r->version);
    }
    std::error_code ec;
    out["custom_conf_exists"] = fs::is_regular_file(apache_custom_conf_file(data_), ec);
    if (const std::string custom = read_apache_custom_conf(data_); !custom.empty()) {
        const auto filtered = filter_apache_custom_conf(custom, web_dir(data_) / "logs");
        if (!filtered.problems.empty()) {
            out["custom_problems"] = filtered.problems;
        }
    }
    if (used != nullptr) {
        // The modules folder, read now: once per request.
        const fs::path modules_dir = apache_server_root(used->dir) / "modules";
        const std::vector<std::string> names = apache_modules_in(modules_dir);
        const std::set<std::string> present(names.begin(), names.end());
        const ApacheModulePlan plan = plan_apache_modules(
            settings.modules, [&present](const std::string& n) { return present.contains(n); }, hooks_.posix);
        const std::set<std::string> loaded(plan.load.begin(), plan.load.end());
        for (const std::string& name : names) {
            const ApacheModuleInfo info = apache_module_info(name, hooks_.posix);
            json m = {{"name", name},
                      {"on", loaded.contains(name) || info.kind == ApacheModuleKind::Needed},
                      {"kind", kind_name(info.kind)}};
            if (!info.why.empty()) {
                m["why"] = info.why;
            }
            if (!info.needs.empty()) {
                m["needs"] = info.needs;
            }
            if (const auto it = plan.needed_by.find(name); it != plan.needed_by.end()) {
                m["needed_by"] = it->second;
            }
            out["modules"].push_back(std::move(m));
        }
        for (const std::string& name : plan.missing) {
            problems.push_back("the module " + name + " isn't in Apache " + used->version + ", so it's left out");
        }
    }
    if (settings.version && (used == nullptr || used->version != *settings.version)) {
        problems.push_back("Apache " + *settings.version + " isn't installed, so the newest is used");
    }
    if (hooks_.sites) {
        const json sites = hooks_.sites();
        if (sites.is_object()) {
            for (const auto& [name, site] : sites.items()) {
                if (site.is_object() && site.contains("app") && site.at("app") == "apache") {
                    out["used_by"].push_back(name);
                }
            }
        }
    }
    if (hooks_.status) {
        const json status = hooks_.status();
        if (status.is_object()) {
            out["running"] = status.contains("running") && status.at("running").is_boolean() && status.at("running").get<bool>();
            if (status.contains("refused") && status.at("refused").is_string()) {
                out["refused"] = status.at("refused");
            }
        }
    }
    if (!problems.empty()) {
        out["problems"] = problems;
    }
    return out;
}

std::optional<json> ApacheSettingsService::get(ServiceRefusal& refusal) const {
    const auto config = projects_.config(refusal);
    if (!config) {
        return std::nullopt;
    }
    return describe(*config, refusal);
}

std::optional<json> ApacheSettingsService::set(const json& changes, ServiceRefusal& refusal) {
    if (!changes.is_object()) {
        refusal = {"bad_params", "the changes must be an object"};
        return std::nullopt;
    }
    const auto installed = list_installed(runtimes_dir(data_));
    const json status_before = hooks_.status ? hooks_.status() : json();
    const bool was_running = status_before.is_object() && status_before.contains("running") &&
                             status_before.at("running").is_boolean() && status_before.at("running").get<bool>();
    bool changed = false;
    const auto saved = projects_.change_config(
        [&](AppConfig& config, ServiceRefusal& why) {
            ApacheSettings settings = read_apache_settings(config);
            const ApacheSettings before = settings;
            if (changes.contains("version")) {
                const json& v = changes.at("version");
                if (v.is_null()) {
                    settings.version.reset();
                } else if (!v.is_string() || !Version::parse(v.get<std::string>())) {
                    why = {"bad_params", "version must be an installed Apache version such as \"2.4.69\", or null"};
                    return false;
                } else {
                    const std::string wanted = v.get<std::string>();
                    const auto installs = apache_installs(installed);
                    if (std::none_of(installs.begin(), installs.end(),
                                     [&](const InstalledRuntime* r) { return r->version == wanted; })) {
                        why = {"not_installed", "Apache " + wanted + " isn't installed"};
                        return false;
                    }
                    settings.version = wanted;
                }
            }
            if (changes.contains("modules")) {
                const json& modules = changes.at("modules");
                if (!modules.is_object()) {
                    why = {"bad_params", R"(modules must be an object such as {"expires": true})"};
                    return false;
                }
                // What the install that will run has.
                const ApacheSettings next = settings;
                const InstalledRuntime* used = nullptr;
                {
                    AppConfig probe = config;
                    write_apache_settings(probe, next);
                    used = chosen_apache(installed, probe);
                }
                const std::vector<std::string> names =
                    used != nullptr ? apache_modules_in(apache_server_root(used->dir) / "modules") : std::vector<std::string>{};
                const std::set<std::string> present(names.begin(), names.end());
                for (const auto& [name, value] : modules.items()) {
                    if (!is_apache_module_name(name)) {
                        why = {"bad_params", "\"" + name + "\" isn't a module name such as expires"};
                        return false;
                    }
                    const ApacheModuleInfo info = apache_module_info(name, hooks_.posix);
                    if (value.is_null()) {
                        settings.modules.erase(name);
                        continue;
                    }
                    if (!value.is_boolean()) {
                        why = {"bad_params", "modules " + name + " must be true, false or null"};
                        return false;
                    }
                    const bool on = value.get<bool>();
                    if (info.kind == ApacheModuleKind::Needed) {
                        if (!on) {
                            why = {"bad_params", name + " can't be turned off: FloServer's config uses it"};
                            return false;
                        }
                        settings.modules.erase(name);
                        continue;
                    }
                    if (on && info.kind == ApacheModuleKind::Off) {
                        why = {"bad_params", name + " can't be turned on: it doesn't apply behind nginx"};
                        return false;
                    }
                    if (on && !present.contains(name)) {
                        std::string words = "this Apache has no " + name;
                        words.append(" module (mod_").append(name).append(".so)");
                        why = {"not_installed", std::move(words)};
                        return false;
                    }
                    if (on) {
                        for (const std::string& need : info.needs) {
                            if (apache_module_info(need, hooks_.posix).kind != ApacheModuleKind::Needed &&
                                !present.contains(need)) {
                                std::string words = name + " needs ";
                                words.append(need).append(", which this Apache doesn't have");
                                why = {"not_installed", std::move(words)};
                                return false;
                            }
                        }
                    }
                    // Back to the default: no change kept.
                    if (on == (info.kind == ApacheModuleKind::Default)) {
                        settings.modules.erase(name);
                    } else {
                        settings.modules[name] = on;
                    }
                }
            }
            changed = settings != before;
            write_apache_settings(config, settings);
            return true;
        },
        refusal);
    if (!saved) {
        return std::nullopt;
    }
    if (hooks_.reload) {
        hooks_.reload();
    }
    auto out = describe(*saved, refusal);
    if (out) {
        (*out)["restarting"] = changed && was_running;
    }
    return out;
}

std::optional<fs::path> ApacheSettingsService::custom_conf(ServiceRefusal& refusal) const {
    std::string error;
    if (!ensure_apache_custom_conf(data_, error)) {
        refusal = {"folder_unwritable", error};
        return std::nullopt;
    }
    return apache_custom_conf_file(data_);
}

std::optional<fs::path> ApacheSettingsService::final_conf(ServiceRefusal& refusal) const {
    std::string error;
    const auto text = hooks_.final_config ? hooks_.final_config(error) : std::nullopt;
    if (!text) {
        refusal = {"not_installed", error.empty() ? std::string("Apache isn't installed") : error};
        return std::nullopt;
    }
    const std::string body =
        "# What FloServer's shared Apache runs, written when you asked to see it: edits here are lost and do nothing.\n"
        "# Turn modules on or off in Settings > Apache; add your own lines to custom.conf beside this file.\n"
        "# {port} is the port Apache is started on.\n\n" +
        *text;
    fs::path file = data_ / "apache" / "final-httpd.conf";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    // Read-only, so Notepad says so on Save; made writable again to replace it.
    fs::permissions(file, fs::perms::owner_write, fs::perm_options::add, ec);
    if (!write_file_atomically(file, body, error)) {
        refusal = {"folder_unwritable", error};
        return std::nullopt;
    }
    fs::permissions(file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove, ec);
    return file;
}

}  // namespace floserver::core

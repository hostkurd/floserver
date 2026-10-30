#include "model/runtimes_page.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace floserver::ui {

namespace {

using nlohmann::json;

std::string text_of(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return {};
}

std::uint64_t count_of(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) {
        return 0;
    }
    const json& v = j[key];
    if (v.is_number_unsigned()) {
        return v.get<std::uint64_t>();
    }
    if (v.is_number_integer() && v.get<std::int64_t>() > 0) {
        return static_cast<std::uint64_t>(v.get<std::int64_t>());
    }
    return 0;
}

const json& array_of(const json& j, const char* key) {
    static const json kEmpty = json::array();
    return j.is_object() && j.contains(key) && j[key].is_array() ? j[key] : kEmpty;
}

// Only PHP's usual build is offered (the service installs nothing else).
bool usual_variant(const std::string& variant) { return variant.empty() || variant == "nts"; }

std::string folder_of(const std::string& version, const std::string& variant) {
    return usual_variant(variant) ? version : version + "-" + variant;
}

struct Spec {
    const char* runtime;
    bool wide;
    bool picker;
};
// The groups in the mockup's order; Web servers (nginx and Apache) come last.
constexpr std::array kGroups{Spec{"php", true, true}, Spec{"mariadb", true, true}, Spec{"node", false, false},
                             Spec{"python", false, false}};

// "3 projects · Shared sites" and who they are, from a used_by list.
void describe_users(const json& used_by, RuntimeRow& row, Lang lang) {
    const bool rtl = is_rtl(lang);
    std::size_t projects = 0;
    bool shared = false;
    std::string names;
    for (const json& u : used_by) {
        if (!u.is_string()) {
            continue;
        }
        const std::string n = u.get<std::string>();
        std::string shown;
        if (n == "_shared") {
            shared = true;
            shown = std::string(text(lang, Str::SharedSites));
        } else {
            if (n != "FloServer") {
                ++projects;
            }
            shown = rtl ? isolate_auto(n) : n;
        }
        names += (names.empty() ? "" : ", ") + shown;
    }
    row.used_by = names;
    if (projects == 1) {
        row.usage = std::string(text(lang, Str::SettingsOneProject));
    } else if (projects > 1) {
        row.usage = fill_pattern(text(lang, Str::SettingsProjects), false, {std::to_string(projects)});
    }
    if (shared) {
        row.usage += (row.usage.empty() ? "" : " · ") + std::string(text(lang, Str::SharedSites));
    }
}

// MariaDB: the port of the server running this exact version.
std::string port_text(const json& databases, const std::string& version, Lang lang) {
    for (const json& server : array_of(databases, "servers")) {
        if (text_of(server, "version") != version) {
            continue;
        }
        const std::uint64_t port = count_of(server, "port");
        if (port == 0 || port > 65535) {
            return {};
        }
        const Str key = text_of(server, "state") == "running" ? Str::SettingsRunningOnPort : Str::SettingsOnPort;
        return fill_pattern(text(lang, key), false, {std::to_string(port)});
    }
    return {};
}

bool newest_first(const RuntimeRow& a, const RuntimeRow& b) {
    if (a.version != b.version) {
        return is_newer_version(a.version, b.version);
    }
    return a.folder < b.folder;
}

}  // namespace

std::optional<RuntimeJob> runtime_job(const json& job) {
    RuntimeJob out;
    if (!job.is_object() || !job.contains("id") || !job["id"].is_number_integer()) {
        return std::nullopt;
    }
    out.id = job["id"].get<std::int64_t>();
    out.name = text_of(job, "name");
    out.version = text_of(job, "version");
    if (out.version.empty()) {
        out.version = text_of(job, "requested");
    }
    out.state = text_of(job, "state");
    out.received = count_of(job, "received");
    out.total = count_of(job, "total");
    if (out.name.empty()) {
        return std::nullopt;
    }
    return out;
}

RuntimesPage runtimes_page(const json& reply, const Installed* installed, const json& databases, Lang lang) {
    RuntimesPage page;
    const json& list = reply.is_object() && reply.contains("list") ? reply["list"] : json();
    page.has_list = list.is_object();
    page.hint = std::string(text(lang, Str::SettingsRuntimesHint)) + " ";
    if (page.has_list) {
        const std::uint64_t sequence = count_of(list, "sequence");
        const std::string expires = text_of(list, "expires").substr(0, 10);  // the date of "2027-01-28T00:00:00Z"
        page.hint += fill(lang, Str::SettingsListFrom, {std::to_string(sequence), expires});
    } else {
        page.hint += text(lang, Str::SettingsNoList);
    }
    const json defaults = reply.is_object() && reply.contains("defaults") && reply["defaults"].is_object()
                              ? reply["defaults"]
                              : json::object();

    // Every row, keyed by runtime then folder.
    std::map<std::string, std::map<std::string, RuntimeRow>> rows;
    const auto is_installed_now = [&](const std::string& name, const std::string& version) {
        if (installed == nullptr) {
            return true;
        }
        const auto it = installed->find(name);
        return it != installed->end() && std::find(it->second.begin(), it->second.end(), version) != it->second.end();
    };
    for (const json& r : array_of(reply, "installed")) {
        RuntimeRow row;
        row.name = text_of(r, "name");
        row.version = text_of(r, "version");
        row.folder = text_of(r, "folder");
        if (row.name.empty() || row.version.empty() || row.folder.empty() || !is_installed_now(row.name, row.version)) {
            continue;  // removed since the reply (the poll says so)
        }
        row.installed = true;
        const std::string variant = text_of(r, "variant");
        row.label = usual_variant(variant) ? row.version : row.version + " " + variant;
        describe_users(array_of(r, "used_by"), row, lang);
        if (row.name == "mariadb") {
            if (const std::string port = port_text(databases, row.version, lang); !port.empty()) {
                row.usage += (row.usage.empty() ? "" : " · ") + port;
            }
        }
        rows[row.name][row.folder] = std::move(row);
    }
    // Installed since the reply: the poll knows the version, not yet who uses it.
    if (installed != nullptr) {
        for (const auto& [name, versions] : *installed) {
            for (const std::string& v : versions) {
                auto& by_folder = rows[name];
                const bool known = std::any_of(by_folder.begin(), by_folder.end(),
                                               [&](const auto& kv) { return kv.second.installed && kv.second.version == v; });
                if (!known) {
                    RuntimeRow row;
                    row.name = name;
                    row.version = v;
                    row.folder = v;
                    row.label = v;
                    row.installed = true;
                    by_folder[row.folder] = std::move(row);
                }
            }
        }
    }
    if (page.has_list) {
        for (const json& r : array_of(reply, "available")) {
            const std::string name = text_of(r, "name");
            const std::string version = text_of(r, "version");
            const std::string variant = text_of(r, "variant");
            if (name.empty() || version.empty() || !usual_variant(variant)) {
                continue;
            }
            RuntimeRow& row = rows[name][folder_of(version, variant)];
            if (row.installed) {
                continue;
            }
            row.name = name;
            row.version = version;
            row.label = version;
            row.size = count_of(r, "size");
        }
    }

    const auto take_rows = [&](const std::string& name) {
        std::vector<RuntimeRow> out;
        if (const auto it = rows.find(name); it != rows.end()) {
            for (auto& [folder, row] : it->second) {
                if (!row.name.empty()) {
                    row.action = row.installed ? RowAction::Remove : RowAction::Download;
                    out.push_back(std::move(row));
                }
            }
        }
        std::stable_sort(out.begin(), out.end(), newest_first);
        return out;
    };

    for (const Spec& spec : kGroups) {
        RuntimeGroup g;
        g.runtime = spec.runtime;
        g.title = runtime_label(spec.runtime);
        g.wide = spec.wide;
        g.rows = take_rows(spec.runtime);

        // The installed series, newest first, and the one new projects get:
        // the stored default when it's installed, else the newest.
        std::vector<std::string> series;
        for (const RuntimeRow& row : g.rows) {
            const std::string s = version_series(g.runtime, row.version);
            if (row.installed && std::find(series.begin(), series.end(), s) == series.end()) {
                series.push_back(s);
            }
        }
        const std::string stored = text_of(defaults, spec.runtime);
        const bool stored_here = std::find(series.begin(), series.end(), stored) != series.end();
        const std::string effective = stored_here ? stored : (series.empty() ? std::string() : series.front());
        for (RuntimeRow& row : g.rows) {
            if (row.installed && version_series(g.runtime, row.version) == effective) {
                row.is_default = true;  // the newest of that series: the rows are newest first
                break;
            }
        }
        if (spec.picker) {
            VersionField f{spec.runtime == std::string_view("php") ? Field::Php : Field::Database,
                           spec.runtime,
                           g.title,
                           {{std::string(text(lang, Str::ChoiceNewest)), std::nullopt}},
                           0};
            for (const std::string& s : series) {
                f.choices.push_back({s, s});
                if (s == stored) {
                    f.current = f.choices.size() - 1;
                }
            }
            if (!stored.empty() && !stored_here) {
                f.choices.push_back({fill(lang, Str::NotInstalled, {stored}), stored});
                f.current = f.choices.size() - 1;
            }
            g.picker = std::move(f);
        }
        if (!g.rows.empty()) {
            page.groups.push_back(std::move(g));
        }
    }

    // Web servers: the newest nginx is always used; Apache is for .htaccess.
    RuntimeGroup web;
    web.title = std::string(text(lang, Str::SettingsWebServers));
    std::vector<RuntimeRow> nginx = take_rows("nginx");
    for (RuntimeRow& row : nginx) {
        if (row.installed) {
            row.always_used = true;
            row.action = RowAction::None;
            break;
        }
    }
    for (RuntimeRow& row : nginx) {
        row.label = "nginx " + row.label;
        web.rows.push_back(std::move(row));
    }
    for (RuntimeRow& row : take_rows("apache")) {
        row.label = "Apache " + row.label;
        row.note = ".htaccess";
        web.rows.push_back(std::move(row));
    }
    if (!web.rows.empty()) {
        page.groups.push_back(std::move(web));
    }
    return page;
}

json merge_runtimes_reply(const json& before, const json& fresh) {
    const bool fresh_has = fresh.is_object() && fresh.contains("list") && fresh["list"].is_object();
    const bool before_has = before.is_object() && before.contains("list") && before["list"].is_object();
    if (fresh_has || !before_has || !fresh.is_object()) {
        return fresh;
    }
    json out = fresh;
    out["list"] = before["list"];
    out["available"] = before.contains("available") ? before["available"] : json::array();
    out.erase("list_error");
    return out;
}

std::string size_text(std::uint64_t bytes) {
    constexpr std::uint64_t kKb = 1024;
    constexpr std::uint64_t kMb = kKb * 1024;
    const bool mb = bytes >= kMb;
    const std::uint64_t unit = mb ? kMb : kKb;
    const std::uint64_t tenths = (bytes * 10 + unit / 2) / unit;
    const std::string suffix = mb ? " MB" : " KB";
    if (tenths >= 100 || tenths % 10 == 0) {
        return std::to_string((tenths + 5) / 10) + suffix;
    }
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + suffix;
}

std::string job_text(const RuntimeJob& job, Lang lang) {
    if (job.state == "downloading") {
        if (job.total > 0) {
            return fill_pattern(text(lang, Str::SettingsDownloadingOf), false,
                                {size_text(std::min(job.received, job.total)), size_text(job.total)});
        }
        return std::string(text(lang, Str::SettingsDownloadingDots));
    }
    if (job.state.empty() || job.state == "queued" || job.state == "checking") {
        return std::string(text(lang, Str::SettingsStarting));
    }
    return std::string(text(lang, Str::SettingsInstallingDots));
}

std::optional<double> job_fraction(const RuntimeJob& job) {
    if (job.state != "downloading" || job.total == 0) {
        return std::nullopt;
    }
    return static_cast<double>(std::min(job.received, job.total)) / static_cast<double>(job.total);
}

const RuntimeJob* job_for(const RuntimeRow& row, const std::vector<RuntimeJob>& jobs) {
    for (const RuntimeJob& j : jobs) {
        if (j.name == row.name && j.version == row.version) {
            return &j;
        }
    }
    return nullptr;
}

std::string settings_install_key(const std::string& name, const std::string& version) {
    return "_settings/" + name + "/" + version;
}

bool is_settings_key(const std::string& key) { return key.starts_with("_settings/"); }

}  // namespace floserver::ui

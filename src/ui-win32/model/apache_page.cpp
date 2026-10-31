#include "model/apache_page.h"

#include "model/database_page.h"  // db_names_text

#include <algorithm>
#include <array>
#include <set>
#include <string_view>

namespace floserver::ui {

using nlohmann::json;

namespace {

// The ones people turn on most, shown after those that are on.
constexpr std::array<std::string_view, 14> kPopular{
    "expires", "deflate", "headers",  "rewrite", "cache",  "cache_disk",     "remoteip",
    "unique_id", "brotli", "include", "status",  "info",   "proxy_wstunnel", "substitute"};

std::string string_of(const json& j, const char* key) {
    return j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string();
}

std::vector<std::string> strings_of(const json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j.at(key).is_array()) {
        for (const json& s : j.at(key)) {
            if (s.is_string()) {
                out.push_back(s.get<std::string>());
            }
        }
    }
    return out;
}

const ApacheModule* find(const ApacheInfo& info, const std::string& name) {
    const auto it = std::find_if(info.modules.begin(), info.modules.end(),
                                 [&](const ApacheModule& m) { return m.name == name; });
    return it == info.modules.end() ? nullptr : &*it;
}

// On by itself once saved: FloServer's own, the change, else as saved
// without counting what others pulled in.
bool on_by_itself(const ApacheModule& m, const ApacheChanges& changes) {
    if (m.kind == "needed") {
        return true;
    }
    if (m.kind == "off") {
        return false;
    }
    if (const auto it = changes.modules.find(m.name); it != changes.modules.end()) {
        return it->second;
    }
    return m.on && m.needed_by.empty();
}

// Everything on once saved, with who pulled each in.
std::map<std::string, std::set<std::string>> on_set(const ApacheInfo& info, const ApacheChanges& changes) {
    std::map<std::string, std::set<std::string>> on;
    std::vector<std::string> todo;
    for (const ApacheModule& m : info.modules) {
        if (on_by_itself(m, changes)) {
            on.try_emplace(m.name);
            todo.push_back(m.name);
        }
    }
    while (!todo.empty()) {
        const std::string name = todo.back();
        todo.pop_back();
        const ApacheModule* m = find(info, name);
        if (m == nullptr) {
            continue;
        }
        for (const std::string& need : m->needs) {
            const ApacheModule* n = find(info, need);
            if (n == nullptr || n->kind == "off") {
                continue;
            }
            const bool fresh = !on.contains(need);
            on[need].insert(name);
            if (fresh) {
                todo.push_back(need);
            }
        }
    }
    return on;
}

}  // namespace

std::optional<ApacheInfo> apache_info_from_reply(const json& reply) {
    if (!reply.is_object() || !reply.contains("modules") || !reply.at("modules").is_array()) {
        return std::nullopt;
    }
    ApacheInfo info;
    info.version = string_of(reply, "version");
    if (reply.contains("chosen") && reply.at("chosen").is_string()) {
        info.chosen = reply.at("chosen").get<std::string>();
    }
    info.versions = strings_of(reply, "versions");
    for (const json& m : reply.at("modules")) {
        if (!m.is_object() || string_of(m, "name").empty()) {
            continue;
        }
        ApacheModule module;
        module.name = string_of(m, "name");
        module.on = m.contains("on") && m.at("on").is_boolean() && m.at("on").get<bool>();
        module.kind = string_of(m, "kind");
        module.why = string_of(m, "why");
        module.needs = strings_of(m, "needs");
        module.needed_by = strings_of(m, "needed_by");
        info.modules.push_back(std::move(module));
    }
    info.custom_conf = string_of(reply, "custom_conf");
    info.custom_conf_exists = reply.contains("custom_conf_exists") && reply.at("custom_conf_exists").is_boolean() &&
                              reply.at("custom_conf_exists").get<bool>();
    info.custom_problems = strings_of(reply, "custom_problems");
    info.used_by = strings_of(reply, "used_by");
    info.running = reply.contains("running") && reply.at("running").is_boolean() && reply.at("running").get<bool>();
    info.refused = string_of(reply, "refused");
    info.problems = strings_of(reply, "problems");
    info.restarting =
        reply.contains("restarting") && reply.at("restarting").is_boolean() && reply.at("restarting").get<bool>();
    return info;
}

std::vector<ApacheModule> ordered_apache_modules(const ApacheInfo& info) {
    const auto rank = [](const ApacheModule& m) {
        if (m.kind == "off") {
            return 3;
        }
        if (m.on) {
            return 0;
        }
        return std::find(kPopular.begin(), kPopular.end(), m.name) != kPopular.end() ? 1 : 2;
    };
    std::vector<ApacheModule> out = info.modules;
    std::stable_sort(out.begin(), out.end(), [&](const ApacheModule& a, const ApacheModule& b) {
        const int ra = rank(a);
        const int rb = rank(b);
        if (ra != rb) {
            return ra < rb;
        }
        if (ra == 1) {
            const auto pa = std::find(kPopular.begin(), kPopular.end(), a.name);
            const auto pb = std::find(kPopular.begin(), kPopular.end(), b.name);
            return pa < pb;
        }
        return a.name < b.name;
    });
    return out;
}

bool apache_module_on(const ApacheInfo& info, const ApacheChanges& changes, const std::string& name) {
    return on_set(info, changes).contains(name);
}

std::vector<std::string> apache_needed_by(const ApacheInfo& info, const ApacheChanges& changes, const std::string& name) {
    const auto on = on_set(info, changes);
    const ApacheModule* m = find(info, name);
    const auto it = on.find(name);
    if (m == nullptr || it == on.end() || on_by_itself(*m, changes)) {
        return {};
    }
    return {it->second.begin(), it->second.end()};
}

bool apache_module_locked(const ApacheInfo& info, const ApacheChanges& changes, const ApacheModule& m) {
    return m.kind == "needed" || m.kind == "off" || !apache_needed_by(info, changes, m.name).empty();
}

void toggle_apache_module(const ApacheInfo& info, ApacheChanges& changes, const std::string& name) {
    const ApacheModule* m = find(info, name);
    if (m == nullptr || apache_module_locked(info, changes, *m)) {
        return;
    }
    const bool next = !on_by_itself(*m, changes);
    if (next == (m->on && m->needed_by.empty())) {
        changes.modules.erase(name);  // back to what's saved
    } else {
        changes.modules[name] = next;
    }
}

std::size_t apache_modules_on(const ApacheInfo& info, const ApacheChanges& changes) {
    const auto on = on_set(info, changes);
    return static_cast<std::size_t>(std::count_if(info.modules.begin(), info.modules.end(),
                                                  [&](const ApacheModule& m) { return on.contains(m.name); }));
}

std::string apache_module_note(const ApacheInfo& info, const ApacheChanges& changes, const ApacheModule& m, Lang lang) {
    if (m.kind == "needed") {
        return m.why == "mpm" ? std::string(text(lang, Str::ApacheModMpm)) : std::string(text(lang, Str::ApacheModNeeded));
    }
    if (m.kind == "off") {
        if (m.why == "https") {
            return std::string(text(lang, Str::ApacheModHttps));
        }
        if (m.why == "http2") {
            return std::string(text(lang, Str::ApacheModHttp2));
        }
        if (m.why == "mpm") {
            return std::string(text(lang, Str::ApacheModMpm));
        }
        return std::string(text(lang, Str::ApacheModUnused));
    }
    const auto by = apache_needed_by(info, changes, m.name);
    if (!by.empty()) {
        std::string names;
        for (const std::string& n : by) {
            names += (names.empty() ? "" : ", ") + n;
        }
        return fill(lang, Str::ApacheModFor, {names});
    }
    return {};
}

std::optional<std::string> apache_shown_choice(const ApacheInfo& info, const ApacheChanges& changes) {
    return changes.version ? *changes.version : info.chosen;
}

std::string apache_choice_label(const ApacheInfo& info, const std::optional<std::string>& choice, Lang lang) {
    if (choice) {
        return "Apache " + *choice;
    }
    const std::string newest = info.versions.empty() ? info.version : info.versions.front();
    return fill(lang, Str::ApacheNewest, {newest});
}

json apache_set_params(const ApacheChanges& changes) {
    json out = json::object();
    if (changes.version) {
        out["version"] = *changes.version ? json(**changes.version) : json(nullptr);
    }
    if (!changes.modules.empty()) {
        out["modules"] = json::object();
        for (const auto& [name, on] : changes.modules) {
            out["modules"][name] = on;
        }
    }
    return out;
}

std::string apache_restart_text(const ApacheInfo& info, Lang lang) {
    if (info.used_by.empty()) {
        return std::string(text(lang, Str::ApacheAppliesNobody));
    }
    if (!info.running) {
        return std::string(text(lang, Str::ApacheAppliesSleeping));
    }
    return fill_pattern(text(lang, Str::ApacheApplies), false, {db_names_text(info.used_by, lang)});
}

}  // namespace floserver::ui

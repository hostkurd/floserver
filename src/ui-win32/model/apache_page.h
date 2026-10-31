#pragma once

// Settings > Apache: apache.get's answer, the changes not saved yet and what
// Save sends. Plain C++ so it's tested on every platform; the window only
// draws it.

#include "model/ui_strings.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

struct ApacheModule {
    std::string name;    // "expires"
    bool on = false;     // as saved
    std::string kind;    // "needed", "default", "optional", "off"
    std::string why;     // needed / off: "fcgi", "https", "http2", "mpm", "suexec", ...
    std::vector<std::string> needs;      // loaded with it
    std::vector<std::string> needed_by;  // on (as saved) only because these need it
};

// apache.get's (and apache.set's) answer.
struct ApacheInfo {
    std::string version;                // the install used; empty when none is
    std::optional<std::string> chosen;  // config.json's; nothing: the newest
    std::vector<std::string> versions;  // installed, newest first
    std::vector<ApacheModule> modules;  // the install's, by name
    std::string custom_conf;
    bool custom_conf_exists = false;
    std::vector<std::string> custom_problems;
    std::vector<std::string> used_by;   // project names, and "_shared"
    bool running = false;
    std::string refused;                // what httpd -t said about the last settings
    std::vector<std::string> problems;
    bool restarting = false;            // apache.set's: Apache restarts with them
};

[[nodiscard]] std::optional<ApacheInfo> apache_info_from_reply(const nlohmann::json& reply);

// Changes until Save.
struct ApacheChanges {
    std::optional<std::optional<std::string>> version;  // set: a version, or nothing for the newest
    std::map<std::string, bool> modules;
    [[nodiscard]] bool empty() const { return !version && modules.empty(); }
};

// The modules in the order shown: on first, then the ones people turn on
// most, then the rest by name, those that don't apply last. From the saved
// answer, so nothing jumps when a box is ticked.
[[nodiscard]] std::vector<ApacheModule> ordered_apache_modules(const ApacheInfo& info);

// On once saved: the change, else as saved, and anything an on module needs.
[[nodiscard]] bool apache_module_on(const ApacheInfo& info, const ApacheChanges& changes, const std::string& name);
// The modules that are on (once saved) and need `name`, sorted.
[[nodiscard]] std::vector<std::string> apache_needed_by(const ApacheInfo& info, const ApacheChanges& changes,
                                                        const std::string& name);
// Can't be ticked or unticked here: FloServer's own, one that doesn't
// apply behind nginx, or one an on module needs.
[[nodiscard]] bool apache_module_locked(const ApacheInfo& info, const ApacheChanges& changes, const ApacheModule& m);
// Turns it on or off; a change back to what's saved is dropped.
void toggle_apache_module(const ApacheInfo& info, ApacheChanges& changes, const std::string& name);
[[nodiscard]] std::size_t apache_modules_on(const ApacheInfo& info, const ApacheChanges& changes);

// The few words beside a module: "FloServer's", "nginx does HTTPS", "for deflate"; empty for most.
[[nodiscard]] std::string apache_module_note(const ApacheInfo& info, const ApacheChanges& changes,
                                             const ApacheModule& m, Lang lang);

// The version picker: nothing for the newest, else that version.
[[nodiscard]] std::optional<std::string> apache_shown_choice(const ApacheInfo& info, const ApacheChanges& changes);
// "Apache 2.4.69", or "Newest installed (2.4.69)".
[[nodiscard]] std::string apache_choice_label(const ApacheInfo& info, const std::optional<std::string>& choice,
                                              Lang lang);

// apache.set's parameters: only what changed.
[[nodiscard]] nlohmann::json apache_set_params(const ApacheChanges& changes);

// The footer: what Save restarts.
[[nodiscard]] std::string apache_restart_text(const ApacheInfo& info, Lang lang);

}  // namespace floserver::ui

#include "model/general_page.h"

#include "model/view_model.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

namespace floserver::ui {

using nlohmann::json;

namespace {

std::string string_or(const json& doc, const char* key) {
    if (doc.is_object() && doc.contains(key) && doc[key].is_string()) {
        return doc[key].get<std::string>();
    }
    return {};
}

// "8.10.1" after "8.9.2": number by number; a part that isn't a number by its text.
bool newer(std::string_view a, std::string_view b) {
    while (!a.empty() || !b.empty()) {
        const auto cut_a = a.find('.');
        const auto cut_b = b.find('.');
        const std::string_view pa = a.substr(0, cut_a);
        const std::string_view pb = b.substr(0, cut_b);
        if (pa != pb) {
            const bool num_a = !pa.empty() && pa.find_first_not_of("0123456789") == std::string_view::npos;
            const bool num_b = !pb.empty() && pb.find_first_not_of("0123456789") == std::string_view::npos;
            if (num_a && num_b && pa.size() != pb.size()) {
                return pa.size() > pb.size();  // no leading zeros in versions: longer is bigger
            }
            return pa > pb;
        }
        a = cut_a == std::string_view::npos ? std::string_view{} : a.substr(cut_a + 1);
        b = cut_b == std::string_view::npos ? std::string_view{} : b.substr(cut_b + 1);
    }
    return false;
}

}  // namespace

WindowPrefs parse_window_prefs(std::string_view text) {
    WindowPrefs prefs;
    const json doc = json::parse(text, nullptr, false);
    if (!doc.is_object()) {
        return prefs;
    }
    const std::string lang = string_or(doc, "language");
    if (lang == "en") {
        prefs.lang = LangChoice::En;
    } else if (lang == "ar") {
        prefs.lang = LangChoice::Ar;
    } else if (lang == "ku") {
        prefs.lang = LangChoice::Ku;
    }
    const std::string theme = string_or(doc, "theme");
    if (theme == "light") {
        prefs.theme = ThemeChoice::Light;
    } else if (theme == "dark") {
        prefs.theme = ThemeChoice::Dark;
    }
    if (string_or(doc, "close") == "quit") {
        prefs.close = CloseAction::Quit;
    }
    if (doc.contains("start_in_tray") && doc["start_in_tray"].is_boolean()) {
        prefs.start_in_tray = doc["start_in_tray"].get<bool>();
    }
    return prefs;
}

std::string window_prefs_json(const WindowPrefs& prefs) {
    const auto lang = [&]() -> std::string_view {
        switch (prefs.lang) {
            case LangChoice::En:
                return "en";
            case LangChoice::Ar:
                return "ar";
            case LangChoice::Ku:
                return "ku";
            case LangChoice::System:
                break;
        }
        return "windows";
    };
    const auto theme = [&]() -> std::string_view {
        switch (prefs.theme) {
            case ThemeChoice::Light:
                return "light";
            case ThemeChoice::Dark:
                return "dark";
            case ThemeChoice::System:
                break;
        }
        return "windows";
    };
    const json doc = {{"language", lang()},
                      {"theme", theme()},
                      {"close", prefs.close == CloseAction::Quit ? "quit" : "tray"},
                      {"start_in_tray", prefs.start_in_tray}};
    return doc.dump(2) + "\n";
}

Lang resolve_lang(LangChoice choice, const char* forced, const std::vector<std::string>& windows_langs) {
    switch (choice) {
        case LangChoice::En:
            return Lang::En;
        case LangChoice::Ar:
            return Lang::Ar;
        case LangChoice::Ku:
            return Lang::Ku;
        case LangChoice::System:
            break;
    }
    if (forced != nullptr && *forced != '\0') {
        return lang_from_setting(forced);
    }
    for (const std::string& name : windows_langs) {
        if (const auto lang = parse_lang(name)) {
            return *lang;
        }
    }
    return Lang::En;
}

bool resolve_dark(ThemeChoice choice, const char* forced, bool windows_dark) {
    switch (choice) {
        case ThemeChoice::Light:
            return false;
        case ThemeChoice::Dark:
            return true;
        case ThemeChoice::System:
            break;
    }
    if (forced != nullptr && *forced != '\0') {
        return std::string_view(forced) == "dark";
    }
    return windows_dark;
}

std::string_view language_name(Lang lang) {
    switch (lang) {
        case Lang::Ar:
            return "العربية";
        case Lang::Ku:
            return "کوردی";
        case Lang::En:
            break;
    }
    return "English";
}

// --- starting with Windows ---------------------------------------------------

std::string run_value_name(std::string_view service_instance) {
    constexpr std::string_view kDefault = "floserverd";
    if (service_instance == kDefault || !service_instance.starts_with("floserverd-")) {
        return "FloServer";
    }
    return "FloServer (" + std::string(service_instance.substr(kDefault.size() + 1)) + ")";
}

std::string instance_suffix(std::string_view service_instance) {
    constexpr std::string_view kPrefix = "floserverd-";
    if (!service_instance.starts_with(kPrefix)) {
        return {};
    }
    return std::string(service_instance.substr(kPrefix.size()));
}

namespace {

// As core::ipc::is_instance_name would take "floserverd-<suffix>".
bool usable_suffix(std::string_view suffix) {
    constexpr std::size_t kMaxName = 48;
    constexpr std::size_t kPrefix = 11;  // "floserverd-"
    return !suffix.empty() && suffix.size() + kPrefix <= kMaxName &&
           std::all_of(suffix.begin(), suffix.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

}  // namespace

StartArgs parse_start_args(const std::vector<std::string>& args) {
    StartArgs out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == kTrayFlag) {
            out.tray = true;
        } else if (args[i] == kInstanceFlag && i + 1 < args.size() && !args[i + 1].starts_with('-')) {
            ++i;
            out.instance = usable_suffix(args[i]) ? args[i] : std::string();
        }
    }
    return out;
}

bool startup_disabled(const std::vector<unsigned char>& value) {
    return !value.empty() && (value.front() & 1U) != 0;
}

std::string run_command(std::string_view exe_path, bool tray, std::string_view service_instance) {
    if (exe_path.empty() || exe_path.find_first_of("\"\r\n") != std::string_view::npos) {
        return {};
    }
    // Absolute: a drive ("C:\") or a UNC path ("\\server\share").
    const bool drive = exe_path.size() >= 3 && exe_path[1] == ':' && (exe_path[2] == '\\' || exe_path[2] == '/');
    const bool unc = exe_path.starts_with("\\\\");
    if (!drive && !unc) {
        return {};
    }
    std::string out = "\"";
    out += exe_path;
    out += '"';
    if (tray) {
        out += ' ';
        out += kTrayFlag;
    }
    // A copy started with FLOSERVER_INSTANCE: the Run entry has no
    // environment, so the instance goes on the command line.
    if (const std::string suffix = instance_suffix(service_instance); usable_suffix(suffix)) {
        out += ' ';
        out += kInstanceFlag;
        out += ' ';
        out += suffix;
    }
    return out;
}

std::optional<RunEntry> parse_run_command(std::string_view command) {
    const auto first = command.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    command.remove_prefix(first);
    RunEntry entry;
    std::string_view rest;
    if (command.front() == '"') {
        const auto close = command.find('"', 1);
        if (close == std::string_view::npos) {
            entry.exe = std::string(command.substr(1));
            return entry;
        }
        entry.exe = std::string(command.substr(1, close - 1));
        rest = command.substr(close + 1);
    } else {
        // Written by hand without quotes: the program ends at ".exe".
        std::string lower(command);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        const auto exe = lower.find(".exe");
        const std::size_t end = exe == std::string::npos ? command.find(' ') : exe + 4;
        entry.exe = std::string(command.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : command.substr(end);
    }
    // The flags as words of their own.
    std::vector<std::string> words;
    while (!rest.empty()) {
        const auto start = rest.find_first_not_of(" \t");
        if (start == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(start);
        const auto end = rest.find_first_of(" \t");
        words.emplace_back(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    }
    const StartArgs args = parse_start_args(words);
    entry.tray = args.tray;
    entry.instance = args.instance;
    return entry;
}

// --- licences ------------------------------------------------------------------

std::vector<LicenceLine> library_licences() {
    // As third_party/README.md lists them; each one's licence file is beside it.
    return {
        {"nlohmann/json", "3.12.0", "MIT", "https://github.com/nlohmann/json", false},
        {"Monocypher", "4.0.2", "BSD-2-Clause OR CC0-1.0", "https://monocypher.org", false},
        {"miniz", "3.0.2", "MIT", "https://github.com/richgel999/miniz", false},
        {"doctest", "2.4.12", "MIT", "https://github.com/doctest/doctest", true},
    };
}

std::vector<LicenceLine> runtime_licences(const json& runtimes_reply) {
    std::vector<LicenceLine> out;
    if (!runtimes_reply.is_object() || !runtimes_reply.contains("installed") ||
        !runtimes_reply["installed"].is_array()) {
        return out;
    }
    std::set<std::tuple<std::string, std::string, std::string>> seen;
    for (const json& r : runtimes_reply["installed"]) {
        const std::string name = string_or(r, "name");
        const std::string version = string_or(r, "version");
        if (name.empty() || version.empty()) {
            continue;
        }
        std::string licence = string_or(r, "license");
        if (!seen.emplace(name, version, licence).second) {
            continue;  // the same runtime twice (two variants): listed once
        }
        out.push_back({runtime_label(name), version, std::move(licence), {}, false});
    }
    std::sort(out.begin(), out.end(), [](const LicenceLine& a, const LicenceLine& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return newer(a.version, b.version);  // the newest first
    });
    return out;
}

std::string licences_text(Lang lang, std::string_view version, const json* runtimes_reply) {
    // Notepad shows it left to right whatever the language: the headings
    // carry no direction marks, the rest is names and licence codes.
    const auto plain = [&](Str key) { return std::string(text(lang, key)); };
    const auto line = [](const LicenceLine& l) {
        std::string s = "  " + l.name + " " + l.version + " — " + (l.licence.empty() ? std::string("?") : l.licence);
        if (!l.where.empty()) {
            s += "  " + l.where;
        }
        return s;
    };
    std::string out = fill_pattern(text(lang, Str::LicencesTitle), false, {version}) + "\n\n";
    out += plain(Str::LicencesOwn) + "\n\n";
    out += plain(Str::LicencesLibraries) + "\n";
    for (const LicenceLine& l : library_licences()) {
        out += line(l);
        if (l.test_only) {
            out += "  (" + plain(Str::LicencesTestOnly) + ")";
        }
        out += "\n";
    }
    out += "\n" + plain(Str::LicencesRuntimes) + "\n";
    if (runtimes_reply == nullptr) {
        out += "  " + plain(Str::LicencesNotRead) + "\n";
    } else if (const auto runtimes = runtime_licences(*runtimes_reply); runtimes.empty()) {
        out += "  " + plain(Str::LicencesNoRuntimes) + "\n";
    } else {
        for (const LicenceLine& l : runtimes) {
            out += line(l) + "\n";
        }
    }
    return out;
}

}  // namespace floserver::ui

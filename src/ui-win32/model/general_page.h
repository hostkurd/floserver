#pragma once

// Settings > General: the window's own preferences (language, theme, what
// closing the window does), the Windows Run entry that starts FloServer at
// sign-in, and the licences list. Plain C++ so it's tested on every
// platform; the window reads the registry and draws it.
//
// The preferences are the window's, not the service's: they're kept per
// user in window.json in FloServer's data folder (%LOCALAPPDATA%\FloServer),
// next to the service's files but written only by FloServer.exe.

#include "model/ui_strings.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// In the order Settings lists them.
enum class LangChoice : std::uint8_t { System, En, Ar, Ku };
enum class ThemeChoice : std::uint8_t { System, Light, Dark };
enum class CloseAction : std::uint8_t { Tray, Quit };

inline constexpr std::array<LangChoice, 4> kLangChoices = {LangChoice::System, LangChoice::En, LangChoice::Ar,
                                                           LangChoice::Ku};
inline constexpr std::array<ThemeChoice, 3> kThemeChoices = {ThemeChoice::System, ThemeChoice::Light,
                                                             ThemeChoice::Dark};

struct WindowPrefs {
    LangChoice lang = LangChoice::System;
    ThemeChoice theme = ThemeChoice::System;
    CloseAction close = CloseAction::Tray;
    // What starting with Windows opens: the tray only (true) or the window.
    // Kept here too so the choice survives turning the Run entry off and on.
    bool start_in_tray = true;
    bool operator==(const WindowPrefs&) const = default;
};

// window.json's text; anything missing or not understood keeps its default.
[[nodiscard]] WindowPrefs parse_window_prefs(std::string_view text);
[[nodiscard]] std::string window_prefs_json(const WindowPrefs& prefs);
inline constexpr std::string_view kWindowPrefsFile = "window.json";

// The language the window shows. A choice made in Settings wins; "Same as
// Windows" takes FLOSERVER_LANG when it's set (screenshots, tests), else the
// first of Windows' display languages FloServer has ("ar-IQ", "ku-Arab-IQ",
// "ckb-IQ"...), else English.
[[nodiscard]] Lang resolve_lang(LangChoice choice, const char* forced, const std::vector<std::string>& windows_langs);

// Dark or not: a choice made in Settings wins; "Same as Windows" takes
// FLOSERVER_THEME ("light", "dark") when it's set, else Windows' app theme.
[[nodiscard]] bool resolve_dark(ThemeChoice choice, const char* forced, bool windows_dark);

// A language's name in its own script, whatever the window's language.
[[nodiscard]] std::string_view language_name(Lang lang);

// --- starting with Windows ---------------------------------------------------

// HKCU\Software\Microsoft\Windows\CurrentVersion\Run
inline constexpr std::string_view kRunKey = R"(Software\Microsoft\Windows\CurrentVersion\Run)";
// FloServer.exe opened with it starts in the tray, the window hidden.
inline constexpr std::string_view kTrayFlag = "--tray";
// FloServer.exe --instance dev: the service instance "floserverd-dev", as
// FLOSERVER_INSTANCE=dev would (a Run entry has no environment of its own).
inline constexpr std::string_view kInstanceFlag = "--instance";
// HKCU\...\Explorer\StartupApproved\Run: Task Manager's on/off for Run entries.
inline constexpr std::string_view kStartupApprovedKey =
    R"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)";

// The Run value's name: "FloServer", or "FloServer (dev)" for the service
// instance "floserverd-dev" (FLOSERVER_INSTANCE), so copies don't clash.
[[nodiscard]] std::string run_value_name(std::string_view service_instance);

// The instance's suffix ("dev" for "floserverd-dev"); empty for the default one.
[[nodiscard]] std::string instance_suffix(std::string_view service_instance);

// "C:\Program Files\FloServer\FloServer.exe" --tray: the full path, always
// in quotes, then --instance <suffix> for a copy that isn't the default.
// Empty when the path can't be written that way (empty, not absolute, or
// holding a quote or a line break).
[[nodiscard]] std::string run_command(std::string_view exe_path, bool tray, std::string_view service_instance);

struct RunEntry {
    std::string exe;  // the program, without quotes
    bool tray = false;
    std::string instance;  // --instance's suffix; empty: the default
};
// What a Run value holds, read back; nothing when it's empty.
[[nodiscard]] std::optional<RunEntry> parse_run_command(std::string_view command);

// FloServer.exe's arguments (without the program): --tray and --instance's suffix.
struct StartArgs {
    bool tray = false;
    std::string instance;  // empty: none given, or not a usable name
};
[[nodiscard]] StartArgs parse_start_args(const std::vector<std::string>& args);

// A StartupApproved\Run value: Task Manager turned it off (its first byte odd: 0x03, 0x01).
[[nodiscard]] bool startup_disabled(const std::vector<unsigned char>& value);

// --- licences ------------------------------------------------------------------

struct LicenceLine {
    std::string name;
    std::string version;
    std::string licence;  // an SPDX name: "MIT", "PHP-3.01"
    std::string where;    // a web address, or empty
    bool test_only = false;
};

// The libraries built into FloServer (third_party/), and doctest (tests only).
[[nodiscard]] std::vector<LicenceLine> library_licences();
// runtimes.list's "installed", by name then version; each folder once.
[[nodiscard]] std::vector<LicenceLine> runtime_licences(const nlohmann::json& runtimes_reply);
// The text opened in Notepad. `runtimes_reply` is null when runtimes.list
// hasn't answered yet.
[[nodiscard]] std::string licences_text(Lang lang, std::string_view version, const nlohmann::json* runtimes_reply);

}  // namespace floserver::ui

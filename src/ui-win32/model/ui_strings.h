#pragma once

// The window's words in English, Arabic and Kurdish (Sorani) (D13).
// Plain C++ with no Windows code, so it's built and tested on every platform.
//
// Text is UTF-8. Patterns hold {0}, {1}... for values; fill() wraps each value
// in a left-to-right isolate (U+2066 ... U+2069) when the language is written
// right to left, because the values are code words: names, addresses,
// versions, ports, commands. Numbers use Western digits in every language.

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace floserver::ui {

enum class Lang : std::uint8_t { En, Ar, Ku };

// "en", "ar", "ku" (also "ckb", Sorani's own code; case and a region such as
// "ar-IQ" are ignored). Nothing for anything else.
[[nodiscard]] std::optional<Lang> parse_lang(std::string_view code);

// FLOSERVER_LANG's value (nullptr when unset): English unless it names one of ours.
[[nodiscard]] Lang lang_from_setting(const char* value);

[[nodiscard]] bool is_rtl(Lang lang);

enum class Str : std::uint8_t {
    Projects,
    SearchProjects,
    NewProject,
    ComingSoon,
    Settings,
    StateOn,
    StateAsleep,
    StateOff,
    StateNeedsDownload,
    StateProblem,
    PhpCantStart,
    Needs,             // {0}: what's missing, "php 8.2, mariadb 11.4"
    Connecting,
    StartingService,
    ServiceFailed,     // {0}: the reason, from the service
    ServiceStopped,    // clickable: starts it again
    ConnectionLost,
    NoProjects,        // {0}: the command that adds one
    NoMatches,         // {0}: the search text
    FooterCounts,      // {0} running, {1} asleep
    FooterNginx,       // {0} http port, {1} https port
    FooterNginxOff,
    FooterMemory,      // {0}: "23.4 MB"
    ChangeFailed,      // {0}: project, {1}: the service's reason
    TrayOpen,
    TrayQuit,
    Stopping,
    Count_,
};

[[nodiscard]] std::string_view text(Lang lang, Str key);

// U+2066 LEFT-TO-RIGHT ISOLATE, the text, U+2069 POP DIRECTIONAL ISOLATE.
[[nodiscard]] std::string isolate_ltr(std::string_view s);
// U+2068 FIRST STRONG ISOLATE ... U+2069: for text the user typed, which may
// be in either direction.
[[nodiscard]] std::string isolate_auto(std::string_view s);

// Puts values into a pattern: "{0}" is the first value, "{1}" the second.
// Values are isolated as left-to-right in right-to-left languages.
[[nodiscard]] std::string fill(Lang lang, Str key, std::initializer_list<std::string_view> values);
[[nodiscard]] std::string fill_pattern(std::string_view pattern, bool isolate,
                                       std::initializer_list<std::string_view> values);

}  // namespace floserver::ui

#pragma once

// What the main window shows, made from the service's replies. Plain C++ so
// it's tested on every platform; the window only draws it (D1: the rules
// stay in the service, this only reads its answers).

#include "model/ui_strings.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

// The row's state label (mockup: On / Sleeping / Off / Needs download / Problem).
enum class RowState : std::uint8_t { On, Asleep, Off, NeedsDownload, Problem };

struct ProjectRow {
    std::string name;
    bool on = false;            // the switch
    bool keep_running = false;
    RowState state = RowState::Off;
    // The address while it's served, else empty.
    std::string address;
    // Shown under the name when there's no address: the folder, what's
    // missing or the problem. Already in the window's language, with code
    // words isolated in right-to-left languages.
    std::string detail;
    // "PHP 8.5.12", "MariaDB 11.4": only what the service reports.
    std::vector<std::string> chips;
};

// projects.list's result ({"projects": [...]}). Entries without a name are skipped.
[[nodiscard]] std::vector<ProjectRow> rows_from_list(const nlohmann::json& result, Lang lang);

[[nodiscard]] Str state_label(RowState state);

// "mariadb-11.4" -> "MariaDB 11.4"; unknown families keep their name.
[[nodiscard]] std::string database_chip(std::string_view database);

// Only http:// or https:// to a host ending in ".localhost" (letters, digits,
// '-' and '.'), with an optional port 1-65535 and nothing after it, or a
// single "/". Anything else is never handed to the shell.
[[nodiscard]] bool is_openable_address(std::string_view address);

// Case-insensitive (ASCII) match of the search text on the name or address.
// Empty or blank text matches every row.
[[nodiscard]] bool row_matches(const ProjectRow& row, std::string_view query);

// "23.4 MB" / "812.0 KB", with Western digits and a dot in every language.
[[nodiscard]] std::string human_bytes(std::uint64_t bytes);

// The footer, from the rows, `status` and `web.status`. Each piece is empty
// when the service didn't say.
struct FooterText {
    std::string counts;  // "2 running · 1 asleep"
    std::string nginx;   // "nginx on ports 80 and 443"
    std::string memory;  // "Service memory: 23.4 MB"
    bool any_running = false;
};

[[nodiscard]] FooterText footer_text(const std::vector<ProjectRow>& rows, const nlohmann::json* status,
                                     const nlohmann::json* web, Lang lang);

}  // namespace floserver::ui

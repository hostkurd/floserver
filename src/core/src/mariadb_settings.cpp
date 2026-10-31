#include "floserver/core/mariadb_settings.h"

#include "floserver/core/child_process.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <set>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::size_t kMaxOptionLength = 64;
constexpr std::size_t kMaxValueLength = 1000;
constexpr std::size_t kMaxLineLength = 1000;  // a custom.ini line, before trimming
constexpr std::size_t kMaxCustomIniBytes = std::size_t{256} * 1024;
constexpr std::size_t kMaxHelpBytes = std::size_t{4} * 1024 * 1024;
constexpr std::size_t kMaxKnownBytes = std::size_t{512} * 1024;
constexpr std::size_t kMinRootPassword = 8;
constexpr std::size_t kMaxRootPassword = 100;
constexpr std::int64_t kMinPort = 1024;

// Set by FloServer, or would move the server's files: written by it or refused.
constexpr std::array<std::string_view, 30> kManaged = {
    "datadir",           "basedir",              "port",
    "socket",            "named_pipe",           "shared_memory",
    "shared_memory_base_name", "bind_address",   "extra_port",
    "skip_networking",   "proxy_protocol_networks", "pid_file",
    "log_error",         "general_log_file",     "slow_query_log_file",
    "log_slow_query_file", "log_bin",            "log_bin_index",
    "log_basename",      "relay_log",            "relay_log_index",
    "log_tc",            "master_info_file",     "relay_log_info_file",
    "plugin_dir",        "plugin_load",          "plugin_load_add",
    "skip_grant_tables", "init_file",            "secure_file_priv",
};
// More of the same, kept apart to keep the table above readable.
constexpr std::array<std::string_view, 13> kManagedMore = {
    "user",           "innodb_data_home_dir", "innodb_log_group_home_dir", "innodb_undo_directory",
    "aria_log_dir_path", "lc_messages_dir",   "character_sets_dir",        "debug",
    "defaults_file",  "defaults_extra_file",  "defaults_group_suffix",     "no_defaults",
    "print_defaults",
};
// Real options that happen to start a managed name: not taken for a
// shortened one ("general_log" is not general_log_file).
constexpr std::array<std::string_view, 5> kRealPrefixes = {"general_log", "slow_query_log", "log_slow_query", "lc_messages",
                                                           "innodb"};

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

std::string trimmed(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r");
    return std::string(s.substr(b, e - b + 1));
}

bool is_option_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

// Why: in the user's words. `managed` is the managed name it was taken for.
std::string why_refused(std::string_view name, std::string_view managed) {
    const std::string n(name);
    if (managed == "bind_address" || managed == "extra_port" || managed == "skip_networking" ||
        managed == "proxy_protocol_networks" || managed.starts_with("wsrep")) {
        return n + " can't be set: FloServer keeps MariaDB on this computer only (127.0.0.1), reached through its own port";
    }
    if (managed == "skip_grant_tables" || managed == "init_file") {
        return n + " can't be set: it would let anyone in without a password";
    }
    if (managed == "port" || managed == "socket" || managed == "named_pipe" || managed.starts_with("shared_memory")) {
        return n + " can't be set here: FloServer chooses how MariaDB is reached (change the port in Settings > Database)";
    }
    return n + " is set by FloServer (where MariaDB keeps its files and logs) and can't be changed";
}

}  // namespace

std::string canonical_mariadb_option(std::string_view name) {
    std::string out = lower(trimmed(name));
    std::replace(out.begin(), out.end(), '-', '_');
    return out;
}

std::optional<std::string> mariadb_option_problem(std::string_view name) {
    const std::string shown = trimmed(name);
    if (shown.empty() || shown.size() > kMaxOptionLength || !std::all_of(shown.begin(), shown.end(), is_option_char) ||
        shown.front() == '-' || shown.front() == '_') {
        const std::string cut = shown.size() > kMaxOptionLength ? shown.substr(0, kMaxOptionLength) + "..." : shown;
        return "\"" + cut + "\" isn't a my.ini option name (letters, digits, _ and -)";
    }
    std::string full = canonical_mariadb_option(shown);
    while (full.starts_with("loose_")) {
        full.erase(0, 6);
    }
    std::string base = full;
    for (const std::string_view prefix : {"skip_", "enable_", "disable_", "maximum_"}) {
        if (base.starts_with(prefix)) {
            base.erase(0, prefix.size());
            break;
        }
    }
    const auto check = [&](const std::string& n) -> std::optional<std::string> {
        if (n.empty()) {
            return std::nullopt;
        }
        if (n.starts_with("wsrep")) {
            return why_refused(shown, "wsrep");
        }
        const bool real = std::find(kRealPrefixes.begin(), kRealPrefixes.end(), n) != kRealPrefixes.end();
        const auto match = [&](std::string_view managed) {
            // The name itself, or a shortened form MariaDB would take for it.
            return n == managed || (!real && managed.starts_with(n));
        };
        for (const std::string_view m : kManaged) {
            if (match(m)) {
                return why_refused(shown, m);
            }
        }
        for (const std::string_view m : kManagedMore) {
            if (match(m)) {
                return why_refused(shown, m);
            }
        }
        return std::nullopt;
    };
    if (auto problem = check(full)) {
        return problem;
    }
    if (base != full) {
        if (auto problem = check(base)) {
            return problem;
        }
        // enable-/disable-networking read as skip-networking's other spelling.
        if ("skip_" + base != full) {
            if (auto problem = check("skip_" + base)) {
                return problem;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> mariadb_value_problem(std::string_view value) {
    if (value.size() > kMaxValueLength) {
        return "the value is longer than 1000 characters";
    }
    if (std::any_of(value.begin(), value.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; })) {
        return "the value must be one line, with no control characters";
    }
    return std::nullopt;
}

const std::vector<std::pair<std::string, std::string>>& mariadb_ini_defaults() {
    // A development server: small memory, utf8mb4 everywhere.
    static const std::vector<std::pair<std::string, std::string>> defaults = {
        {"character_set_server", "utf8mb4"},
        {"collation_server", "utf8mb4_unicode_ci"},
        {"innodb_buffer_pool_size", "64M"},
    };
    return defaults;
}

std::optional<std::string> mariadb_request_series(std::string_view series) {
    const auto v = Version::parse(series);
    if (!v || v->parts().size() != 2) {
        return std::nullopt;
    }
    return v->to_string();
}

MariaDbSettings read_mariadb_settings(const AppConfig& app, std::vector<std::string>* problems) {
    MariaDbSettings out;
    const auto problem = [problems](std::string_view series, std::string_view what) {
        if (problems != nullptr) {
            std::string text = "config.json: mariadb ";
            if (!series.empty()) {
                text.append(series).append(" ");
            }
            problems->push_back(text.append(what));
        }
    };
    // A copy: GCC's -O3 null-dereference check trips on iterators into nlohmann::json.
    const json stored = app.other.is_object() && app.other.contains("mariadb") ? app.other.at("mariadb") : json();
    if (stored.is_null()) {
        return out;
    }
    if (!stored.is_object()) {
        problem("", R"(must be an object such as {"11.4": {...}})");
        return out;
    }
    for (const auto& [name, entry] : stored.items()) {
        const auto parsed = mariadb_request_series(name);
        if (!parsed) {
            problem(name, "isn't a MariaDB series such as 11.4");
            continue;
        }
        const std::string& series = *parsed;
        if (!entry.is_object()) {
            problem(name, "must be an object");
            continue;
        }
        MariaDbSeriesSettings s;
        if (entry.contains("port")) {
            const json port = entry.at("port");
            if (port.is_number_integer() && port.get<std::int64_t>() >= kMinPort && port.get<std::int64_t>() <= 65535) {
                s.port = static_cast<std::uint16_t>(port.get<std::int64_t>());
            } else if (!port.is_null()) {
                problem(name, "port must be a number from 1024 to 65535");
            }
        }
        if (entry.contains("keep_running")) {
            const json keep = entry.at("keep_running");
            if (keep.is_boolean()) {
                s.keep_running = keep.get<bool>();
            } else {
                problem(name, "keep_running must be true or false");
            }
        }
        if (entry.contains("ini")) {
            const json ini = entry.at("ini");
            if (!ini.is_object()) {
                problem(name, "ini must be an object of text values");
            } else {
                for (const auto& [k, v] : ini.items()) {
                    if (!v.is_string() || mariadb_option_problem(k) || mariadb_value_problem(v.get<std::string>())) {
                        problem(name, "ini " + k + " is left out");
                        continue;
                    }
                    s.ini[canonical_mariadb_option(k)] = v.get<std::string>();
                }
            }
        }
        out.try_emplace(series, std::move(s));
    }
    return out;
}

void write_mariadb_series(AppConfig& app, const std::string& series, const MariaDbSeriesSettings& settings) {
    if (!app.other.is_object()) {
        app.other = json::object();
    }
    json& all = app.other["mariadb"];
    if (!all.is_object()) {
        all = json::object();
    }
    std::vector<std::string> same;
    for (const auto& [key, value] : all.items()) {
        if (key != series && mariadb_request_series(key) == series) {
            same.push_back(key);
        }
    }
    for (const std::string& key : same) {
        all.erase(key);
    }
    json& entry = all[series];
    if (!entry.is_object()) {
        entry = json::object();
    }
    if (settings.port) {
        entry["port"] = *settings.port;
    } else {
        entry.erase("port");
    }
    if (settings.keep_running) {
        entry["keep_running"] = true;
    } else {
        entry.erase("keep_running");
    }
    if (settings.ini.empty()) {
        entry.erase("ini");
    } else {
        entry["ini"] = settings.ini;
    }
    if (entry.empty()) {
        all.erase(series);
    }
    if (all.empty()) {
        app.other.erase("mariadb");
    }
}

fs::path mariadb_custom_ini_file(const fs::path& data, const std::string& series) {
    return data / "mariadb" / path_from_utf8(series) / "custom.ini";
}

std::string mariadb_custom_ini_header(const std::string& series) {
    return "# Your own my.ini lines for MariaDB " + series +
           ".\n"
           "# FloServer adds them after the values in Settings > Database, in the [mysqld] group, so a line here wins.\n"
           "# Lines that would move MariaDB's files, change its port, open it beyond this computer or let\n"
           "# anyone in without a password are left out (Show the final my.ini says which).\n"
           "# Changes are used the next time MariaDB " +
           series + " starts: Restart it in Settings > Database.\n\n";
}

bool ensure_mariadb_custom_ini(const fs::path& data, const std::string& series, std::string& error) {
    const fs::path file = mariadb_custom_ini_file(data, series);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(file, ec))) {
        return true;
    }
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "can't make " + path_to_utf8(file.parent_path()) + ": " + error_text(ec);
        return false;
    }
    return write_file_atomically(file, mariadb_custom_ini_header(series), error);
}

std::string read_mariadb_custom_ini(const fs::path& data, const std::string& series) {
    const fs::path file = mariadb_custom_ini_file(data, series);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return {};
    }
    std::string text = read_small_file(file, kMaxCustomIniBytes).value_or(std::string());
    if (text == mariadb_custom_ini_header(series)) {
        return {};
    }
    return text;
}

FilteredCustomIni filter_mariadb_custom_ini(std::string_view text) {
    FilteredCustomIni out;
    std::set<std::string> keys;
    std::size_t pos = 0;
    std::size_t number = 0;
    // A UTF-8 byte order mark (Notepad's "UTF-8 with BOM") isn't part of the first line.
    if (text.starts_with("\xEF\xBB\xBF")) {
        pos = 3;
    }
    while (pos < text.size()) {
        const auto end = text.find('\n', pos);
        const std::string_view raw = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? text.size() : end + 1;
        ++number;
        // Nothing the user wrote goes into my.ini unchecked, and no line of
        // it is long: MariaDB reads option files in 4 KB pieces, so the end
        // of a long line (a comment's too) would be read as a line of its own.
        const auto leave_out = [&](const std::string& why) {
            out.problems.push_back("line " + std::to_string(number) + ": " + why);
            out.text += "# FloServer left out line " + std::to_string(number) + "\n";
        };
        if (raw.size() > kMaxLineLength) {
            leave_out("the line is longer than 1000 characters");
            continue;
        }
        const std::string line = trimmed(raw);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;  // comments stay in custom.ini only
        }
        if (line.front() == '!') {
            leave_out("!include can't be used: every line must be here, where FloServer can check it");
            continue;
        }
        if (line.front() == '[') {
            const bool group = line.size() > 2 && line.back() == ']' &&
                               std::all_of(line.begin() + 1, line.end() - 1,
                                           [](char c) { return is_option_char(c) || c == '.'; });
            if (group) {
                out.text += line + "\n";
            } else {
                leave_out("a group is written as [name]");
            }
            continue;
        }
        const auto eq = line.find('=');
        const std::string key = trimmed(std::string_view(line).substr(0, eq));
        const std::string value = eq == std::string::npos ? std::string() : trimmed(std::string_view(line).substr(eq + 1));
        if (const auto problem = mariadb_option_problem(key)) {
            leave_out(*problem);
            continue;
        }
        if (const auto problem = mariadb_value_problem(value)) {
            leave_out(key + ": " + *problem);
            continue;
        }
        keys.insert(canonical_mariadb_option(key));
        out.text += key;
        if (eq != std::string::npos) {
            out.text.append("=").append(value);
        }
        out.text += "\n";
    }
    out.keys.assign(keys.begin(), keys.end());
    return out;
}

MariaDbExtras mariadb_extras(const fs::path& data, const std::string& series, const MariaDbSeriesSettings* settings) {
    MariaDbExtras out;
    if (settings != nullptr) {
        for (const auto& [k, v] : settings->ini) {
            if (!mariadb_option_problem(k) && !mariadb_value_problem(v)) {
                out.ini[k] = v;
            }
        }
    }
    const std::string custom = read_mariadb_custom_ini(data, series);
    if (!custom.empty()) {
        out.custom_ini = filter_mariadb_custom_ini(custom).text;
        out.custom_ini_path = path_to_utf8(mariadb_custom_ini_file(data, series));
    }
    return out;
}

std::optional<std::string> root_password_problem(std::string_view password) {
    if (password.size() < kMinRootPassword) {
        return "the password must be at least 8 characters long";
    }
    if (password.size() > kMaxRootPassword) {
        return "the password must be at most 100 characters long";
    }
    if (!std::all_of(password.begin(), password.end(), [](char c) { return c > 0x20 && c < 0x7f; })) {
        return "the password may use English letters, digits and symbols, with no spaces";
    }
    return std::nullopt;
}

std::vector<std::string> parse_mariadb_help(std::string_view text) {
    std::set<std::string> names;
    bool table = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto end = text.find('\n', pos);
        std::string_view line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (table) {
            // "max-connections        151", until the empty line after the table.
            if (line.empty()) {
                table = false;
                continue;
            }
            const auto stop = line.find(' ');
            const std::string_view name = line.substr(0, stop);
            if (!name.empty() && std::all_of(name.begin(), name.end(), is_option_char)) {
                names.insert(canonical_mariadb_option(name));
            }
            continue;
        }
        if (line.starts_with("-----") && line.find(" -----") != std::string_view::npos) {
            table = true;
            continue;
        }
        // "  --max-connections=# The number..." or "  -h, --datadir=name ...".
        if (!line.starts_with("  -")) {
            continue;
        }
        const auto dashes = line.find("--");
        if (dashes > 8) {  // npos too
            continue;
        }
        std::size_t e = dashes + 2;
        while (e < line.size() && is_option_char(line[e])) {
            ++e;
        }
        if (e > dashes + 2) {
            names.insert(canonical_mariadb_option(line.substr(dashes + 2, e - dashes - 2)));
        }
    }
    return {names.begin(), names.end()};
}

namespace {

std::optional<std::vector<std::string>> names_from_json(const std::optional<std::string>& text) {
    if (!text) {
        return std::nullopt;
    }
    const json doc = json::parse(*text, nullptr, false);
    if (!doc.is_array() || doc.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    out.reserve(doc.size());
    for (const json& name : doc) {
        if (name.is_string()) {
            out.push_back(name.get<std::string>());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace

std::optional<std::vector<std::string>> known_mariadb_options(const fs::path& server_program, const fs::path& cache_file,
                                                              std::string& error, bool& timed_out,
                                                              std::chrono::milliseconds limit) {
    timed_out = false;
    if (auto cached = names_from_json(read_small_file(cache_file, kMaxKnownBytes))) {
        return cached;
    }
    std::error_code ec;
    if (!fs::is_regular_file(server_program, ec)) {
        error = "mariadbd is missing";
        return std::nullopt;
    }
    const fs::path folder = cache_file.parent_path();
    fs::create_directories(folder, ec);
    // A file of this run alone: the window and the CLI may ask at once.
    static std::atomic<std::uint64_t> runs{0};
    const fs::path out = folder / path_from_utf8(path_to_utf8(cache_file.stem()) + "." + std::to_string(current_pid()) +
                                                 "-" + std::to_string(runs.fetch_add(1)) + ".out");
    ChildOptions o;
    o.program = server_program;
    // --no-defaults first: no option file of this computer's is read.
    o.args = {"--no-defaults", "--verbose", "--help"};
    o.working_dir = folder;
    o.output_log = out;
    std::optional<std::vector<std::string>> names;
    if (auto child = ChildProcess::start(o, error)) {
        if (!child->wait(limit)) {
            child->kill();  // stuck: it would hold its files open for good
            timed_out = true;
            error = "mariadbd didn't list its options in time";
        } else {
            auto list = parse_mariadb_help(read_small_file(out, kMaxHelpBytes).value_or(std::string()));
            // A real list has hundreds; a few names are an error message.
            if (list.size() >= 50) {
                names = std::move(list);
            } else {
                error = "mariadbd didn't list its options";
            }
        }
    }
    fs::remove(out, ec);
    if (!names) {
        return std::nullopt;
    }
    // Kept for next time; an older install's list goes.
    std::vector<fs::path> older;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = path_to_utf8(it->path().filename());
        if (name.starts_with("known-options-") && name.ends_with(".json") && it->path().filename() != cache_file.filename()) {
            older.push_back(it->path());
        }
    }
    for (const fs::path& p : older) {
        fs::remove(p, ec);
    }
    std::string ignored;
    (void)write_file_atomically(cache_file, json(*names).dump(), ignored);
    return names;
}

}  // namespace floserver::core

#include "floserver/core/php_settings.h"

#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/sha256.h"
#include "floserver/core/version.h"

#include <algorithm>
#include <set>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::size_t kMaxExtensionLength = 32;
constexpr std::size_t kMaxIniKeyLength = 64;
constexpr std::size_t kMaxIniValueLength = 1000;
constexpr std::size_t kMaxCustomIniBytes = std::size_t{256} * 1024;

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

}  // namespace

const std::vector<std::string>& base_php_extensions() {
    static const std::vector<std::string> base = {"curl",      "fileinfo",   "gd",     "intl",   "mbstring", "mysqli",
                                                  "openssl",   "pdo_mysql",  "pdo_sqlite", "sodium", "zip"};
    return base;
}

const std::vector<std::pair<std::string, std::string>>& php_ini_defaults() {
    static const std::vector<std::pair<std::string, std::string>> defaults = {
        {"memory_limit", "512M"},
        {"max_execution_time", "300"},
        {"max_input_time", "300"},
        {"post_max_size", "512M"},
        {"upload_max_filesize", "512M"},
        {"max_file_uploads", "50"},
        {"display_errors", "On"},
        {"display_startup_errors", "On"},
        {"error_reporting", "E_ALL"},
        {"log_errors", "On"},
        {"default_charset", "\"UTF-8\""},
        {"cgi.fix_pathinfo", "1"},
        {"opcache.enable", "1"},
        {"opcache.validate_timestamps", "1"},
        {"opcache.revalidate_freq", "0"},
    };
    return defaults;
}

bool is_php_extension_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxExtensionLength) {
        return false;
    }
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

std::optional<std::string> php_ini_key_problem(std::string_view key) {
    const bool plain = !key.empty() && key.size() <= kMaxIniKeyLength &&
                       std::all_of(key.begin(), key.end(), [](char c) {
                           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                  c == '_' || c == '.' || c == '-';
                       });
    if (!plain) {
        return "\"" + std::string(key) + "\" isn't a php.ini setting name (letters, digits, _ . -)";
    }
    const std::string k = lower(key);
    if (k == "extension" || k == "zend_extension") {
        return "extensions are turned on and off in the extensions list, not as a php.ini value";
    }
    if (k == "extension_dir" || k == "error_log") {
        return std::string(key) + " is set by FloServer and can't be changed";
    }
    // What keeps opcache on and seeing edited files at once; its other
    // settings (the JIT, memory) can be changed.
    if (k == "opcache.enable" || k == "opcache.validate_timestamps" || k == "opcache.revalidate_freq") {
        return std::string(key) + " is set by FloServer (opcache always on, edits seen at once) and can't be changed";
    }
    return std::nullopt;
}

std::optional<std::string> php_ini_value_problem(std::string_view value) {
    if (value.size() > kMaxIniValueLength) {
        return "the value is longer than 1000 characters";
    }
    // One line: a value can't start a line of its own in php.ini.
    if (std::any_of(value.begin(), value.end(),
                    [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; })) {
        return "the value must be one line, with no control characters";
    }
    // php.ini stops reading at a line it can't parse, so one bad value would
    // silently drop every setting after it (custom.ini, the project's). Only
    // what PHP reads as a plain value is let through: no $ (PHP expands
    // ${...}), and = ; " ' { } [ ] only inside one pair of double quotes.
    if (value.find('$') != std::string_view::npos) {
        return "the value can't contain $";
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        const std::string_view inner = value.substr(1, value.size() - 2);
        if (inner.find('"') != std::string_view::npos || inner.ends_with('\\')) {
            return R"(a value in double quotes can't contain another " or end with \)";
        }
        return std::nullopt;
    }
    if (value.find_first_of(R"(=;"'{}[]`)") != std::string_view::npos) {
        return R"(put the value in double quotes when it contains = ; " ' { } [ ] or `)";
    }
    return std::nullopt;
}

std::optional<std::string> php_series_of(std::string_view version) {
    const auto v = Version::parse(version);
    if (!v || v->parts().size() < 2) {
        return std::nullopt;
    }
    return std::to_string(v->parts()[0]) + "." + std::to_string(v->parts()[1]);
}

PhpSettings read_php_settings(const AppConfig& app, std::vector<std::string>* problems) {
    PhpSettings out;
    // "config.json: php <series> <what>".
    const auto problem = [problems](std::string_view series, std::string_view what, std::string_view item = {},
                                    std::string_view rest = {}) {
        if (problems != nullptr) {
            std::string text = "config.json: php ";
            if (!series.empty()) {
                text.append(series).append(" ");
            }
            text.append(what).append(item).append(rest);
            problems->push_back(std::move(text));
        }
    };
    // A copy (a few keys): GCC's -O3 null-dereference check trips on
    // iterators into nlohmann::json.
    const json stored = app.other.is_object() && app.other.contains("php") ? app.other.at("php") : json();
    if (stored.is_null()) {
        return out;
    }
    if (!stored.is_object()) {
        problem("", R"(must be an object such as {"8.5": {...}})");
        return out;
    }
    for (const auto& [name, entry] : stored.items()) {
        const auto parsed = Version::parse(name);
        if (!parsed || parsed->parts().size() != 2) {
            problem(name, "isn't a PHP series such as 8.5");
            continue;
        }
        const std::string series = parsed->to_string();
        if (!entry.is_object()) {
            problem(name, "must be an object");
            continue;
        }
        PhpSeriesSettings s;
        if (entry.contains("extensions")) {
            const json ext = entry.at("extensions");
            if (!ext.is_object()) {
                problem(name, "extensions must be an object of true/false");
            } else {
                for (const auto& [e, on] : ext.items()) {
                    if (!is_php_extension_name(e) || !on.is_boolean()) {
                        problem(name, "extension ", e, " is left out");
                        continue;
                    }
                    if (e != "opcache") {
                        s.extensions[e] = on.get<bool>();
                    }
                }
            }
        }
        if (entry.contains("ini")) {
            const json ini = entry.at("ini");
            if (!ini.is_object()) {
                problem(name, "ini must be an object of text values");
            } else {
                for (const auto& [k, v] : ini.items()) {
                    if (!v.is_string() || php_ini_key_problem(k) || php_ini_value_problem(v.get<std::string>())) {
                        problem(name, "ini ", k, " is left out");
                        continue;
                    }
                    s.ini[k] = v.get<std::string>();
                }
            }
        }
        // "8.05" and "8.5" are the same series; the first one read wins.
        out.try_emplace(series, std::move(s));
    }
    return out;
}

void write_php_series(AppConfig& app, const std::string& series, const PhpSeriesSettings& settings) {
    if (!app.other.is_object()) {
        app.other = json::object();
    }
    json& all = app.other["php"];
    if (!all.is_object()) {
        all = json::object();  // unusable before: replaced
    }
    // Other spellings of the same series ("08.5" for "8.5"), written by
    // hand, would hide this one from the reader: they go.
    std::vector<std::string> same;
    for (const auto& [key, value] : all.items()) {
        if (key != series && php_series_of(key) == series) {
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
    if (settings.extensions.empty()) {
        entry.erase("extensions");
    } else {
        entry["extensions"] = settings.extensions;
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
        app.other.erase("php");
    }
}

fs::path php_custom_ini_file(const fs::path& data, const std::string& series) {
    return data / "php" / path_from_utf8(series) / "custom.ini";
}

std::string php_custom_ini_header(const std::string& series) {
    return "; Your own php.ini settings for PHP " + series +
           ".\n"
           "; FloServer adds this file after its own settings for every project on PHP " +
           series +
           "\n"
           "; and for shared sites, so a value here wins over Settings -> PHP.\n"
           "; A project's own php.ini values (in its floserver.json) still win over this file.\n"
           "; Changes are used within a few seconds; PHP restarts by itself.\n\n";
}

bool ensure_php_custom_ini(const fs::path& data, const std::string& series, std::string& error) {
    const fs::path file = php_custom_ini_file(data, series);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(file, ec))) {
        return true;
    }
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "can't make " + path_to_utf8(file.parent_path()) + ": " + error_text(ec);
        return false;
    }
    return write_file_atomically(file, php_custom_ini_header(series), error);
}

std::string read_php_custom_ini(const fs::path& data, const std::string& series) {
    const fs::path file = php_custom_ini_file(data, series);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return {};
    }
    std::string text = read_small_file(file, kMaxCustomIniBytes).value_or(std::string());
    if (text == php_custom_ini_header(series)) {
        return {};
    }
    return text;
}

PhpIniSettings effective_php_settings(const std::string& series, const PhpSeriesSettings* series_settings,
                                      std::string custom_ini, const ProjectConfig* project) {
    const auto& base = base_php_extensions();
    std::set<std::string> on(base.begin(), base.end());
    const auto apply = [&on](const std::string& name, bool want) {
        if (name == "opcache") {
            return;  // always on, written by FloServer
        }
        if (want) {
            on.insert(name);
        } else {
            on.erase(name);
        }
    };
    PhpIniSettings out;
    out.series = series;
    if (series_settings != nullptr) {
        for (const auto& [name, want] : series_settings->extensions) {
            apply(name, want);
        }
        for (const auto& [k, v] : series_settings->ini) {
            if (!php_ini_key_problem(k) && !php_ini_value_problem(v)) {
                out.series_ini[k] = v;
            }
        }
    }
    if (project != nullptr) {
        for (const std::string& name : project->extensions) {
            apply(name, true);
        }
        for (const std::string& name : project->extensions_off) {
            apply(name, false);
        }
        for (const auto& [k, v] : project->php_ini) {
            if (!php_ini_key_problem(k) && !php_ini_value_problem(v)) {
                out.project_ini[k] = v;
            }
        }
    }
    for (const std::string& name : on) {
        if (std::find(base.begin(), base.end(), name) == base.end()) {
            out.extensions.push_back(name);  // a set: already sorted
        }
    }
    for (const std::string& name : base) {
        if (!on.contains(name)) {
            out.removed.push_back(name);
        }
    }
    std::sort(out.removed.begin(), out.removed.end());
    out.custom_ini = std::move(custom_ini);
    return out;
}

std::string php_settings_key(const PhpIniSettings& settings) {
    std::string key;
    for (const std::string& e : settings.extensions) {
        key += "+" + e;
    }
    if (settings.removed.empty() && settings.series_ini.empty() && settings.custom_ini.empty() &&
        settings.project_ini.empty()) {
        return key;  // as before D20: pools without settings keep their keys
    }
    // Length-prefixed, so no two different settings give the same text.
    std::string text;
    const auto add = [&text](std::string_view part) {
        text += std::to_string(part.size());
        text += ':';
        text += part;
    };
    add("removed");
    for (const std::string& e : settings.removed) {
        add(e);
    }
    add("series");
    for (const auto& [k, v] : settings.series_ini) {
        add(k);
        add(v);
    }
    add("custom");
    add(settings.custom_ini);
    add("project");
    for (const auto& [k, v] : settings.project_ini) {
        add(k);
        add(v);
    }
    Sha256 hash;
    hash.update(text);
    const Sha256Digest digest = hash.finish();
    return key + "-" + to_hex(std::span<const std::uint8_t>(digest.data(), 4));
}

}  // namespace floserver::core

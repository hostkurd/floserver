#include "model/folders_page.h"

#include "model/runtimes_page.h"  // size_text

#include <algorithm>

namespace floserver::ui {

using nlohmann::json;

namespace {

std::string string_of(const json& j, const char* key) {
    return j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string();
}

std::uint64_t bytes_of(const json& j, const char* key, std::uint64_t fallback) {
    if (!j.contains(key) || !j.at(key).is_number_integer()) {
        return fallback;
    }
    const json& v = j.at(key);
    if (v.is_number_unsigned()) {
        return v.get<std::uint64_t>();
    }
    const auto signed_value = v.get<std::int64_t>();
    return signed_value < 0 ? fallback : static_cast<std::uint64_t>(signed_value);
}

bool bool_of(const json& j, const char* key, bool fallback) {
    return j.contains(key) && j.at(key).is_boolean() ? j.at(key).get<bool>() : fallback;
}

}  // namespace

std::optional<FoldersInfo> folders_from_reply(const json& result) {
    if (!result.is_object() || !result.contains("data_dir") || !result.at("data_dir").is_string()) {
        return std::nullopt;
    }
    FoldersInfo info;
    merge_folders_reply(info, result);
    return info;
}

void merge_folders_reply(FoldersInfo& info, const json& result) {
    if (!result.is_object()) {
        return;
    }
    if (result.contains("custom")) {  // folders.get's and folders.set's
        info.projects_dir = string_of(result, "projects_dir");
        info.default_projects_dir = string_of(result, "default_projects_dir");
        info.custom = bool_of(result, "custom", false);
    }
    if (result.contains("data_dir")) {
        info.data_dir = string_of(result, "data_dir");
        info.data_bytes = bytes_of(result, "data_bytes", 0);
        info.data_complete = bool_of(result, "data_complete", true);
    } else if (result.contains("freed")) {
        const std::uint64_t freed = bytes_of(result, "freed", 0);
        info.data_bytes = freed < info.data_bytes ? info.data_bytes - freed : 0;
    }
    info.logs_bytes = bytes_of(result, "logs_bytes", info.logs_bytes);
    info.leftovers_bytes = bytes_of(result, "leftovers_bytes", info.leftovers_bytes);
    info.installing = bool_of(result, "installing", info.installing);
}

std::string disk_size_text(std::uint64_t bytes) {
    constexpr std::uint64_t kGb = std::uint64_t{1024} * 1024 * 1024;
    if (bytes < kGb) {
        return size_text(bytes);
    }
    const std::uint64_t tenths = (bytes * 10 + kGb / 2) / kGb;
    if (tenths >= 100 || tenths % 10 == 0) {
        return std::to_string((tenths + 5) / 10) + " GB";
    }
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " GB";
}

std::string data_usage_text(const FoldersInfo& info, Lang lang) {
    return fill(lang, info.data_complete ? Str::FoldersUses : Str::FoldersUsesAtLeast, {disk_size_text(info.data_bytes)});
}

std::string clean_size_text(std::uint64_t bytes, bool installing, Lang lang) {
    if (bytes > 0) {
        return fill(lang, Str::FoldersFrees, {disk_size_text(bytes)});
    }
    return std::string(text(lang, installing ? Str::FoldersWaitInstall : Str::FoldersNothing));
}

std::vector<SkippedItem> skipped_from_reply(const json& result) {
    std::vector<SkippedItem> out;
    if (!result.is_object() || !result.contains("skipped_items") || !result.at("skipped_items").is_array()) {
        return out;
    }
    for (const json& item : result.at("skipped_items")) {
        if (item.is_object() && !string_of(item, "path").empty()) {
            out.push_back(
                {string_of(item, "path"), string_of(item, "why"), string_of(item, "reason"), string_of(item, "inside")});
        }
    }
    return out;
}

std::string skip_reason_text(const SkippedItem& item, Lang lang) {
    if (item.why == "in_use") {
        return std::string(text(lang, Str::FoldersSkipInUse));
    }
    if (item.why == "link") {
        return std::string(text(lang, Str::FoldersSkipLink));
    }
    if (item.why == "hard_link") {
        return std::string(text(lang, Str::FoldersSkipHardLink));
    }
    if (item.why == "protected") {
        if (item.inside.starts_with("project:")) {
            return fill(lang, Str::FoldersSkipInProject, {item.inside.substr(8)});
        }
        if (item.inside == "projects") {
            return std::string(text(lang, Str::FoldersSkipInProjects));
        }
        if (item.inside == "www") {
            return std::string(text(lang, Str::FoldersSkipInWww));
        }
        return std::string(text(lang, Str::FoldersSkipProtected));
    }
    if (item.why == "unreadable") {
        return std::string(text(lang, Str::FoldersSkipUnreadable));
    }
    if (item.why == "not_allowed") {
        return std::string(text(lang, Str::FoldersSkipNotAllowed));
    }
    // A reason a newer service has: its own words (English), read left to right.
    return is_rtl(lang) && !item.reason.empty() ? isolate_ltr(item.reason) : item.reason;
}

std::string skipped_list_text(const std::vector<SkippedItem>& items, Lang lang) {
    std::string out;
    const std::string_view separator = is_rtl(lang) ? "\xD8\x9B " : "; ";  // ؛ in Arabic script
    const std::size_t shown = std::min(items.size(), kSkippedShown);
    for (std::size_t i = 0; i < shown; ++i) {
        if (i > 0) {
            out += separator;
        }
        // The path is read left to right whatever the language.
        out += is_rtl(lang) ? isolate_ltr(items[i].path) : items[i].path;
        const std::string why = skip_reason_text(items[i], lang);
        if (!why.empty()) {
            out += " (" + why + ")";
        }
    }
    if (items.size() > shown) {
        out += separator;
        out += fill(lang, Str::FoldersSkippedMore, {std::to_string(items.size() - shown)});
    }
    return out;
}

std::string clean_done_text(const json& result, Lang lang) {
    const std::uint64_t freed = result.is_object() ? bytes_of(result, "freed", 0) : 0;
    const std::uint64_t skipped = result.is_object() ? bytes_of(result, "skipped", 0) : 0;
    if (const auto items = skipped_from_reply(result); !items.empty()) {
        const std::string size = disk_size_text(freed);
        return fill_pattern(text(lang, Str::FoldersFreedLeft), false,
                            {is_rtl(lang) ? isolate_ltr(size) : size, skipped_list_text(items, lang)});
    }
    if (skipped > 0) {  // an older service: only how many
        return fill(lang, Str::FoldersFreedSkipped, {disk_size_text(freed), std::to_string(skipped)});
    }
    return fill(lang, Str::FoldersFreed, {disk_size_text(freed)});
}

SharedPage shared_page(const SharedSites& shared, const Installed& installed, const json& databases, Lang lang) {
    SharedPage page;
    const SharedRow row = shared_row(shared, installed, lang);
    // What they do (with the address when they're served), or what's wrong.
    page.hint = row.detail;
    page.problem = row.state == RowState::Problem;
    if (row.state != RowState::NeedsDownload && row.state != RowState::Problem) {
        const std::string host = row.address.empty() ? std::string("localhost") : row.address;
        page.hint = fill(lang, Str::SharedHint, {shared.dir, host + "/<folder>/"});
    }
    page.fields = shared_fields(shared, installed, lang);
    // User www: the www database and any www_... it makes (D19). The login
    // exists only while shared sites are on.
    if (!shared.database.empty() && shared.on) {
        page.login = true;
        const std::uint16_t port = database_port(databases, shared.database);
        const std::string host = port != 0 ? "127.0.0.1:" + std::to_string(port) : std::string("127.0.0.1");
        page.login_title = fill(lang, Str::DatabaseTitle, {"www"});
        page.login_detail = fill(lang, Str::SharedDatabaseDetail, {host, "www", "www_"});
    }
    return page;
}

}  // namespace floserver::ui

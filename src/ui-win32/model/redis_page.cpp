#include "model/redis_page.h"

#include "floserver/core/garnet.h"

#include <algorithm>

namespace floserver::ui {

namespace {

using nlohmann::json;

std::string text_of(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
    }
    return {};
}

bool flag_of(const json& j, const char* key, bool fallback) {
    if (j.is_object() && j.contains(key) && j[key].is_boolean()) {
        return j[key].get<bool>();
    }
    return fallback;
}

MailState state_of(std::string_view s) {
    if (s == "asleep") {
        return MailState::Asleep;
    }
    if (s == "starting") {
        return MailState::Starting;
    }
    if (s == "running" || s == "saving") {
        return MailState::Running;
    }
    if (s == "failed") {
        return MailState::Failed;
    }
    return MailState::NotInstalled;
}

std::optional<int> parse_memory(std::string_view s) {
    if (s.empty() || s.size() > 6) {
        return std::nullopt;
    }
    int n = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        n = n * 10 + (c - '0');
    }
    if (n < kRedisMemoryLeast || n > kRedisMemoryMost) {
        return std::nullopt;
    }
    return n;
}

std::vector<std::string> names_of(const json& j, const char* key) {
    std::vector<std::string> out;
    if (j.is_object() && j.contains(key) && j[key].is_array()) {
        for (const json& n : j[key]) {
            if (n.is_string() && !n.get_ref<const std::string&>().empty()) {
                out.push_back(mail_plain_subject(n.get<std::string>(), 80));
            }
        }
    }
    return out;
}

}  // namespace

std::optional<RedisStatus> redis_status_from_reply(const json& reply) {
    if (!reply.is_object()) {
        return std::nullopt;
    }
    RedisStatus s;
    s.installed = flag_of(reply, "installed", false);
    s.version = mail_plain_subject(text_of(reply, "version"), 40);
    s.state = s.installed ? state_of(text_of(reply, "state")) : MailState::NotInstalled;
    s.error = mail_plain_subject(text_of(reply, "error"), kMailErrorMaxChars);
    s.save_error = mail_plain_subject(text_of(reply, "save_error"), kMailErrorMaxChars);
    s.saving = s.installed && text_of(reply, "state") == "saving";
    if (reply.contains("port") && reply["port"].is_number_integer()) {
        const auto port = reply["port"].get<std::int64_t>();
        if (port > 0 && port < 65536) {
            s.port = static_cast<int>(port);
        }
    }
    if (reply.contains("keys") && reply["keys"].is_number_integer() && reply["keys"].get<std::int64_t>() > 0) {
        s.keys = static_cast<std::uint64_t>(reply["keys"].get<std::int64_t>());
    }
    s.projects_known = reply.contains("projects") && reply["projects"].is_array();
    s.projects = names_of(reply, "projects");
    s.phpredis = names_of(reply, "phpredis");
    if (reply.contains("settings") && reply["settings"].is_object()) {
        const json& o = reply["settings"];
        s.settings.keep_running = flag_of(o, "keep_running", s.settings.keep_running);
        if (o.contains("memory_mb") && o["memory_mb"].is_number_integer()) {
            const auto m = o["memory_mb"].get<std::int64_t>();
            if (m >= kRedisMemoryLeast && m <= kRedisMemoryMost) {
                s.settings.memory_mb = static_cast<int>(m);
            }
        }
    }
    return s;
}

std::string redis_state_text(const RedisStatus& status, Lang lang) {
    return status.saving ? std::string(text(lang, Str::RedisSaving)) : mail_state_text(status.state, lang);
}

std::string redis_save_failed_text(const RedisStatus& status, Lang lang) {
    if (!status.installed || status.save_error.empty()) {
        return {};
    }
    // fill() isolates the value left to right in Arabic and Kurdish: Garnet's words are English.
    return fill(lang, Str::RedisSaveFailed, {status.save_error});
}

std::string redis_address_text(const RedisStatus& status) { return "127.0.0.1 : " + std::to_string(status.port); }
std::string redis_address_copy(const RedisStatus& status) { return "127.0.0.1:" + std::to_string(status.port); }

std::string_view redis_password_mask() {
    return "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"
           "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";  // twelve U+2022 BULLETs, whatever the password's length
}

std::string redis_data_text(const RedisStatus& status, Lang lang) {
    std::string out = status.keys == 1 ? std::string(text(lang, Str::RedisKeysOne))
                                       : fill(lang, Str::RedisKeys, {std::to_string(status.keys)});
    if (status.projects_known) {
        const std::size_t n = status.projects.size();
        out += " \xC2\xB7 ";
        out += n == 0   ? std::string(text(lang, Str::RedisUsedByNone))
               : n == 1 ? std::string(text(lang, Str::RedisUsedByOne))
                        : fill(lang, Str::RedisUsedBy, {std::to_string(n)});
    }
    return out;
}

bool redis_keep_running(const RedisStatus& status, const RedisChanges& changes) {
    return changes.keep_running.value_or(status.settings.keep_running);
}

void redis_toggle_keep_running(const RedisStatus& status, RedisChanges& changes) {
    const bool next = !redis_keep_running(status, changes);
    if (next == status.settings.keep_running) {
        changes.keep_running.reset();
    } else {
        changes.keep_running = next;
    }
}

std::string redis_memory_text(const RedisStatus& status, const RedisChanges& changes) {
    return changes.memory.value_or(std::to_string(status.settings.memory_mb));
}

void redis_set_memory_text(const RedisStatus& status, RedisChanges& changes, const std::string& typed) {
    if (typed == std::to_string(status.settings.memory_mb)) {
        changes.memory.reset();
    } else {
        changes.memory = typed;
    }
}

bool redis_memory_bad(const RedisChanges& changes) { return changes.memory && !parse_memory(*changes.memory); }

json redis_set_params(const RedisChanges& changes) {
    json params = json::object();
    if (changes.keep_running) {
        params["keep_running"] = *changes.keep_running;
    }
    if (changes.memory) {
        if (const auto n = parse_memory(*changes.memory)) {
            params["memory_mb"] = *n;
        }
    }
    return params;
}

void redis_apply(RedisStatus& status, const RedisChanges& changes) {
    if (changes.keep_running) {
        status.settings.keep_running = *changes.keep_running;
    }
    if (changes.memory) {
        if (const auto n = parse_memory(*changes.memory)) {
            status.settings.memory_mb = *n;
        }
    }
}

std::string redis_flush_question(const RedisStatus& status, Lang lang) {
    return fill(lang, Str::RedisFlushAsk, {std::to_string(status.keys)});
}

std::string redis_project_prefix(std::string_view project) { return core::redis_prefix(project); }

bool redis_uses_phpredis(const Installed& installed, std::string_view php_version) {
    const auto garnet = installed.find(std::string(core::kGarnetRuntime));
    const auto series = installed.find(std::string(kInstalledPhpRedisSeries));
    if (garnet == installed.end() || garnet->second.empty() || series == installed.end() || php_version.empty()) {
        return false;
    }
    const std::string want = version_series("php", php_version);
    return std::find(series->second.begin(), series->second.end(), want) != series->second.end();
}

std::string redis_panel_note(const ProjectRow& row, const Installed& installed, Lang lang) {
    const auto garnet = installed.find(std::string(core::kGarnetRuntime));
    if (garnet == installed.end() || garnet->second.empty()) {
        return std::string(text(lang, Str::PanelRedisNotInstalled));
    }
    std::string out = fill(lang, Str::PanelRedisNote, {redis_project_prefix(row.name)});
    if (row.wants.redis && !row.php.empty()) {
        out += "\n";
        out += text(lang, redis_uses_phpredis(installed, row.php) ? Str::PanelRedisPhpredis : Str::PanelRedisPredis);
    }
    return out;
}

std::string redis_switch_value(bool on) { return on ? "on" : "off"; }

}  // namespace floserver::ui

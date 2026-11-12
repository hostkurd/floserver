#pragma once

// Redis (D30, approved 11 Nov): one shared Garnet server for the projects.
// redis.status's answer, what Settings > Redis shows from it, the changes not
// saved yet and what Save sends, and the project panel's Redis switch and its
// words. Plain C++ so it's tested on every platform; the windows only draw it.

#include "model/mail_page.h"  // MailState and its words: the same five states
#include "model/ui_strings.h"
#include "model/view_model.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::ui {

struct RedisOptions {
    int memory_mb = 256;
    bool keep_running = false;
};

struct RedisStatus {
    // Fields ordered largest first (clang-tidy's padding check).
    std::uint64_t keys = 0;
    std::string version;
    std::string error;
    std::string save_error;  // the last SAVE failed (kept until one works)
    std::vector<std::string> projects;  // with Redis on; known only from Settings' own ask
    std::vector<std::string> phpredis;  // the PHP series phpredis is installed for
    RedisOptions settings;
    int port = 6379;
    MailState state = MailState::NotInstalled;
    bool installed = false;
    bool saving = false;  // saving before it's stopped: shown as such, Running's colour
    bool projects_known = false;
    // The last redis.status failed: these numbers are from before and aren't shown.
    bool stale = false;
};

// redis.status's result; missing fields take the defaults above. Nothing when
// `reply` isn't an object.
[[nodiscard]] std::optional<RedisStatus> redis_status_from_reply(const nlohmann::json& reply);

// --- Settings > Redis ---------------------------------------------------------------

// The pill's words: "Saving…" while it saves before stopping, else the state's.
[[nodiscard]] std::string redis_state_text(const RedisStatus& status, Lang lang);
// The banner when the last SAVE failed, or empty.
[[nodiscard]] std::string redis_save_failed_text(const RedisStatus& status, Lang lang);

// "127.0.0.1 : 6379", and what Copy puts on the clipboard: "127.0.0.1:6379".
[[nodiscard]] std::string redis_address_text(const RedisStatus& status);
[[nodiscard]] std::string redis_address_copy(const RedisStatus& status);
// The password as shown: never the password itself.
[[nodiscard]] std::string_view redis_password_mask();
// "12 keys · used by 3 projects" (only the keys when the projects aren't known yet).
[[nodiscard]] std::string redis_data_text(const RedisStatus& status, Lang lang);

struct RedisChanges {
    std::optional<bool> keep_running;
    std::optional<std::string> memory;  // as typed
    [[nodiscard]] bool empty() const { return !keep_running && !memory; }
};

[[nodiscard]] bool redis_keep_running(const RedisStatus& status, const RedisChanges& changes);
void redis_toggle_keep_running(const RedisStatus& status, RedisChanges& changes);

inline constexpr int kRedisMemoryLeast = 16;
inline constexpr int kRedisMemoryMost = 65536;
[[nodiscard]] std::string redis_memory_text(const RedisStatus& status, const RedisChanges& changes);
// A typed number goes back to "what's saved" when it equals it.
void redis_set_memory_text(const RedisStatus& status, RedisChanges& changes, const std::string& text);
// The typed number can't be sent: not a whole number from 16 to 65536.
[[nodiscard]] bool redis_memory_bad(const RedisChanges& changes);

// redis.set's parameters: only what changed (and a number that's valid).
[[nodiscard]] nlohmann::json redis_set_params(const RedisChanges& changes);
// The status as it is once `changes` were saved (until the next answer).
void redis_apply(RedisStatus& status, const RedisChanges& changes);

// Flush all's question, with the number of keys.
[[nodiscard]] std::string redis_flush_question(const RedisStatus& status, Lang lang);

// --- the project panel ---------------------------------------------------------------

// "shop_": the prefix the project's keys get (garnet.h's redis_prefix()).
[[nodiscard]] std::string redis_project_prefix(std::string_view project);
// Whether the project's PHP gets phpredis: Garnet is installed and phpredis
// is for its PHP's series (installed_from_list() keeps those as
// kInstalledPhpRedisSeries). PHP off or not resolved yet: predis.
[[nodiscard]] bool redis_uses_phpredis(const Installed& installed, std::string_view php_version);
// The note under the switch: where the keys go and which client the .env
// lines name, or that Garnet isn't installed.
[[nodiscard]] std::string redis_panel_note(const ProjectRow& row, const Installed& installed, Lang lang);
// projects.use's value for the switch: "on" or "off".
[[nodiscard]] std::string redis_switch_value(bool on);

}  // namespace floserver::ui

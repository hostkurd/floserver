// floserverd: the background service in the user's session (D2).
//
// The private channel, the runtimes (the signed list, installs as jobs,
// removal), the project list, the local certificate authority, and running
// the projects: nginx and the PHP pools that sleep when idle.

#include "floserver/core/builtin_templates.h"
#include "floserver/core/build_info.h"
#include "floserver/core/builtin.h"
#include "floserver/core/certificates.h"
#include "floserver/core/encoding.h"
#include "floserver/core/http.h"
#include "floserver/core/integration.h"
#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/project_service.h"
#include "floserver/core/protocol.h"
#include "floserver/core/request_log.h"
#include "floserver/core/shared_sites.h"
#include "floserver/core/runtime_service.h"
#include "floserver/core/runtime_store.h"
#include "floserver/core/trust_store.h"
#include "floserver/core/database_service.h"
#include "floserver/core/mariadb_settings.h"
#include "floserver/core/mariadb.h"
#include "floserver/core/project_config.h"
#include "floserver/core/site_plan.h"
#include "floserver/core/secret_store.h"
#include "floserver/core/template_service.h"
#include "floserver/core/web_service.h"
#include "floserver/core/web_settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace ipc = floserver::core::ipc;
namespace protocol = floserver::core::protocol;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

// Same-user programs only can connect (the channel sees to that), but cap
// them anyway so a stuck one can't pile up threads without end.
constexpr int kMaxClients = 64;

// Counts client threads so shutdown can wait for replies to be sent.
class ActiveClients {
public:
    // False when kMaxClients are already being served.
    bool try_add() {
        const std::lock_guard lock(mutex_);
        if (count_ >= kMaxClients) {
            return false;
        }
        ++count_;
        return true;
    }
    void remove() {
        {
            const std::lock_guard lock(mutex_);
            --count_;
        }
        changed_.notify_all();
    }
    // True when every client has finished.
    bool wait_until_idle(std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit, [this] { return count_ == 0; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int count_ = 0;
};

// The runtime work, or nothing when there's no data folder to keep it in.
std::unique_ptr<floserver::core::RuntimeService> make_runtimes() {
    namespace core = floserver::core;
    const auto data = core::data_dir();
    if (!data) {
        return nullptr;
    }
    core::RuntimeServiceConfig config;
    config.data_dir = *data;
    config.platform = std::string(core::current_platform());
    config.keys = core::builtin_manifest_keys();
    config.source = {std::string(core::kManifestUrl), std::string(core::kManifestSignatureUrl)};
    config.builtin_templates_json = std::string(core::kBuiltInTemplatesJson);
    // The Visual C++ runtime DLLs ship next to floserverd (cmake: vcruntime/).
    if (const auto self = core::executable_path()) {
        const auto vc = self->parent_path() / "vcruntime";
        std::error_code ec;
        if (std::filesystem::is_directory(vc, ec)) {
            config.vc_runtime_dir = vc;
        }
    }
    return std::make_unique<core::RuntimeService>(std::move(config), core::make_system_transport());
}

// FLOSERVER_WEB_PORTS ("18080,18443"), FLOSERVER_IDLE_SECONDS and
// FLOSERVER_PHP_WORKERS replace the usual ports, idle time and PHP workers
// per pool, for tests and dev copies.
floserver::core::WebServiceConfig web_config(const std::filesystem::path& data) {
    namespace core = floserver::core;
    core::WebServiceConfig config;
    config.data_dir = data;
    if (const auto ports = core::get_env("FLOSERVER_WEB_PORTS")) {
        const auto comma = ports->find(',');
        try {
            const auto http = std::stoul(ports->substr(0, comma));
            const auto https = comma == std::string::npos ? http + 363 : std::stoul(ports->substr(comma + 1));
            if (http > 0 && http < 65536 && https > 0 && https < 65536) {
                config.fixed_ports = core::WebPorts{static_cast<core::Port>(http), static_cast<core::Port>(https), true};
            }
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): ignored, the usual ports are used
        }
    }
    if (const auto workers = core::get_env("FLOSERVER_PHP_WORKERS")) {
        try {
            config.php_workers = std::clamp<std::size_t>(std::stoul(*workers), 1, 16);
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): ignored, the usual count is used
        }
    }
    if (const auto idle = core::get_env("FLOSERVER_IDLE_SECONDS")) {
        try {
            config.idle_after = std::chrono::seconds(std::max(1L, std::stol(*idle)));
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): ignored, the usual idle time is used
        }
    }
    return config;
}

std::optional<std::int64_t> job_id(const json& params) {
    if (params.contains("id") && params["id"].is_number_integer()) {
        return params["id"].get<std::int64_t>();
    }
    return std::nullopt;
}

std::string string_param(const json& params, const char* key) {
    if (params.contains(key) && params[key].is_string()) {
        return params[key].get<std::string>();
    }
    return {};
}

std::optional<bool> bool_param(const json& params, const char* key) {
    if (params.contains(key) && params[key].is_boolean()) {
        return params[key].get<bool>();
    }
    return std::nullopt;
}

struct Service {
    std::shared_ptr<ipc::Server> server;
    std::unique_ptr<floserver::core::RuntimeService> runtimes;
    std::unique_ptr<floserver::core::ProjectService> projects;
    std::unique_ptr<floserver::core::CertStore> certs;
    std::unique_ptr<floserver::core::WebService> web;
    std::unique_ptr<floserver::core::WebSettingsService> web_settings;
    std::unique_ptr<floserver::core::SecretStore> secrets;
    std::unique_ptr<floserver::core::DatabaseService> databases;
    std::unique_ptr<floserver::core::TemplateService> templates;
    std::optional<std::filesystem::path> data;

    // integration.json for FloCMS Studio (D14): rewritten when what it says
    // changed (an install, a removal, a database port).
    // Never throws: it runs on its own thread, where an exception would end
    // the service.
    void refresh_integration() const noexcept {
        if (!data) {
            return;
        }
        try {
            const json status = databases ? databases->status() : json::object();
            std::string error;
            (void)floserver::core::write_integration(
                *data, floserver::core::integration_json(floserver::core::collect_integration(*data, status)),
                error);
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): tried again in 5 s
        }
    }

    // Wakes the integration thread when the service quits.
    std::mutex integration_mutex;
    std::condition_variable integration_wake;

    // Adds what's running to a project's description: its address and
    // whether its PHP or its own server is running or asleep (and why it
    // failed).
    void add_run_state(json& project, const json& sites) const {
        const std::string name = project.value("name", std::string());
        if (!sites.contains(name)) {
            return;
        }
        const json& s = sites[name];
        for (const char* key : {"address", "run", "error"}) {
            if (s.contains(key)) {
                project[key] = s[key];
            }
        }
        // Held back by what the service found (nginx missing, a pool that
        // can't open): that wins over the plan's "ready".
        for (const char* key : {"state", "missing", "problem"}) {
            if (s.contains(key)) {
                project[key] = s[key];
            }
        }
    }
    Clock::time_point started = Clock::now();
    ActiveClients clients;
    std::atomic<bool> quitting{false};

    // The reply to one runtime or job request.
    [[nodiscard]] std::string runtime_request(const protocol::Request& r) const {
        if (!runtimes) {
            return protocol::make_error(r.id, "no_data_folder",
                                        "FloServer can't find a folder to keep runtimes in (LOCALAPPDATA or HOME)");
        }
        floserver::core::ServiceRefusal refusal;
        if (r.method == "runtimes.list") {
            const bool refresh = r.params.contains("refresh") && r.params["refresh"].is_boolean() &&
                                 r.params["refresh"].get<bool>();
            const bool installed_only = bool_param(r.params, "installed_only").value_or(false);
            json out = runtimes->list(refresh, installed_only, bool_param(r.params, "cached").value_or(false));
            // Settings (D20): who uses each install, and the defaults. Not
            // for the window's 2 s poll (installed_only): it reads every
            // project's floserver.json.
            if (!installed_only && projects && data) {
                floserver::core::ServiceRefusal ignored;
                const auto users = projects->runtime_users(
                    floserver::core::list_installed(floserver::core::runtimes_dir(*data)), ignored);
                for (json& entry : out["installed"]) {
                    const std::string key = entry.value("name", std::string()) + "/" + entry.value("folder", std::string());
                    entry["used_by"] = users && users->contains(key) ? (*users)[key] : json::array();
                }
                if (const auto defaults = projects->defaults(ignored)) {
                    out["defaults"] = *defaults;
                }
            }
            return protocol::make_result(r.id, out);
        }
        if (r.method == "runtimes.default") {
            if (!projects) {
                return protocol::make_error(r.id, "no_data_folder", "FloServer can't find its data folder");
            }
            std::optional<std::string> series;
            if (r.params.contains("series") && r.params["series"].is_string()) {
                series = r.params["series"].get<std::string>();
            } else if (r.params.contains("series") && !r.params["series"].is_null()) {
                return protocol::make_error(r.id, "bad_params", R"("series" is a version such as "8.5", or null)");
            }
            const auto result = projects->set_default(string_param(r.params, "name"), series, refusal);
            return result ? protocol::make_result(r.id, {{"defaults", *result}})
                          : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        if (r.method == "runtimes.install") {
            const auto id = runtimes->install(string_param(r.params, "name"), string_param(r.params, "version"),
                                              refusal);
            return id ? protocol::make_result(r.id, {{"job", *id}})
                      : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        if (r.method == "runtimes.remove") {
            // A runtime a project or shared sites use isn't removed (D20):
            // that project would stop working, or ask to download it again.
            if (projects && data) {
                const std::string name = string_param(r.params, "name");
                const std::string version = string_param(r.params, "version");
                const auto users = projects->runtime_users(
                    floserver::core::list_installed(floserver::core::runtimes_dir(*data)), refusal);
                if (!users) {
                    return protocol::make_error(r.id, refusal.code, refusal.message);
                }
                const std::string key = name + "/" + version;
                if (users->contains(key) && !(*users)[key].empty()) {
                    std::string who;
                    for (const json& u : (*users)[key]) {
                        const std::string n = u.get<std::string>();
                        who += (who.empty() ? "" : ", ") + (n == floserver::core::kSharedSiteName ? std::string("shared sites") : n);
                    }
                    return protocol::make_error(r.id, "in_use",
                                                name + " " + version + " is used by " + who +
                                                    "; give them another version first");
                }
            }
            return runtimes->remove(string_param(r.params, "name"), string_param(r.params, "version"), refusal)
                       ? protocol::make_result(r.id, json::object())
                       : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        const auto id = job_id(r.params);
        if (!id) {
            return protocol::make_error(r.id, "bad_params", "a job id is needed");
        }
        if (r.method == "jobs.get") {
            const auto job = runtimes->job(*id);
            return job ? protocol::make_result(r.id, *job)
                       : protocol::make_error(r.id, "unknown_job", "there's no job " + std::to_string(*id));
        }
        return protocol::make_result(r.id, {{"cancelled", runtimes->cancel(*id)}});
    }

    // The reply to one projects.* request.
    [[nodiscard]] std::string project_request(const protocol::Request& r) const {
        namespace core = floserver::core;
        if (!projects) {
            return protocol::make_error(r.id, "no_data_folder",
                                        "FloServer can't find a folder to keep its settings in (LOCALAPPDATA or HOME)");
        }
        core::ServiceRefusal refusal;
        std::optional<json> result;
        const std::string name = string_param(r.params, "name");
        if (r.method == "projects.list") {
            result = projects->list(refusal);
            if (result && web) {
                const json sites = web->status()["sites"];
                for (json& p : (*result)["projects"]) {
                    add_run_state(p, sites);
                }
            }
            return result ? protocol::make_result(r.id, *result) : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        if (r.method == "shared.get" || r.method == "shared.set") {
            // Shared sites (D19): the settings, the folders and how they run.
            // "folders": false leaves the www folder unread (the window asks
            // every 2 seconds and never shows the folders).
            json params = r.params.is_object() ? r.params : json::object();
            const bool folders = !params.contains("folders") || !params["folders"].is_boolean() ||
                                 params["folders"].get<bool>();
            params.erase("folders");
            if (r.method == "shared.get") {
                result = projects->shared(refusal, folders);
            } else {
                result = projects->set_shared(params, refusal, folders);
                if (result && databases) {
                    (void)databases->reload();
                }
            }
            if (result && web) {
                const json status = r.method == "shared.set" ? web->reload() : web->status();
                const json& run = status.value("shared", json());
                (*result)["status"] = run.is_object() ? run : json(nullptr);
            }
            return result ? protocol::make_result(r.id, *result) : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        if (r.method == "projects.log") {
            // Only a registered project's log (or shared sites'), never a path from the request.
            if (name != core::kSharedSiteName && !projects->folder(name, refusal)) {
                return protocol::make_error(r.id, refusal.code, refusal.message);
            }
            std::size_t lines = 50;
            if (r.params.contains("lines") && r.params["lines"].is_number_unsigned()) {
                lines = std::min<std::size_t>(r.params["lines"].get<std::size_t>(), core::kRequestLogMaxEntries);
            }
            json entries = json::array();
            for (const core::RequestLogEntry& e :
                 core::read_request_log(core::request_log_file(core::web_dir(data.value_or(std::filesystem::path())), name), lines)) {
                entries.push_back({{"t", e.time_ms},
                                   {"status", e.status},
                                   {"ms", e.duration_ms},
                                   {"method", e.method},
                                   {"uri", e.uri}});
            }
            return protocol::make_result(r.id, {{"entries", std::move(entries)}});
        }
        if (r.method == "projects.add") {
            const std::filesystem::path dir = core::path_from_utf8(string_param(r.params, "path"));
            if (!dir.is_absolute()) {
                return protocol::make_error(r.id, "bad_params", "the project folder must be a full path");
            }
            result = projects->add(dir, name, refusal);
        } else if (r.method == "projects.remove") {
            if (projects->remove(name, refusal)) {
                result = json::object();
                // Its request log goes too, so a new project with the name starts empty.
                if (data) {
                    std::error_code ec;
                    std::filesystem::remove(core::request_log_file(core::web_dir(*data), name), ec);
                }
            }
        } else if (r.method == "projects.php") {
            // The project's own PHP extensions and php.ini values (D20), in its floserver.json.
            const json none;
            result = projects->project_php(name, r.params.is_object() ? r.params.value("extensions", none) : none,
                                           r.params.is_object() ? r.params.value("ini", none) : none, refusal);
        } else if (r.method == "projects.use") {
            // "php" may be false (no PHP), the same as "none".
            std::optional<std::string> value;
            const bool php_false = string_param(r.params, "key") == "php" && r.params.contains("value") &&
                                   r.params.at("value").is_boolean() && !r.params.at("value").get<bool>();
            if (php_false) {
                value = std::string(floserver::core::kPhpNone);
            } else if (r.params.contains("value") && r.params.at("value").is_string()) {
                value = r.params.at("value").get<std::string>();
            } else if (r.params.contains("value") && !r.params.at("value").is_null()) {
                return protocol::make_error(r.id, "bad_params",
                                            "value is a string (false also turns PHP off), or null to remove the key");
            }
            result = projects->use(name, string_param(r.params, "key"), value, refusal);
        } else {  // projects.set
            result = projects->set(name, bool_param(r.params, "on"), bool_param(r.params, "keep_running"), refusal);
        }
        // Every change takes effect at once.
        if (result && web) {
            const json sites = web->reload()["sites"];
            if (result->contains("name")) {
                add_run_state(*result, sites);
            }
        }
        if (result && databases) {
            (void)databases->reload();
        }
        return result ? protocol::make_result(r.id, *result) : protocol::make_error(r.id, refusal.code, refusal.message);
    }

    // The reply to one php.* request: Settings → PHP (D20), per series.
    //   php.get {series}; php.set {series, extensions?, ini?}; php.custom_ini {series} -> {"path"};
    //   php.final_ini {series} -> {"path"} (the php.ini it uses, to look at)
    [[nodiscard]] std::string php_request(const protocol::Request& r) const {
        if (!projects) {
            return protocol::make_error(r.id, "no_data_folder",
                                        "FloServer can't find a folder to keep its settings in (LOCALAPPDATA or HOME)");
        }
        floserver::core::ServiceRefusal refusal;
        const std::string series = string_param(r.params, "series");
        if (r.method == "php.final_ini") {
            const auto path = projects->php_final_ini(series, refusal);
            return path ? protocol::make_result(r.id, {{"path", floserver::core::path_to_utf8(*path)}})
                        : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        if (r.method == "php.custom_ini") {
            // Made here, opened by the window; its text is read on the next reload.
            const auto path = projects->php_custom_ini(series, refusal);
            return path ? protocol::make_result(r.id, {{"path", floserver::core::path_to_utf8(*path)}})
                        : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        std::optional<json> result;
        if (r.method == "php.get") {
            result = projects->php_get(series, refusal);
        } else {
            const json none;
            result = projects->php_set(series, r.params.is_object() ? r.params.value("extensions", none) : none,
                                       r.params.is_object() ? r.params.value("ini", none) : none, refusal);
            // Pools on the series get new keys: they restart with the new php.ini now.
            if (result && web) {
                (void)web->reload();
            }
        }
        return result ? protocol::make_result(r.id, *result) : protocol::make_error(r.id, refusal.code, refusal.message);
    }

    static json ca_json(const floserver::core::CaInfo& ca) {
        namespace core = floserver::core;
        const char* trust = "unsupported";
        switch (core::ca_trust_state(ca.der)) {
            case core::TrustState::Trusted:
                trust = "yes";
                break;
            case core::TrustState::NotTrusted:
                trust = "no";
                break;
            case core::TrustState::Unsupported:
                break;
        }
        return {{"common_name", ca.common_name},
                {"fingerprint", ca.fingerprint},
                {"not_after", ca.not_after.time_since_epoch().count()},
                // The CA is never replaced on its own (the user trusted it); the
                // window warns ahead and `flo untrust` + `flo trust` replace it.
                {"expires_soon", ca.not_after - std::chrono::system_clock::now() < core::kRenewBefore},
                {"path", core::path_to_utf8(ca.certificate)},
                {"der", core::to_base64(ca.der)},
                {"trusted", trust}};
    }

    // The reply to one certs.* request. Trusting the CA is the caller's job
    // (Windows shows a prompt for it), so the service only makes and removes.
    [[nodiscard]] std::string cert_request(const protocol::Request& r) const {
        if (!certs) {
            return protocol::make_error(r.id, "no_data_folder",
                                        "FloServer can't find a folder to keep certificates in (LOCALAPPDATA or HOME)");
        }
        std::string error;
        if (r.method == "certs.status") {
            const auto ca = certs->load_ca(error);
            if (!error.empty()) {
                return protocol::make_error(r.id, "ca_damaged", error);
            }
            return protocol::make_result(r.id, {{"ca", ca ? ca_json(*ca) : json(nullptr)}});
        }
        if (r.method == "certs.ensure_ca") {
            const auto ca = certs->ensure_ca(error);
            return ca ? protocol::make_result(r.id, {{"ca", ca_json(*ca)}})
                      : protocol::make_error(r.id, "ca_failed", error);
        }
        // certs.remove: sites go back to plain http.
        if (!certs->remove_all(error)) {
            return protocol::make_error(r.id, "remove_failed", error);
        }
        if (web) {
            (void)web->reload();
        }
        return protocol::make_result(r.id, json::object());
    }

    // Settings > Ports & HTTPS (web_settings.h): web.settings.get, once when
    // the section opens; web.settings.set {"ports"?, "http_port"?,
    // "https_port"?, "https"?, "shared_https"?}. Both reply with the
    // settings, the ports in use, the ports asked for that another program
    // holds (and which program), and the CA as certs.status says it, with
    // "viewer": its .crt for Windows' certificate viewer.
    [[nodiscard]] std::string web_settings_request(const protocol::Request& r) const {
        if (!web_settings) {
            return protocol::make_error(r.id, "no_data_folder",
                                        "FloServer can't find a folder to keep its settings in (LOCALAPPDATA or HOME)");
        }
        floserver::core::ServiceRefusal refusal;
        const auto result = r.method == "web.settings.get"
                                ? web_settings->get(refusal)
                                : web_settings->set(r.params.is_object() ? r.params : json::object(), refusal);
        return result ? protocol::make_result(r.id, *result) : protocol::make_error(r.id, refusal.code, refusal.message);
    }

    // The CA for Settings > Ports & HTTPS: certs.status's "ca" and its .crt,
    // {"error"} when it's damaged, or null when there's none yet.
    [[nodiscard]] json ca_for_settings() const {
        if (!certs) {
            return nullptr;
        }
        std::string error;
        const auto ca = certs->load_ca(error);
        if (!ca) {
            return error.empty() ? json(nullptr) : json{{"error", error}};
        }
        json out = ca_json(*ca);
        if (const auto viewer = certs->ca_viewer_file(error)) {
            out["viewer"] = floserver::core::path_to_utf8(*viewer);
        }
        return out;
    }

    // web.status, web.reload: what runs, or bring it in line now (after
    // `flo trust`, say).
    [[nodiscard]] std::string web_request(const protocol::Request& r) const {
        if (!web) {
            return protocol::make_error(r.id, "no_data_folder",
                                        "FloServer can't find a folder to keep its settings in (LOCALAPPDATA or HOME)");
        }
        return protocol::make_result(r.id, r.method == "web.reload" ? web->reload() : web->status());
    }

    // databases.list: the servers; databases.login {"name"}: a project's
    // database login (made the first time) and its .env lines.
    [[nodiscard]] std::string database_request(const protocol::Request& r) const {
        namespace core = floserver::core;
        if (!databases) {
            return protocol::make_error(r.id, "no_data_folder",
                                        "FloServer can't find a folder to keep its settings in (LOCALAPPDATA or HOME)");
        }
        if (r.method == "databases.list") {
            return protocol::make_result(r.id, databases->status());
        }
        if (r.method == "shared.login") {
            // Shared sites' database login (D19): user www, databases www and www_*.
            core::ServiceRefusal refusal;
            const auto login = databases->shared_login(refusal);
            if (!login) {
                return protocol::make_error(r.id, refusal.code, refusal.message);
            }
            return protocol::make_result(r.id, {{"host", "127.0.0.1"},
                                                {"port", login->port},
                                                {"database", login->database},
                                                {"user", login->user},
                                                {"password", login->password},
                                                {"prefix", std::string(core::kSharedDatabasePrefix)}});
        }
        const std::string name = string_param(r.params, "name");
        core::ServiceRefusal refusal;
        const auto login = databases->project_login(name, refusal);
        if (!login) {
            return protocol::make_error(r.id, refusal.code, refusal.message);
        }
        json result = {{"host", "127.0.0.1"},
                       {"port", login->port},
                       {"database", login->database},
                       {"user", login->user},
                       {"password", login->password}};
        if (const auto list = projects->list(refusal)) {
            for (const json& p : (*list)["projects"]) {
                if (p.value("name", std::string()) == name && p.contains("path")) {
                    const auto dir = core::path_from_utf8(p["path"].get<std::string>());
                    const bool laravel = core::env_style(dir) == core::EnvStyle::Laravel;
                    json env = json::array();
                    for (const auto& [key, value] : core::env_values(laravel ? core::EnvStyle::Laravel : core::EnvStyle::FloCms, *login)) {
                        env.push_back({key, value});
                    }
                    // Python projects also get the database as one URL (D17).
                    const auto parsed = core::read_project_config_file(core::Project{name, dir, true, false, {}});
                    if (parsed.config && parsed.config->python) {
                        std::error_code ec;
                        env.push_back({"DATABASE_URL",
                                       core::database_url(*login, std::filesystem::is_regular_file(dir / "manage.py", ec))});
                    }
                    result["env_style"] = laravel ? "laravel" : "flocms";
                    result["env"] = env;
                    result["env_file"] = core::path_to_utf8(dir / ".env");
                }
            }
        }
        return protocol::make_result(r.id, result);
    }

    // Settings → Database (MariaDB), per series ("11.4"):
    //   database.get {series}: ProjectService::database_get() with "server"
    //     and "databases" from DatabaseService::series_status();
    //   database.set {series, ini?, port?, keep_running?}: the same reply, and
    //     "restarting": true when the server restarts for the new my.ini;
    //   database.server {series, action: start|stop|restart} -> series_status();
    //   database.custom_ini / database.final_ini {series} -> {"path"};
    //   database.root {series} -> {"password"}; database.root_set {series, password} -> {}.
    [[nodiscard]] std::string database_settings_request(const protocol::Request& r) const {
        namespace core = floserver::core;
        if (!projects || !databases) {
            return protocol::make_error(r.id, "no_data_folder",
                                        "FloServer can't find a folder to keep its settings in (LOCALAPPDATA or HOME)");
        }
        core::ServiceRefusal refusal;
        const std::string text = string_param(r.params, "series");
        const auto series = core::mariadb_request_series(text);
        if (!series) {
            return protocol::make_error(r.id, "bad_params", "\"" + text + "\" isn't a MariaDB series such as 11.4");
        }
        const auto with_status = [&](json reply) {
            json status = databases->series_status(*series);
            reply["server"] = status["server"];
            reply["databases"] = status["databases"];
            return reply;
        };
        if (r.method == "database.custom_ini" || r.method == "database.final_ini") {
            const auto path = r.method == "database.custom_ini" ? projects->database_custom_ini(*series, refusal)
                                                                : databases->final_ini(*series, refusal);
            return path ? protocol::make_result(r.id, {{"path", core::path_to_utf8(*path)}})
                        : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        if (r.method == "database.root") {
            const auto password = databases->root_password(*series, refusal);
            return password ? protocol::make_result(r.id, {{"password", *password}})
                            : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        if (r.method == "database.root_set") {
            return databases->set_root_password(*series, string_param(r.params, "password"), refusal)
                       ? protocol::make_result(r.id, json::object())
                       : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        if (r.method == "database.server") {
            return databases->server_action(*series, string_param(r.params, "action"), refusal)
                       ? protocol::make_result(r.id, databases->series_status(*series))
                       : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        if (r.method == "database.get") {
            const auto result = projects->database_get(*series, refusal);
            return result ? protocol::make_result(r.id, with_status(*result))
                          : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        // database.set
        const json none;
        const json params = r.params.is_object() ? r.params : json::object();
        const json port = params.contains("port") ? params.at("port") : json();
        const json keep = params.contains("keep_running") ? params.at("keep_running") : json();
        bool changed_ini = false;
        auto result = projects->database_set(*series, params.value("ini", none), params.contains("port") ? &port : nullptr,
                                             params.contains("keep_running") ? &keep : nullptr, changed_ini, refusal);
        if (!result) {
            return protocol::make_error(r.id, refusal.code, refusal.message);
        }
        (void)databases->reload();  // a new port or "keep running" now
        (*result)["restarting"] = changed_ini && databases->restart_if_running(*series);
        return protocol::make_result(r.id, with_status(*result));
    }

    // templates.list; projects.create {"template", "name", "parent", "versions"?};
    // create.get / create.cancel {"id"} (D9).
    [[nodiscard]] std::string template_request(const protocol::Request& r) const {
        namespace core = floserver::core;
        if (!templates || !runtimes) {
            return protocol::make_error(r.id, "no_data_folder",
                                        "FloServer can't find a folder to keep its settings in (LOCALAPPDATA or HOME)");
        }
        if (r.method == "templates.list") {
            std::string error;
            const auto list = runtimes->templates(error);
            if (!list) {
                return protocol::make_error(r.id, "no_list", error);
            }
            json out = json::array();
            for (const core::ProjectTemplate& t : *list) {
                out.push_back(core::template_summary(t));
            }
            return protocol::make_result(r.id, {{"templates", std::move(out)}});
        }
        if (r.method == "projects.create") {
            core::CreateRequest request;
            request.template_id = string_param(r.params, "template");
            request.name = string_param(r.params, "name");
            request.parent = core::path_from_utf8(string_param(r.params, "parent"));
            if (r.params.contains("versions") && r.params["versions"].is_object()) {
                for (const auto& [key, value] : r.params["versions"].items()) {
                    if (value.is_string()) {
                        request.versions[key] = value.get<std::string>();
                    } else if (value.is_null()) {
                        request.versions[key] = std::nullopt;
                    } else {
                        return protocol::make_error(r.id, "bad_params", "a version is a string, or null to leave it out");
                    }
                }
            }
            core::ServiceRefusal refusal;
            const auto id = templates->create(request, refusal);
            return id ? protocol::make_result(r.id, {{"job", *id}})
                      : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        const auto id = job_id(r.params);
        if (!id) {
            return protocol::make_error(r.id, "bad_params", "a job id is needed");
        }
        if (r.method == "create.get") {
            const auto job = templates->job(*id);
            return job ? protocol::make_result(r.id, *job)
                       : protocol::make_error(r.id, "unknown_job", "there's no job " + std::to_string(*id));
        }
        return protocol::make_result(r.id, {{"cancelled", templates->cancel(*id)}});
    }

    [[nodiscard]] json status() const {
        const auto memory = floserver::core::memory_in_use();
        return {{"version", floserver::core::kVersion},
                {"pid", floserver::core::current_pid()},
                {"uptime_s", std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count()},
                {"memory_bytes", memory ? json(*memory) : json(nullptr)}};
    }

    // One client: requests in, replies out, until it hangs up. Nothing a
    // client sends may take the service down.
    void serve(ipc::Connection connection) noexcept {
        try {
            serve_requests(connection);
        } catch (...) {  // NOLINT(bugprone-empty-catch): drop this client, keep serving others
        }
    }

    void serve_requests(ipc::Connection& connection) {
        while (auto text = connection.receive()) {
            std::string error_code;
            const auto request = protocol::parse_request(*text, error_code);
            std::string reply;
            bool quit = false;
            if (!request) {
                reply = protocol::make_error(0, error_code, "the request couldn't be read");
            } else if (request->method == "status") {
                reply = protocol::make_result(request->id, status());
            } else if (request->method == "runtimes.list" || request->method == "runtimes.install" ||
                       request->method == "runtimes.remove" || request->method == "runtimes.default" ||
                       request->method == "jobs.get" ||
                       request->method == "jobs.cancel") {
                reply = runtime_request(*request);
            } else if (request->method == "projects.list" || request->method == "projects.add" ||
                       request->method == "projects.remove" || request->method == "projects.set" ||
                       request->method == "projects.use" || request->method == "projects.log" ||
                       request->method == "projects.php" ||
                       request->method == "shared.get" || request->method == "shared.set") {
                reply = project_request(*request);
            } else if (request->method == "php.get" || request->method == "php.set" ||
                       request->method == "php.custom_ini" || request->method == "php.final_ini") {
                reply = php_request(*request);
            } else if (request->method == "certs.status" || request->method == "certs.ensure_ca" ||
                       request->method == "certs.remove") {
                reply = cert_request(*request);
            } else if (request->method == "web.status" || request->method == "web.reload") {
                reply = web_request(*request);
            } else if (request->method == "web.settings.get" || request->method == "web.settings.set") {
                reply = web_settings_request(*request);
            } else if (request->method == "databases.list" || request->method == "databases.login" ||
                       request->method == "shared.login") {
                reply = database_request(*request);
            } else if (request->method == "database.get" || request->method == "database.set" ||
                       request->method == "database.server" || request->method == "database.custom_ini" ||
                       request->method == "database.final_ini" || request->method == "database.root" ||
                       request->method == "database.root_set") {
                reply = database_settings_request(*request);
            } else if (request->method == "templates.list" || request->method == "projects.create" ||
                       request->method == "create.get" || request->method == "create.cancel") {
                reply = template_request(*request);
            } else if (request->method == "quit") {
                reply = protocol::make_result(request->id, json::object());
                quit = true;
            } else {
                reply = protocol::make_error(request->id, "unknown_method",
                                             "this FloServer doesn't know \"" + request->method + "\"");
            }
            if (!connection.send(reply)) {
                break;
            }
            if (quit && !quitting.exchange(true)) {
                server->stop();
            }
        }
    }
};

// What the template jobs need from the rest of the service (D9). The
// service outlives them: TemplateService::stop() runs before anything else
// stops at quit.
floserver::core::TemplateHooks template_hooks(const std::shared_ptr<Service>& service) {
    namespace core = floserver::core;
    core::TemplateHooks hooks;
    Service* s = service.get();
    hooks.find_template = [s](const std::string& id, std::string& error) -> std::optional<core::ProjectTemplate> {
        const auto list = s->runtimes->templates(error, false);
        if (!list) {
            return std::nullopt;
        }
        for (const core::ProjectTemplate& t : *list) {
            if (t.id == id) {
                return t;
            }
        }
        error = "the runtime list has no template called " + id;
        return std::nullopt;
    };
    hooks.install = [s](const std::string& name, const std::string& version, const std::atomic<bool>& cancel,
                        const std::function<void(const std::string&)>& progress, std::string& error) {
        core::ServiceRefusal refusal;
        const auto id = s->runtimes->install(name, version, refusal);
        if (!id) {
            error = refusal.message;
            return false;
        }
        while (true) {
            const auto job = s->runtimes->job(*id);
            if (!job) {
                error = "the install was forgotten";
                return false;
            }
            const std::string state = job->value("state", std::string());
            if (state == "done") {
                return true;
            }
            if (state == "failed" || state == "cancelled") {
                error = job->value("message", state);
                return false;
            }
            if (cancel) {
                // The install may be shared with one the user started: it goes on.
                error = "cancelled";
                return false;
            }
            if (state == "downloading" && job->value("total", std::uint64_t{0}) > 0) {
                const auto received = job->value("received", std::uint64_t{0});
                const auto total = job->value("total", std::uint64_t{1});
                progress("Downloading " + name + " " + job->value("version", version) + "… " +
                         std::to_string(std::min<std::uint64_t>(100, received * 100 / total)) + "%");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    };
    hooks.add_project = [s](const std::filesystem::path& dir, const std::string& name,
                            std::string& error) -> std::optional<std::string> {
        core::ServiceRefusal refusal;
        const auto added = s->projects->add(dir, name, refusal);
        if (!added) {
            error = refusal.message;
            return std::nullopt;
        }
        std::string registered = added->value("name", name);
        (void)s->projects->set(registered, true, std::nullopt, refusal);
        if (s->web) {
            (void)s->web->reload();
        }
        if (s->databases) {
            (void)s->databases->reload();
        }
        return registered;
    };
    hooks.database_login = [s](const std::string& project, std::string& error) -> std::optional<core::DatabaseLogin> {
        core::ServiceRefusal refusal;
        auto login = s->databases ? s->databases->project_login(project, refusal) : std::nullopt;
        if (!login) {
            error = refusal.message;
        }
        return login;
    };
    return hooks;
}

int run() {
    const std::string instance = ipc::default_instance();
    std::unique_ptr<ipc::Server> server;
    std::string error;
    switch (ipc::Server::listen(instance, server, error)) {
        case ipc::ListenStatus::AlreadyRunning:
            std::cout << "FloServer is already running.\n";
            return 0;
        case ipc::ListenStatus::Failed:
            std::cerr << "floserverd: " << error << "\n";
            return 1;
        case ipc::ListenStatus::Listening:
            break;
    }

    auto service = std::make_shared<Service>();
    std::thread integration_thread;
    service->server = std::move(server);
    service->runtimes = make_runtimes();
    if (const auto data = floserver::core::data_dir()) {
        service->projects = std::make_unique<floserver::core::ProjectService>(*data);
        service->certs = std::make_unique<floserver::core::CertStore>(*data);
        service->web = std::make_unique<floserver::core::WebService>(web_config(*data), *service->certs);
        service->web->start();
        {
            floserver::core::WebSettingsService::Hooks hooks;
            const Service* s = service.get();
            hooks.in_use = [s] { return s->web->ports_in_use(); };
            hooks.reload = [s] { (void)s->web->reload(); };
            hooks.ca = [s] { return s->ca_for_settings(); };
            service->web_settings =
                std::make_unique<floserver::core::WebSettingsService>(*service->projects, std::move(hooks));
        }
        service->secrets = std::make_unique<floserver::core::SecretStore>(*data);
        floserver::core::DatabaseServiceConfig db;
        db.data_dir = *data;
        db.idle_after = web_config(*data).idle_after;
        service->databases = std::make_unique<floserver::core::DatabaseService>(db, *service->secrets);
        service->databases->start();
        service->templates = std::make_unique<floserver::core::TemplateService>(*data, template_hooks(service));
        service->data = *data;
        service->refresh_integration();
        try {
            integration_thread = std::thread([service] {
                std::unique_lock lock(service->integration_mutex);
                while (!service->integration_wake.wait_for(lock, std::chrono::seconds(5),
                                                            [&] { return service->quitting.load(); })) {
                    lock.unlock();
                    service->refresh_integration();
                    lock.lock();
                }
            });
        } catch (const std::system_error&) {  // NOLINT(bugprone-empty-catch): the file is still written at every start
        }
    }

    while (auto connection = service->server->accept()) {
        if (!service->clients.try_add()) {
            continue;  // too many clients: this one is closed
        }
        try {
            std::thread([service, c = std::move(*connection)]() mutable {
                service->serve(std::move(c));
                service->clients.remove();
            }).detach();
        } catch (const std::system_error&) {
            service->clients.remove();  // no thread for this client; keep serving
        }
    }
    // The integration thread ends before anything it reads stops.
    {
        const std::lock_guard lock(service->integration_mutex);
        service->quitting.store(true);
    }
    service->integration_wake.notify_all();
    if (integration_thread.joinable()) {
        integration_thread.join();
    }
    // Templates first: a job waits on installs and adds projects.
    if (service->templates) {
        service->templates->stop();
    }
    if (service->runtimes) {
        service->runtimes->stop();
    }
    // nginx and php-cgi end with the service ("quit stops everything").
    if (service->web) {
        service->web->stop();
    }
    if (service->databases) {
        service->databases->stop();  // a clean shutdown of each MariaDB
    }
    if (!service->clients.wait_until_idle(std::chrono::seconds(2))) {
        // A client thread is still busy and holds the service, so it can't be
        // torn down safely here. Exit now; the next start clears what an
        // interrupted install left.
        std::cout.flush();
        std::_Exit(0);
    }
    // Waits for the cancelled install to wind down (a moment: the transfer
    // ends as soon as it's cancelled).
    service->runtimes.reset();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // No "System Error" box for a missing DLL or an empty drive: in the
    // service's session nobody would see it, and the program asking (the
    // php CLI listing its settings) would wait on it forever. Programs the
    // service starts inherit this; they report such errors themselves.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#endif
    if (argc > 1 && std::string_view(argv[1]) == "--version") {
        std::cout << "floserverd " << floserver::core::kVersion << "\n";
        return 0;
    }
    try {
        return run();
    } catch (const std::exception& e) {
        std::cerr << "floserverd: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "floserverd: unexpected error\n";
    }
    return 1;
}

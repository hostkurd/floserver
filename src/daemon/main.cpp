// floserverd: the background service in the user's session (D2).
//
// The private channel, the runtimes (the signed list, installs as jobs,
// removal), the project list, the local certificate authority, and running
// the projects: nginx and the PHP pools that sleep when idle.

#include "floserver/core/build_info.h"
#include "floserver/core/builtin.h"
#include "floserver/core/certificates.h"
#include "floserver/core/encoding.h"
#include "floserver/core/http.h"
#include "floserver/core/ipc.h"
#include "floserver/core/paths.h"
#include "floserver/core/process.h"
#include "floserver/core/project_service.h"
#include "floserver/core/protocol.h"
#include "floserver/core/runtime_service.h"
#include "floserver/core/trust_store.h"
#include "floserver/core/database_service.h"
#include "floserver/core/mariadb.h"
#include "floserver/core/project_config.h"
#include "floserver/core/site_plan.h"
#include "floserver/core/secret_store.h"
#include "floserver/core/web_service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>

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
    std::unique_ptr<floserver::core::SecretStore> secrets;
    std::unique_ptr<floserver::core::DatabaseService> databases;

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
            return protocol::make_result(r.id, runtimes->list(refresh));
        }
        if (r.method == "runtimes.install") {
            const auto id = runtimes->install(string_param(r.params, "name"), string_param(r.params, "version"),
                                              refusal);
            return id ? protocol::make_result(r.id, {{"job", *id}})
                      : protocol::make_error(r.id, refusal.code, refusal.message);
        }
        if (r.method == "runtimes.remove") {
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
        if (r.method == "projects.add") {
            const std::filesystem::path dir = core::path_from_utf8(string_param(r.params, "path"));
            if (!dir.is_absolute()) {
                return protocol::make_error(r.id, "bad_params", "the project folder must be a full path");
            }
            result = projects->add(dir, name, refusal);
        } else if (r.method == "projects.remove") {
            if (projects->remove(name, refusal)) {
                result = json::object();
            }
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
                       request->method == "runtimes.remove" || request->method == "jobs.get" ||
                       request->method == "jobs.cancel") {
                reply = runtime_request(*request);
            } else if (request->method == "projects.list" || request->method == "projects.add" ||
                       request->method == "projects.remove" || request->method == "projects.set") {
                reply = project_request(*request);
            } else if (request->method == "certs.status" || request->method == "certs.ensure_ca" ||
                       request->method == "certs.remove") {
                reply = cert_request(*request);
            } else if (request->method == "web.status" || request->method == "web.reload") {
                reply = web_request(*request);
            } else if (request->method == "databases.list" || request->method == "databases.login") {
                reply = database_request(*request);
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
    service->server = std::move(server);
    service->runtimes = make_runtimes();
    if (const auto data = floserver::core::data_dir()) {
        service->projects = std::make_unique<floserver::core::ProjectService>(*data);
        service->certs = std::make_unique<floserver::core::CertStore>(*data);
        service->web = std::make_unique<floserver::core::WebService>(web_config(*data), *service->certs);
        service->web->start();
        service->secrets = std::make_unique<floserver::core::SecretStore>(*data);
        floserver::core::DatabaseServiceConfig db;
        db.data_dir = *data;
        db.idle_after = web_config(*data).idle_after;
        service->databases = std::make_unique<floserver::core::DatabaseService>(db, *service->secrets);
        service->databases->start();
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
    // Give the client that asked us to quit time to read the reply.
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

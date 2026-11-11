#include "floserver/core/redis_service.h"

#include "floserver/core/crypto.h"
#include "floserver/core/encoding.h"
#include "floserver/core/mailpit.h"  // clean_error_text
#include "floserver/core/paths.h"
#include "floserver/core/runtime_store.h"

#include <algorithm>
#include <array>
#include <set>
#include <system_error>
#include <vector>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kMaxConnections = 512;
constexpr auto kFailedRetry = 30s;
constexpr auto kCommandLimit = 5s;
constexpr auto kSaveLimit = 60s;
constexpr std::size_t kMaxStatusFile = 4096;
constexpr std::size_t kMaxReply = std::size_t{1} << 20;

template <class F>
class AtExit {
public:
    explicit AtExit(F f) : f_(std::move(f)) {}
    AtExit(const AtExit&) = delete;
    AtExit& operator=(const AtExit&) = delete;
    AtExit(AtExit&&) = delete;
    AtExit& operator=(AtExit&&) = delete;
    ~AtExit() { f_(); }

private:
    F f_;
};

std::string log_since(const fs::path& file, std::uintmax_t from, std::size_t max = 600) {
    auto text = read_small_file(file, std::size_t{8} * 1024 * 1024).value_or("");
    text = from < text.size() ? text.substr(static_cast<std::size_t>(from)) : std::string();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text.size() > max ? "..." + text.substr(text.size() - max) : text;
}

std::uintmax_t file_bytes(const fs::path& file) {
    std::error_code ec;
    const auto n = fs::file_size(file, ec);
    return ec ? 0 : n;
}

// One reply on `socket`, reading on into `buffer`; nothing when the time ran out.
std::optional<RespReply> read_reply(std::uintptr_t socket, std::string& buffer, std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    for (;;) {
        if (auto reply = take_resp_reply(buffer)) {
            return reply;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left <= 0ms || buffer.size() > kMaxReply) {
            return std::nullopt;
        }
        const auto got = receive_some(socket, left);
        if (!got || got->empty()) {
            return std::nullopt;
        }
        buffer += *got;
    }
}

// AUTH then `args` on a fresh connection to `socket_path`.
std::optional<RespReply> authed_command(const fs::path& socket_path, const std::string& password,
                                        const std::vector<std::string_view>& args, std::chrono::milliseconds limit) {
    const auto socket = connect_local(socket_path);
    if (!socket) {
        return std::nullopt;
    }
    const AtExit closed([&socket] { close_socket(*socket); });
    std::string buffer;
    if (!send_all(*socket, resp_command({"AUTH", password}) + resp_command(args))) {
        return std::nullopt;
    }
    const auto auth = read_reply(*socket, buffer, kCommandLimit);
    if (!auth || auth->type != '+') {
        return auth ? auth : std::nullopt;
    }
    return read_reply(*socket, buffer, limit);
}

}  // namespace

RedisService::RedisService(RedisServiceConfig config, SecretStore secrets)
    : secrets_(std::move(secrets)), config_(std::move(config)), paths_(garnet_paths(config_.data_dir)) {
    if (!config_.probe) {
        config_.probe = can_listen_on_loopback;
    }
    touch();
}

RedisService::~RedisService() { stop(); }

void RedisService::start() {
    load_status();
    reload();
    thread_ = std::thread([this] { supervise(); });
}

void RedisService::stop() {
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    {
        const std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    changed_.notify_all();
    stop_process();
    {
        std::unique_lock lock(threads_mutex_);
        threads_done_.wait(lock, [this] { return threads_ == 0; });
    }
    const std::lock_guard lock(mutex_);
    listener_.reset();
}

void RedisService::touch() { last_activity_ = Clock::now().time_since_epoch().count(); }

void RedisService::run_detached(std::function<void()> work) {
    {
        const std::lock_guard lock(threads_mutex_);
        ++threads_;
    }
    auto shared = std::make_shared<std::function<void()>>(std::move(work));
    const auto done = [this] {
        const std::lock_guard lock(threads_mutex_);
        --threads_;
        threads_done_.notify_all();
    };
    try {
        std::thread([shared, done] {
            try {
                (*shared)();
            } catch (...) {  // NOLINT(bugprone-empty-catch): a detached thread's exception would end the service
            }
            done();
        }).detach();
    } catch (const std::system_error&) {
        (*shared)();
        done();
    }
}

void RedisService::reload() {
    const std::lock_guard serial(reload_mutex_);
    if (stopping_) {
        return;
    }
    std::string error;
    const auto app = load_app_config(app_config_file(config_.data_dir), error);
    if (!app) {
        return;  // keep what runs
    }
    const RedisSettings settings = read_redis_settings(*app);
    const auto installed = list_installed(runtimes_dir(config_.data_dir));
    const InstalledRuntime* garnet = newest_garnet(installed);
    const InstalledRuntime* dotnet = newest_dotnet(installed);

    if (garnet == nullptr) {
        std::unique_ptr<ChildProcess> process;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return state_ != State::Starting && killing_ == 0; });
            installed_ = false;
            version_.clear();
            dotnet_version_.clear();
            entry_.clear();
            dotnet_dir_.clear();
            install_error_.clear();
            settings_ = settings;
            process = std::move(process_);
            if (process) {
                ++killing_;
            }
            state_ = State::Asleep;
            error_.clear();
            listener_.reset();
        }
        kill_and_note(std::move(process));
        return;
    }

    std::string install_error;
    fs::path entry;
    if (const auto e = garnet_entry(garnet->dir)) {
        entry = *e;
    } else {
        install_error = "Garnet's GarnetServer program is missing from " + path_to_utf8(garnet->dir);
    }
    if (install_error.empty() && entry.extension() == ".dll" && dotnet == nullptr) {
        install_error = "the .NET " + std::string(kGarnetDotnetSeries) +
                        " runtime Garnet needs isn't installed: install Garnet again in Settings → Runtimes "
                        "(or: flo install garnet)";
    }
    const fs::path dotnet_dir = dotnet != nullptr ? dotnet->dir : fs::path();

    std::unique_ptr<ChildProcess> retired;
    bool save_port = false;
    Port port_to_save = 0;
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return state_ != State::Starting && killing_ == 0; });
        if (stopped_) {
            return;
        }
        const bool replaced = installed_ && (version_ != garnet->version || entry_ != entry || dotnet_dir_ != dotnet_dir);
        const bool restart = process_ && (replaced || settings.memory_mb != settings_.memory_mb);
        installed_ = true;
        version_ = garnet->version;
        dotnet_version_ = dotnet != nullptr ? dotnet->version : std::string();
        entry_ = entry;
        dotnet_dir_ = dotnet_dir;
        if (restart) {
            retired = std::move(process_);
            ++killing_;
            state_ = State::Asleep;
        }
        if (replaced && state_ == State::Failed) {
            state_ = State::Asleep;
        }
        settings_ = settings;

        // The port: the one kept in config.json (else 6379) when it can be
        // had; else the one held now; else the next free one from 6379.
        const Port wanted = settings.port.value_or(kFirstRedisPort);
        if (!listener_ || listener_->port() != wanted) {
            std::string why;
            std::shared_ptr<LoopbackListener> opened = LoopbackListener::open(wanted, why);
            if (!opened && !listener_) {
                std::set<Port> avoid = {wanted};
                for (unsigned tries = 0; !opened && tries < 10; ++tries) {
                    const auto free = pick_port_from(kFirstRedisPort, avoid, config_.probe);
                    if (!free) {
                        break;
                    }
                    opened = LoopbackListener::open(*free, why);
                    avoid.insert(*free);
                }
            }
            if (opened) {
                listener_ = std::move(opened);
            }
        }
        if (install_error.empty() && !listener_) {
            install_error = "no port is free for Redis (from " + std::to_string(kFirstRedisPort) + " up)";
        }
        install_error_ = install_error;
        if (listener_ && settings.port != listener_->port()) {
            save_port = true;
            port_to_save = listener_->port();
        }
    }
    kill_and_note(std::move(retired));
    if (install_error.empty()) {
        std::string why;
        (void)ensure_secret_files(why);  // tried again at the start
    }
    if (save_port) {
        std::string why;
        const auto change = [port_to_save](AppConfig& config) {
            RedisSettings s = read_redis_settings(config);
            s.port = port_to_save;
            write_redis_settings(config, s);
        };
        bool saved = false;
        if (config_.change_config) {
            saved = config_.change_config(change, why);
        } else if (auto config = load_app_config(app_config_file(config_.data_dir), why)) {
            change(*config);
            saved = save_app_config(app_config_file(config_.data_dir), *config, why);
        }
        if (saved) {
            const std::lock_guard lock(mutex_);
            settings_.port = port_to_save;
        }
    }
}

bool RedisService::ensure_secret_files(std::string& error) {
    const std::lock_guard serial(secret_mutex_);
    {
        const std::lock_guard lock(mutex_);
        std::error_code ec;
        if (!password_.empty() && fs::is_regular_file(paths_.config, ec)) {
            return true;
        }
    }
    std::string password = secrets_.read(std::string(kRedisSecretName)).value_or(std::string());
    if (password.size() != 64 || !from_hex(password)) {
        std::array<std::uint8_t, 32> bytes{};
        if (!random_bytes(bytes, error)) {
            return false;
        }
        password = to_hex(bytes);
        wipe(bytes);
        if (!secrets_.save(std::string(kRedisSecretName), password, error)) {
            error = "can't keep Redis's password: " + error;
            return false;
        }
    }
    // Owner-only before anything secret goes in.
    if (!make_owner_only_folder(paths_.dir, error)) {
        return false;
    }
    const std::string text = garnet_config_text(password);
    if (read_small_file(paths_.config, 4096) != text && !write_private_file_atomically(paths_.config, text, error)) {
        return false;
    }
    const std::lock_guard lock(mutex_);
    password_ = std::move(password);
    return true;
}

std::optional<std::string> RedisService::password(std::string& error) {
    if (!ensure_secret_files(error)) {
        return std::nullopt;
    }
    const std::lock_guard lock(mutex_);
    return password_;
}

bool RedisService::remove_stale_socket(std::string& error) const {
    std::error_code ec;
    const auto status = fs::symlink_status(paths_.socket, ec);
    if (ec || !fs::exists(status) || paths_.socket.parent_path() != paths_.dir) {
        return true;
    }
    if (fs::is_directory(status) || !fs::remove(paths_.socket, ec) || ec) {
        error = "can't remove the old socket " + path_to_utf8(paths_.socket) + (ec ? ": " + error_text(ec) : std::string());
        return false;
    }
    return true;
}

bool RedisService::ensure_running(std::string& error) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return state_ != State::Starting && killing_ == 0; });
    if (stopped_ || stopping_) {
        error = "FloServer is stopping";
        return false;
    }
    if (held_ > 0) {
        error = "Garnet is being updated or removed; try again in a moment";
        return false;
    }
    if (!installed_) {
        error = "Redis (Garnet) isn't installed: download it in Settings → Runtimes (or: flo install garnet)";
        return false;
    }
    if (!install_error_.empty()) {
        error = install_error_;
        return false;
    }
    if (state_ == State::Running && process_ && process_->running()) {
        return true;
    }
    if (state_ == State::Failed && Clock::now() < retry_after_) {
        error = error_;
        return false;
    }
    state_ = State::Starting;
    const fs::path entry = entry_;
    const fs::path dotnet_dir = dotnet_dir_;
    const int memory_mb = settings_.memory_mb;
    lock.unlock();

    std::string why;
    std::unique_ptr<ChildProcess> process;
    try {
        std::string password;
        const auto launch = garnet_launch(entry, dotnet_dir, why);
        if (!launch) {
            // why is set
        } else if (const auto problem = garnet_socket_problem(paths_)) {
            why = *problem;  // never TCP instead: another program could take a TCP port first
        } else if (ensure_secret_files(why) && make_owner_only_folder(paths_.dir, why) && remove_stale_socket(why) &&
                   make_owner_only_files(paths_.dir, why)) {
            {
                const std::lock_guard l(mutex_);
                password = password_;
            }
            std::error_code ec;
            fs::create_directories(paths_.checkpoints, ec);
            fs::create_directories(paths_.log.parent_path(), ec);
            const std::uintmax_t log_from = file_bytes(paths_.log);
            ChildOptions o;
            o.program = launch->program;
            o.args = launch->args;
            for (std::string& a : garnet_args(paths_, memory_mb)) {
                o.args.push_back(std::move(a));
            }
            o.working_dir = paths_.dir;
            if (!dotnet_dir.empty()) {
                o.env = garnet_env(dotnet_dir);
            }
            o.remove_env = garnet_env_names_removed();
            o.output_log = paths_.log;
#ifdef _WIN32
            // A real (empty) standard input: a child without a console and
            // with INVALID_HANDLE_VALUE there can fail as it starts (Mailpit did).
            o.stdin_file = L"\\\\.\\NUL";
#endif
            process = ChildProcess::start(o, why);
            if (process) {
                // Ready once the socket takes the password and answers PING.
                // Only this user can make files in the folder, so it's Garnet's.
                const auto deadline = Clock::now() + config_.start_limit;
                bool ready = false;
                std::string answer;
                while (!ready && Clock::now() < deadline && !stopping_) {
                    if (!process->running()) {
                        break;
                    }
                    const auto pong = authed_command(paths_.socket, password, {"PING"}, kCommandLimit);
                    ready = pong && pong->type == '+' && pong->text == "PONG" && process->running();
                    if (pong && !ready) {
                        answer = pong->text;
                    }
                    if (!ready) {
                        std::this_thread::sleep_for(100ms);
                    }
                }
                if (!ready) {
                    const bool ended = !process->running();
                    process->kill();
                    process.reset();
                    const std::string said = log_since(paths_.log, log_from);
                    why = ended ? "Garnet stopped as it started" : "Garnet didn't answer in time";
                    if (!said.empty()) {
                        why += ": " + said;
                    } else if (!answer.empty()) {
                        why += ": " + answer;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        process.reset();
        why = std::string("Garnet couldn't be started: ") + e.what();
    } catch (...) {
        process.reset();
        why = "Garnet couldn't be started";
    }
    try {
        why = clean_error_text(why);
    } catch (...) {
        why = "Garnet couldn't be started";
    }
    touch();
    lock.lock();
    if (process) {
        process_ = std::move(process);
        state_ = State::Running;
        error_.clear();
    } else {
        state_ = State::Failed;
        error_ = why;
        retry_after_ = Clock::now() + kFailedRetry;
        error = why;
    }
    changed_.notify_all();
    return state_ == State::Running;
}

void RedisService::stop_process() {
    std::unique_ptr<ChildProcess> process;
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return state_ != State::Starting && killing_ == 0; });
        process = std::move(process_);
        if (state_ == State::Running) {
            state_ = State::Asleep;
        }
        if (process) {
            ++killing_;
        }
    }
    kill_and_note(std::move(process));
}

void RedisService::kill_and_note(std::unique_ptr<ChildProcess> process, bool save) {
    if (process) {
        if (save && process->running()) {
            // The data comes back with --recover: SAVE writes the checkpoint.
            std::string password;
            {
                const std::lock_guard lock(mutex_);
                password = password_;
            }
            if (!password.empty()) {
                try {
                    const auto saved = authed_command(paths_.socket, password, {"SAVE"}, kSaveLimit);
                    if (!saved || saved->type != '+') {
                        const std::lock_guard lock(mutex_);
                        error_ = clean_error_text("Garnet's data couldn't be saved before it stopped" +
                                                  (saved ? ": " + saved->text : std::string()));
                    }
                } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch): it still stops
                }
            }
        }
        process->kill();
        const std::lock_guard lock(mutex_);
        --killing_;
    }
    changed_.notify_all();
}

RedisService::RemovalHold RedisService::hold_for_removal() {
    {
        const std::lock_guard lock(mutex_);
        ++held_;
    }
    stop_process();
    return RemovalHold(this);
}

void RedisService::release_hold() {
    {
        const std::lock_guard lock(mutex_);
        --held_;
    }
    changed_.notify_all();
}

void RedisService::supervise() {
    auto last_tick = Clock::now();
    auto last_reload = Clock::now();
    while (!stopping_) {
        std::shared_ptr<LoopbackListener> listener;
        {
            const std::lock_guard lock(mutex_);
            listener = listener_;
        }
        if (!listener) {
            std::this_thread::sleep_for(200ms);
        } else {
            for ([[maybe_unused]] const std::size_t i : waiting_connections({listener.get()}, 200ms)) {
                auto client = listener->accept_connection();
                if (!client) {
                    continue;
                }
                if (active_ >= kMaxConnections) {
                    close_socket(*client);
                    continue;
                }
                ++active_;
                touch();
                run_detached([this, c = *client] { serve(c); });
            }
        }
        if (stopping_) {
            break;
        }
        const auto now = Clock::now();
        if (now - last_tick >= 1s) {
            last_tick = now;
            tick(now);
        }
        if (now - last_reload >= config_.reload_every) {
            last_reload = now;
            reload();
        }
    }
}

void RedisService::serve(std::uintptr_t client) {
    bool relayed = false;
    {
        const AtExit counted([this] {
            touch();
            --active_;
        });
        // The first bytes, before anything is started or forwarded.
        std::string first;
        FirstBytes kind = FirstBytes::NeedMore;
        const auto deadline = Clock::now() + config_.first_bytes_limit;
        while (kind == FirstBytes::NeedMore) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (left <= 0ms) {
                break;
            }
            const auto got = receive_some(client, left);
            if (!got || got->empty()) {
                break;
            }
            first += *got;
            kind = classify_first_bytes(first);
        }
        if (kind != FirstBytes::Redis) {
            if (kind == FirstBytes::Http) {
                ++refused_;
            }
            close_socket(client);  // no answer: a page learns nothing
            return;
        }
        std::string error;
        std::optional<std::uintptr_t> upstream;
        if (ensure_running(error)) {
            upstream = connect_local(paths_.socket);
        }
        if (upstream && !send_all(*upstream, first)) {
            close_socket(*upstream);
            upstream.reset();
        }
        relayed = upstream.has_value();
        if (upstream) {
            relay_sockets(client, *upstream, [this] { touch(); });
        } else {
            // A Redis client reads this as an error; no paths or log text in it.
            answer_and_close(client, "-ERR Redis (Garnet) isn't available just now; FloServer's window says why\r\n");
        }
    }
    if (relayed) {
        keys_dirty_ = true;  // counted on the next tick
    }
}

void RedisService::tick(Clock::time_point now) {
    std::unique_ptr<ChildProcess> idle;
    bool wake = false;
    {
        const std::lock_guard lock(mutex_);
        if (state_ == State::Running && process_ && !process_->running()) {
            error_ = clean_error_text("Garnet stopped: " + log_since(paths_.log, 0));
            process_.reset();
            state_ = State::Asleep;
        }
        const auto last = Clock::time_point(Clock::duration(last_activity_.load()));
        if (state_ == State::Running && process_ && !settings_.keep_running && active_ == 0 &&
            now - last >= config_.idle_after) {
            idle = std::move(process_);
            ++killing_;
            state_ = State::Asleep;
        }
        wake = installed_ && install_error_.empty() && settings_.keep_running && held_ == 0 && !stopped_ &&
               (state_ == State::Asleep || (state_ == State::Failed && now >= retry_after_));
    }
    if (idle) {
        // SAVE may take a moment: not on the supervisor's thread.
        auto holder = std::make_shared<std::unique_ptr<ChildProcess>>(std::move(idle));
        run_detached([this, holder] { kill_and_note(std::move(*holder)); });
    }
    if (wake) {
        run_detached([this] {
            std::string ignored;  // shown by the status
            (void)ensure_running(ignored);
        });
    }
    if (keys_dirty_ && !refreshing_.exchange(true)) {
        keys_dirty_ = false;
        run_detached([this] {
            refresh_keys();
            refreshing_ = false;
        });
    }
}

std::optional<RespReply> RedisService::command(const std::vector<std::string_view>& args,
                                               std::chrono::milliseconds limit) {
    std::string password;
    {
        const std::lock_guard lock(mutex_);
        if (!process_ || !process_->running()) {
            return std::nullopt;  // the password goes only to the Garnet this service started
        }
        password = password_;
    }
    if (password.empty()) {
        return std::nullopt;
    }
    return authed_command(paths_.socket, password, args, limit);
}

void RedisService::refresh_keys() noexcept {
    try {
        const std::lock_guard serial(refresh_mutex_);
        const auto reply = command({"DBSIZE"}, kCommandLimit);
        if (!reply || reply->type != ':') {
            return;
        }
        const std::int64_t n = std::stoll(reply->text);
        const std::lock_guard lock(mutex_);
        keys_ = std::max<std::int64_t>(0, n);
        save_status_locked();
    } catch (...) {  // NOLINT(bugprone-empty-catch): the last count stands
    }
}

void RedisService::save_status_locked() const {
    std::string ignored;
    std::error_code ec;
    if (fs::is_directory(paths_.dir, ec)) {
        (void)write_private_file_atomically(paths_.status, json{{"keys", keys_}}.dump(), ignored);
    }
}

void RedisService::load_status() {
    const auto text = read_small_file(paths_.status, kMaxStatusFile);
    const json doc = text ? json::parse(*text, nullptr, false) : json();
    if (doc.is_object() && doc.contains("keys") && doc.at("keys").is_number_integer()) {
        const std::lock_guard lock(mutex_);
        keys_ = std::max<std::int64_t>(0, doc.at("keys").get<std::int64_t>());
    }
}

json RedisService::status() const {
    const std::lock_guard lock(mutex_);
    json s = json::object();
    s["installed"] = installed_;
    if (installed_) {
        s["version"] = version_;
        s["dotnet_version"] = dotnet_version_;
    }
    const char* state = "not_installed";
    std::string error;
    if (installed_ && !install_error_.empty()) {
        state = "failed";
        error = install_error_;
    } else if (installed_) {
        switch (state_) {
            case State::Asleep:
                state = "asleep";
                break;
            case State::Starting:
                state = "starting";
                break;
            case State::Running:
                state = "running";
                break;
            case State::Failed:
                state = "failed";
                break;
        }
        error = error_;
    }
    s["state"] = state;
    if (!error.empty()) {
        s["error"] = clean_error_text(error);
    }
    s["host"] = "127.0.0.1";
    s["port"] = listener_ ? listener_->port() : settings_.port.value_or(kFirstRedisPort);
    s["keys"] = keys_;
    s["refused"] = refused_.load();
    s["settings"] = {{"keep_running", settings_.keep_running}, {"memory_mb", settings_.memory_mb}};
    return s;
}

std::optional<Port> RedisService::port() const {
    const std::lock_guard lock(mutex_);
    if (!installed_ || !listener_) {
        return std::nullopt;
    }
    return listener_->port();
}

bool RedisService::set(const json& changes, ServiceRefusal& refusal) {
    if (!changes.is_object()) {
        refusal = {"bad_params", "the settings are an object"};
        return false;
    }
    for (const auto& [key, value] : changes.items()) {
        if (key == "keep_running") {
            if (!value.is_boolean()) {
                refusal = {"bad_params", "keep_running is true or false"};
                return false;
            }
        } else if (key == "memory_mb") {
            if (!value.is_number_integer() || value.get<std::int64_t>() < kMinRedisMemoryMb ||
                value.get<std::int64_t>() > kMaxRedisMemoryMb) {
                refusal = {"bad_params", "memory_mb is the memory limit in MB, from " +
                                             std::to_string(kMinRedisMemoryMb) + " to " +
                                             std::to_string(kMaxRedisMemoryMb)};
                return false;
            }
        } else {
            refusal = {"bad_params", "\"" + key + "\" isn't a Redis setting (keep_running, memory_mb)"};
            return false;
        }
    }
    const auto change = [&changes](AppConfig& config) {
        RedisSettings s = read_redis_settings(config);
        for (const auto& [key, value] : changes.items()) {
            if (key == "keep_running") {
                s.keep_running = value.get<bool>();
            } else if (key == "memory_mb") {
                s.memory_mb = static_cast<int>(value.get<std::int64_t>());
            }
        }
        write_redis_settings(config, s);
    };
    std::string error;
    bool saved = false;
    if (config_.change_config) {
        saved = config_.change_config(change, error);
    } else if (auto config = load_app_config(app_config_file(config_.data_dir), error)) {
        change(*config);
        saved = save_app_config(app_config_file(config_.data_dir), *config, error);
    }
    if (!saved) {
        refusal = {"not_saved", error.empty() ? std::string("config.json can't be changed") : error};
        return false;
    }
    reload();  // a new memory limit restarts Garnet (saved first); Keep running wakes it on the next tick
    return true;
}

bool RedisService::flush(ServiceRefusal& refusal) {
    ++active_;
    touch();
    const AtExit done([this] {
        touch();
        --active_;
    });
    std::string error;
    if (!ensure_running(error)) {
        const std::lock_guard lock(mutex_);
        refusal = {installed_ ? "redis_failed" : "not_installed", error};
        return false;
    }
    const auto reply = command({"FLUSHALL"}, kSaveLimit);
    if (!reply || reply->type != '+') {
        refusal = {"redis_failed", "Garnet didn't delete the keys" +
                                       (reply ? ": " + clean_error_text(reply->text) : std::string())};
        return false;
    }
    (void)command({"SAVE"}, kSaveLimit);  // so an older checkpoint doesn't bring them back
    const std::lock_guard lock(mutex_);
    keys_ = 0;
    save_status_locked();
    return true;
}

}  // namespace floserver::core

#include "floserver/core/mail_service.h"

#include "floserver/core/crypto.h"
#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/runtime_store.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <set>
#include <system_error>
#include <vector>


namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kMaxConnections = 256;
constexpr auto kFailedRetry = 30s;
constexpr std::size_t kMaxStatusFile = 65536;
constexpr auto kApiLimit = 5s;
constexpr std::size_t kMaxApiReply = std::size_t{4} * 1024 * 1024;

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

// What was added to `file` since `from`, its last `max` bytes.
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

// A ui-auth line as mailpit_auth_line() writes it, for this password.
bool auth_line_matches(std::string_view text, std::string_view password) {
    const std::string prefix = std::string(kMailUiUser) + ":";
    if (!text.starts_with(prefix)) {
        return false;
    }
    std::string_view hash = text.substr(prefix.size());
    while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r')) {
        hash.remove_suffix(1);
    }
    // $5$rounds=N$salt$hash or $5$salt$hash
    if (!hash.starts_with("$5$")) {
        return false;
    }
    std::string_view rest = hash.substr(3);
    unsigned rounds = 5000;
    if (rest.starts_with("rounds=")) {
        const auto end = rest.find('$');
        if (end == std::string_view::npos) {
            return false;
        }
        try {
            rounds = static_cast<unsigned>(std::stoul(std::string(rest.substr(7, end - 7))));
        } catch (const std::exception&) {
            return false;
        }
        rest = rest.substr(end + 1);
    }
    const auto end = rest.find('$');
    if (end == std::string_view::npos) {
        return false;
    }
    return sha256_crypt(password, rest.substr(0, end), rounds) == hash;
}

// "Sat, 07 Nov 2026 10:00:00 +0000"
std::string mail_date(std::chrono::system_clock::time_point now) {
    const auto secs = std::chrono::floor<std::chrono::seconds>(now);
    const auto days = std::chrono::floor<std::chrono::days>(secs);
    const std::chrono::year_month_day ymd{days};
    const std::chrono::hh_mm_ss hms{secs - days};
    const std::chrono::weekday wd{days};
    static constexpr std::array<const char*, 7> kDays = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr std::array<const char*, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::array<char, 64> text{};
    const int n = std::snprintf(text.data(), text.size(), "%s, %02u %s %04d %02d:%02d:%02d +0000",
                                kDays.at(wd.c_encoding()), static_cast<unsigned>(ymd.day()),
                                kMonths.at(static_cast<unsigned>(ymd.month()) - 1), static_cast<int>(ymd.year()),
                                static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()),
                                static_cast<int>(hms.seconds().count()));
    return {text.data(), n > 0 ? static_cast<std::size_t>(n) : 0};
}

// A plain address for a test email: no spaces, quotes, brackets or line breaks.
bool plain_address(std::string_view to) {
    const auto at = to.find('@');
    return !to.empty() && to.size() <= 254 && at != std::string_view::npos && at > 0 && at + 1 < to.size() &&
           std::none_of(to.begin(), to.end(), [](char c) {
               const auto u = static_cast<unsigned char>(c);
               return u <= 0x20 || u == 0x7F || c == '<' || c == '>' || c == '"' || c == '(' || c == ')' ||
                      c == ',' || c == ';' || c == '\\';
           });
}

// One SMTP reply's code (its last line's), reading on from `buffer`; 0 when
// the connection ended or the time ran out.
int smtp_reply(std::uintptr_t socket, std::string& buffer, std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    for (;;) {
        // A complete reply: lines "250-..." then "250 ...".
        std::size_t start = 0;
        for (auto end = buffer.find("\r\n"); end != std::string::npos; end = buffer.find("\r\n", start)) {
            const std::string line = buffer.substr(start, end - start);
            start = end + 2;
            if (line.size() >= 3 && (line.size() == 3 || line[3] == ' ')) {
                buffer.erase(0, start);
                try {
                    return std::stoi(line.substr(0, 3));
                } catch (const std::exception&) {
                    return 0;
                }
            }
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left <= 0ms || buffer.size() > 65536) {
            return 0;
        }
        const auto got = receive_some(socket, left);
        if (!got || got->empty()) {
            return 0;
        }
        buffer += *got;
    }
}

std::string string_field(const json& j, const char* key) {
    return j.is_object() && j.contains(key) && j.at(key).is_string() ? j.at(key).get<std::string>() : std::string();
}

// The recipients as one line to show: each cleaned, all of it at most 200 characters.
std::string recipients(const json& to) {
    std::string out;
    if (!to.is_array()) {
        return out;
    }
    for (const json& r : to) {
        if (!r.is_object()) {
            continue;
        }
        const std::string name = clean_mail_text(string_field(r, "Name"));
        const std::string address = clean_mail_text(string_field(r, "Address"));
        std::string one = address;
        if (!name.empty()) {
            one = name;
            one.append(" <").append(address).append(">");
        }
        if (one.empty()) {
            continue;
        }
        out += (out.empty() ? "" : ", ") + one;
        if (out.size() > 1000) {
            break;
        }
    }
    return clean_mail_text(out);
}

std::int64_t number(const json& j, const char* key) {
    return j.is_object() && j.contains(key) && j.at(key).is_number_integer() ? j.at(key).get<std::int64_t>() : 0;
}

}  // namespace

MailService::MailService(MailServiceConfig config, SecretStore secrets)
    : secrets_(std::move(secrets)), config_(std::move(config)), paths_(mailpit_paths(config_.data_dir)) {
    if (!config_.probe) {
        config_.probe = can_listen_on_loopback;
    }
    touch();
}

MailService::~MailService() { stop(); }

void MailService::start() {
    load_counts();
    reload();
    thread_ = std::thread([this] { supervise(); });
}

void MailService::stop() {
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
    smtp_listener_.reset();
    smtp6_listener_.reset();
    inbox_listener_.reset();
}

void MailService::touch() { last_activity_ = Clock::now().time_since_epoch().count(); }

void MailService::run_detached(std::function<void()> work) {
    {
        const std::lock_guard lock(threads_mutex_);
        ++threads_;
    }
    auto shared = std::make_shared<std::function<void()>>(std::move(work));
    const auto done = [this] {
        // Told under the lock: once it's let go, stop() may return and the
        // service be gone, so nothing of it is touched after that.
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
        (*shared)();  // no thread: here
        done();
    }
}

MailService::SmtpListeners MailService::open_smtp(Port port) const {
    // [::1] too where the machine has it, on the same port, so mail sent to
    // "localhost" by hand can't reach another program there.
    SmtpListeners out;
    std::string why;
    std::shared_ptr<LoopbackListener> v4 = LoopbackListener::open(port, why);
    if (!v4) {
        return out;
    }
    if (config_.ipv6 && loopback_ipv6_available()) {
        out.v6 = LoopbackListener::open_ipv6(port, why);
        if (!out.v6) {
            return out;  // both or neither
        }
    }
    out.v4 = std::move(v4);
    return out;
}

void MailService::reload() {
    const std::lock_guard serial(reload_mutex_);
    if (stopping_) {
        return;
    }
    std::string error;
    const auto app = load_app_config(app_config_file(config_.data_dir), error);
    if (!app) {
        return;  // keep what runs
    }
    const MailSettings settings = read_mail_settings(*app);
    const std::string blocked = find_project(*app, kMailHostName) != nullptr ? std::string(kMailHostName) : "";
    const auto installed = list_installed(runtimes_dir(config_.data_dir));
    const InstalledRuntime* best = newest_mailpit(installed);
    const InstalledRuntime* any = newest_mailpit_any(installed);

    if (best == nullptr && any == nullptr) {
        // Not installed (or removed): nothing runs and the ports go.
        std::unique_ptr<ChildProcess> process;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return state_ != State::Starting && killing_ == 0; });
            installed_ = false;
            version_.clear();
            program_.clear();
            install_error_.clear();
            settings_ = settings;
            blocked_by_ = blocked;
            process = std::move(process_);
            if (process) {
                ++killing_;
            }
            state_ = State::Asleep;
            error_.clear();
            smtp_listener_.reset();
            smtp6_listener_.reset();
            inbox_listener_.reset();
            php_problem_.clear();
        }
        kill_and_note(std::move(process));
        return;
    }

    std::string install_error;
    fs::path program;
    const std::string version = best != nullptr ? best->version : any->version;
    if (best == nullptr) {
        install_error = "Mailpit " + any->version + " is older than " + std::string(kMinMailpitVersion) +
                        " and has a known security problem; update it in Settings → Runtimes";
    } else if (const auto p = mailpit_program(best->dir)) {
        program = *p;
    } else {
        install_error = "the mailpit program is missing from " + path_to_utf8(best->dir);
    }

    std::unique_ptr<ChildProcess> retired;  // stopped below, outside the lock
    bool save_port = false;
    Port port_to_save = 0;
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return state_ != State::Starting && killing_ == 0; });
        if (stopped_) {
            return;
        }
        const bool replaced = installed_ && (version_ != version || program_ != program);
        const bool restart = process_ && (replaced || settings.max != settings_.max);
        installed_ = true;
        version_ = version;
        program_ = program;
        blocked_by_ = blocked;
        if (restart) {
            retired = std::move(process_);
            ++killing_;
            state_ = State::Asleep;
        }
        if (replaced && state_ == State::Failed) {
            state_ = State::Asleep;  // a new install: try it at once
        }
        settings_ = settings;

        // The SMTP port: the one kept in config.json (else 1025) when it can
        // be had; else the one held now; else the next free one from 1025.
        const Port wanted = settings.port.value_or(kFirstMailPort);
        if (!smtp_listener_ || smtp_listener_->port() != wanted) {
            SmtpListeners listeners = open_smtp(wanted);
            if (!listeners.v4 && !smtp_listener_) {
                std::set<Port> avoid = {wanted};
                if (inbox_listener_) {
                    avoid.insert(inbox_listener_->port());
                }
                // The probe asks for 127.0.0.1 and [::1] both.
                for (unsigned tries = 0; !listeners.v4 && tries < 10; ++tries) {
                    const auto free = pick_port_from(kFirstMailPort, avoid, config_.probe);
                    if (!free) {
                        break;
                    }
                    listeners = open_smtp(*free);
                    avoid.insert(*free);
                }
            }
            if (listeners.v4) {
                smtp_listener_ = std::move(listeners.v4);
                smtp6_listener_ = std::move(listeners.v6);
            }
        }
        // [::1] came later (or was busy then): held as soon as it can be.
        if (smtp_listener_ && !smtp6_listener_ && config_.ipv6 && loopback_ipv6_available()) {
            std::string why;
            smtp6_listener_ = LoopbackListener::open_ipv6(smtp_listener_->port(), why);
        }
        if (!inbox_listener_) {
            std::set<Port> avoid;
            if (smtp_listener_) {
                avoid.insert(smtp_listener_->port());
            }
            if (const auto free = pick_port_from(kFirstMailInboxPort, avoid, config_.probe)) {
                std::string why;
                inbox_listener_ = LoopbackListener::open(*free, why);
            }
        }
        if (install_error.empty() && !smtp_listener_) {
            install_error = "no port is free for Mailpit's SMTP (from " + std::to_string(kFirstMailPort) + " up)";
        } else if (install_error.empty() && !inbox_listener_) {
            install_error = "no port is free for Mailpit's inbox (from " + std::to_string(kFirstMailInboxPort) + " up)";
        }
        install_error_ = install_error;
        php_problem_ = mail_php_problem(*app, installed).value_or(std::string());
        if (smtp_listener_ && settings.port != smtp_listener_->port()) {
            save_port = true;
            port_to_save = smtp_listener_->port();
        }
    }
    kill_and_note(std::move(retired));
    if (install_error.empty()) {
        std::string why;
        (void)ensure_secret_files(why);  // nginx needs its file before the inbox is served; tried again next time
    }
    if (save_port) {
        // Kept from now on, so .env files and php.ini stay right.
        std::string why;
        const auto change = [port_to_save](AppConfig& config) {
            MailSettings s = read_mail_settings(config);
            s.port = port_to_save;
            write_mail_settings(config, s);
        };
        bool saved = false;
        if (config_.change_config) {
            saved = config_.change_config(change, why);
        } else if (auto config = load_app_config(app_config_file(config_.data_dir), why)) {
            change(*config);
            saved = save_app_config(app_config_file(config_.data_dir), *config, why);
        }
        if (saved) {
            {
                const std::lock_guard lock(mutex_);
                settings_.port = port_to_save;
            }
            if (config_.php_changed) {
                config_.php_changed();
            }
        }
    }
}

bool MailService::ensure_secret_files(std::string& error) {
    const std::lock_guard serial(secret_mutex_);  // one password, however many ask at once
    {
        const std::lock_guard lock(mutex_);
        std::error_code ec;
        if (!password_.empty() && fs::is_regular_file(paths_.ui_auth, ec) && fs::is_regular_file(paths_.nginx_auth, ec)) {
            return true;
        }
    }
    std::string password = secrets_.read(std::string(kMailSecretName)).value_or(std::string());
    if (password.size() != 64 || !from_hex(password)) {
        std::array<std::uint8_t, 32> bytes{};
        if (!random_bytes(bytes, error)) {
            return false;
        }
        password = to_hex(bytes);
        wipe(bytes);
        if (!secrets_.save(std::string(kMailSecretName), password, error)) {
            error = "can't keep the inbox's password: " + error;
            return false;
        }
    }
    // Owner-only before anything secret goes in (and again for what's there).
    if (!make_owner_only_folder(paths_.dir, error)) {
        return false;
    }
    const auto auth = read_small_file(paths_.ui_auth, 4096);
    if (!auth || !auth_line_matches(*auth, password)) {
        const auto line = mailpit_auth_line(password, error);
        if (!line || !write_private_file_atomically(paths_.ui_auth, *line, error)) {
            return false;
        }
    }
    const std::string header = mailpit_nginx_auth(password);
    if (read_small_file(paths_.nginx_auth, 4096) != header &&
        !write_private_file_atomically(paths_.nginx_auth, header, error)) {
        return false;
    }
    const std::lock_guard lock(mutex_);
    password_ = std::move(password);
    return true;
}

bool MailService::remove_stale_sockets(std::string& error) const {
    // Only these two names in Mailpit's own folder; a link goes as the link.
    for (const fs::path& socket : {paths_.smtp_socket, paths_.http_socket}) {
        std::error_code ec;
        const auto status = fs::symlink_status(socket, ec);
        if (ec || !fs::exists(status) || socket.parent_path() != paths_.dir) {
            continue;
        }
        if (fs::is_directory(status) || !fs::remove(socket, ec) || ec) {
            error = "can't remove the old socket " + path_to_utf8(socket) + (ec ? ": " + error_text(ec) : std::string());
            return false;
        }
    }
    return true;
}

bool MailService::ensure_running(std::string& error) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return state_ != State::Starting && killing_ == 0; });
    if (stopped_ || stopping_) {
        error = "FloServer is stopping";
        return false;
    }
    if (held_ > 0) {
        error = "Mailpit is being updated or removed; try again in a moment";
        return false;
    }
    if (!installed_) {
        error = "Mailpit isn't installed: download it in Settings → Runtimes (or: flo install mailpit)";
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
    const fs::path program = program_;
    const int max = settings_.max;
    lock.unlock();

    std::string why;
    std::unique_ptr<ChildProcess> process;
    // Whatever throws below, the state leaves Starting (else every later
    // start and stop would wait on it for good): it's caught and Failed.
    try {
    if (const auto problem = mailpit_socket_problem(paths_)) {
        why = *problem;  // never TCP instead: another program could take a TCP port first
    } else if (ensure_secret_files(why) && make_owner_only_folder(paths_.dir, why) &&
               // Before anything in the folder is touched: a socket left by a
               // killed Mailpit (Windows ends it outright) is a reparse point there.
               remove_stale_sockets(why) && make_owner_only_files(paths_.dir, why)) {
        std::error_code ec;
        fs::create_directories(paths_.log.parent_path(), ec);
        const std::uintmax_t log_from = file_bytes(paths_.log);
        ChildOptions o;
        o.program = program;
        o.args = mailpit_args(paths_, max);
        o.working_dir = paths_.dir;
        o.remove_env = mailpit_env_names();
        o.output_log = paths_.log;
        process = ChildProcess::start(o, why);
        if (process) {
            // Ready once both sockets take connections and it still runs.
            // Only this user can make files in the folder, so they're its.
            const auto deadline = Clock::now() + config_.start_limit;
            bool ready = false;
            while (!ready && Clock::now() < deadline && !stopping_) {
                if (!process->running()) {
                    break;
                }
                const auto a = connect_local(paths_.smtp_socket);
                const auto b = a ? connect_local(paths_.http_socket) : std::nullopt;
                if (a) {
                    close_socket(*a);
                }
                if (b) {
                    close_socket(*b);
                }
                ready = a && b && process->running();
                if (!ready) {
                    std::this_thread::sleep_for(50ms);
                }
            }
            if (!ready) {
                const bool ended = !process->running();
                process->kill();
                process.reset();
                const std::string said = log_since(paths_.log, log_from);
                why = ended ? "Mailpit stopped as it started" : "Mailpit didn't start listening in time";
                if (!said.empty()) {
                    why += ": " + said;
                }
            }
        }
    }
    } catch (const std::exception& e) {
        process.reset();
        why = std::string("Mailpit couldn't be started: ") + e.what();
    } catch (...) {
        process.reset();
        why = "Mailpit couldn't be started";
    }
    try {
        why = clean_error_text(why);
    } catch (...) {
        why = "Mailpit couldn't be started";
    }
    touch();
    lock.lock();
    if (process) {
        process_ = std::move(process);
        state_ = State::Running;
        error_.clear();
        accepted_seen_ = 0;  // Mailpit counts from its own start
    } else {
        state_ = State::Failed;
        error_ = why;
        retry_after_ = Clock::now() + kFailedRetry;
        error = why;
    }
    changed_.notify_all();
    return state_ == State::Running;
}

void MailService::stop_process() {
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

void MailService::kill_and_note(std::unique_ptr<ChildProcess> process) {
    if (process) {
        process->kill();  // nothing starts again meanwhile: the internal ports are its
        const std::lock_guard lock(mutex_);
        --killing_;
    }
    changed_.notify_all();
}

MailService::RemovalHold MailService::hold_for_removal() {
    {
        const std::lock_guard lock(mutex_);
        ++held_;
    }
    stop_process();
    return RemovalHold(this);
}

void MailService::release_hold() {
    {
        const std::lock_guard lock(mutex_);
        --held_;
    }
    changed_.notify_all();
}

bool MailService::delete_emails(std::string& error) {
    const std::lock_guard lock(mutex_);
    if (held_ == 0 || process_) {
        error = "Mailpit is running";
        return false;
    }
    for (const char* suffix : {"", "-wal", "-shm"}) {
        fs::path file = paths_.database;
        file += suffix;
        std::error_code ec;
        fs::remove(file, ec);
        if (ec) {
            error = "can't delete " + path_to_utf8(file) + ": " + error_text(ec);
            return false;
        }
    }
    std::error_code ec;
    fs::remove(paths_.status, ec);
    unread_ = 0;
    total_ = 0;
    latest_ = nullptr;
    return true;
}

void MailService::supervise() {
    auto last_tick = Clock::now();
    auto last_reload = Clock::now();
    while (!stopping_) {
        std::shared_ptr<LoopbackListener> smtp;
        std::shared_ptr<LoopbackListener> smtp6;
        std::shared_ptr<LoopbackListener> inbox;
        {
            const std::lock_guard lock(mutex_);
            smtp = smtp_listener_;
            smtp6 = smtp6_listener_;
            inbox = inbox_listener_;
        }
        std::vector<const LoopbackListener*> listeners;
        std::vector<bool> is_smtp;
        for (const auto& l : {smtp, smtp6}) {
            if (l) {
                listeners.push_back(l.get());
                is_smtp.push_back(true);
            }
        }
        if (inbox) {
            listeners.push_back(inbox.get());
            is_smtp.push_back(false);
        }
        if (listeners.empty()) {
            std::this_thread::sleep_for(200ms);
        }
        for (const std::size_t i : waiting_connections(listeners, 200ms)) {
            auto client = listeners[i]->accept_connection();
            if (!client) {
                continue;
            }
            if (active_ >= kMaxConnections) {
                close_socket(*client);
                continue;
            }
            // Counted now, before the idle check below can see it.
            ++active_;
            touch();
            const bool smtp_connection = is_smtp[i];  // a bool, not vector<bool>'s reference into the vector
            run_detached([this, c = *client, smtp_connection] { serve(c, smtp_connection); });
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

void MailService::serve(std::uintptr_t client, bool smtp) {
    bool relayed = false;
    {
        // Counted by the supervisor; let go here whatever happens, and
        // before the service's own look at the counts below (not activity).
        const AtExit counted([this] {
            touch();
            --active_;
        });
        std::string error;
        std::optional<std::uintptr_t> upstream;
        if (ensure_running(error)) {
            upstream = connect_local(smtp ? paths_.smtp_socket : paths_.http_socket);
        }
        relayed = upstream.has_value();
        if (upstream) {
            relay_sockets(client, *upstream, [this] { touch(); });
        } else if (smtp) {
            // Anyone who can connect reads this: no paths, no log text. FloServer says why.
            answer_and_close(client, "421 4.3.0 Mailpit isn't available just now; FloServer's window says why\r\n");
        } else {
            // A page aimed at 127.0.0.1 (DNS rebinding) can read this: nothing in it but the fact.
            const std::string body = "Mailpit isn't available just now. FloServer's window says why.\n";
            answer_and_close(client, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain; charset=utf-8\r\n"
                                     "Content-Length: " +
                                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        }
    }
    if (relayed) {
        if (smtp) {
            refresh(true);  // the new-mail notice
        } else {
            inbox_dirty_ = true;  // counted on the next tick: an inbox page makes many requests
        }
    }
}

void MailService::tick(Clock::time_point now) {
    std::unique_ptr<ChildProcess> idle;
    bool wake = false;
    {
        const std::lock_guard lock(mutex_);
        if (state_ == State::Running && process_ && !process_->running()) {
            error_ = clean_error_text("Mailpit stopped: " + log_since(paths_.log, 0));
            process_.reset();
            state_ = State::Asleep;  // the next connection starts it again
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
        kill_and_note(std::move(idle));
    }
    if (wake) {
        run_detached([this] {
            std::string ignored;  // shown by the status
            (void)ensure_running(ignored);
        });
    }
    // Inbox requests are counted at most once a tick (a page makes many).
    if (inbox_dirty_ && !refreshing_.exchange(true)) {
        inbox_dirty_ = false;
        run_detached([this] {
            refresh(false);
            refreshing_ = false;
        });
    }
}

std::optional<std::pair<int, std::string>> MailService::api(const std::string& method, const std::string& path) {
    std::string password;
    {
        const std::lock_guard lock(mutex_);
        if (!process_ || !process_->running()) {
            return std::nullopt;  // the password goes only to the Mailpit this service started
        }
        password = password_;
    }
    if (password.empty()) {
        return std::nullopt;
    }
    const auto socket = connect_local(paths_.http_socket);
    if (!socket) {
        return std::nullopt;
    }
    const std::string request = method + " " + path + " HTTP/1.0\r\nHost: localhost" +
                                "\r\nAuthorization: " + mailpit_basic_auth(password) +
                                "\r\nAccept: application/json\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    std::string response;
    if (send_all(*socket, request)) {
        const auto deadline = Clock::now() + kApiLimit;
        while (response.size() < kMaxApiReply) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (left <= 0ms) {
                break;
            }
            const auto got = receive_some(*socket, left);
            if (!got || got->empty()) {
                break;
            }
            response += *got;
        }
    }
    close_socket(*socket);
    const auto body = response.find("\r\n\r\n");
    if (!response.starts_with("HTTP/1.") || response.size() < 12 || body == std::string::npos) {
        return std::nullopt;
    }
    try {
        return std::pair<int, std::string>{std::stoi(response.substr(9, 3)), response.substr(body + 4)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void MailService::refresh(bool after_smtp) noexcept {
    // On a thread of its own: nothing Mailpit says may end the service.
    try {
        refresh_now(after_smtp);
    } catch (...) {  // NOLINT(bugprone-empty-catch): the last values stand
    }
}

void MailService::refresh_now(bool after_smtp) {
    (void)after_smtp;  // either way: a batch Mailpit took since the last look is a notice
    const std::lock_guard serial(refresh_mutex_);
    const auto info = api("GET", "/api/v1/info");
    if (!info || info->first != 200) {
        return;  // asleep or busy: the last values stand
    }
    const json doc = json::parse(info->second, nullptr, false);
    if (!doc.is_object()) {
        return;
    }
    const std::int64_t accepted = doc.contains("RuntimeStats") ? number(doc.at("RuntimeStats"), "SMTPAccepted") : 0;
    std::int64_t batch = 0;
    {
        const std::lock_guard lock(mutex_);
        batch = accepted - accepted_seen_;
        accepted_seen_ = accepted;
    }
    json latest;
    if (batch > 0) {
        const auto list = api("GET", "/api/v1/messages?limit=1");
        const json messages = list && list->first == 200 ? json::parse(list->second, nullptr, false) : json();
        if (messages.is_object() && messages.contains("messages") && messages.at("messages").is_array() &&
            !messages.at("messages").empty() && messages.at("messages").front().is_object()) {
            const json& m = messages.at("messages").front();
            // Anyone can send any text: shown cleaned and cut short.
            latest = {{"subject", clean_mail_text(string_field(m, "Subject"))},
                      {"to", recipients(m.contains("To") ? m.at("To") : json())},
                      {"new_in_batch", batch}};
        } else {
            latest = {{"subject", ""}, {"to", ""}, {"new_in_batch", batch}};
        }
    }
    const std::lock_guard lock(mutex_);
    total_ = number(doc, "Messages");
    unread_ = number(doc, "Unread");
    if (batch > 0) {
        ++notice_seq_;
        latest_ = std::move(latest);
    }
    save_counts_locked();
}

void MailService::save_counts_locked() const {
    json doc = {{"unread", unread_}, {"total", total_}};
    if (latest_.is_object()) {
        doc["latest"] = latest_;
    }
    std::string text = doc.dump(-1, ' ', false, json::error_handler_t::replace);
    if (text.size() > kMaxStatusFile) {
        doc.erase("latest");  // can't happen with cleaned text; never a file load_counts() refuses
        text = doc.dump();
    }
    std::string ignored;  // only for showing while asleep after a restart
    std::error_code ec;
    if (fs::is_directory(paths_.dir, ec)) {
        (void)write_private_file_atomically(paths_.status, text, ignored);
    }
}

void MailService::load_counts() {
    const auto text = read_small_file(paths_.status, kMaxStatusFile);
    const json doc = text ? json::parse(*text, nullptr, false) : json();
    if (!doc.is_object()) {
        return;
    }
    const std::lock_guard lock(mutex_);
    unread_ = std::max<std::int64_t>(0, number(doc, "unread"));
    total_ = std::max<std::int64_t>(0, number(doc, "total"));
    if (doc.contains("latest") && doc.at("latest").is_object()) {
        const json& l = doc.at("latest");
        latest_ = {{"subject", clean_mail_text(string_field(l, "subject"))},
                   {"to", clean_mail_text(string_field(l, "to"))},
                   {"new_in_batch", std::max<std::int64_t>(0, number(l, "new_in_batch"))}};
    }
}

json MailService::status() const {
    const std::lock_guard lock(mutex_);
    json s = json::object();
    s["installed"] = installed_;
    if (installed_) {
        s["version"] = version_;
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
    if (!php_problem_.empty()) {
        s["php_error"] = clean_error_text(php_problem_);  // PHP's mail() isn't set up, and why
    }
    s["smtp_port"] = smtp_listener_ ? smtp_listener_->port() : settings_.port.value_or(kFirstMailPort);
    if (!blocked_by_.empty()) {
        s["inbox_blocked_by"] = blocked_by_;
    }
    s["unread"] = unread_;
    s["total"] = total_;
    std::uintmax_t size = 0;
    for (const char* suffix : {"", "-wal"}) {
        fs::path file = paths_.database;
        file += suffix;
        size += file_bytes(file);
    }
    s["size_bytes"] = size;
    s["notice_seq"] = notice_seq_;
    if (latest_.is_object()) {
        s["latest"] = latest_;
    }
    s["settings"] = {{"php", settings_.php},
                     {"notices", settings_.notices},
                     {"keep_running", settings_.keep_running},
                     {"max", settings_.max}};
    return s;
}

std::optional<MailInbox> MailService::inbox() const {
    const std::lock_guard lock(mutex_);
    if (!installed_ || !install_error_.empty() || !inbox_listener_ || password_.empty()) {
        return std::nullopt;
    }
    return MailInbox{inbox_listener_->port(), paths_.nginx_auth};
}

std::optional<Port> MailService::smtp_port() const {
    const std::lock_guard lock(mutex_);
    if (!installed_ || !smtp_listener_) {
        return std::nullopt;
    }
    return smtp_listener_->port();
}

bool MailService::set(const json& changes, ServiceRefusal& refusal) {
    if (!changes.is_object()) {
        refusal = {"bad_params", "the settings are an object"};
        return false;
    }
    for (const auto& [key, value] : changes.items()) {
        if (key == "php" || key == "notices" || key == "keep_running") {
            if (!value.is_boolean()) {
                refusal = {"bad_params", key + " is true or false"};
                return false;
            }
        } else if (key == "max") {
            if (!value.is_number_integer() || value.get<std::int64_t>() < 0 || value.get<std::int64_t>() > kMaxMailMax) {
                refusal = {"bad_params", "max is how many emails to keep, from 0 (all) to " + std::to_string(kMaxMailMax)};
                return false;
            }
        } else {
            refusal = {"bad_params", "\"" + key + "\" isn't a mail setting (php, notices, keep_running, max)"};
            return false;
        }
    }
    MailSettings before;
    {
        const std::lock_guard lock(mutex_);
        before = settings_;
    }
    const auto change = [&changes](AppConfig& config) {
        MailSettings s = read_mail_settings(config);
        for (const auto& [key, value] : changes.items()) {
            if (key == "php") {
                s.php = value.get<bool>();
            } else if (key == "notices") {
                s.notices = value.get<bool>();
            } else if (key == "keep_running") {
                s.keep_running = value.get<bool>();
            } else if (key == "max") {
                s.max = static_cast<int>(value.get<std::int64_t>());
            }
        }
        write_mail_settings(config, s);
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
    reload();  // the new settings now: a new max restarts Mailpit, Keep running wakes it on the next tick
    MailSettings after;
    {
        const std::lock_guard lock(mutex_);
        after = settings_;
    }
    if (after.php != before.php && config_.php_changed) {
        config_.php_changed();
    }
    return true;
}

bool MailService::send_test(const std::string& to_in, ServiceRefusal& refusal) {
    const std::string to = to_in.empty() ? std::string("you@example.com") : to_in;
    if (!plain_address(to)) {
        refusal = {"bad_params", "\"" + to + "\" isn't an email address such as you@example.com"};
        return false;
    }
    Port port = 0;
    std::chrono::seconds limit{};
    {
        const std::lock_guard lock(mutex_);
        if (!installed_) {
            refusal = {"not_installed", "Mailpit isn't installed: download it in Settings → Runtimes (or: flo install mailpit)"};
            return false;
        }
        if (!install_error_.empty()) {
            refusal = {"mail_failed", install_error_};
            return false;
        }
        port = smtp_listener_ ? smtp_listener_->port() : 0;
        limit = config_.start_limit + 10s;
    }
    const auto socket = port != 0 ? connect_loopback(port) : std::nullopt;
    if (!socket) {
        refusal = {"mail_failed", "Mailpit's SMTP port can't be reached"};
        return false;
    }
    const AtExit closed([&socket] { close_socket(*socket); });
    std::string buffer;
    const auto step = [&](const std::string& line, std::initializer_list<int> ok, std::chrono::milliseconds wait) {
        if (!line.empty() && !send_all(*socket, line)) {
            return 0;
        }
        const int code = smtp_reply(*socket, buffer, wait);
        return std::find(ok.begin(), ok.end(), code) != ok.end() ? code : -code;
    };
    const auto now = std::chrono::system_clock::now();
    std::array<std::uint8_t, 8> id{};
    std::string ignored;
    (void)random_bytes(id, ignored);
    const std::string message =
        "From: FloServer <" + std::string(kMailFrom) + ">\r\n"
        "To: <" + to + ">\r\n"
        "Subject: FloServer test email\r\n"
        "Date: " + mail_date(now) + "\r\n"
        "Message-ID: <" + to_hex(id) + "@floserver.localhost>\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "This is a test email from FloServer.\r\n"
        "\r\n"
        "Email your projects send arrives in this inbox and goes nowhere else.\r\n";
    int code = step("", {220}, limit);  // the first connection waits for Mailpit to start
    if (code > 0) {
        code = step("EHLO floserver.localhost\r\n", {250}, kApiLimit);
    }
    if (code > 0) {
        code = step("MAIL FROM:<" + std::string(kMailFrom) + ">\r\n", {250}, kApiLimit);
    }
    if (code > 0) {
        code = step("RCPT TO:<" + to + ">\r\n", {250, 251}, kApiLimit);
    }
    if (code > 0) {
        code = step("DATA\r\n", {354}, kApiLimit);
    }
    if (code > 0) {
        code = step(message + ".\r\n", {250}, kApiLimit);
    }
    if (code <= 0) {
        std::string why;
        {
            const std::lock_guard lock(mutex_);
            why = error_;
        }
        refusal = {"mail_failed", "Mailpit didn't take the test email" +
                                      (code < 0 ? " (it answered " + std::to_string(-code) + ")" : std::string()) +
                                      (why.empty() ? std::string() : ": " + why)};
        return false;
    }
    (void)step("QUIT\r\n", {221}, 2s);
    // The counts now, so the reply's caller sees the email; the relay's own
    // look when the connection ends then finds nothing new (no second notice).
    refresh(true);
    return true;
}

bool MailService::clear(ServiceRefusal& refusal) {
    // Counts as a connection meanwhile, so it doesn't fall asleep under it.
    ++active_;
    touch();
    const AtExit done([this] {
        touch();
        --active_;
    });
    std::string error;
    if (!ensure_running(error)) {
        const std::lock_guard lock(mutex_);
        refusal = {installed_ ? "mail_failed" : "not_installed", error};
        return false;
    }
    const auto reply = api("DELETE", "/api/v1/messages");
    if (!reply || reply->first != 200) {
        refusal = {"mail_failed", "Mailpit didn't delete the emails" +
                                      (reply ? " (it answered " + std::to_string(reply->first) + ")" : std::string())};
        return false;
    }
    const std::lock_guard lock(mutex_);
    unread_ = 0;
    total_ = 0;
    latest_ = nullptr;
    save_counts_locked();
    return true;
}

}  // namespace floserver::core

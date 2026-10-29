#include "floserver/core/php_pool.h"

#include "floserver/core/paths.h"
#include "floserver/core/process.h"

#include <algorithm>
#include <sstream>
#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr std::uintmax_t kMaxIdleLogBytes = std::uintmax_t{8} * 1024 * 1024;
constexpr auto kQuickExit = 5s;          // php-cgi that ends this soon after starting
constexpr std::size_t kQuickExitsToFail = 3;
constexpr auto kQuickExitWindow = 60s;
constexpr auto kFailedRetry = 30s;

std::string ini_path(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return "\"" + std::string(u.begin(), u.end()) + "\"";
}

#ifdef _WIN32
bool has_ext(const fs::path& php_dir, const std::string& name) {
    std::error_code ec;
    return fs::is_regular_file(php_dir / "ext" / path_from_utf8("php_" + name + ".dll"), ec);
}
#endif

}  // namespace

const std::vector<std::string>& base_php_extensions() {
    static const std::vector<std::string> base = {"curl",      "fileinfo",   "gd",     "intl",   "mbstring", "mysqli",
                                                  "openssl",   "pdo_mysql",  "pdo_sqlite", "sodium", "zip"};
    return base;
}

std::optional<fs::path> php_cgi_program(const fs::path& php_dir) {
#ifdef _WIN32
    const char* names[] = {"php-cgi.exe"};  // NOLINT(modernize-avoid-c-arrays)
#else
    const char* names[] = {"php-cgi", "bin/php-cgi"};  // NOLINT(modernize-avoid-c-arrays)
#endif
    for (const char* n : names) {
        std::error_code ec;
        fs::path p = php_dir / n;
        if (fs::is_regular_file(p, ec)) {
            return p;
        }
    }
    return std::nullopt;
}

PhpIniResult make_php_ini(const fs::path& php_dir, const std::vector<std::string>& extensions, const fs::path& error_log) {
    PhpIniResult result;
    std::ostringstream ini;
    ini << "; Written by FloServer. It is replaced whenever the pool changes, so edits here are lost.\n"
        << "; Add extensions with \"extensions\" in the project's floserver.json.\n\n"
        << "memory_limit = 512M\n"
        << "max_execution_time = 300\n"
        << "max_input_time = 300\n"
        << "post_max_size = 512M\n"
        << "upload_max_filesize = 512M\n"
        << "max_file_uploads = 50\n"
        << "display_errors = On\n"
        << "display_startup_errors = On\n"
        << "error_reporting = E_ALL\n"
        << "log_errors = On\n"
        << "error_log = " << ini_path(error_log) << "\n"
        << "default_charset = \"UTF-8\"\n"
        << "cgi.fix_pathinfo = 1\n"
        << "opcache.enable = 1\n"
        << "opcache.validate_timestamps = 1\n"
        << "opcache.revalidate_freq = 0\n";
#ifdef _WIN32
    ini << "extension_dir = " << ini_path(php_dir / "ext") << "\n\n";
    if (has_ext(php_dir, "opcache")) {
        ini << "zend_extension = opcache\n";  // 8.2-8.4; built in from 8.5
    }
    std::vector<std::string> all;
    for (const std::string& e : base_php_extensions()) {
        if (has_ext(php_dir, e)) {
            all.push_back(e);
        }
    }
    for (const std::string& e : extensions) {
        if (std::find(all.begin(), all.end(), e) != all.end()) {
            continue;
        }
        if (has_ext(php_dir, e)) {
            all.push_back(e);
        } else {
            result.missing.push_back(e);
        }
    }
    for (const std::string& e : all) {
        ini << "extension = " << e << "\n";
    }
#else
    // Extensions come with the build here (the system's packages, or the
    // static builds of D5), so none are named.
    (void)php_dir;
    (void)extensions;
#endif
    result.text = ini.str();
    return result;
}

PhpPoolRunner::PhpPoolRunner(PhpPool pool, std::vector<std::unique_ptr<LoopbackListener>> listeners, fs::path ini,
                             fs::path access_log)
    : pool_(std::move(pool)), listeners_(std::move(listeners)), ini_(std::move(ini)), access_log_(std::move(access_log)) {
    std::error_code ec;
    log_size_ = fs::exists(access_log_, ec) ? fs::file_size(access_log_, ec) : 0;
}

std::unique_ptr<PhpPoolRunner> PhpPoolRunner::open(const PhpPool& pool, const fs::path& work_dir, std::string& error) {
    const fs::path dir = work_dir / "pools" / path_from_utf8(pool.key);
    const fs::path logs = work_dir / "logs";
    const auto ini = make_php_ini(pool.php_dir, pool.extensions, logs / path_from_utf8(pool.key + ".php-errors.log"));
    if (!ini.missing.empty()) {
        error = "PHP " + pool.version + " has no extension";
        error += ini.missing.size() > 1 ? "s " : " ";
        for (std::size_t i = 0; i < ini.missing.size(); ++i) {
            error += (i > 0 ? ", " : "") + ini.missing[i];
        }
        return nullptr;
    }
    if (!php_cgi_program(pool.php_dir)) {
        error = "PHP " + pool.version + " in " + path_to_utf8(pool.php_dir) + " has no php-cgi";
        return nullptr;
    }
    std::error_code ec;
    fs::create_directories(logs, ec);
    if (!write_file_atomically(dir / "php.ini", ini.text, error)) {
        return nullptr;
    }
    if (pool.ports.empty()) {
        error = "PHP " + pool.version + " has no port";
        return nullptr;
    }
    std::vector<std::unique_ptr<LoopbackListener>> listeners;
    for (const Port port : pool.ports) {
        auto listener = LoopbackListener::open(port, error);
        if (!listener) {
            return nullptr;
        }
        listeners.push_back(std::move(listener));
    }
    return std::unique_ptr<PhpPoolRunner>(new PhpPoolRunner(pool, std::move(listeners), dir / "php.ini",
                                                            logs / path_from_utf8(pool.key + ".access.log")));
}

std::vector<const LoopbackListener*> PhpPoolRunner::listeners() const {
    std::vector<const LoopbackListener*> out;
    out.reserve(listeners_.size());
    for (const auto& l : listeners_) {
        out.push_back(l.get());
    }
    return out;
}

std::optional<std::uint32_t> PhpPoolRunner::pid() const {
    if (!workers_.empty()) {
        return workers_.front().process->pid();
    }
    return std::nullopt;
}

std::vector<std::uint32_t> PhpPoolRunner::pids() const {
    std::vector<std::uint32_t> out;
    out.reserve(workers_.size());
    for (const Worker& w : workers_) {
        out.push_back(w.process->pid());
    }
    return out;
}

bool PhpPoolRunner::wants_wake_on_connection(Clock::time_point now) const {
    return state_ != PoolState::Running && now >= retry_after_;
}

// One php-cgi worker on one of the pool's sockets.
//
// Each worker is a php-cgi of its own with a socket of its own as standard
// input and PHP_FCGI_CHILDREN=0. On Windows, several processes accepting on
// one shared socket (php-cgi's own PHP_FCGI_CHILDREN workers, or several
// php-cgi given the same socket) lose connections: they're taken and never
// answered. One process per socket answers every one (found on a real
// Windows PC, 29 Oct). The same scheme runs everywhere, so the Linux tests
// cover it.
bool PhpPoolRunner::start_worker(std::size_t socket, Clock::time_point now, std::string& error) {
    const auto program = php_cgi_program(pool_.php_dir);
    if (!program) {
        error = "php-cgi is missing from " + path_to_utf8(pool_.php_dir);
        return false;
    }
    ChildOptions o;
    o.program = *program;
    // Relative to the pool's folder: php-cgi reads its arguments in the ANSI
    // code page on Windows, which can't hold every user folder name.
    o.args = {"-c", "php.ini"};
    o.working_dir = ini_.parent_path();
    // The user's own PHP settings (a global XAMPP or Laragon install) must
    // not load other ini files or extensions into FloServer's pools.
    o.remove_env = {"PHPRC", "PHP_INI_SCAN_DIR"};
    // php-cgi ends itself after this many requests; FloServer starts a fresh one.
    o.env = {{"PHP_FCGI_CHILDREN", "0"}, {"PHP_FCGI_MAX_REQUESTS", "500"}};
    o.stdin_socket = listeners_.at(socket).get();
    auto process = ChildProcess::start(o, error);
    if (!process) {
        return false;
    }
    workers_.push_back({socket, std::move(process), now});
    return true;
}

void PhpPoolRunner::fail(Clock::time_point now, std::string message) {
    workers_.clear();
    state_ = PoolState::Failed;
    last_error_ = std::move(message);
    retry_after_ = now + kFailedRetry;
    quick_exits_.clear();
}

void PhpPoolRunner::wake(Clock::time_point now) {
    if (state_ == PoolState::Running || now < retry_after_) {
        return;
    }
    std::string error;
    for (std::size_t i = 0; i < listeners_.size(); ++i) {
        if (!start_worker(i, now, error)) {
            fail(now, error);
            return;
        }
    }
    state_ = PoolState::Running;
    last_request_ = now;  // idle time counts from the wake
}

void PhpPoolRunner::sleep() {
    workers_.clear();  // ends every php-cgi; the socket stays
    if (state_ == PoolState::Running) {
        state_ = PoolState::Sleeping;
    }
}

void PhpPoolRunner::tick(Clock::time_point now, std::chrono::seconds idle_after) {
    std::error_code ec;
    const std::uintmax_t size = fs::exists(access_log_, ec) ? fs::file_size(access_log_, ec) : 0;
    if (!ec && size != log_size_) {
        last_request_ = now;
        log_size_ = size;
        if (size > kMaxIdleLogBytes) {
            fs::resize_file(access_log_, 0, ec);
            log_size_ = ec ? size : 0;
        }
    }

    // Workers that ended: one that ran a while (php-cgi ends itself after
    // PHP_FCGI_MAX_REQUESTS) is replaced; several ending right after they
    // started means this PHP can't run (a broken php.ini or extension).
    std::optional<int> last_code;
    std::erase_if(workers_, [&](Worker& w) {
        if (w.process->running()) {
            return false;
        }
        last_code = w.process->wait(0ms);
        if (now - w.started < kQuickExit) {
            quick_exits_.push_back(now);
        }
        return true;
    });
    std::erase_if(quick_exits_, [now](Clock::time_point t) { return now - t > kQuickExitWindow; });
    if (quick_exits_.size() >= kQuickExitsToFail) {
        fail(now, "php-cgi stops right after starting (exit " + std::to_string(last_code.value_or(-1)) + "); see " +
                      pool_.key + ".php-errors.log");
    } else if (state_ == PoolState::Running) {
        // Every socket has its worker again.
        std::string error;
        for (std::size_t i = 0; i < listeners_.size() && state_ == PoolState::Running; ++i) {
            const bool served = std::any_of(workers_.begin(), workers_.end(), [i](const Worker& w) { return w.socket == i; });
            if (!served && !start_worker(i, now, error)) {
                fail(now, error);
            }
        }
    }

    if (state_ == PoolState::Running && !pool_.keep_running && last_request_ && now - *last_request_ >= idle_after) {
        // The log has a line per finished request; a request still running
        // (or one just arriving) shows as an open connection instead.
        if (std::any_of(pool_.ports.begin(), pool_.ports.end(), has_open_connections)) {
            last_request_ = now;
        } else {
            sleep();
        }
    }
    if (pool_.keep_running && state_ != PoolState::Running && now >= retry_after_) {
        wake(now);
    }
}

}  // namespace floserver::core

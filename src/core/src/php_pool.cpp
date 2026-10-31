#include "floserver/core/php_pool.h"

#include "floserver/core/paths.h"
#include "floserver/core/process.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
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

namespace {

// One section of values, leaving out what FloServer manages (in case the
// settings were written by hand around the checks).
void write_values(std::ostringstream& ini, const std::map<std::string, std::string>& values) {
    for (const auto& [key, value] : values) {
        if (!php_ini_key_problem(key) && !php_ini_value_problem(value)) {
            ini << key << " = " << value << "\n";
        }
    }
}

}  // namespace

std::vector<std::string> php_extensions_in(const fs::path& ext_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::directory_iterator it(ext_dir, ec);
    for (std::size_t looked = 0; !ec && it != fs::directory_iterator() && looked < 1000; it.increment(ec), ++looked) {
        std::string file = path_to_utf8(it->path().filename());
        std::transform(file.begin(), file.end(), file.begin(),
                       [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
        if (file.size() > 8 && file.starts_with("php_") && file.ends_with(".dll")) {
            std::string name = file.substr(4, file.size() - 8);
            if (is_php_extension_name(name)) {
                out.push_back(std::move(name));
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::vector<std::string>> available_php_extensions(const fs::path& php_dir) {
#ifdef _WIN32
    return php_extensions_in(php_dir / "ext");
#else
    (void)php_dir;
    return std::nullopt;
#endif
}

PhpIniResult make_php_ini(const fs::path& php_dir, const PhpIniSettings& settings, const fs::path& error_log) {
    PhpIniResult result;
    std::ostringstream ini;
    ini << "; Written by FloServer. It is replaced whenever the pool changes, so edits here are lost.\n"
        << "; Change PHP in FloServer's Settings -> PHP (or the project's own panel), or add your own\n"
        << "; lines to the series' custom.ini (Settings -> PHP -> Edit the full php.ini).\n\n";
    for (const auto& [key, value] : php_ini_defaults()) {
        ini << key << " = " << value << "\n";
    }
    ini << "error_log = " << ini_path(error_log) << "\n";
#ifdef _WIN32
    ini << "extension_dir = " << ini_path(php_dir / "ext") << "\n\n";
    if (has_ext(php_dir, "opcache")) {
        ini << "zend_extension = opcache\n";  // 8.2-8.4; built in from 8.5
    }
    std::vector<std::string> all;
    for (const std::string& e : base_php_extensions()) {
        if (has_ext(php_dir, e) &&
            std::find(settings.removed.begin(), settings.removed.end(), e) == settings.removed.end()) {
            all.push_back(e);
        }
    }
    for (const std::string& e : settings.extensions) {
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
        // Xdebug hooks into the engine, so it loads as a Zend extension.
        ini << (e == "xdebug" ? "zend_extension = " : "extension = ") << e << "\n";
    }
#else
    // Extensions come with the build here (the system's packages, or the
    // static builds of D5), so none are named.
    (void)php_dir;
#endif
    // Later values win in php.ini: the series', its custom.ini, the project's.
    const std::string series = settings.series.empty() ? std::string("this PHP") : "PHP " + settings.series;
    if (!settings.series_ini.empty()) {
        ini << "\n; " << series << " (Settings -> PHP)\n";
        write_values(ini, settings.series_ini);
    }
    if (!settings.custom_ini.empty()) {
        ini << "\n; " << series << "'s custom.ini\n" << settings.custom_ini;
        if (!settings.custom_ini.ends_with('\n')) {
            ini << "\n";
        }
    }
    if (!settings.project_ini.empty()) {
        ini << "\n; The project's php_ini (floserver.json)\n";
        write_values(ini, settings.project_ini);
    }
    result.text = ini.str();
    return result;
}

std::optional<fs::path> php_cli_program(const fs::path& php_dir) {
#ifdef _WIN32
    const char* names[] = {"php.exe"};  // NOLINT(modernize-avoid-c-arrays)
#else
    const char* names[] = {"bin/php", "php"};  // NOLINT(modernize-avoid-c-arrays)
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

namespace {

constexpr std::size_t kMaxKnownIniBytes = std::size_t{512} * 1024;
constexpr auto kKnownIniLimit = 20s;

// A JSON array of names, or nothing.
std::optional<std::vector<std::string>> names_from_json(const std::optional<std::string>& text) {
    if (!text) {
        return std::nullopt;
    }
    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, false);
    if (!doc.is_array() || doc.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> out;
    out.reserve(doc.size());
    for (const nlohmann::json& name : doc) {
        if (name.is_string()) {
            out.push_back(name.get<std::string>());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace

std::optional<std::vector<std::string>> known_php_ini_names(const fs::path& php_dir, const fs::path& cache_file,
                                                            std::string& error) {
    if (auto cached = names_from_json(read_small_file(cache_file, kMaxKnownIniBytes))) {
        return cached;
    }
    const auto program = php_cli_program(php_dir);
    if (!program) {
        error = "this PHP has no php program";
        return std::nullopt;
    }
    // A pool's php.ini with every extension the install has turned on, so
    // their settings count as known. Ones that need other software
    // installed first (Oracle, Firebird) are left out: they'd only fail.
    PhpIniSettings settings;
    if (const auto available = available_php_extensions(php_dir)) {
        for (const std::string& e : *available) {
            const bool needs_more = e.starts_with("oci8") || e == "pdo_oci" || e == "pdo_firebird" || e == "interbase";
            const bool base =
                std::find(base_php_extensions().begin(), base_php_extensions().end(), e) != base_php_extensions().end();
            if (!needs_more && !base && e != "opcache") {
                settings.extensions.push_back(e);
            }
        }
    }
    const fs::path folder = cache_file.parent_path();
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path ini = folder / path_from_utf8(path_to_utf8(cache_file.stem()) + ".ini");
    const fs::path out = folder / path_from_utf8(path_to_utf8(cache_file.stem()) + ".out");
    const PhpIniResult made = make_php_ini(php_dir, settings, folder / "known-ini-errors.log");
    if (!write_file_atomically(ini, made.text, error)) {
        return std::nullopt;
    }
    fs::remove(out, ec);
    const std::vector<std::string> args = {
        "-c", path_to_utf8(ini), "-r", "file_put_contents($argv[1], json_encode(array_keys(ini_get_all(null, false))));",
        path_to_utf8(out)};
    const auto code = run_and_wait(*program, args, kKnownIniLimit, error);
    auto names = names_from_json(read_small_file(out, kMaxKnownIniBytes));
    fs::remove(ini, ec);
    fs::remove(out, ec);
    fs::remove(folder / "known-ini-errors.log", ec);
    if (!code || !names) {
        if (error.empty()) {
            error = "php didn't list its settings";
        }
        return std::nullopt;
    }
    // Kept for next time; an older install's list goes.
    std::vector<fs::path> older;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = path_to_utf8(it->path().filename());
        if (name.starts_with("known-ini-") && name.ends_with(".json") && it->path().filename() != cache_file.filename()) {
            older.push_back(it->path());
        }
    }
    for (const fs::path& p : older) {
        fs::remove(p, ec);
    }
    std::string ignored;
    (void)write_file_atomically(cache_file, nlohmann::json(*names).dump(), ignored);
    return names;
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
    const auto ini = make_php_ini(pool.php_dir, pool.settings, logs / path_from_utf8(pool.key + ".php-errors.log"));
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

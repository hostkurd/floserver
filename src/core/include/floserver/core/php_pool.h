#pragma once

// One PHP FastCGI pool (D10, D11, Q1): one PHP install and one set of
// extensions, shared by the projects that ask for the same pair.
//
// The pool's listening socket is opened once and held for as long as the
// pool exists. While the pool sleeps nothing else runs; when a connection
// waits on the socket, php-cgi is started with the socket as its standard
// input and serves it from the backlog, so the request that wakes the pool
// waits instead of failing. When no PHP request has been logged for the idle
// time, php-cgi is stopped and the socket stays.
//
// php.ini is written by FloServer for each pool: development settings, the
// base extensions every project gets, and the series' and project's own
// extensions and values (php_settings.h).

#include "floserver/core/child_process.h"
#include "floserver/core/php_settings.h"
#include "floserver/core/site_plan.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace floserver::core {

// The php-cgi program in a PHP install, or nothing when it has none.
[[nodiscard]] std::optional<std::filesystem::path> php_cgi_program(const std::filesystem::path& php_dir);

// The extensions in an ext folder: "gd" for php_gd.dll, sorted.
[[nodiscard]] std::vector<std::string> php_extensions_in(const std::filesystem::path& ext_dir);

// The extensions a PHP install can turn on: on Windows, its ext/php_*.dll.
// Nothing elsewhere: there they come with the build and can't be changed.
[[nodiscard]] std::optional<std::vector<std::string>> available_php_extensions(const std::filesystem::path& php_dir);

struct PhpIniResult {
    std::string text;
    std::vector<std::string> missing;  // listed extensions the install doesn't have
};

// php.ini for a pool (php_settings.h): FloServer's defaults, then the
// series' values, its custom.ini and the project's values, so later ones
// win. On Windows an extension is php_<name>.dll in ext/; one turned on
// that's missing is reported, a missing base one is skipped. Elsewhere
// extensions come with the build and aren't named. Reads no files beyond
// checking ext/ on Windows: the caller reads custom.ini.
[[nodiscard]] PhpIniResult make_php_ini(const std::filesystem::path& php_dir, const PhpIniSettings& settings,
                                        const std::filesystem::path& error_log);

enum class PoolState : std::uint8_t { Sleeping, Running, Failed };

class PhpPoolRunner {
public:
    using Clock = std::chrono::steady_clock;

    // Opens the pool's socket and writes its php.ini under `work_dir`/pools.
    // Nothing (with `error`) when the port is taken or files can't be written.
    [[nodiscard]] static std::unique_ptr<PhpPoolRunner> open(const PhpPool& pool, const std::filesystem::path& work_dir,
                                                             std::string& error);
    PhpPoolRunner(const PhpPoolRunner&) = delete;
    PhpPoolRunner& operator=(const PhpPoolRunner&) = delete;
    PhpPoolRunner(PhpPoolRunner&&) = delete;
    PhpPoolRunner& operator=(PhpPoolRunner&&) = delete;
    ~PhpPoolRunner() = default;

    [[nodiscard]] const PhpPool& pool() const { return pool_; }
    void set_pool(const PhpPool& pool) { pool_ = pool; }  // same key: sites and Keep running may change
    // One socket per worker, in the order of pool().ports.
    [[nodiscard]] std::vector<const LoopbackListener*> listeners() const;
    [[nodiscard]] const std::filesystem::path& access_log() const { return access_log_; }
    [[nodiscard]] PoolState state() const { return state_; }
    [[nodiscard]] std::optional<std::uint32_t> pid() const;  // the first worker's
    [[nodiscard]] std::vector<std::uint32_t> pids() const;
    [[nodiscard]] const std::string& last_error() const { return last_error_; }
    [[nodiscard]] std::optional<Clock::time_point> last_request() const { return last_request_; }

    // Starts php-cgi (on a waiting connection, or for Keep running).
    // A pool whose php-cgi keeps ending at once is marked Failed and waits
    // a moment before it's tried again.
    void wake(Clock::time_point now);
    // Stops php-cgi; the socket stays.
    void sleep();
    // Housekeeping, called about once a second: notices requests (the idle
    // log grew), a php-cgi that ended, and the idle time running out.
    void tick(Clock::time_point now, std::chrono::seconds idle_after);
    // True when a connection on the socket should wake it now.
    [[nodiscard]] bool wants_wake_on_connection(Clock::time_point now) const;

private:
    PhpPoolRunner(PhpPool pool, std::vector<std::unique_ptr<LoopbackListener>> listeners, std::filesystem::path ini,
                  std::filesystem::path access_log);

    PhpPool pool_;
    std::vector<std::unique_ptr<LoopbackListener>> listeners_;
    std::filesystem::path ini_;
    std::filesystem::path access_log_;
    struct Worker {
        std::size_t socket = 0;  // index into listeners_
        std::unique_ptr<ChildProcess> process;
        Clock::time_point started;
    };
    [[nodiscard]] bool start_worker(std::size_t socket, Clock::time_point now, std::string& error);
    void fail(Clock::time_point now, std::string message);

    std::vector<Worker> workers_;
    PoolState state_ = PoolState::Sleeping;
    std::string last_error_;
    std::uintmax_t log_size_ = 0;
    std::optional<Clock::time_point> last_request_;
    std::vector<Clock::time_point> quick_exits_;
    Clock::time_point retry_after_;
};

}  // namespace floserver::core

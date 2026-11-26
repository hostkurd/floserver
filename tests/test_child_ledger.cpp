// The child ledger (child_process.h): what a service that crashed left
// running is ended when the next one starts, and nothing else is.

#ifdef __linux__

#include "floserver/core/child_process.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <csignal>
#include <cstdio>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <thread>

using namespace floserver::core;
namespace fs = std::filesystem;

namespace {

bool alive(pid_t pid) { return ::kill(pid, 0) == 0; }

// The ledger setting is global: set for one test, cleared however it ends.
struct LedgerFor {
    explicit LedgerFor(const fs::path& file) { set_child_ledger(file); }
    ~LedgerFor() { set_child_ledger({}); }
    LedgerFor(const LedgerFor&) = delete;
    LedgerFor& operator=(const LedgerFor&) = delete;
    LedgerFor(LedgerFor&&) = delete;
    LedgerFor& operator=(LedgerFor&&) = delete;
};

std::unique_ptr<ChildProcess> start(const fs::path& dir, const char* program, std::vector<std::string> args) {
    ChildOptions o;
    o.program = program;
    o.args = std::move(args);
    o.working_dir = dir;
    std::string error;
    auto child = ChildProcess::start(o, error);
    REQUIRE_MESSAGE(child, error);
    return child;
}

// A sleep that isn't this test's child (as a crashed service's children
// aren't the new service's): the leader of its own session and group.
pid_t orphan_sleep() {
    FILE* p = popen("setsid /bin/sleep 60 >/dev/null 2>&1 < /dev/null & echo $!", "r");
    REQUIRE(p != nullptr);
    long pid = 0;
    REQUIRE(std::fscanf(p, "%ld", &pid) == 1);
    pclose(p);
    for (int i = 0; i < 100 && getpgid(static_cast<pid_t>(pid)) != static_cast<pid_t>(pid); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // setsid() not run yet
    }
    return static_cast<pid_t>(pid);
}

std::string start_time(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    std::getline(in, stat);  // one line
    std::istringstream fields(stat.substr(stat.rfind(')') + 1));
    std::string field;
    for (int i = 3; i <= 22; ++i) {
        fields >> field;
    }
    return field;
}

std::string boot_line() {
    std::ifstream in("/proc/sys/kernel/random/boot_id");
    std::string id;
    std::getline(in, id);
    return "floserver-children 1 boot=" + id + "\n";
}

}  // namespace

TEST_CASE("the child ledger lists running children") {
    if (!fs::exists("/bin/sleep") || !fs::exists("/bin/sh")) {
        MESSAGE("no /bin/sleep here; skipped");
        return;
    }
    const test_util::TempDir dir;
    const fs::path ledger = dir.path() / "state" / "children-test";
    const LedgerFor use(ledger);

    SUBCASE("while they run, with their start time, taken off when stopped") {
        auto a = start(dir.path(), "/bin/sleep", {"60"});
        auto b = start(dir.path(), "/bin/sleep", {"60"});
        const std::string text = test_util::read_file(ledger);
        CHECK(text.starts_with(boot_line()));
        CHECK(text.find(std::to_string(a->pid()) + " " + start_time(static_cast<pid_t>(a->pid())) + " g ") !=
              std::string::npos);
        CHECK(text.find(std::to_string(b->pid()) + " ") != std::string::npos);
        CHECK((fs::status(ledger).permissions() & fs::perms::others_all) == fs::perms::none);
        a->kill();
        const std::string after = test_util::read_file(ledger);
        CHECK(after.find("\n" + std::to_string(a->pid()) + " ") == std::string::npos);
        CHECK(after.find("\n" + std::to_string(b->pid()) + " ") != std::string::npos);
        b.reset();
        CHECK(test_util::read_file(ledger) == boot_line());
    }

    SUBCASE("a child that ended by itself is taken off, and nothing is signalled") {
        auto quick = start(dir.path(), "/bin/sh", {"-c", "exit 3"});
        CHECK(quick->wait(std::chrono::seconds(5)) == 3);
        quick->kill();
        CHECK(test_util::read_file(ledger) == boot_line());
    }
}

TEST_CASE("a sweep ends what a crashed copy left, and only that") {
    if (!fs::exists("/bin/sleep") || !fs::exists("/usr/bin/setsid")) {
        MESSAGE("no sleep or setsid here; skipped");
        return;
    }
    const test_util::TempDir dir;
    const fs::path old = dir.path() / "children-old";

    SUBCASE("the recorded process is ended with TERM, at once") {
        const pid_t left = orphan_sleep();
        test_util::write_file(old, boot_line() + std::to_string(left) + " " + start_time(left) + " g /bin/sleep\n" +
                                       "1 1 g /sbin/init\ngarbage\n");
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(sweep_child_ledger(old) == 1);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));  // TERM did it, no KILL wait
        CHECK_FALSE(alive(left));
        CHECK_FALSE(fs::exists(old));
    }

    SUBCASE("another start time, another program, or another boot: left alone") {
        const pid_t a = orphan_sleep();
        const pid_t b = orphan_sleep();
        const pid_t c = orphan_sleep();
        test_util::write_file(old, boot_line() + std::to_string(a) + " 1 g /bin/sleep\n" + std::to_string(b) + " " +
                                       start_time(b) + " g /bin/true\n");
        CHECK(sweep_child_ledger(old) == 0);
        test_util::write_file(old, "floserver-children 1 boot=another-boot\n" + std::to_string(c) + " " +
                                       start_time(c) + " g /bin/sleep\n");
        CHECK(sweep_child_ledger(old) == 0);
        CHECK(alive(a));
        CHECK(alive(b));
        CHECK(alive(c));
        for (const pid_t p : {a, b, c}) {
            ::kill(p, SIGKILL);
        }
    }

    SUBCASE("a leader that ignores its signal is killed after the grace time") {
        FILE* p = popen("setsid /bin/sh -c 'trap \"\" INT TERM; exec /bin/sleep 60' >/dev/null 2>&1 < /dev/null & echo $!", "r");
        REQUIRE(p != nullptr);
        long pid = 0;
        REQUIRE(std::fscanf(p, "%ld", &pid) == 1);
        pclose(p);
        const auto stubborn = static_cast<pid_t>(pid);
        for (int i = 0; i < 100 && getpgid(stubborn) != stubborn; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // exec'd sleep, with INT/TERM ignored
        test_util::write_file(old, boot_line() + std::to_string(stubborn) + " " + start_time(stubborn) + " i /bin/sleep\n");
        CHECK(sweep_child_ledger(old, std::chrono::milliseconds(300)) == 1);
        CHECK_FALSE(alive(stubborn));
    }

    SUBCASE("no ledger, nothing to sweep") {
        CHECK(sweep_child_ledger(dir.path() / "none") == 0);
    }
}

#endif

// The child ledger (child_process.h): what a service that crashed left
// running is ended when the next one starts, and nothing else is.

#ifndef _WIN32

#include "floserver/core/child_process.h"
#include "test_util.h"

#include <doctest/doctest.h>

#include <csignal>
#include <unistd.h>

#include <fstream>
#include <thread>

using namespace floserver::core;
namespace fs = std::filesystem;

namespace {

bool alive(std::uint32_t pid) { return ::kill(static_cast<pid_t>(pid), 0) == 0; }

std::unique_ptr<ChildProcess> start_sleep(const fs::path& dir) {
    ChildOptions o;
    o.program = "/bin/sleep";
    o.args = {"60"};
    o.working_dir = dir;
    std::string error;
    auto child = ChildProcess::start(o, error);
    REQUIRE_MESSAGE(child, error);
    return child;
}

}  // namespace

TEST_CASE("the child ledger lists running children and ends what an earlier copy left") {
    if (!fs::exists("/bin/sleep")) {
        MESSAGE("no /bin/sleep here; skipped");
        return;
    }
    const test_util::TempDir dir;
    const fs::path ledger = dir.path() / "state" / "children";
    set_child_ledger(ledger);

    SUBCASE("children are listed while they run and taken off when stopped") {
        auto a = start_sleep(dir.path());
        auto b = start_sleep(dir.path());
        const std::string text = test_util::read_file(ledger);
        CHECK(text.find(std::to_string(a->pid()) + " ") != std::string::npos);
        CHECK(text.find(std::to_string(b->pid()) + " ") != std::string::npos);
        a->kill();
        const std::string after = test_util::read_file(ledger);
        CHECK(after.find(std::to_string(a->pid()) + " ") == std::string::npos);
        CHECK(after.find(std::to_string(b->pid()) + " ") != std::string::npos);
        b.reset();
        CHECK(test_util::read_file(ledger).empty());
    }

    SUBCASE("a sweep ends the groups a crashed copy left, and only those") {
        auto left = start_sleep(dir.path());
        auto other = start_sleep(dir.path());
        // What a copy that crashed would have left: its record, with one
        // entry whose pid now runs a different program.
        const fs::path old = dir.path() / "old-children";
        fs::copy_file(ledger, old);
        {
            std::ofstream f(old, std::ios::trunc);
            f << left->pid() << " /bin/sleep\n" << other->pid() << " /bin/true\n" << "1 /sbin/init\n" << "garbage\n";
        }
        CHECK(sweep_child_ledger(old) == 1);
        CHECK_FALSE(fs::exists(old));
        for (int i = 0; i < 100 && left->running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK_FALSE(left->running());
        CHECK(alive(other->pid()));
        CHECK(other->running());
    }

    SUBCASE("no ledger, nothing to sweep") {
        CHECK(sweep_child_ledger(dir.path() / "none") == 0);
    }

    set_child_ledger({});
}

#endif

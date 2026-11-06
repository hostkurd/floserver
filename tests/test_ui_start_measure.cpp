// D15's budgets and FloServer.exe --measure's file (src/ui-win32/model/start_measure.h).

#include "model/start_measure.h"

#include <doctest/doctest.h>

using namespace floserver::ui;

TEST_CASE("measure: --measure takes the file after it, beside --instance") {
    CHECK(parse_measure_args({}) == std::nullopt);
    CHECK(parse_measure_args({"--tray"}) == std::nullopt);
    CHECK(parse_measure_args({"--measure"}) == std::nullopt);  // no file
    CHECK(parse_measure_args({"--measure", "--instance", "ci"}) == std::nullopt);
    CHECK(parse_measure_args({"--measure", R"(C:\t\m.json)"}) == std::string(R"(C:\t\m.json)"));
    CHECK(parse_measure_args({"--instance", "ci", "--measure", "m.json"}) == std::string("m.json"));
}

TEST_CASE("measure: the file's three numbers, written and read back") {
    const StartMeasure m{142, 9437184, 15728640};
    CHECK(measure_text(m) == "{\"first_window_ms\":142,\"private_bytes\":9437184,\"working_set_bytes\":15728640}\n");
    CHECK(parse_measure(measure_text(m)) == m);
    CHECK(parse_measure(measure_text(StartMeasure{})) == StartMeasure{});
    CHECK(parse_measure("") == std::nullopt);
    CHECK(parse_measure("[]") == std::nullopt);
    CHECK(parse_measure(R"({"first_window_ms":1,"private_bytes":2})") == std::nullopt);
    CHECK(parse_measure(R"({"first_window_ms":-1,"private_bytes":2,"working_set_bytes":3})") == std::nullopt);
    CHECK(parse_measure(R"({"first_window_ms":"1","private_bytes":2,"working_set_bytes":3})") == std::nullopt);
}

TEST_CASE("measure: milliseconds between FILETIMEs") {
    CHECK(filetime_ms_between(0, 0) == 0);
    CHECK(filetime_ms_between(1'000'000, 1'000'000 + 2'345'678) == 234);  // 100 ns ticks, rounded down
    CHECK(filetime_ms_between(5'000'000, 1'000'000) == 0);               // a clock that went back
}

TEST_CASE("measure: the budgets, kept up to and including the limit") {
    const StartMeasure kept{kFirstWindowBudgetMs, kWindowBudgetBytes, 3 * kWindowBudgetBytes};
    CHECK(budgets_broken(kept, kServiceBudgetBytes).empty());
    CHECK(budgets_broken(kept, std::nullopt).empty());
    CHECK(kWindowBudgetBytes == 20U * 1024U * 1024U);
    CHECK(kServiceBudgetBytes == 10U * 1024U * 1024U);
    CHECK(kFirstWindowBudgetMs == 300);

    const auto service = budgets_broken(kept, kServiceBudgetBytes + 1);
    REQUIRE(service.size() == 1);
    CHECK(service[0] == "the service uses 10.0 MiB with nothing running, over 10.0 MiB");

    const StartMeasure heavy{kFirstWindowBudgetMs + 1, 21 * kMiB, 0};
    const auto both = budgets_broken(heavy, 4 * kMiB);
    REQUIRE(both.size() == 2);
    CHECK(both[0] == "the window uses 21.0 MiB, over 20.0 MiB");
    CHECK(both[1] == "the first window took 301 ms, over 300 ms");
    CHECK(budgets_broken(heavy, 11 * kMiB).size() == 3);
}

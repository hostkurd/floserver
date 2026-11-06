#include "model/start_measure.h"

#include <nlohmann/json.hpp>

#include <ios>
#include <sstream>

namespace floserver::ui {

namespace {

std::string mib(std::uint64_t bytes) {
    std::ostringstream out;
    out.precision(1);
    out << std::fixed << static_cast<double>(bytes) / static_cast<double>(kMiB) << " MiB";
    return out.str();
}

}  // namespace

std::optional<std::string> parse_measure_args(const std::vector<std::string>& args) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == kMeasureFlag && !args[i + 1].empty() && !args[i + 1].starts_with('-')) {
            return args[i + 1];
        }
    }
    return std::nullopt;
}

std::string measure_text(const StartMeasure& measure) {
    const nlohmann::json out = {{"first_window_ms", measure.first_window_ms},
                                {"private_bytes", measure.private_bytes},
                                {"working_set_bytes", measure.working_set_bytes}};
    return out.dump() + "\n";
}

std::optional<StartMeasure> parse_measure(std::string_view text) {
    const nlohmann::json in = nlohmann::json::parse(text, nullptr, false);
    if (!in.is_object()) {
        return std::nullopt;
    }
    const auto count = [&in](const char* key) -> std::optional<std::uint64_t> {
        const auto it = in.find(key);
        if (it == in.end() || !it->is_number_unsigned()) {
            return std::nullopt;
        }
        return it->get<std::uint64_t>();
    };
    const auto ms = count("first_window_ms");
    const auto private_bytes = count("private_bytes");
    const auto working_set = count("working_set_bytes");
    if (!ms || !private_bytes || !working_set) {
        return std::nullopt;
    }
    return StartMeasure{static_cast<std::int64_t>(*ms), *private_bytes, *working_set};
}

std::int64_t filetime_ms_between(std::uint64_t from, std::uint64_t to) {
    constexpr std::uint64_t kTicksPerMs = 10'000;
    return to > from ? static_cast<std::int64_t>((to - from) / kTicksPerMs) : 0;
}

std::vector<std::string> budgets_broken(const StartMeasure& window, std::optional<std::uint64_t> service_bytes) {
    std::vector<std::string> out;
    if (service_bytes && *service_bytes > kServiceBudgetBytes) {
        out.push_back("the service uses " + mib(*service_bytes) + " with nothing running, over " +
                      mib(kServiceBudgetBytes));
    }
    if (window.private_bytes > kWindowBudgetBytes) {
        out.push_back("the window uses " + mib(window.private_bytes) + ", over " + mib(kWindowBudgetBytes));
    }
    if (window.first_window_ms > kFirstWindowBudgetMs) {
        out.push_back("the first window took " + std::to_string(window.first_window_ms) + " ms, over " +
                      std::to_string(kFirstWindowBudgetMs) + " ms");
    }
    return out;
}

}  // namespace floserver::ui

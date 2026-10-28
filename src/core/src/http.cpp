#include "floserver/core/http.h"

#include <array>
#include <string_view>

namespace floserver::core {

std::optional<std::chrono::sys_seconds> parse_http_date(std::string_view text) {
    // "Wed, 28 Oct 2026 19:12:28 GMT": exactly 29 characters.
    if (text.size() != 29 || text[3] != ',' || text[4] != ' ' || text[7] != ' ' || text[11] != ' ' ||
        text[16] != ' ' || text[19] != ':' || text[22] != ':' || text.substr(25) != " GMT") {
        return std::nullopt;
    }
    const auto number = [&](std::size_t at, std::size_t digits) -> std::optional<int> {
        int value = 0;
        for (std::size_t i = at; i < at + digits; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return std::nullopt;
            }
            value = value * 10 + (text[i] - '0');
        }
        return value;
    };
    constexpr std::array<std::string_view, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    unsigned month = 0;
    for (unsigned i = 0; i < kMonths.size(); ++i) {
        if (text.substr(8, 3) == kMonths[i]) {
            month = i + 1;
        }
    }
    const auto day = number(5, 2);
    const auto year = number(12, 4);
    const auto hour = number(17, 2);
    const auto minute = number(20, 2);
    const auto second = number(23, 2);
    if (month == 0 || !day || !year || !hour || !minute || !second || *hour > 23 || *minute > 59 ||
        *second > 60) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{*year}, std::chrono::month{month},
                                           std::chrono::day{static_cast<unsigned>(*day)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
           std::chrono::seconds{*second};
}

#ifndef _WIN32
std::unique_ptr<HttpTransport> make_system_transport() { return nullptr; }
#endif

}  // namespace floserver::core

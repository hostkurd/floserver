#pragma once

// Runtime version strings: "8", "8.2", "8.2.27". A short version in a project
// file ("8.2") matches any installed version with the same leading parts.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

class Version {
public:
    // One to three dot-separated numbers, each at most 6 digits.
    [[nodiscard]] static std::optional<Version> parse(std::string_view text);

    [[nodiscard]] const std::vector<std::uint32_t>& parts() const { return parts_; }
    [[nodiscard]] std::string to_string() const;

    // True when every part of `this` equals the same part of `installed`:
    // "8.2" matches "8.2.27" but not "8.3.1"; "8.2.27" matches only itself.
    [[nodiscard]] bool matches(const Version& installed) const;

    // Missing parts count as zero: 8.2 == 8.2.0.
    [[nodiscard]] std::strong_ordering operator<=>(const Version& other) const;
    [[nodiscard]] bool operator==(const Version& other) const;

private:
    std::vector<std::uint32_t> parts_;
};

// Picks the newest installed version that `wanted` matches.
[[nodiscard]] std::optional<Version> best_match(const Version& wanted,
                                                const std::vector<Version>& installed);

}  // namespace floserver::core

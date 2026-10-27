#include "floserver/core/version.h"

#include <algorithm>
#include <cstddef>

namespace floserver::core {

std::optional<Version> Version::parse(std::string_view text) {
    constexpr std::size_t kMaxParts = 3;
    constexpr std::size_t kMaxDigits = 6;

    Version v;
    std::size_t i = 0;
    while (true) {
        std::size_t digits = 0;
        std::uint32_t value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (++digits > kMaxDigits) {
                return std::nullopt;
            }
            value = value * 10U + static_cast<std::uint32_t>(text[i] - '0');
            ++i;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        v.parts_.push_back(value);
        if (i == text.size()) {
            break;
        }
        if (text[i] != '.' || v.parts_.size() == kMaxParts) {
            return std::nullopt;
        }
        ++i;
    }
    return v;
}

std::string Version::to_string() const {
    std::string out;
    for (std::size_t i = 0; i < parts_.size(); ++i) {
        if (i != 0) {
            out += '.';
        }
        out += std::to_string(parts_[i]);
    }
    return out;
}

bool Version::matches(const Version& installed) const {
    if (parts_.size() > installed.parts_.size()) {
        return false;
    }
    return std::equal(parts_.begin(), parts_.end(), installed.parts_.begin());
}

std::strong_ordering Version::operator<=>(const Version& other) const {
    const std::size_t n = std::max(parts_.size(), other.parts_.size());
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t a = i < parts_.size() ? parts_[i] : 0U;
        const std::uint32_t b = i < other.parts_.size() ? other.parts_[i] : 0U;
        if (a != b) {
            return a <=> b;
        }
    }
    return std::strong_ordering::equal;
}

bool Version::operator==(const Version& other) const {
    return (*this <=> other) == std::strong_ordering::equal;
}

std::optional<Version> best_match(const Version& wanted, const std::vector<Version>& installed) {
    std::optional<Version> best;
    for (const Version& v : installed) {
        if (wanted.matches(v) && (!best || v > *best)) {
            best = v;
        }
    }
    return best;
}

}  // namespace floserver::core

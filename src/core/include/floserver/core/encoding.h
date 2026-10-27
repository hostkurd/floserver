#pragma once

// Hex and base64 (RFC 4648, standard alphabet, with padding), strict: any
// character outside the alphabet, wrong padding or wrong length fails.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);
// Lower- or upper-case hex, even length.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> from_hex(std::string_view text);

[[nodiscard]] std::string to_base64(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> from_base64(std::string_view text);

}  // namespace floserver::core

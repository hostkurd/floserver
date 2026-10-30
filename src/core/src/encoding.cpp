#include "floserver/core/encoding.h"

#include <array>

namespace floserver::core {

namespace {

constexpr std::string_view kHex = "0123456789abcdef";
constexpr std::string_view kBase64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int base64_value(char c) {
    const auto pos = kBase64.find(c);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

}  // namespace

std::string to_hex(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out += kHex[b >> 4U];
        out += kHex[b & 0x0FU];
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> from_hex(std::string_view text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = hex_value(text[i]);
        const int lo = hex_value(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string to_base64(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8U) | bytes[i + 2];
        out += kBase64[(v >> 18U) & 63U];
        out += kBase64[(v >> 12U) & 63U];
        out += kBase64[(v >> 6U) & 63U];
        out += kBase64[v & 63U];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(bytes[i]) << 16U;
        out += kBase64[(v >> 18U) & 63U];
        out += kBase64[(v >> 12U) & 63U];
        out += "==";
    } else if (rest == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8U);
        out += kBase64[(v >> 18U) & 63U];
        out += kBase64[(v >> 12U) & 63U];
        out += kBase64[(v >> 6U) & 63U];
        out += '=';
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> from_base64(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    // Base64 may carry a key seed: what was decoded before a bad character is
    // wiped, not left in freed memory.
    const auto refuse = [&out]() -> std::optional<std::vector<std::uint8_t>> {
        volatile std::uint8_t* p = out.data();
        for (std::size_t k = 0; k < out.size(); ++k) {
            p[k] = 0;
        }
        return std::nullopt;
    };
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        std::array<int, 4> v{};
        int pad = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=' && last && j >= 2) {
                ++pad;
                v[j] = 0;
                continue;
            }
            if (pad != 0) {
                return refuse();  // data after padding
            }
            v[j] = base64_value(c);
            if (v[j] < 0) {
                return refuse();
            }
        }
        const auto n = (static_cast<std::uint32_t>(v[0]) << 18U) |
                       (static_cast<std::uint32_t>(v[1]) << 12U) |
                       (static_cast<std::uint32_t>(v[2]) << 6U) | static_cast<std::uint32_t>(v[3]);
        // Unused bits must be zero, so every value has one spelling.
        if ((pad == 1 && (n & 0xFFU) != 0) || (pad == 2 && (n & 0xFFFFU) != 0)) {
            return refuse();
        }
        out.push_back(static_cast<std::uint8_t>(n >> 16U));
        if (pad < 2) {
            out.push_back(static_cast<std::uint8_t>(n >> 8U));
        }
        if (pad < 1) {
            out.push_back(static_cast<std::uint8_t>(n));
        }
    }
    return out;
}

}  // namespace floserver::core

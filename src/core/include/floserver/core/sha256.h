#pragma once

// SHA-256 (FIPS 180-4), used to check every downloaded file against the
// manifest (D5). One implementation on every platform, tested against the
// NIST vectors.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace floserver::core {

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256 {
public:
    Sha256();
    void update(std::span<const std::uint8_t> data);
    void update(std::string_view text);
    // Finishes the hash. The object must not be updated afterwards.
    [[nodiscard]] Sha256Digest finish();

private:
    void block(const std::uint8_t* p);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

[[nodiscard]] Sha256Digest sha256(std::string_view data);

}  // namespace floserver::core

#include "floserver/core/crypto.h"

#include <monocypher.h>

#include <algorithm>

namespace floserver::core {

void wipe(std::span<std::uint8_t> bytes) { crypto_wipe(bytes.data(), bytes.size()); }

EcKeyPair::~EcKeyPair() { wipe(d_); }

std::vector<std::uint8_t> EcKeyPair::to_raw() const {
    std::vector<std::uint8_t> out;
    out.reserve(96);
    out.insert(out.end(), d_.begin(), d_.end());
    out.insert(out.end(), pub_.x.begin(), pub_.x.end());
    out.insert(out.end(), pub_.y.begin(), pub_.y.end());
    return out;
}

std::optional<EcKeyPair> EcKeyPair::from_raw(std::span<const std::uint8_t> raw) {
    if (raw.size() != 96) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 32> d{};
    EcPublicKey pub;
    std::copy_n(raw.begin(), 32, d.begin());
    std::copy_n(raw.begin() + 32, 32, pub.x.begin());
    std::copy_n(raw.begin() + 64, 32, pub.y.begin());
    EcKeyPair key(d, pub);
    wipe(d);
    return key;
}

}  // namespace floserver::core

#pragma once

// The user's trust store (D7): where the local CA is added so browsers accept
// the project certificates.
//
// Windows: the current user's "Root" store, no admin rights. Adding or
// removing a root makes Windows show its own security prompt, so these are
// called from the CLI or the window, never from the background service.
// Elsewhere: not done yet (macOS and Linux come in their own stage, D3).

#include <cstdint>
#include <span>
#include <string>

namespace floserver::core {

enum class TrustState : std::uint8_t { Trusted, NotTrusted, Unsupported };

[[nodiscard]] TrustState ca_trust_state(std::span<const std::uint8_t> certificate_der);

// Adds the CA. False with `error` when the user said no or it failed;
// `cancelled` is set when the user said no.
[[nodiscard]] bool trust_ca(std::span<const std::uint8_t> certificate_der, bool& cancelled, std::string& error);

// Removes the CA. True when it's gone (or was never there).
[[nodiscard]] bool untrust_ca(std::span<const std::uint8_t> certificate_der, bool& cancelled, std::string& error);

}  // namespace floserver::core

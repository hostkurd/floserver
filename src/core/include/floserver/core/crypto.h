#pragma once

// The few crypto operations the local certificate authority needs (D7),
// from the operating system's own crypto:
//
//   Windows        CNG (bcrypt) for keys, signatures and random bytes;
//                  DPAPI to protect the CA key with the user's login.
//   Linux, macOS   OpenSSL's libcrypto when the build found it (the Linux
//                  version will use the system's). The CA key file is
//                  protected by its 0600 permissions instead of DPAPI.
//
// Keys are ECDSA P-256 (prime256v1): accepted by every browser, small, fast.
// Private values are wiped from memory when a key is destroyed.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace floserver::core {

// False when this build has no crypto (a POSIX build without OpenSSL); every
// function below then fails with a message saying so.
[[nodiscard]] bool crypto_available();

// Cryptographically secure random bytes from the operating system.
[[nodiscard]] bool random_bytes(std::span<std::uint8_t> out, std::string& error);

struct EcPublicKey {
    std::array<std::uint8_t, 32> x{};
    std::array<std::uint8_t, 32> y{};
    bool operator==(const EcPublicKey&) const = default;
};

// An ECDSA P-256 key pair. The private scalar is wiped on destruction.
class EcKeyPair {
public:
    EcKeyPair() = default;
    EcKeyPair(const std::array<std::uint8_t, 32>& d, const EcPublicKey& pub) : d_(d), pub_(pub) {}
    EcKeyPair(const EcKeyPair&) = default;
    EcKeyPair& operator=(const EcKeyPair&) = default;
    EcKeyPair(EcKeyPair&&) = default;
    EcKeyPair& operator=(EcKeyPair&&) = default;
    ~EcKeyPair();

    [[nodiscard]] const std::array<std::uint8_t, 32>& private_scalar() const { return d_; }
    [[nodiscard]] const EcPublicKey& public_key() const { return pub_; }

    // d || x || y, 96 bytes: how the CA key is kept (inside DPAPI on Windows).
    [[nodiscard]] std::vector<std::uint8_t> to_raw() const;
    [[nodiscard]] static std::optional<EcKeyPair> from_raw(std::span<const std::uint8_t> raw);

private:
    std::array<std::uint8_t, 32> d_{};
    EcPublicKey pub_{};
};

[[nodiscard]] std::optional<EcKeyPair> generate_ec_key(std::string& error);

// ECDSA over SHA-256 of `message`: r || s, 32 bytes each.
[[nodiscard]] std::optional<std::array<std::uint8_t, 64>> ecdsa_sign_sha256(const EcKeyPair& key,
                                                                            std::span<const std::uint8_t> message,
                                                                            std::string& error);

[[nodiscard]] bool ecdsa_verify_sha256(const EcPublicKey& key, std::span<const std::uint8_t> message,
                                       const std::array<std::uint8_t, 64>& signature);

// Seals a secret so only this user on this computer can open it (DPAPI on
// Windows). Elsewhere it returns the bytes unchanged: the caller keeps them
// in an owner-only file.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> protect_secret(std::span<const std::uint8_t> secret,
                                                                      std::string& error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> unprotect_secret(std::span<const std::uint8_t> sealed,
                                                                        std::string& error);

// Overwrites memory so the compiler can't skip it.
void wipe(std::span<std::uint8_t> bytes);

}  // namespace floserver::core

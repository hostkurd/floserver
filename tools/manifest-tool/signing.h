#pragma once

// Signing a runtime list (D5), shared by floserver-manifest and FloServer
// Publisher. The secret key only lives in a SecretKey, which wipes itself.

#include "floserver/core/manifest.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::manifest_tool {

// 1-32 characters: a-z, 0-9 and '-'.
[[nodiscard]] bool is_key_id(std::string_view id);

struct SecretKey {
    std::string id;
    std::array<std::uint8_t, 64> secret{};  // monocypher's seed + public key
    std::array<std::uint8_t, 32> public_key{};
    SecretKey() = default;
    SecretKey(const SecretKey&) = delete;
    SecretKey& operator=(const SecretKey&) = delete;
    SecretKey(SecretKey&&) = delete;
    SecretKey& operator=(SecretKey&&) = delete;
    ~SecretKey();
};

// The key file has two lines: the key id, then the 32-byte seed in base64.
// `text` is wiped before returning, whatever happens.
[[nodiscard]] bool parse_secret_key(std::string& text, SecretKey& key);

struct SignResult {
    std::optional<std::string> signature_file;  // manifest.json.sig, when signed
    std::string error;
    std::vector<std::string> warnings;
    std::int64_t sequence = 0;
    std::size_t runtime_count = 0;
    std::chrono::sys_seconds expires{};
};

// Signs `manifest` and checks the result opens with `key` itself (and with
// `must_open_with` too, when given: the keys built into FloServer, so a list
// signed with the wrong key is never written). Refuses a list that expires
// more than kMaxExpiryDays after `now`.
[[nodiscard]] SignResult sign_manifest(std::string_view manifest, const SecretKey& key,
                                       std::chrono::sys_seconds now,
                                       const std::vector<core::TrustedKey>& must_open_with = {});

}  // namespace floserver::manifest_tool

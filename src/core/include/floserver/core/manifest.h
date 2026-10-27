#pragma once

// The signed runtime manifest (D5).
//
// Two files on our server, mirrored:
//   manifest.json      the list below, as exact bytes
//   manifest.json.sig  {"key": "<key id>", "signature": "<base64, 64 bytes>"}
//
// The signature is Ed25519 (RFC 8032) over the exact bytes of manifest.json,
// checked before anything in it is parsed. Public keys are built into the app;
// several may be trusted at once so a key can be replaced without a gap.
//
// manifest.json, schema 1:
//   {
//     "schema": 1,
//     "sequence": 42,                       // only ever goes up
//     "expires": "2026-12-01T00:00:00Z",    // a stale copy can't be replayed
//     "runtimes": [
//       {
//         "name": "php", "version": "8.4.14", "platform": "windows-x64",
//         "variant": "nts",                 // optional
//         "archive": "zip",                 // zip | tar.gz | tar.xz
//         "size": 31457280, "sha256": "<64 hex>",
//         "license": "PHP-3.01",
//         "urls": ["https://mirror.hostkurd.net/...", "https://windows.php.net/..."]
//       }
//     ]
//   }
//
// Unknown keys are ignored so newer manifests still open in older apps. An
// entry that fails its checks is skipped with a warning, not the whole file.

#include "floserver/core/sha256.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

using Ed25519PublicKey = std::array<std::uint8_t, 32>;

struct TrustedKey {
    std::string id;
    Ed25519PublicKey public_key{};
};

struct RuntimeFile {
    std::string name;      // php, node, mariadb, nginx, apache, composer
    std::string version;   // "8.4.14"
    std::string platform;  // windows-x64, windows-arm64, macos-arm64, linux-x64...
    std::string variant;   // "nts", "ts", or empty
    std::string archive;   // zip, tar.gz, tar.xz
    std::uint64_t size = 0;
    Sha256Digest sha256{};
    std::string license;
    std::vector<std::string> urls;  // https only, in the order to try
};

struct Manifest {
    std::int64_t sequence = 0;
    std::chrono::sys_seconds expires;
    std::vector<RuntimeFile> runtimes;
};

enum class ManifestError : std::uint8_t {
    None,
    BadSignatureFile,  // .sig isn't the expected JSON
    UnknownKey,        // signed with a key this app doesn't trust
    BadSignature,      // the bytes don't match the signature
    NotJson,           // signed, but not readable (our mistake)
    UnsupportedSchema, // newer than this app understands: update FloServer
    BadField,          // schema, sequence or expires missing or wrong
    Expired,           // a stale copy; try another mirror
    Rollback,          // older than one we've already used
};

struct ManifestResult {
    std::optional<Manifest> manifest;
    ManifestError error = ManifestError::None;
    std::string message;                // plain English for logs and the UI
    std::vector<std::string> warnings;  // skipped entries
};

struct ManifestCheck {
    std::vector<TrustedKey> trusted_keys;
    std::chrono::sys_seconds now;
    std::int64_t last_sequence = 0;  // highest sequence accepted before
};

[[nodiscard]] ManifestResult open_manifest(std::string_view manifest_bytes,
                                           std::string_view signature_file,
                                           const ManifestCheck& check);

// Strict RFC 3339 UTC time, "YYYY-MM-DDTHH:MM:SSZ" only.
[[nodiscard]] std::optional<std::chrono::sys_seconds> parse_utc_time(std::string_view text);

// The runtimes for one platform, newest version first.
[[nodiscard]] std::vector<RuntimeFile> runtimes_for(const Manifest& manifest,
                                                    std::string_view name,
                                                    std::string_view platform);

// True when a downloaded file matches its manifest entry.
[[nodiscard]] bool matches_entry(const RuntimeFile& entry, std::uint64_t size,
                                 const Sha256Digest& digest);

inline constexpr int kManifestSchema = 1;

}  // namespace floserver::core

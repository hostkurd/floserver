#pragma once

// Values built into every FloServer release.

#include "floserver/core/manifest.h"

#include <string_view>
#include <vector>

namespace floserver::core {

// Where the signed runtime list lives (D5): Amazon S3 behind CloudFront.
inline constexpr std::string_view kManifestUrl = "https://cdn.floserver.org/manifest.json";
inline constexpr std::string_view kManifestSignatureUrl =
    "https://cdn.floserver.org/manifest.json.sig";

// The public keys a runtime list may be signed with. The secret halves are
// kept offline (docs/manifest.md). To change keys, add the new one here first
// and keep the old one until most users have that release.
[[nodiscard]] std::vector<TrustedKey> builtin_manifest_keys();

}  // namespace floserver::core

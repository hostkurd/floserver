#include "floserver/core/builtin.h"
#include "floserver/core/encoding.h"

#include <doctest/doctest.h>

using namespace floserver::core;

TEST_CASE("the built-in key is exactly the one made offline") {
    // The text floserver-manifest keygen printed on 27 Oct 2026. If this
    // fails, the bytes in builtin.cpp were changed or mistyped.
    const auto keys = builtin_manifest_keys();
    REQUIRE(keys.size() == 1);
    CHECK(keys[0].id == "floserver-2026");
    CHECK(to_base64(keys[0].public_key) == "jwJF3yZAweyJCvFKbZvb/IjJRjlBbnDiEU3NloC38Cc=");
}

TEST_CASE("the manifest is fetched over https from the CDN") {
    CHECK(kManifestUrl == "https://cdn.floserver.org/manifest.json");
    CHECK(kManifestSignatureUrl == std::string(kManifestUrl) + ".sig");
}

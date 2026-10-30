#include "signing.h"

#include "build.h"
#include "floserver/core/encoding.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace floserver::manifest_tool {

using namespace floserver::core;

bool is_key_id(std::string_view id) {
    return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

SecretKey::~SecretKey() { crypto_wipe(secret.data(), secret.size()); }

bool parse_secret_key(std::string& text, SecretKey& key) {
    class Wipe {
    public:
        explicit Wipe(std::string& s) : s_(&s) {}
        Wipe(const Wipe&) = delete;
        Wipe& operator=(const Wipe&) = delete;
        Wipe(Wipe&&) = delete;
        Wipe& operator=(Wipe&&) = delete;
        ~Wipe() {
            crypto_wipe(s_->data(), s_->size());
            s_->clear();
        }

    private:
        std::string* s_;
    };
    const Wipe wipe(text);

    // Notepad may have saved it with \r\n.
    const std::string_view t = text;
    const std::size_t nl = t.find('\n');
    const std::size_t end = nl == std::string_view::npos ? nl : t.find('\n', nl + 1);
    if (nl == std::string_view::npos || end == std::string_view::npos || end + 1 != t.size()) {
        return false;
    }
    std::string_view id = t.substr(0, nl);
    std::string_view seed_text = t.substr(nl + 1, end - nl - 1);
    if (id.ends_with('\r')) {
        id.remove_suffix(1);
    }
    if (seed_text.ends_with('\r')) {
        seed_text.remove_suffix(1);
    }
    auto seed = from_base64(seed_text);
    if (!is_key_id(id) || !seed || seed->size() != 32) {
        if (seed) {
            crypto_wipe(seed->data(), seed->size());
        }
        return false;
    }
    key.id = std::string(id);
    crypto_ed25519_key_pair(key.secret.data(), key.public_key.data(), seed->data());  // wipes seed
    return true;
}

SignResult sign_manifest(std::string_view manifest, const SecretKey& key,
                         std::chrono::sys_seconds now,
                         const std::vector<TrustedKey>& must_open_with) {
    SignResult result;
    std::array<std::uint8_t, 64> signature{};
    crypto_ed25519_sign(signature.data(), key.secret.data(),
                        reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size());
    std::string sig_file =
        nlohmann::json{{"key", key.id}, {"signature", to_base64(signature)}}.dump() + "\n";

    // Check what we're about to publish opens, so a bad file never ships.
    ManifestCheck check;
    check.trusted_keys = {{key.id, key.public_key}};
    check.now = now;
    const auto opened = open_manifest(manifest, sig_file, check);
    if (!opened.manifest) {
        result.error = "Not signed: " + opened.message;
        return result;
    }
    if (opened.manifest->expires - now > std::chrono::days{kMaxExpiryDays}) {
        result.error = "Not signed: \"expires\" is more than " + std::to_string(kMaxExpiryDays) +
                       " days ahead. A stale list must not stay valid for long.";
        return result;
    }
    if (!must_open_with.empty()) {
        check.trusted_keys = must_open_with;
        if (const auto app = open_manifest(manifest, sig_file, check); !app.manifest) {
            result.error = "Not signed: FloServer wouldn't accept this signature (" + app.message +
                           "). Is this the right key file?";
            return result;
        }
    }
    result.warnings = opened.warnings;
    result.sequence = opened.manifest->sequence;
    result.runtime_count = opened.manifest->runtimes.size();
    result.expires = opened.manifest->expires;
    result.signature_file = std::move(sig_file);
    return result;
}

}  // namespace floserver::manifest_tool

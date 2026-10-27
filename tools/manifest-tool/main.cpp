// floserver-manifest: makes the signing key and signs manifest.json (D5).
// For HostKurd's release process, not shipped to users.
//
//   floserver-manifest keygen <secret-key-file> <key-id>
//   floserver-manifest sign   <manifest.json> <secret-key-file>
//   floserver-manifest verify <manifest.json> <public-key-base64> <key-id>
//
// The secret key is only ever read from a file, never taken as an argument,
// and it's wiped from memory after use. Keep the file on an offline machine.

#include "floserver/core/encoding.h"
#include "floserver/core/manifest.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace floserver::core;

bool random_bytes(std::span<std::uint8_t> out) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = getrandom(out.data() + done, out.size() - done, 0);
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
#endif
}

std::optional<std::string> read_file(const fs::path& p) {
    const std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The secret key file: {"key": "<id>", "seed": "<base64 of 32 bytes>"}.
struct SecretKey {
    std::string id;
    std::array<std::uint8_t, 64> secret{};  // monocypher's seed + public key
    std::array<std::uint8_t, 32> public_key{};
    SecretKey() = default;
    SecretKey(const SecretKey&) = delete;
    SecretKey& operator=(const SecretKey&) = delete;
    SecretKey(SecretKey&&) = delete;
    SecretKey& operator=(SecretKey&&) = delete;
    ~SecretKey() { crypto_wipe(secret.data(), secret.size()); }
};

bool load_secret(const fs::path& file, SecretKey& key) {
    auto text = read_file(file);
    if (!text) {
        std::cerr << "Can't read the secret key file.\n";
        return false;
    }
    const auto doc = nlohmann::json::parse(*text, nullptr, false);
    crypto_wipe(text->data(), text->size());
    if (!doc.is_object() || !doc.contains("key") || !doc["key"].is_string() ||
        !doc.contains("seed") || !doc["seed"].is_string()) {
        std::cerr << "The secret key file is damaged.\n";
        return false;
    }
    auto seed = from_base64(doc["seed"].get_ref<const std::string&>());
    if (!seed || seed->size() != 32) {
        std::cerr << "The secret key file is damaged.\n";
        return false;
    }
    key.id = doc["key"].get<std::string>();
    crypto_ed25519_key_pair(key.secret.data(), key.public_key.data(), seed->data());  // wipes seed
    return true;
}

int keygen(const fs::path& file, const std::string& id) {
    if (fs::exists(file)) {
        std::cerr << "Refusing to overwrite an existing key file.\n";
        return 1;
    }
    SecretKey key;
    key.id = id;
    std::array<std::uint8_t, 32> seed{};
    if (!random_bytes(seed)) {
        std::cerr << "The system random number generator failed.\n";
        return 1;
    }
    std::string seed_b64 = to_base64(seed);
    crypto_ed25519_key_pair(key.secret.data(), key.public_key.data(), seed.data());
    // Create the file empty and owner-only first, so the key is never in a
    // file others can read, even for a moment.
    std::ofstream(file, std::ios::binary).close();
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << nlohmann::json{{"key", id}, {"seed", seed_b64}}.dump() << "\n";
        if (!out) {
            std::cerr << "Can't write the key file.\n";
            return 1;
        }
    }
    crypto_wipe(seed_b64.data(), seed_b64.size());
    std::cout << "Key id:     " << id << "\n"
              << "Public key: " << to_base64(key.public_key) << "\n"
              << "Build the public key and id into FloServer. Keep the key file offline.\n";
    return 0;
}

int sign(const fs::path& manifest_file, const fs::path& key_file) {
    const auto bytes = read_file(manifest_file);
    if (!bytes) {
        std::cerr << "Can't read the manifest file.\n";
        return 1;
    }
    SecretKey key;
    if (!load_secret(key_file, key)) {
        return 1;
    }
    std::array<std::uint8_t, 64> signature{};
    crypto_ed25519_sign(signature.data(), key.secret.data(),
                        reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size());

    // Check what we're about to publish opens, so a bad file never ships.
    ManifestCheck check;
    check.trusted_keys = {{key.id, key.public_key}};
    check.now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    const std::string sig_file =
        nlohmann::json{{"key", key.id}, {"signature", to_base64(signature)}}.dump() + "\n";
    const auto result = open_manifest(*bytes, sig_file, check);
    if (!result.manifest) {
        std::cerr << "Not signed: " << result.message << "\n";
        return 1;
    }
    for (const auto& w : result.warnings) {
        std::cerr << "warning: " << w << "\n";
    }
    fs::path out_path = manifest_file;
    out_path += ".sig";
    std::ofstream(out_path, std::ios::binary) << sig_file;
    std::cout << "Signed " << result.manifest->runtimes.size() << " runtimes, sequence "
              << result.manifest->sequence << ".\n";
    return 0;
}

int verify(const fs::path& manifest_file, const std::string& public_b64, const std::string& id) {
    const auto bytes = read_file(manifest_file);
    fs::path sig_path = manifest_file;
    sig_path += ".sig";
    const auto sig = read_file(sig_path);
    const auto pub = from_base64(public_b64);
    if (!bytes || !sig || !pub || pub->size() != 32) {
        std::cerr << "Can't read the manifest, its .sig file, or the public key.\n";
        return 1;
    }
    ManifestCheck check;
    TrustedKey trusted;
    trusted.id = id;
    std::copy(pub->begin(), pub->end(), trusted.public_key.begin());
    check.trusted_keys = {trusted};
    check.now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto result = open_manifest(*bytes, *sig, check);
    for (const auto& w : result.warnings) {
        std::cout << "warning: " << w << "\n";
    }
    if (!result.manifest) {
        std::cout << "FAILED: " << result.message << "\n";
        return 1;
    }
    std::cout << "OK: sequence " << result.manifest->sequence << ", "
              << result.manifest->runtimes.size() << " runtimes\n";
    return 0;
}

int run(const std::vector<std::string>& args) {
    if (args.size() == 3 && args[0] == "keygen") {
        return keygen(args[1], args[2]);
    }
    if (args.size() == 3 && args[0] == "sign") {
        return sign(args[1], args[2]);
    }
    if (args.size() == 4 && args[0] == "verify") {
        return verify(args[1], args[2], args[3]);
    }
    std::cerr << "Usage:\n"
              << "  floserver-manifest keygen <secret-key-file> <key-id>\n"
              << "  floserver-manifest sign   <manifest.json> <secret-key-file>\n"
              << "  floserver-manifest verify <manifest.json> <public-key-base64> <key-id>\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "Unexpected error\n";
    }
    return 1;
}

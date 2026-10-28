// Windows: CNG for keys, ECDSA and random bytes; DPAPI for the CA key.

#ifdef _WIN32

#include "floserver/core/crypto.h"

#include "floserver/core/sha256.h"

#include <windows.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace floserver::core {

namespace {

bool ok(NTSTATUS status) { return status >= 0; }

std::string status_text(const char* what, NTSTATUS status) {
    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "0x%08lX", static_cast<unsigned long>(status));
    return std::string(what) + " failed (" + buf.data() + ")";
}

class Algorithm {
public:
    Algorithm() { status_ = BCryptOpenAlgorithmProvider(&handle_, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0); }
    ~Algorithm() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }
    Algorithm(const Algorithm&) = delete;
    Algorithm& operator=(const Algorithm&) = delete;
    Algorithm(Algorithm&&) = delete;
    Algorithm& operator=(Algorithm&&) = delete;
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const { return handle_; }
    [[nodiscard]] NTSTATUS status() const { return status_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
    NTSTATUS status_ = 0;
};

class Key {
public:
    Key() = default;
    ~Key() {
        if (handle_ != nullptr) {
            BCryptDestroyKey(handle_);
        }
    }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    Key(Key&&) = delete;
    Key& operator=(Key&&) = delete;
    BCRYPT_KEY_HANDLE* out() { return &handle_; }
    [[nodiscard]] BCRYPT_KEY_HANDLE get() const { return handle_; }

private:
    BCRYPT_KEY_HANDLE handle_ = nullptr;
};

constexpr ULONG kCoord = 32;

// BCRYPT_ECCKEY_BLOB header, then X, Y (and d for a private blob).
std::vector<std::uint8_t> make_blob(ULONG magic, const EcPublicKey& pub, const std::array<std::uint8_t, 32>* d) {
    BCRYPT_ECCKEY_BLOB header{magic, kCoord};
    std::vector<std::uint8_t> blob(sizeof header + kCoord * (d != nullptr ? 3 : 2));
    std::memcpy(blob.data(), &header, sizeof header);
    std::copy(pub.x.begin(), pub.x.end(), blob.begin() + sizeof header);
    std::copy(pub.y.begin(), pub.y.end(), blob.begin() + sizeof header + kCoord);
    if (d != nullptr) {
        std::copy(d->begin(), d->end(), blob.begin() + sizeof header + 2 * kCoord);
    }
    return blob;
}

}  // namespace

bool crypto_available() { return true; }

bool random_bytes(std::span<std::uint8_t> out, std::string& error) {
    const NTSTATUS s = BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!ok(s)) {
        error = status_text("BCryptGenRandom", s);
        return false;
    }
    return true;
}

std::optional<EcKeyPair> generate_ec_key(std::string& error) {
    const Algorithm alg;
    if (!ok(alg.status())) {
        error = status_text("BCryptOpenAlgorithmProvider", alg.status());
        return std::nullopt;
    }
    Key key;
    NTSTATUS s = BCryptGenerateKeyPair(alg.get(), key.out(), 256, 0);
    if (ok(s)) {
        s = BCryptFinalizeKeyPair(key.get(), 0);
    }
    if (!ok(s)) {
        error = status_text("BCryptGenerateKeyPair", s);
        return std::nullopt;
    }
    std::vector<std::uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 3 * kCoord);
    ULONG got = 0;
    s = BCryptExportKey(key.get(), nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), static_cast<ULONG>(blob.size()), &got, 0);
    if (!ok(s) || got != blob.size()) {
        wipe(blob);
        error = status_text("BCryptExportKey", s);
        return std::nullopt;
    }
    const std::size_t h = sizeof(BCRYPT_ECCKEY_BLOB);
    EcPublicKey pub;
    std::array<std::uint8_t, 32> d{};
    std::copy_n(blob.begin() + static_cast<std::ptrdiff_t>(h), kCoord, pub.x.begin());
    std::copy_n(blob.begin() + static_cast<std::ptrdiff_t>(h + kCoord), kCoord, pub.y.begin());
    std::copy_n(blob.begin() + static_cast<std::ptrdiff_t>(h + 2 * kCoord), kCoord, d.begin());
    wipe(blob);
    EcKeyPair pair(d, pub);
    wipe(d);
    return pair;
}

std::optional<std::array<std::uint8_t, 64>> ecdsa_sign_sha256(const EcKeyPair& key,
                                                              std::span<const std::uint8_t> message,
                                                              std::string& error) {
    const Algorithm alg;
    if (!ok(alg.status())) {
        error = status_text("BCryptOpenAlgorithmProvider", alg.status());
        return std::nullopt;
    }
    std::vector<std::uint8_t> blob = make_blob(BCRYPT_ECDSA_PRIVATE_P256_MAGIC, key.public_key(), &key.private_scalar());
    Key handle;
    NTSTATUS s = BCryptImportKeyPair(alg.get(), nullptr, BCRYPT_ECCPRIVATE_BLOB, handle.out(), blob.data(),
                                     static_cast<ULONG>(blob.size()), 0);
    wipe(blob);
    if (!ok(s)) {
        error = status_text("BCryptImportKeyPair", s);
        return std::nullopt;
    }
    Sha256 hasher;
    hasher.update(message);
    Sha256Digest digest = hasher.finish();
    std::array<std::uint8_t, 64> sig{};
    ULONG got = 0;
    s = BCryptSignHash(handle.get(), nullptr, digest.data(), static_cast<ULONG>(digest.size()), sig.data(),
                       static_cast<ULONG>(sig.size()), &got, 0);
    if (!ok(s) || got != sig.size()) {
        error = status_text("BCryptSignHash", s);
        return std::nullopt;
    }
    return sig;
}

bool ecdsa_verify_sha256(const EcPublicKey& key, std::span<const std::uint8_t> message,
                         const std::array<std::uint8_t, 64>& signature) {
    const Algorithm alg;
    if (!ok(alg.status())) {
        return false;
    }
    std::vector<std::uint8_t> blob = make_blob(BCRYPT_ECDSA_PUBLIC_P256_MAGIC, key, nullptr);
    Key handle;
    if (!ok(BCryptImportKeyPair(alg.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB, handle.out(), blob.data(),
                                static_cast<ULONG>(blob.size()), 0))) {
        return false;
    }
    Sha256 hasher;
    hasher.update(message);
    Sha256Digest digest = hasher.finish();
    std::array<std::uint8_t, 64> sig = signature;
    return ok(BCryptVerifySignature(handle.get(), nullptr, digest.data(), static_cast<ULONG>(digest.size()),
                                    sig.data(), static_cast<ULONG>(sig.size()), 0));
}

std::optional<std::vector<std::uint8_t>> protect_secret(std::span<const std::uint8_t> secret, std::string& error) {
    DATA_BLOB in{static_cast<DWORD>(secret.size()), const_cast<BYTE*>(secret.data())};  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    DATA_BLOB out{};
    if (CryptProtectData(&in, L"FloServer local CA key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) ==
        FALSE) {
        error = "CryptProtectData failed (" + std::to_string(GetLastError()) + ")";
        return std::nullopt;
    }
    std::vector<std::uint8_t> sealed(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return sealed;
}

std::optional<std::vector<std::uint8_t>> unprotect_secret(std::span<const std::uint8_t> sealed, std::string& error) {
    DATA_BLOB in{static_cast<DWORD>(sealed.size()), const_cast<BYTE*>(sealed.data())};  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    DATA_BLOB out{};
    if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) == FALSE) {
        error = "the CA key can't be opened by this Windows user (CryptUnprotectData " +
                std::to_string(GetLastError()) + ")";
        return std::nullopt;
    }
    std::vector<std::uint8_t> secret(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return secret;
}

}  // namespace floserver::core

#endif  // _WIN32

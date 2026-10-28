// Linux and macOS: OpenSSL's libcrypto (3.x) when the build found it.
// Without it every operation says that crypto isn't available.

#ifndef _WIN32

#include "floserver/core/crypto.h"

#ifdef FLOSERVER_HAVE_OPENSSL
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#endif

#include <algorithm>
#include <memory>

namespace floserver::core {

#ifdef FLOSERVER_HAVE_OPENSSL

namespace {

struct FreePkey {
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct FreePkeyCtx {
    void operator()(EVP_PKEY_CTX* p) const { EVP_PKEY_CTX_free(p); }
};
struct FreeMdCtx {
    void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); }
};
struct FreeBn {
    void operator()(BIGNUM* p) const { BN_clear_free(p); }
};
struct FreeParamBld {
    void operator()(OSSL_PARAM_BLD* p) const { OSSL_PARAM_BLD_free(p); }
};
struct FreeParams {
    void operator()(OSSL_PARAM* p) const { OSSL_PARAM_free(p); }
};
struct FreeSig {
    void operator()(ECDSA_SIG* p) const { ECDSA_SIG_free(p); }
};
using Pkey = std::unique_ptr<EVP_PKEY, FreePkey>;
using Bn = std::unique_ptr<BIGNUM, FreeBn>;

std::array<std::uint8_t, 65> uncompressed(const EcPublicKey& pub) {
    std::array<std::uint8_t, 65> point{};
    point[0] = 0x04;
    std::copy(pub.x.begin(), pub.x.end(), point.begin() + 1);
    std::copy(pub.y.begin(), pub.y.end(), point.begin() + 33);
    return point;
}

// An EVP_PKEY from raw values; `d` null for a public key only.
Pkey make_pkey(const EcPublicKey& pub, const std::array<std::uint8_t, 32>* d) {
    const std::unique_ptr<OSSL_PARAM_BLD, FreeParamBld> bld(OSSL_PARAM_BLD_new());
    if (!bld) {
        return nullptr;
    }
    std::array<std::uint8_t, 65> point = uncompressed(pub);
    Bn priv;
    if (OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) != 1 ||
        OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) != 1) {
        return nullptr;
    }
    if (d != nullptr) {
        priv.reset(BN_bin2bn(d->data(), static_cast<int>(d->size()), nullptr));
        if (!priv || OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PRIV_KEY, priv.get()) != 1) {
            return nullptr;
        }
    }
    const std::unique_ptr<OSSL_PARAM, FreeParams> params(OSSL_PARAM_BLD_to_param(bld.get()));
    const std::unique_ptr<EVP_PKEY_CTX, FreePkeyCtx> ctx(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
    if (!params || !ctx || EVP_PKEY_fromdata_init(ctx.get()) != 1) {
        return nullptr;
    }
    EVP_PKEY* out = nullptr;
    if (EVP_PKEY_fromdata(ctx.get(), &out, d != nullptr ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params.get()) != 1) {
        return nullptr;
    }
    return Pkey(out);
}

}  // namespace

bool crypto_available() { return true; }

bool random_bytes(std::span<std::uint8_t> out, std::string& error) {
    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
        error = "the system's random number generator failed";
        return false;
    }
    return true;
}

std::optional<EcKeyPair> generate_ec_key(std::string& error) {
    const Pkey pkey(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
    if (!pkey) {
        error = "OpenSSL couldn't make a P-256 key";
        return std::nullopt;
    }
    BIGNUM* raw_bn = nullptr;
    std::array<std::uint8_t, 65> point{};
    std::size_t point_len = 0;
    if (EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_PRIV_KEY, &raw_bn) != 1 ||
        EVP_PKEY_get_octet_string_param(pkey.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size(), &point_len) != 1 ||
        point_len != point.size() || point[0] != 0x04) {
        BN_clear_free(raw_bn);
        error = "OpenSSL couldn't export the new key";
        return std::nullopt;
    }
    const Bn bn(raw_bn);
    std::array<std::uint8_t, 32> d{};
    if (BN_bn2binpad(bn.get(), d.data(), static_cast<int>(d.size())) != 32) {
        error = "OpenSSL couldn't export the new key";
        return std::nullopt;
    }
    EcPublicKey pub;
    std::copy_n(point.begin() + 1, 32, pub.x.begin());
    std::copy_n(point.begin() + 33, 32, pub.y.begin());
    EcKeyPair pair(d, pub);
    wipe(d);
    return pair;
}

std::optional<std::array<std::uint8_t, 64>> ecdsa_sign_sha256(const EcKeyPair& key,
                                                              std::span<const std::uint8_t> message,
                                                              std::string& error) {
    const Pkey pkey = make_pkey(key.public_key(), &key.private_scalar());
    const std::unique_ptr<EVP_MD_CTX, FreeMdCtx> md(EVP_MD_CTX_new());
    std::array<std::uint8_t, 80> der{};
    std::size_t der_len = der.size();
    if (!pkey || !md || EVP_DigestSignInit(md.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) != 1 ||
        EVP_DigestSign(md.get(), der.data(), &der_len, message.data(), message.size()) != 1) {
        error = "OpenSSL couldn't sign";
        return std::nullopt;
    }
    const std::uint8_t* p = der.data();
    const std::unique_ptr<ECDSA_SIG, FreeSig> sig(d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der_len)));
    std::array<std::uint8_t, 64> out{};
    if (!sig || BN_bn2binpad(ECDSA_SIG_get0_r(sig.get()), out.data(), 32) != 32 ||
        BN_bn2binpad(ECDSA_SIG_get0_s(sig.get()), out.data() + 32, 32) != 32) {
        error = "OpenSSL gave an unexpected signature";
        return std::nullopt;
    }
    return out;
}

bool ecdsa_verify_sha256(const EcPublicKey& key, std::span<const std::uint8_t> message,
                         const std::array<std::uint8_t, 64>& signature) {
    const Pkey pkey = make_pkey(key, nullptr);
    const std::unique_ptr<ECDSA_SIG, FreeSig> sig(ECDSA_SIG_new());
    BIGNUM* r = BN_bin2bn(signature.data(), 32, nullptr);
    BIGNUM* s = BN_bin2bn(signature.data() + 32, 32, nullptr);
    if (!pkey || !sig || r == nullptr || s == nullptr || ECDSA_SIG_set0(sig.get(), r, s) != 1) {
        BN_free(r);
        BN_free(s);
        return false;
    }
    std::uint8_t* der = nullptr;
    const int der_len = i2d_ECDSA_SIG(sig.get(), &der);
    if (der_len <= 0) {
        return false;
    }
    const std::unique_ptr<EVP_MD_CTX, FreeMdCtx> md(EVP_MD_CTX_new());
    const bool good = md && EVP_DigestVerifyInit(md.get(), nullptr, EVP_sha256(), nullptr, pkey.get()) == 1 &&
                      EVP_DigestVerify(md.get(), der, static_cast<std::size_t>(der_len), message.data(),
                                       message.size()) == 1;
    OPENSSL_free(der);
    return good;
}

#else  // no OpenSSL

namespace {
constexpr const char* kNoCrypto = "this FloServer build has no crypto library for certificates";
}

bool crypto_available() { return false; }

bool random_bytes(std::span<std::uint8_t> /*out*/, std::string& error) {
    error = kNoCrypto;
    return false;
}

std::optional<EcKeyPair> generate_ec_key(std::string& error) {
    error = kNoCrypto;
    return std::nullopt;
}

std::optional<std::array<std::uint8_t, 64>> ecdsa_sign_sha256(const EcKeyPair& /*key*/,
                                                              std::span<const std::uint8_t> /*message*/,
                                                              std::string& error) {
    error = kNoCrypto;
    return std::nullopt;
}

bool ecdsa_verify_sha256(const EcPublicKey& /*key*/, std::span<const std::uint8_t> /*message*/,
                         const std::array<std::uint8_t, 64>& /*signature*/) {
    return false;
}

#endif

// No DPAPI here: the CA key file itself is owner-only (0600).
std::optional<std::vector<std::uint8_t>> protect_secret(std::span<const std::uint8_t> secret, std::string& /*error*/) {
    return std::vector<std::uint8_t>(secret.begin(), secret.end());
}

std::optional<std::vector<std::uint8_t>> unprotect_secret(std::span<const std::uint8_t> sealed,
                                                          std::string& /*error*/) {
    return std::vector<std::uint8_t>(sealed.begin(), sealed.end());
}

}  // namespace floserver::core

#endif  // !_WIN32

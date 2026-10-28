// The local certificate authority (D7): DER pieces, the certificates, and
// verification by the platform's own verifier (OpenSSL on Linux, CryptoAPI
// on Windows) with only our CA trusted: a project name passes, a real
// website's name is refused by the CA's name constraints.

#include "floserver/core/certificates.h"
#include "floserver/core/crypto.h"
#include "floserver/core/der.h"
#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/trust_store.h"
#include "test_util.h"

#include <doctest/doctest.h>

#ifdef _WIN32
#include <windows.h>

#include <wincrypt.h>
#elif defined(FLOSERVER_TEST_OPENSSL)
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#endif

#include <chrono>

using namespace floserver::core;
namespace fs = std::filesystem;
using std::chrono::sys_seconds;

namespace {

std::string hex(const der::Bytes& b) { return to_hex(b); }

sys_seconds at(int y, unsigned m, unsigned d) {
    return sys_seconds{std::chrono::sys_days(std::chrono::year(y) / std::chrono::month(m) / std::chrono::day(d))};
}

// A CA and a certificate it signs for `host`, built by hand so a test can
// also make one for a name our store would never issue.
struct Chain {
    der::Bytes ca;
    der::Bytes leaf;
};

std::optional<Chain> make_chain(const std::string& host, sys_seconds start) {
    std::string error;
    const auto ca_key = generate_ec_key(error);
    const auto leaf_key = generate_ec_key(error);
    if (!ca_key || !leaf_key) {
        return std::nullopt;
    }
    CertificateFields ca;
    ca.kind = CertificateKind::Authority;
    ca.serial = {0x01, 0x02};
    ca.issuer = certificate_name("Test CA", "FloServer");
    ca.subject = ca.issuer;
    ca.not_before = start;
    ca.not_after = start + kCaLifetime;
    ca.subject_key = ca_key->public_key();
    ca.issuer_key = ca_key->public_key();
    CertificateFields leaf;
    leaf.serial = {0x03};
    leaf.issuer = ca.subject;
    leaf.subject = certificate_name(host, "FloServer");
    leaf.not_before = start;
    leaf.not_after = start + kSiteLifetime;
    leaf.subject_key = leaf_key->public_key();
    leaf.issuer_key = ca_key->public_key();
    leaf.dns_names = {host};
    auto ca_der = sign_certificate(ca, *ca_key, error);
    auto leaf_der = sign_certificate(leaf, *ca_key, error);
    if (!ca_der || !leaf_der) {
        return std::nullopt;
    }
    return Chain{std::move(*ca_der), std::move(*leaf_der)};
}

#ifdef _WIN32
// CryptoAPI with only `ca` trusted, as an SSL server certificate for `host`.
// Returns the policy's error code (0 = accepted).
long verify_with_platform(const der::Bytes& ca, const der::Bytes& leaf, const std::string& host, sys_seconds when) {
    HCERTSTORE roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
    REQUIRE(roots != nullptr);
    REQUIRE(CertAddEncodedCertificateToStore(roots, X509_ASN_ENCODING, ca.data(), static_cast<DWORD>(ca.size()),
                                             CERT_STORE_ADD_ALWAYS, nullptr));
    CERT_CHAIN_ENGINE_CONFIG config{};
    config.cbSize = sizeof config;
    config.hExclusiveRoot = roots;
    HCERTCHAINENGINE engine = nullptr;
    REQUIRE(CertCreateCertificateChainEngine(&config, &engine));
    PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, leaf.data(), static_cast<DWORD>(leaf.size()));
    REQUIRE(cert != nullptr);

    const auto ft_ticks = (when.time_since_epoch().count() + 11644473600LL) * 10000000LL;
    FILETIME ft{static_cast<DWORD>(ft_ticks & 0xFFFFFFFF), static_cast<DWORD>(ft_ticks >> 32)};
    CERT_CHAIN_PARA para{};
    para.cbSize = sizeof para;
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    REQUIRE(CertGetCertificateChain(engine, cert, &ft, nullptr, &para, 0, nullptr, &chain));

    std::wstring whost(host.begin(), host.end());
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
    ssl.cbSize = sizeof ssl;
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = whost.data();
    CERT_CHAIN_POLICY_PARA policy{};
    policy.cbSize = sizeof policy;
    policy.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof status;
    REQUIRE(CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status));
    // A name-constraint failure shows in the chain's trust status even when
    // the SSL policy reports the name itself as fine.
    long result = static_cast<long>(status.dwError);
    if (result == 0 && (chain->TrustStatus.dwErrorStatus & ~CERT_TRUST_IS_OFFLINE_REVOCATION &
                        ~CERT_TRUST_REVOCATION_STATUS_UNKNOWN) != 0) {
        result = static_cast<long>(chain->TrustStatus.dwErrorStatus);
    }
    CertFreeCertificateChain(chain);
    CertFreeCertificateContext(cert);
    CertFreeCertificateChainEngine(engine);
    CertCloseStore(roots, 0);
    return result;
}
#elif defined(FLOSERVER_TEST_OPENSSL)
X509* parse(const der::Bytes& d) {
    const unsigned char* p = d.data();
    return d2i_X509(nullptr, &p, static_cast<long>(d.size()));
}

// OpenSSL with only `ca` trusted, checking `host` like a TLS client.
// Returns the verify error (0 = accepted).
long verify_with_platform(const der::Bytes& ca, const der::Bytes& leaf, const std::string& host, sys_seconds when) {
    X509* ca_cert = parse(ca);
    X509* leaf_cert = parse(leaf);
    REQUIRE(ca_cert != nullptr);
    REQUIRE(leaf_cert != nullptr);
    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, ca_cert);
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    REQUIRE(X509_STORE_CTX_init(ctx, store, leaf_cert, nullptr) == 1);
    X509_VERIFY_PARAM* param = X509_STORE_CTX_get0_param(ctx);
    X509_VERIFY_PARAM_set_time(param, static_cast<time_t>(when.time_since_epoch().count()));
    X509_VERIFY_PARAM_set1_host(param, host.c_str(), host.size());
    X509_VERIFY_PARAM_set_purpose(param, X509_PURPOSE_SSL_SERVER);
    X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_X509_STRICT);
    const int ok = X509_verify_cert(ctx);
    const long err = ok == 1 ? 0 : X509_STORE_CTX_get_error(ctx);
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
    X509_free(ca_cert);
    X509_free(leaf_cert);
    return err;
}
#endif

}  // namespace

TEST_CASE("DER pieces are encoded as X.690 says") {
    CHECK(hex(der::small_integer(0)) == "020100");
    CHECK(hex(der::small_integer(127)) == "02017f");
    CHECK(hex(der::small_integer(128)) == "02020080");
    CHECK(hex(der::small_integer(256)) == "02020100");
    const der::Bytes padded = {0x00, 0x00, 0x81};
    CHECK(hex(der::unsigned_integer(padded)) == "02020081");
    CHECK(hex(der::oid("1.2.840.10045.2.1")) == "06072a8648ce3d0201");
    CHECK(hex(der::oid("2.5.29.30")) == "0603551d1e");
    CHECK_THROWS(static_cast<void>(der::oid("1")));
    CHECK_THROWS(static_cast<void>(der::oid("1..2")));
    CHECK(hex(der::boolean(true)) == "0101ff");
    CHECK(hex(der::time(at(2026, 10, 29))) == hex(der::tlv(0x17, der::Bytes{'2', '6', '1', '0', '2', '9', '0', '0', '0', '0', '0', '0', 'Z'})));
    CHECK(der::time(at(2050, 1, 1))[0] == 0x18);  // GeneralizedTime from 2050
    // Long-form lengths.
    const der::Bytes big(300, 0xAB);
    const der::Bytes enc = der::octet_string(big);
    CHECK(hex(der::Bytes(enc.begin(), enc.begin() + 4)) == "0482012c");
    CHECK(enc.size() == 304);
    CHECK(der::pem("CERTIFICATE", big).starts_with("-----BEGIN CERTIFICATE-----\n"));
}

TEST_CASE("the CA allows only localhost names") {
    if (!crypto_available()) {
        MESSAGE("no crypto in this build");
        return;
    }
    const sys_seconds start = at(2026, 10, 28);
    const sys_seconds when = at(2026, 11, 1);
    const auto good = make_chain("shop.localhost", start);
    REQUIRE(good);
#if defined(_WIN32) || defined(FLOSERVER_TEST_OPENSSL)
    CHECK(verify_with_platform(good->ca, good->leaf, "shop.localhost", when) == 0);
    CHECK(verify_with_platform(good->ca, good->leaf, "other.localhost", when) != 0);  // wrong host
    CHECK(verify_with_platform(good->ca, good->leaf, "shop.localhost", at(2028, 1, 1)) != 0);  // expired

    // Even with the CA's own key, a real website's name doesn't verify.
    const auto rogue = make_chain("www.example.com", start);
    REQUIRE(rogue);
    CHECK(verify_with_platform(rogue->ca, rogue->leaf, "www.example.com", when) != 0);
    // A name that only contains "localhost" is outside it too.
    const auto lookalike = make_chain("shop.localhost.example.com", start);
    REQUIRE(lookalike);
    CHECK(verify_with_platform(lookalike->ca, lookalike->leaf, "shop.localhost.example.com", when) != 0);
#else
    MESSAGE("no platform verifier in this build");
#endif
}

TEST_CASE("signatures verify and keys round-trip") {
    if (!crypto_available()) {
        return;
    }
    std::string error;
    const auto key = generate_ec_key(error);
    REQUIRE(key);
    const std::string msg = "FloServer";
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size());
    const auto sig = ecdsa_sign_sha256(*key, bytes, error);
    REQUIRE(sig);
    CHECK(ecdsa_verify_sha256(key->public_key(), bytes, *sig));
    auto bad = *sig;
    bad[10] ^= 1U;
    CHECK_FALSE(ecdsa_verify_sha256(key->public_key(), bytes, bad));

    const auto back = EcKeyPair::from_raw(key->to_raw());
    REQUIRE(back);
    CHECK(back->public_key() == key->public_key());
    CHECK_FALSE(EcKeyPair::from_raw(std::vector<std::uint8_t>(95)));

    const auto sealed = protect_secret(key->to_raw(), error);
    REQUIRE(sealed);
    const auto opened = unprotect_secret(*sealed, error);
    REQUIRE(opened);
    CHECK(*opened == key->to_raw());
#ifdef _WIN32
    CHECK(*sealed != key->to_raw());  // DPAPI really sealed it
    auto broken = *sealed;
    broken[broken.size() / 2] ^= 0xFF;
    CHECK_FALSE(unprotect_secret(broken, error));
#endif
}

TEST_CASE("the store makes the CA once and project certificates on demand") {
    if (!crypto_available()) {
        return;
    }
    const test_util::TempDir data;
    sys_seconds clock = at(2026, 10, 29);
    CertStore store(data.path(), [&clock] { return clock; });
    std::string error;

    CHECK_FALSE(store.load_ca(error));
    CHECK(error.empty());
    const auto ca = store.ensure_ca(error);
    INFO(error);
    REQUIRE(ca);
    CHECK(ca->common_name.starts_with("FloServer local CA "));
    CHECK(ca->not_after == at(2026, 10, 28) + kCaLifetime);
    CHECK(fs::is_regular_file(certs_dir(data.path()) / "ca.pem"));
    CHECK(store.ensure_ca(error)->fingerprint == ca->fingerprint);  // made once
    CHECK(ca_trust_state(ca->der) != TrustState::Trusted);           // never added by the store

#ifndef _WIN32
    const auto perms = fs::status(certs_dir(data.path()) / "ca.key").permissions();
    CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif

    const auto site = store.ensure_site("shop", error);
    INFO(error);
    REQUIRE(site);
    CHECK(site->issued);
    CHECK(site->not_after == at(2026, 10, 28) + kSiteLifetime);
    const std::string pem = test_util::read_file(site->certificate);
    CHECK(pem.find("-----END CERTIFICATE-----\n-----BEGIN CERTIFICATE-----") != std::string::npos);  // leaf + CA
    CHECK(test_util::read_file(site->key).starts_with("-----BEGIN PRIVATE KEY-----\n"));

    // Asked again: the same certificate. Near its end: a new one.
    CHECK_FALSE(store.ensure_site("shop", error)->issued);
    clock = site->not_after - kRenewBefore + std::chrono::days(1);
    const auto renewed = store.ensure_site("shop", error);
    REQUIRE(renewed);
    CHECK(renewed->issued);
    CHECK(test_util::read_file(renewed->certificate) != pem);

    CHECK_FALSE(store.ensure_site("Bad Name", error));
    CHECK_FALSE(store.ensure_site("../ca", error));

#if defined(_WIN32) || defined(FLOSERVER_TEST_OPENSSL)
    // What nginx will serve verifies with only the CA trusted.
    const auto first_cert = [](const std::string& text) {
        const auto b = text.find('\n') + 1;
        const auto e = text.find("-----END CERTIFICATE-----");
        std::string body = text.substr(b, e - b);
        std::erase(body, '\n');
        return *from_base64(body);
    };
    CHECK(verify_with_platform(ca->der, first_cert(test_util::read_file(renewed->certificate)), "shop.localhost",
                               clock) == 0);
#endif
#ifdef FLOSERVER_TEST_OPENSSL
    // The key file is the certificate's key, in a form nginx (OpenSSL) reads.
    BIO* bio = BIO_new_file(path_to_utf8(renewed->key).c_str(), "r");
    REQUIRE(bio != nullptr);
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    REQUIRE(pkey != nullptr);
    const der::Bytes leaf_der = first_cert(test_util::read_file(renewed->certificate));
    const unsigned char* p = leaf_der.data();
    X509* leaf = d2i_X509(nullptr, &p, static_cast<long>(leaf_der.size()));
    REQUIRE(leaf != nullptr);
    CHECK(X509_check_private_key(leaf, pkey) == 1);
    X509_free(leaf);
    EVP_PKEY_free(pkey);
#endif

    SUBCASE("a damaged CA is reported, never replaced silently") {
        test_util::write_file(certs_dir(data.path()) / "ca.pem", "garbage");
        CHECK_FALSE(store.ensure_ca(error));
        CHECK(error.find("damaged") != std::string::npos);
    }
    SUBCASE("a key that doesn't match the CA is refused") {
        std::string e;
        const auto other = generate_ec_key(e);
        REQUIRE(other);
        const auto sealed = protect_secret(other->to_raw(), e);
        REQUIRE(sealed);
        REQUIRE(write_file_atomically(certs_dir(data.path()) / "ca.key",
                                      std::string(sealed->begin(), sealed->end()), e));
        clock += kSiteLifetime;  // force a new certificate
        CHECK_FALSE(store.ensure_site("shop", error));
        CHECK(error.find("doesn't match") != std::string::npos);
    }
    SUBCASE("remove_all clears everything") {
        CHECK(store.remove_all(error));
        CHECK_FALSE(fs::exists(certs_dir(data.path())));
        CHECK_FALSE(store.load_ca(error));
    }
}

#pragma once

// The local certificate authority and the per-project certificates (D7).
//
// The CA is made once per user, on first need. It can sign only local names:
// its Name Constraints extension (critical) permits the DNS name "localhost"
// and its subdomains, and the loopback addresses 127.0.0.0/8 and ::1. So
// even a copy of its key can't make a certificate a browser would accept for
// any real website. It is valid for 10 years; project certificates for 397
// days, renewed when 30 days are left or the CA changed.
//
//   <data>/certs/ca.pem         the CA certificate (added to the user's trust store)
//   <data>/certs/ca.key         the CA key: DPAPI-sealed on Windows, 0600 elsewhere
//   <data>/certs/ca.json        fingerprint, expiry
//   <data>/certs/sites/<name>.pem   the project's certificate, then the CA's
//   <data>/certs/sites/<name>.key   its key (PKCS#8 PEM, for nginx)
//   <data>/certs/sites/<name>.json  which CA issued it, expiry
//
// Adding the CA to the trust store isn't done here (trust_store.h): Windows
// shows a prompt for it, and only the window or the CLI should show prompts.

#include "floserver/core/crypto.h"
#include "floserver/core/der.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core {

enum class CertificateKind : std::uint8_t { Authority, Site };

inline constexpr std::string_view kPurposeServerAuth = "1.3.6.1.5.5.7.3.1";
inline constexpr std::string_view kPurposeClientAuth = "1.3.6.1.5.5.7.3.2";
inline constexpr std::string_view kPurposeCodeSigning = "1.3.6.1.5.5.7.3.3";
inline constexpr std::string_view kPurposeEmail = "1.3.6.1.5.5.7.3.4";

struct CertificateFields {
    CertificateKind kind = CertificateKind::Site;
    std::vector<std::uint8_t> serial;  // 1 to 20 bytes, top bit clear
    der::Bytes issuer;                 // a Name, from certificate_name()
    der::Bytes subject;
    std::chrono::sys_seconds not_before;
    std::chrono::sys_seconds not_after;
    EcPublicKey subject_key;
    EcPublicKey issuer_key;              // for the Authority Key Identifier
    std::vector<std::string> dns_names;  // Site: the subjectAltName entries
    // Site: extended key usages (OIDs). FloServer issues serverAuth only; the
    // tests use others to prove the CA refuses them, as with email_names.
    std::vector<std::string> purposes = {std::string(kPurposeServerAuth)};
    std::vector<std::string> email_names;
};

// A Name with one CN and one O, as UTF8Strings.
[[nodiscard]] der::Bytes certificate_name(std::string_view common_name, std::string_view organisation);

// The to-be-signed part, exactly as it will be signed.
[[nodiscard]] der::Bytes tbs_certificate(const CertificateFields& fields);

// The whole certificate, signed by `issuer` (ecdsa-with-SHA256).
[[nodiscard]] std::optional<der::Bytes> sign_certificate(const CertificateFields& fields, const EcKeyPair& issuer,
                                                         std::string& error);

// The key as PKCS#8 (RFC 5208) DER, as nginx reads it from "PRIVATE KEY" PEM.
[[nodiscard]] der::Bytes pkcs8_private_key(const EcKeyPair& key);

// SHA-256 of the public key point, first 20 bytes (RFC 7093 method 1 style).
[[nodiscard]] std::vector<std::uint8_t> key_identifier(const EcPublicKey& key);

// SHA-256 of the certificate, lower-case hex.
[[nodiscard]] std::string certificate_fingerprint(std::span<const std::uint8_t> certificate_der);

struct CaInfo {
    std::filesystem::path certificate;  // ca.pem
    std::vector<std::uint8_t> der;
    std::string fingerprint;            // SHA-256 hex
    std::string common_name;
    std::chrono::sys_seconds not_after;
};

struct SiteCertificate {
    std::filesystem::path certificate;  // leaf then CA, PEM
    std::filesystem::path key;
    std::chrono::sys_seconds not_after;
    bool issued = false;                 // true when made or renewed by this call
};

inline constexpr std::chrono::days kCaLifetime{3650};
inline constexpr std::chrono::days kSiteLifetime{397};
inline constexpr std::chrono::days kRenewBefore{30};

[[nodiscard]] std::filesystem::path certs_dir(const std::filesystem::path& data);

class CertStore {
public:
    using Clock = std::function<std::chrono::sys_seconds()>;
    explicit CertStore(const std::filesystem::path& data_dir, Clock now = {});

    // The CA as it is on disk, or nothing when there's none yet (or it's
    // damaged, with `error`).
    [[nodiscard]] std::optional<CaInfo> load_ca(std::string& error) const;

    // The CA, made first when there's none. A damaged or expired one is
    // reported (with `error`), never replaced here.
    [[nodiscard]] std::optional<CaInfo> ensure_ca(std::string& error);

    // The certificate for <name>.localhost, issued or renewed when needed.
    // `name` must pass project_name_problem().
    [[nodiscard]] std::optional<SiteCertificate> ensure_site(const std::string& name, std::string& error);

    // Deletes the CA and every project certificate (for "Remove FloServer"
    // and `flo untrust`). The trust store entry is removed separately.
    [[nodiscard]] bool remove_all(std::string& error);

private:
    [[nodiscard]] std::optional<CaInfo> create_ca(std::string& error);
    [[nodiscard]] std::optional<CaInfo> ensure_ca_locked(std::string& error);
    [[nodiscard]] std::optional<EcKeyPair> load_ca_key(std::string& error) const;
    [[nodiscard]] std::chrono::sys_seconds now() const;

    std::filesystem::path dir_;
    Clock now_;
    mutable std::mutex mutex_;
};

}  // namespace floserver::core

#include "floserver/core/certificates.h"

#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/project_registry.h"
#include "floserver/core/sha256.h"

#include <nlohmann/json.hpp>

#include <system_error>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;
using std::chrono::sys_seconds;

namespace {

// OIDs (RFC 5280, RFC 5480, RFC 5758).
constexpr std::string_view kEcPublicKey = "1.2.840.10045.2.1";
constexpr std::string_view kPrime256v1 = "1.2.840.10045.3.1.7";
constexpr std::string_view kEcdsaSha256 = "1.2.840.10045.4.3.2";
constexpr std::string_view kCommonName = "2.5.4.3";
constexpr std::string_view kOrganisation = "2.5.4.10";
constexpr std::string_view kSubjectKeyId = "2.5.29.14";
constexpr std::string_view kKeyUsage = "2.5.29.15";
constexpr std::string_view kSubjectAltName = "2.5.29.17";
constexpr std::string_view kBasicConstraints = "2.5.29.19";
constexpr std::string_view kNameConstraints = "2.5.29.30";
constexpr std::string_view kAuthorityKeyId = "2.5.29.35";
constexpr std::string_view kExtKeyUsage = "2.5.29.37";
constexpr std::string_view kServerAuth = "1.3.6.1.5.5.7.3.1";

constexpr std::size_t kMaxMetaBytes = std::size_t{16} * 1024;
constexpr std::size_t kMaxPemBytes = std::size_t{64} * 1024;
constexpr std::size_t kMaxKeyBytes = std::size_t{8} * 1024;

der::Bytes bytes_of(std::string_view s) { return {s.begin(), s.end()}; }

der::Bytes ec_algorithm() { return der::sequence({der::oid(kEcPublicKey), der::oid(kPrime256v1)}); }

der::Bytes point_of(const EcPublicKey& key) {
    der::Bytes p;
    p.reserve(65);
    p.push_back(0x04);
    p.insert(p.end(), key.x.begin(), key.x.end());
    p.insert(p.end(), key.y.begin(), key.y.end());
    return p;
}

der::Bytes extension(std::string_view id, bool critical, const der::Bytes& value) {
    if (critical) {
        return der::sequence({der::oid(id), der::boolean(true), der::octet_string(value)});
    }
    return der::sequence({der::oid(id), der::octet_string(value)});
}

der::Bytes authority_extensions(const CertificateFields& f) {
    // keyCertSign (bit 5) and cRLSign (bit 6): 0000 0110, one unused bit.
    const der::Bytes usage = {0x06};
    // permittedSubtrees: dNSName "localhost" (covers *.localhost),
    // iPAddress 127.0.0.0/8 and ::1/128. Nothing else may be named.
    const der::Bytes v4 = {127, 0, 0, 0, 255, 0, 0, 0};
    der::Bytes v6(32, 0);
    v6[15] = 1;
    std::fill(v6.begin() + 16, v6.end(), static_cast<std::uint8_t>(0xFF));
    const der::Bytes permitted = der::concat({
        der::sequence({der::context_primitive(2, bytes_of("localhost"))}),
        der::sequence({der::context_primitive(7, v4)}),
        der::sequence({der::context_primitive(7, v6)}),
    });
    return der::sequence({
        extension(kBasicConstraints, true, der::sequence({der::boolean(true), der::small_integer(0)})),
        extension(kKeyUsage, true, der::bit_string(usage, 1)),
        extension(kNameConstraints, true, der::sequence({der::context_constructed(0, permitted)})),
        extension(kSubjectKeyId, false, der::octet_string(key_identifier(f.subject_key))),
    });
}

der::Bytes site_extensions(const CertificateFields& f) {
    // digitalSignature (bit 0): 1000 0000, seven unused bits.
    const der::Bytes usage = {0x80};
    std::vector<der::Bytes> names;
    names.reserve(f.dns_names.size());
    for (const std::string& n : f.dns_names) {
        names.push_back(der::context_primitive(2, bytes_of(n)));
    }
    return der::sequence({
        extension(kBasicConstraints, true, der::sequence({})),
        extension(kKeyUsage, true, der::bit_string(usage, 7)),
        extension(kExtKeyUsage, false, der::sequence({der::oid(kServerAuth)})),
        // Critical when the subject would otherwise say nothing usable; ours
        // has a CN, so it isn't marked critical.
        extension(kSubjectAltName, false, der::sequence_of(names)),
        extension(kSubjectKeyId, false, der::octet_string(key_identifier(f.subject_key))),
        extension(kAuthorityKeyId, false, der::sequence({der::context_primitive(0, key_identifier(f.issuer_key))})),
    });
}

std::int64_t to_unix(sys_seconds t) { return t.time_since_epoch().count(); }
sys_seconds from_unix(std::int64_t s) { return sys_seconds(std::chrono::seconds(s)); }

std::optional<json> read_meta(const fs::path& file) {
    const auto text = read_small_file(file, kMaxMetaBytes);
    if (!text) {
        return std::nullopt;
    }
    json doc = json::parse(*text, nullptr, false);
    if (!doc.is_object() || doc.value("schema", 0) != 1) {
        return std::nullopt;
    }
    return doc;
}

}  // namespace

der::Bytes certificate_name(std::string_view common_name, std::string_view organisation) {
    return der::sequence({
        der::tlv(0x31, der::sequence({der::oid(kOrganisation), der::utf8_string(organisation)})),
        der::tlv(0x31, der::sequence({der::oid(kCommonName), der::utf8_string(common_name)})),
    });
}

std::vector<std::uint8_t> key_identifier(const EcPublicKey& key) {
    Sha256 h;
    h.update(point_of(key));
    const Sha256Digest d = h.finish();
    return {d.begin(), d.begin() + 20};
}

der::Bytes tbs_certificate(const CertificateFields& f) {
    const der::Bytes extensions =
        f.kind == CertificateKind::Authority ? authority_extensions(f) : site_extensions(f);
    return der::sequence({
        der::context_constructed(0, der::small_integer(2)),  // v3
        der::unsigned_integer(f.serial),
        der::sequence({der::oid(kEcdsaSha256)}),
        f.issuer,
        der::sequence({der::time(f.not_before), der::time(f.not_after)}),
        f.subject,
        der::sequence({ec_algorithm(), der::bit_string(point_of(f.subject_key))}),
        der::context_constructed(3, extensions),
    });
}

std::optional<der::Bytes> sign_certificate(const CertificateFields& fields, const EcKeyPair& issuer,
                                           std::string& error) {
    const der::Bytes tbs = tbs_certificate(fields);
    const auto rs = ecdsa_sign_sha256(issuer, tbs, error);
    if (!rs) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> r(rs->data(), 32);
    const std::span<const std::uint8_t> s(rs->data() + 32, 32);
    const der::Bytes signature = der::sequence({der::unsigned_integer(r), der::unsigned_integer(s)});
    return der::sequence({tbs, der::sequence({der::oid(kEcdsaSha256)}), der::bit_string(signature)});
}

der::Bytes pkcs8_private_key(const EcKeyPair& key) {
    der::Bytes d(key.private_scalar().begin(), key.private_scalar().end());
    // ECPrivateKey (RFC 5915) without parameters: the curve is in the
    // PKCS#8 algorithm identifier.
    der::Bytes ec_private = der::sequence({
        der::small_integer(1),
        der::octet_string(d),
        der::context_constructed(1, der::bit_string(point_of(key.public_key()))),
    });
    der::Bytes out = der::sequence({der::small_integer(0), ec_algorithm(), der::octet_string(ec_private)});
    wipe(d);
    wipe(ec_private);
    return out;
}

std::string certificate_fingerprint(std::span<const std::uint8_t> certificate_der) {
    Sha256 h;
    h.update(certificate_der);
    const Sha256Digest d = h.finish();
    return to_hex(d);
}

fs::path certs_dir(const fs::path& data) { return data / "certs"; }

CertStore::CertStore(const fs::path& data_dir, Clock now) : dir_(certs_dir(data_dir)), now_(std::move(now)) {}

sys_seconds CertStore::now() const {
    return now_ ? now_() : std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

std::optional<CaInfo> CertStore::load_ca(std::string& error) const {
    std::error_code ec;
    if (!fs::exists(dir_ / "ca.json", ec)) {
        return std::nullopt;
    }
    const auto meta = read_meta(dir_ / "ca.json");
    const auto pem = read_small_file(dir_ / "ca.pem", kMaxPemBytes);
    if (!meta || !pem || !(*meta)["der"].is_string() || !(*meta)["fingerprint"].is_string() ||
        !(*meta)["not_after"].is_number_integer()) {
        error = "the FloServer certificate authority in " + path_to_utf8(dir_) + " is damaged";
        return std::nullopt;
    }
    auto der_bytes = from_base64((*meta)["der"].get<std::string>());
    if (!der_bytes || certificate_fingerprint(*der_bytes) != (*meta)["fingerprint"].get<std::string>() ||
        der::pem("CERTIFICATE", *der_bytes) != *pem) {
        error = "the FloServer certificate authority in " + path_to_utf8(dir_) + " is damaged";
        return std::nullopt;
    }
    CaInfo info;
    info.certificate = dir_ / "ca.pem";
    info.der = std::move(*der_bytes);
    info.fingerprint = (*meta)["fingerprint"].get<std::string>();
    info.common_name = meta->value("common_name", std::string());
    info.not_after = from_unix((*meta)["not_after"].get<std::int64_t>());
    return info;
}

std::optional<EcKeyPair> CertStore::load_ca_key(std::string& error) const {
    const auto sealed = read_small_file(dir_ / "ca.key", kMaxKeyBytes);
    if (!sealed) {
        error = "the FloServer certificate authority's key is missing";
        return std::nullopt;
    }
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(sealed->data()), sealed->size());
    auto raw = unprotect_secret(bytes, error);
    if (!raw) {
        return std::nullopt;
    }
    auto key = EcKeyPair::from_raw(*raw);
    wipe(*raw);
    if (!key) {
        error = "the FloServer certificate authority's key is damaged";
    }
    return key;
}

std::optional<CaInfo> CertStore::create_ca(std::string& error) {
    auto key = generate_ec_key(error);
    if (!key) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> serial{};
    std::array<std::uint8_t, 4> tag{};
    if (!random_bytes(serial, error) || !random_bytes(tag, error)) {
        return std::nullopt;
    }
    serial[0] = static_cast<std::uint8_t>((serial[0] & 0x7F) | 0x01);
    // A short random tag tells this CA apart from an older one in the
    // certificate manager.
    const std::string cn = "FloServer local CA " + to_hex(tag);
    const sys_seconds start = now() - std::chrono::days(1);  // a slightly slow clock elsewhere still accepts it

    CertificateFields f;
    f.kind = CertificateKind::Authority;
    f.serial.assign(serial.begin(), serial.end());
    f.issuer = certificate_name(cn, "FloServer");
    f.subject = f.issuer;
    f.not_before = start;
    f.not_after = start + kCaLifetime;
    f.subject_key = key->public_key();
    f.issuer_key = key->public_key();
    const auto cert = sign_certificate(f, *key, error);
    if (!cert) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> raw = key->to_raw();
    const auto sealed = protect_secret(raw, error);
    wipe(raw);
    if (!sealed) {
        return std::nullopt;
    }
    const std::string fingerprint = certificate_fingerprint(*cert);
    const json meta = {{"schema", 1},
                       {"fingerprint", fingerprint},
                       {"common_name", cn},
                       {"not_after", to_unix(f.not_after)},
                       {"key_id", to_hex(key_identifier(key->public_key()))},
                       {"der", to_base64(*cert)}};
    // Key first, metadata last: ca.json is what makes a CA count as there.
    std::error_code ec;
    fs::remove(dir_ / "ca.json", ec);
    if (!write_private_file_atomically(dir_ / "ca.key", std::string_view(reinterpret_cast<const char*>(sealed->data()), sealed->size()), error) ||
        !write_file_atomically(dir_ / "ca.pem", der::pem("CERTIFICATE", *cert), error) ||
        !write_file_atomically(dir_ / "ca.json", meta.dump(2) + "\n", error)) {
        return std::nullopt;
    }
    CaInfo info;
    info.certificate = dir_ / "ca.pem";
    info.der = *cert;
    info.fingerprint = fingerprint;
    info.common_name = cn;
    info.not_after = f.not_after;
    return info;
}

std::optional<CaInfo> CertStore::ensure_ca(std::string& error) {
    const std::lock_guard lock(mutex_);
    std::string load_error;
    auto ca = load_ca(load_error);
    if (ca && ca->not_after - now() > kRenewBefore) {
        return ca;
    }
    if (!load_error.empty()) {
        // Never replace a damaged CA silently: the user trusted it, and a new
        // one needs trusting again. Say so instead.
        error = load_error + "; remove it with: flo untrust";
        return std::nullopt;
    }
    return create_ca(error);
}

std::optional<SiteCertificate> CertStore::ensure_site(const std::string& name, std::string& error) {
    if (const auto problem = project_name_problem(name)) {
        error = "can't make a certificate for \"" + name + "\": " + *problem;
        return std::nullopt;
    }
    const auto ca = ensure_ca(error);
    if (!ca) {
        return std::nullopt;
    }
    const std::lock_guard lock(mutex_);
    const fs::path sites = dir_ / "sites";
    const fs::path base = sites / path_from_utf8(name);
    SiteCertificate out;
    out.certificate = fs::path(base) += ".pem";
    out.key = fs::path(base) += ".key";
    const fs::path meta_file = fs::path(base) += ".json";

    std::error_code ec;
    if (const auto meta = read_meta(meta_file);
        meta && meta->value("ca", std::string()) == ca->fingerprint && meta->value("name", std::string()) == name &&
        (*meta)["not_after"].is_number_integer() && fs::is_regular_file(out.certificate, ec) &&
        fs::is_regular_file(out.key, ec)) {
        out.not_after = from_unix((*meta)["not_after"].get<std::int64_t>());
        if (out.not_after - now() > kRenewBefore) {
            return out;
        }
    }

    const auto ca_key = load_ca_key(error);
    if (!ca_key) {
        return std::nullopt;
    }
    // The key must be the one the CA certificate was made for.
    const auto meta_ca = read_meta(dir_ / "ca.json");
    if (!meta_ca || meta_ca->value("key_id", std::string()) != to_hex(key_identifier(ca_key->public_key()))) {
        error = "the FloServer certificate authority's key doesn't match its certificate; remove it with: flo untrust";
        return std::nullopt;
    }
    auto key = generate_ec_key(error);
    if (!key) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> serial{};
    if (!random_bytes(serial, error)) {
        return std::nullopt;
    }
    serial[0] = static_cast<std::uint8_t>((serial[0] & 0x7F) | 0x01);
    const std::string host = name + ".localhost";
    const sys_seconds start = now() - std::chrono::days(1);

    CertificateFields f;
    f.kind = CertificateKind::Site;
    f.serial.assign(serial.begin(), serial.end());
    f.issuer = certificate_name(ca->common_name, "FloServer");
    f.subject = certificate_name(host, "FloServer");
    f.not_before = start;
    f.not_after = std::min(start + kSiteLifetime, ca->not_after);
    f.subject_key = key->public_key();
    f.issuer_key = ca_key->public_key();
    f.dns_names = {host};
    const auto cert = sign_certificate(f, *ca_key, error);
    if (!cert) {
        return std::nullopt;
    }

    der::Bytes pkcs8 = pkcs8_private_key(*key);
    std::string key_pem = der::pem("PRIVATE KEY", pkcs8);
    wipe(pkcs8);
    const json meta = {{"schema", 1}, {"name", name}, {"ca", ca->fingerprint}, {"not_after", to_unix(f.not_after)}};
    fs::remove(meta_file, ec);
    const bool written = write_private_file_atomically(out.key, key_pem, error) &&
                         write_file_atomically(out.certificate,
                                               der::pem("CERTIFICATE", *cert) + der::pem("CERTIFICATE", ca->der),
                                               error) &&
                         write_file_atomically(meta_file, meta.dump(2) + "\n", error);
    wipe(std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(key_pem.data()), key_pem.size()));
    if (!written) {
        return std::nullopt;
    }
    out.not_after = f.not_after;
    out.issued = true;
    return out;
}

bool CertStore::remove_all(std::string& error) {
    const std::lock_guard lock(mutex_);
    std::error_code ec;
    fs::remove_all(dir_, ec);
    if (ec) {
        error = "can't remove " + path_to_utf8(dir_) + ": " + error_text(ec);
        return false;
    }
    return true;
}

}  // namespace floserver::core

#include "floserver/core/trust_store.h"

#ifdef _WIN32
#include <windows.h>

#include <wincrypt.h>

#include <string_view>
#include <vector>
#endif

namespace floserver::core {

#ifdef _WIN32

namespace {

class Store {
public:
    explicit Store(bool read_only) {
        DWORD flags = CERT_SYSTEM_STORE_CURRENT_USER;
        if (read_only) {
            flags |= CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG;
        }
        handle_ = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, flags, L"Root");
    }
    ~Store() {
        if (handle_ != nullptr) {
            CertCloseStore(handle_, 0);
        }
    }
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;
    [[nodiscard]] HCERTSTORE get() const { return handle_; }

private:
    HCERTSTORE handle_ = nullptr;
};

class Context {
public:
    explicit Context(std::span<const std::uint8_t> der)
        : ctx_(CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()))) {}
    explicit Context(PCCERT_CONTEXT owned) : ctx_(owned) {}
    ~Context() {
        if (ctx_ != nullptr) {
            CertFreeCertificateContext(ctx_);
        }
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;
    [[nodiscard]] PCCERT_CONTEXT get() const { return ctx_; }
    PCCERT_CONTEXT release() {
        PCCERT_CONTEXT c = ctx_;
        ctx_ = nullptr;
        return c;
    }

private:
    PCCERT_CONTEXT ctx_ = nullptr;
};

std::string last_error_text(const char* what) { return std::string(what) + " failed (" + std::to_string(GetLastError()) + ")"; }

// Only a FloServer-made CA is ever added: one with critical Name Constraints
// and a server-only key purpose. A tampered ca.pem is refused, not trusted.
bool is_limited_ca(PCCERT_CONTEXT cert) {
    const CERT_INFO* info = cert->pCertInfo;
    const PCERT_EXTENSION nc = CertFindExtension(szOID_NAME_CONSTRAINTS, info->cExtension, info->rgExtension);
    if (nc == nullptr || nc->fCritical == FALSE) {
        return false;
    }
    DWORD size = 0;
    if (CertGetEnhancedKeyUsage(cert, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, nullptr, &size) == FALSE || size == 0) {
        return false;
    }
    std::vector<std::uint8_t> buf(size);
    auto* usage = reinterpret_cast<CERT_ENHKEY_USAGE*>(buf.data());
    if (CertGetEnhancedKeyUsage(cert, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, usage, &size) == FALSE) {
        return false;
    }
    return usage->cUsageIdentifier == 1 && std::string_view(usage->rgpszUsageIdentifier[0]) == szOID_PKIX_KP_SERVER_AUTH;
}

bool was_cancelled() {
    const DWORD e = GetLastError();
    return e == ERROR_CANCELLED || static_cast<HRESULT>(e) == HRESULT_FROM_WIN32(ERROR_CANCELLED);
}

}  // namespace

TrustState ca_trust_state(std::span<const std::uint8_t> certificate_der) {
    const Context cert(certificate_der);
    const Store store(true);
    if (cert.get() == nullptr || store.get() == nullptr) {
        return TrustState::NotTrusted;
    }
    const Context found(CertFindCertificateInStore(store.get(), X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, cert.get(), nullptr));
    return found.get() != nullptr ? TrustState::Trusted : TrustState::NotTrusted;
}

bool trust_ca(std::span<const std::uint8_t> certificate_der, bool& cancelled, std::string& error) {
    cancelled = false;
    const Context cert(certificate_der);
    if (cert.get() == nullptr || !is_limited_ca(cert.get())) {
        error = "this isn't a FloServer certificate authority limited to localhost, so it won't be added";
        return false;
    }
    const Store store(false);
    if (store.get() == nullptr) {
        error = last_error_text("opening your certificate store");
        return false;
    }
    // Windows shows its security prompt here and waits for the answer.
    if (CertAddEncodedCertificateToStore(store.get(), X509_ASN_ENCODING, certificate_der.data(),
                                         static_cast<DWORD>(certificate_der.size()), CERT_STORE_ADD_USE_EXISTING,
                                         nullptr) == FALSE) {
        cancelled = was_cancelled();
        error = cancelled ? "you said no to Windows' prompt" : last_error_text("adding the certificate");
        return false;
    }
    return true;
}

bool untrust_ca(std::span<const std::uint8_t> certificate_der, bool& cancelled, std::string& error) {
    cancelled = false;
    const Context cert(certificate_der);
    const Store store(false);
    if (cert.get() == nullptr || store.get() == nullptr) {
        error = last_error_text("opening your certificate store");
        return false;
    }
    Context found(CertFindCertificateInStore(store.get(), X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, cert.get(), nullptr));
    if (found.get() == nullptr) {
        return true;
    }
    // CertDeleteCertificateFromStore frees the context, also on failure.
    if (CertDeleteCertificateFromStore(found.release()) == FALSE) {
        cancelled = was_cancelled();
        error = cancelled ? "you said no to Windows' prompt" : last_error_text("removing the certificate");
        return false;
    }
    return true;
}

#else

TrustState ca_trust_state(std::span<const std::uint8_t> /*certificate_der*/) { return TrustState::Unsupported; }

bool trust_ca(std::span<const std::uint8_t> /*certificate_der*/, bool& cancelled, std::string& error) {
    cancelled = false;
    error = "adding the certificate authority isn't automatic on this system yet; add ca.pem to your browser or system trust store by hand";
    return false;
}

bool untrust_ca(std::span<const std::uint8_t> /*certificate_der*/, bool& cancelled, std::string& /*error*/) {
    cancelled = false;
    return true;
}

#endif

}  // namespace floserver::core

#include "floserver/core/secret_store.h"

#include "floserver/core/crypto.h"
#include "floserver/core/paths.h"

#include <algorithm>
#include <array>
#include <system_error>

#ifdef _WIN32
#include <windows.h>

#include <wincred.h>
#endif

namespace floserver::core {

namespace fs = std::filesystem;

SecretStore::SecretStore(const fs::path& data_dir) : dir_(data_dir / "secrets") {}

bool SecretStore::valid_name(const std::string& name) {
    if (name.empty() || name.size() > 128 || name.front() == '/' || name.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '/';
    });
}

#ifdef _WIN32

namespace {

std::wstring target(const std::string& name) {
    std::wstring t = L"FloServer:";
    t.append(name.begin(), name.end());  // ASCII by valid_name()
    return t;
}

}  // namespace

bool SecretStore::save(const std::string& name, const std::string& secret, std::string& error) const {
    if (!valid_name(name)) {
        error = "bad secret name";
        return false;
    }
    std::wstring t = target(name);
    std::wstring user = L"FloServer";
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = t.data();
    cred.UserName = user.data();
    cred.CredentialBlobSize = static_cast<DWORD>(secret.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret.data()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;  // this user, this computer
    if (CredWriteW(&cred, 0) == FALSE) {
        error = "Windows Credential Manager refused to save it (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

std::optional<std::string> SecretStore::read(const std::string& name) const {
    if (!valid_name(name)) {
        return std::nullopt;
    }
    const std::wstring t = target(name);
    PCREDENTIALW cred = nullptr;
    if (CredReadW(t.c_str(), CRED_TYPE_GENERIC, 0, &cred) == FALSE) {
        return std::nullopt;
    }
    std::string secret(reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize);
    SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    return secret;
}

bool SecretStore::remove(const std::string& name) const {
    if (!valid_name(name)) {
        return false;
    }
    const std::wstring t = target(name);
    return CredDeleteW(t.c_str(), CRED_TYPE_GENERIC, 0) != FALSE || GetLastError() == ERROR_NOT_FOUND;
}

#else

bool SecretStore::save(const std::string& name, const std::string& secret, std::string& error) const {
    if (!valid_name(name)) {
        error = "bad secret name";
        return false;
    }
    return write_private_file_atomically(dir_ / name, secret, error);
}

std::optional<std::string> SecretStore::read(const std::string& name) const {
    if (!valid_name(name)) {
        return std::nullopt;
    }
    return read_small_file(dir_ / name, 4096);
}

bool SecretStore::remove(const std::string& name) const {
    if (!valid_name(name)) {
        return false;
    }
    std::error_code ec;
    fs::remove(dir_ / name, ec);
    return !ec;
}

#endif

std::optional<std::string> generate_password(std::size_t length) {
    // No 0/O, 1/l/I: easy to read back if someone must type it.
    static constexpr std::string_view kAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    std::string out;
    out.reserve(length);
    std::array<std::uint8_t, 64> buf{};
    std::string error;
    while (out.size() < length) {
        if (!random_bytes(buf, error)) {
            return std::nullopt;
        }
        for (const std::uint8_t b : buf) {
            // Rejection sampling: no bias towards the first letters.
            const std::size_t limit = 256 - (256 % kAlphabet.size());
            if (b < limit && out.size() < length) {
                out += kAlphabet[b % kAlphabet.size()];
            }
        }
    }
    wipe(buf);
    return out;
}

}  // namespace floserver::core

// floserver-manifest: makes the signing key and signs manifest.json (D5).
// For HostKurd's release process, not shipped to users.
//
//   floserver-manifest build  <report.json> --sequence N [--days D] [--accept PATH]... [--out FILE]
//   floserver-manifest keygen <secret-key-file> <key-id>
//   floserver-manifest sign   <manifest.json> <secret-key-file>
//   floserver-manifest verify <manifest.json> [<public-key-base64> <key-id>]
//
// build works on any computer (nothing secret). sign belongs on the offline one.
//
// The secret key is only ever read from a file, never taken as an argument.
// The key file is created owner-only from its first byte (an ACL on Windows,
// mode 0600 elsewhere) and never overwritten. Copies of the key in this
// program's memory are wiped after use, as far as the code controls them.
// Keep the file on an offline machine.

#include "build.h"
#include "floserver/core/builtin.h"
#include "floserver/core/encoding.h"
#include "floserver/core/manifest.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
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
#include <sddl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/random.h>  // getrandom on Linux, getentropy on macOS
#endif

namespace {

namespace fs = std::filesystem;
using namespace floserver::core;

// Wipes a string's characters when it goes out of scope.
class WipedString {
public:
    WipedString() = default;
    explicit WipedString(std::string s) : value_(std::move(s)) {}
    ~WipedString() { crypto_wipe(value_.data(), value_.size()); }
    WipedString(const WipedString&) = delete;
    WipedString& operator=(const WipedString&) = delete;
    WipedString(WipedString&&) = delete;
    WipedString& operator=(WipedString&&) = delete;
    std::string& get() { return value_; }

private:
    std::string value_;
};

bool random_bytes(std::span<std::uint8_t> out) {
#ifdef _WIN32
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#elif defined(__APPLE__)
    constexpr std::size_t kMaxChunk = 256;  // getentropy's limit
    for (std::size_t done = 0; done < out.size(); done += kMaxChunk) {
        if (getentropy(out.data() + done, std::min(kMaxChunk, out.size() - done)) != 0) {
            return false;
        }
    }
    return true;
#else
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = getrandom(out.data() + done, out.size() - done, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
#endif
}

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const std::streamoff size = in.tellg();
    if (size < 0 || size > 64LL * 1024 * 1024) {
        return std::nullopt;
    }
    std::string data(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    if (!in.read(data.data(), size)) {
        return std::nullopt;
    }
    return data;
}

// Creates `file` (it must not exist) readable and writable by its owner only,
// and writes `content`. The file is never readable by others, even briefly.
bool write_new_private_file(const fs::path& file, std::string_view content) {
#ifdef _WIN32
    PSECURITY_DESCRIPTOR sd = nullptr;
    // Protected DACL: full access for the file's owner, nobody else.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)", SDDL_REVISION_1,
                                                              &sd, nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), sd, FALSE};
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    LocalFree(sd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written,
                              nullptr) &&
                    written == content.size() && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
#else
    const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    std::size_t done = 0;
    bool ok = true;
    while (done < content.size()) {
        const ssize_t n = write(fd, content.data() + done, content.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ok = false;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    return ok;
#endif
}

bool is_key_id(std::string_view id) {
    return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

// The secret key file has two lines: the key id, then the 32-byte seed in
// base64. Plain text, so no parser keeps copies of the seed.
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
    auto read = read_file(file);
    if (!read) {
        std::cerr << "Can't read the secret key file.\n";
        return false;
    }
    WipedString text(std::move(*read));
    const std::string& t = text.get();
    const std::size_t nl = t.find('\n');
    const std::size_t end = nl == std::string::npos ? nl : t.find('\n', nl + 1);
    if (nl == std::string::npos || end == std::string::npos || end + 1 != t.size()) {
        std::cerr << "The secret key file is damaged.\n";
        return false;
    }
    const std::string id = t.substr(0, nl);
    auto seed = from_base64(std::string_view(t).substr(nl + 1, end - nl - 1));
    if (!is_key_id(id) || !seed || seed->size() != 32) {
        if (seed) {
            crypto_wipe(seed->data(), seed->size());
        }
        std::cerr << "The secret key file is damaged.\n";
        return false;
    }
    key.id = id;
    crypto_ed25519_key_pair(key.secret.data(), key.public_key.data(), seed->data());  // wipes seed
    return true;
}

int keygen(const fs::path& file, const std::string& id) {
    if (!is_key_id(id)) {
        std::cerr << "A key id is 1-32 characters: a-z, 0-9 and '-', such as floserver-2026.\n";
        return 1;
    }
    SecretKey key;
    key.id = id;
    std::array<std::uint8_t, 32> seed{};
    if (!random_bytes(seed)) {
        std::cerr << "The system random number generator failed.\n";
        return 1;
    }
    WipedString content(id + "\n" + to_base64(seed) + "\n");
    crypto_ed25519_key_pair(key.secret.data(), key.public_key.data(), seed.data());  // wipes seed
    if (!write_new_private_file(file, content.get())) {
        std::cerr << "Can't create the key file (it must not exist yet).\n";
        return 1;
    }
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
    if (result.manifest->expires - check.now >
        std::chrono::days{floserver::manifest_tool::kMaxExpiryDays}) {
        std::cerr << "Not signed: \"expires\" is more than "
                  << floserver::manifest_tool::kMaxExpiryDays
                  << " days ahead. A stale list must not stay valid for long.\n";
        return 1;
    }
    for (const auto& w : result.warnings) {
        std::cerr << "warning: " << w << "\n";
    }
    fs::path out_path = manifest_file;
    out_path += ".sig";
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    out << sig_file;
    out.close();
    if (!out) {
        std::cerr << "Can't write the .sig file.\n";
        return 1;
    }
    std::cout << "Signed " << result.manifest->runtimes.size() << " runtimes, sequence "
              << result.manifest->sequence << ".\n";
    return 0;
}

// With no key given, checks against the keys built into FloServer, which is
// exactly what the app will do.
int verify(const fs::path& manifest_file, const std::string& public_b64, const std::string& id) {
    const auto bytes = read_file(manifest_file);
    fs::path sig_path = manifest_file;
    sig_path += ".sig";
    const auto sig = read_file(sig_path);
    if (!bytes || !sig) {
        std::cerr << "Can't read the manifest or its .sig file.\n";
        return 1;
    }
    ManifestCheck check;
    if (public_b64.empty()) {
        check.trusted_keys = builtin_manifest_keys();
    } else {
        const auto pub = from_base64(public_b64);
        if (!pub || pub->size() != 32) {
            std::cerr << "The public key isn't valid base64 of 32 bytes.\n";
            return 1;
        }
        TrustedKey trusted;
        trusted.id = id;
        std::copy(pub->begin(), pub->end(), trusted.public_key.begin());
        check.trusted_keys = {trusted};
    }
    check.now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto result = open_manifest(*bytes, *sig, check);
    for (const auto& w : result.warnings) {
        std::cout << "warning: " << w << "\n";
    }
    if (!result.manifest) {
        std::cout << "FAILED: " << result.message << "\n";
        return 1;
    }
    std::cout << "OK: signed by " << result.key_id << ", sequence " << result.manifest->sequence
              << ", " << result.manifest->runtimes.size() << " runtimes, expires "
              << floserver::manifest_tool::format_utc_time(result.manifest->expires) << "\n";
    return 0;
}

bool parse_count(const std::string& text, long long low, long long high, long long& out) {
    if (text.empty() || text.size() > 12 ||
        !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    out = std::stoll(text);
    return out >= low && out <= high;
}

int build(const std::vector<std::string>& args) {
    namespace mt = floserver::manifest_tool;
    mt::BuildOptions options;
    options.now = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
    fs::path out = "manifest.json";
    bool have_sequence = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool has_value = i + 1 < args.size();
        long long n = 0;
        if (a == "--sequence" && has_value && parse_count(args[i + 1], 1, 1'000'000'000'000, n)) {
            options.sequence = n;
            have_sequence = true;
            ++i;
        } else if (a == "--days" && has_value && parse_count(args[i + 1], 1, mt::kMaxExpiryDays, n)) {
            options.days = static_cast<int>(n);
            ++i;
        } else if (a == "--accept" && has_value) {
            options.accepted.insert(args[++i]);
        } else if (a == "--cdn" && has_value) {
            options.cdn = args[++i];
            while (options.cdn.ends_with('/')) {
                options.cdn.pop_back();
            }
        } else if (a == "--out" && has_value) {
            out = args[++i];
        } else {
            std::cerr << "Unknown or incomplete option: " << a << "\n";
            return 2;
        }
    }
    if (!have_sequence) {
        std::cerr << "--sequence is required: one more than the last published list (1 for the first).\n";
        return 2;
    }
    const auto report = read_file(args[1]);
    if (!report) {
        std::cerr << "Can't read the report file.\n";
        return 1;
    }
    const auto result = mt::build_manifest(*report, options);
    for (const auto& note : result.notes) {
        std::cout << "note: " << note << "\n";
    }
    for (const auto& e : result.errors) {
        std::cerr << "error: " << e << "\n";
    }
    if (!result.manifest_json) {
        std::cerr << "No manifest written.\n";
        return 1;
    }
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    file << *result.manifest_json;
    file.close();
    if (!file) {
        std::cerr << "Can't write the manifest file.\n";
        return 1;
    }
    std::cout << "Wrote " << result.runtime_count << " runtimes, sequence " << options.sequence
              << ", expires " << mt::format_utc_time(options.now + std::chrono::days{options.days})
              << ".\nNext: sign it on the offline computer.\n";
    return 0;
}

int run(const std::vector<std::string>& args) {
    if (args.size() >= 2 && args[0] == "build") {
        return build(args);
    }
    if (args.size() == 3 && args[0] == "keygen") {
        return keygen(args[1], args[2]);
    }
    if (args.size() == 3 && args[0] == "sign") {
        return sign(args[1], args[2]);
    }
    if (args.size() == 4 && args[0] == "verify") {
        return verify(args[1], args[2], args[3]);
    }
    if (args.size() == 2 && args[0] == "verify") {
        return verify(args[1], "", "");
    }
    std::cerr << "Usage:\n"
              << "  floserver-manifest build  <report.json> --sequence N [--days D] [--accept PATH]... [--out FILE]\n"
              << "  floserver-manifest keygen <secret-key-file> <key-id>\n"
              << "  floserver-manifest sign   <manifest.json> <secret-key-file>\n"
              << "  floserver-manifest verify <manifest.json> [<public-key-base64> <key-id>]\n";
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

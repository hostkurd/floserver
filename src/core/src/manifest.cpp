#include "floserver/core/manifest.h"

#include "floserver/core/encoding.h"
#include "floserver/core/version.h"

#include <monocypher-ed25519.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>

namespace floserver::core {

namespace {

using nlohmann::json;
using namespace std::chrono;

constexpr std::uint64_t kMaxFileSize = 4ULL * 1024 * 1024 * 1024;  // 4 GiB
constexpr std::size_t kMaxNameLength = 32;
constexpr std::size_t kMaxUrls = 8;
constexpr std::size_t kMaxUrlLength = 2048;

ManifestResult fail(ManifestError error, std::string message) {
    ManifestResult r;
    r.error = error;
    r.message = std::move(message);
    return r;
}

bool is_name(std::string_view s) {
    return !s.empty() && s.size() <= kMaxNameLength &&
           std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
           });
}

// https://host[:port]/path with a plain lower-case host name, no user info,
// no fragment, no NUL. The SHA-256 is what makes a download safe; this keeps
// the downloader's input simple and predictable.
bool is_https_url(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    if (url.size() > kMaxUrlLength || !url.starts_with(kScheme)) {
        return false;
    }
    const std::string_view rest = url.substr(kScheme.size());
    const std::size_t host_end = rest.find_first_of(":/");
    const std::string_view host = rest.substr(0, host_end);
    if (host.empty() || host.front() == '.' || host.back() == '.' || host.front() == '-' ||
        !std::all_of(host.begin(), host.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        })) {
        return false;
    }
    std::string_view path = host_end == std::string_view::npos ? std::string_view{}
                                                                : rest.substr(host_end);
    if (path.starts_with(':')) {
        const std::size_t slash = path.find('/');
        const std::string_view port = path.substr(1, slash == std::string_view::npos ? path.npos : slash - 1);
        unsigned value = 0;
        if (port.empty() || port.size() > 5 ||
            !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }
        for (const char c : port) {
            value = value * 10U + static_cast<unsigned>(c - '0');
        }
        if (value == 0 || value > 65535) {
            return false;
        }
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash);
    }
    if (path.find("%00") != std::string_view::npos) {
        return false;
    }
    // No spaces, quotes, fragments or control characters.
    return std::all_of(path.begin(), path.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u > 0x20 && u < 0x7F && c != '"' && c != '\'' && c != '\\' && c != '<' &&
               c != '>' && c != '`' && c != '#' && c != '@';
    });
}

std::optional<std::string> get_string(const json& o, const char* key) {
    const auto it = o.find(key);
    if (it == o.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

// Checks one entry; returns why it's unusable, or nothing when it's fine.
std::optional<std::string> read_entry(const json& e, RuntimeFile& out) {
    if (!e.is_object()) {
        return "is not an object";
    }
    auto name = get_string(e, "name");
    if (!name || !is_name(*name)) {
        return "has no valid name";
    }
    out.name = std::move(*name);
    auto version = get_string(e, "version");
    if (!version || !Version::parse(*version)) {
        return "has no valid version";
    }
    out.version = std::move(*version);
    auto platform = get_string(e, "platform");
    if (!platform || !is_name(*platform)) {
        return "has no valid platform";
    }
    out.platform = std::move(*platform);
    if (e.contains("variant")) {
        auto variant = get_string(e, "variant");
        if (!variant || !is_name(*variant)) {
            return "has an invalid variant";
        }
        out.variant = std::move(*variant);
    }
    auto archive = get_string(e, "archive");
    if (!archive || (*archive != "zip" && *archive != "tar.gz" && *archive != "tar.xz")) {
        return "has no valid archive type";
    }
    out.archive = std::move(*archive);

    const auto size = e.find("size");
    if (size == e.end() || !size->is_number_unsigned() || size->get<std::uint64_t>() == 0 ||
        size->get<std::uint64_t>() > kMaxFileSize) {
        return "has no valid size";
    }
    out.size = size->get<std::uint64_t>();

    const auto hash_text = get_string(e, "sha256");
    const auto hash = hash_text && hash_text->size() == 64 ? from_hex(*hash_text) : std::nullopt;
    if (!hash) {
        return "has no valid sha256";
    }
    std::copy(hash->begin(), hash->end(), out.sha256.begin());

    auto license = get_string(e, "license");
    if (!license || license->empty() || license->size() > 64) {
        return "has no licence";
    }
    out.license = std::move(*license);

    const auto urls = e.find("urls");
    if (urls == e.end() || !urls->is_array() || urls->empty() || urls->size() > kMaxUrls) {
        return "has no download URLs";
    }
    for (const json& u : *urls) {
        if (!u.is_string() || !is_https_url(u.get_ref<const std::string&>())) {
            return "has a URL that isn't plain https";
        }
        out.urls.emplace_back(u.get<std::string>());
    }
    return std::nullopt;
}

}  // namespace

std::optional<sys_seconds> parse_utc_time(std::string_view t) {
    // YYYY-MM-DDTHH:MM:SSZ
    if (t.size() != 20 || t[4] != '-' || t[7] != '-' || t[10] != 'T' || t[13] != ':' ||
        t[16] != ':' || t[19] != 'Z') {
        return std::nullopt;
    }
    const auto num = [&](std::size_t pos, std::size_t len) -> int {
        int v = 0;
        for (std::size_t i = pos; i < pos + len; ++i) {
            if (t[i] < '0' || t[i] > '9') {
                return -1;
            }
            v = v * 10 + (t[i] - '0');
        }
        return v;
    };
    const int y = num(0, 4);
    const int mo = num(5, 2);
    const int d = num(8, 2);
    const int h = num(11, 2);
    const int mi = num(14, 2);
    const int s = num(17, 2);
    if (y < 0 || mo < 0 || d < 0 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
        return std::nullopt;
    }
    const year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)},
                             day{static_cast<unsigned>(d)}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return sys_days{ymd} + hours{h} + minutes{mi} + seconds{s};
}

ManifestResult open_manifest(std::string_view manifest_bytes, std::string_view signature_file,
                             const ManifestCheck& check) {
    // 1. The signature file, then the signature over the exact bytes. Nothing
    //    in manifest.json is read before this passes.
    const json sig = json::parse(signature_file, nullptr, /*allow_exceptions=*/false);
    if (!sig.is_object() || !sig.contains("key") || !sig["key"].is_string() ||
        !sig.contains("signature") || !sig["signature"].is_string()) {
        return fail(ManifestError::BadSignatureFile, "the signature file is damaged");
    }
    const auto signature = from_base64(sig["signature"].get_ref<const std::string&>());
    if (!signature || signature->size() != 64) {
        return fail(ManifestError::BadSignatureFile, "the signature file is damaged");
    }
    const auto& key_id = sig["key"].get_ref<const std::string&>();
    const auto key = std::find_if(check.trusted_keys.begin(), check.trusted_keys.end(),
                                  [&](const TrustedKey& k) { return k.id == key_id; });
    if (key == check.trusted_keys.end()) {
        return fail(ManifestError::UnknownKey,
                    "the runtime list is signed with a key this version doesn't know");
    }
    if (crypto_ed25519_check(signature->data(), key->public_key.data(),
                             reinterpret_cast<const std::uint8_t*>(manifest_bytes.data()),
                             manifest_bytes.size()) != 0) {
        return fail(ManifestError::BadSignature,
                    "the runtime list failed its signature check and was ignored");
    }

    // 2. Signed by us: now read it.
    const json doc = json::parse(manifest_bytes, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        return fail(ManifestError::NotJson, "the runtime list can't be read");
    }
    const auto schema = doc.find("schema");
    if (schema == doc.end() || !schema->is_number_integer()) {
        return fail(ManifestError::BadField, "the runtime list has no schema number");
    }
    if ((schema->is_number_unsigned() &&
         schema->get<std::uint64_t>() > static_cast<std::uint64_t>(kManifestSchema)) ||
        schema->get<std::int64_t>() > kManifestSchema) {
        return fail(ManifestError::UnsupportedSchema,
                    "the runtime list needs a newer FloServer; please update");
    }
    if (schema->get<std::int64_t>() < 1) {
        return fail(ManifestError::BadField, "the runtime list has an invalid schema number");
    }
    const auto sequence = doc.find("sequence");
    if (sequence == doc.end() || !sequence->is_number_unsigned() ||
        sequence->get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fail(ManifestError::BadField, "the runtime list has no sequence number");
    }
    const auto expires_text = get_string(doc, "expires");
    const auto expires = expires_text ? parse_utc_time(*expires_text) : std::nullopt;
    if (!expires) {
        return fail(ManifestError::BadField, "the runtime list has no expiry time");
    }

    Manifest m;
    m.sequence = static_cast<std::int64_t>(sequence->get<std::uint64_t>());
    m.expires = *expires;

    // 3. Freshness: a stale or older copy, even correctly signed, is refused.
    if (check.now >= m.expires) {
        // Also what a computer whose clock runs ahead sees from every mirror;
        // the caller says so when all mirrors give Expired.
        return fail(ManifestError::Expired,
                    "the runtime list from this server is out of date");
    }
    const auto previous = check.last_sequences.find(key_id);
    const std::int64_t last_sequence =
        previous == check.last_sequences.end() ? 0 : previous->second;
    if (m.sequence < last_sequence) {
        return fail(ManifestError::Rollback,
                    "this server sent an older runtime list than one already used");
    }
    if (m.sequence - last_sequence > kMaxSequenceJump) {
        return fail(ManifestError::SequenceJump,
                    "the runtime list's sequence number jumps too far ahead and was ignored");
    }

    // 4. Entries.
    ManifestResult result;
    const auto runtimes = doc.find("runtimes");
    if (runtimes != doc.end() && runtimes->is_array()) {
        for (std::size_t i = 0; i < runtimes->size(); ++i) {
            RuntimeFile entry;
            if (auto problem = read_entry((*runtimes)[i], entry)) {
                result.warnings.push_back("runtime entry " + std::to_string(i) + " " + *problem +
                                          " and was skipped");
                continue;
            }
            m.runtimes.push_back(std::move(entry));
        }
    } else {
        result.warnings.emplace_back("the runtime list has no runtimes");
    }
    result.key_id = key_id;
    result.manifest = std::move(m);
    return result;
}

std::vector<RuntimeFile> runtimes_for(const Manifest& manifest, std::string_view name,
                                      std::string_view platform) {
    // Entries from open_manifest always have a valid version; a hand-built
    // Manifest might not, so those sort last instead of crashing.
    std::vector<std::pair<Version, RuntimeFile>> found;
    for (const RuntimeFile& r : manifest.runtimes) {
        if (r.name == name && r.platform == platform) {
            found.emplace_back(Version::parse(r.version).value_or(Version{}), r);
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<RuntimeFile> out;
    out.reserve(found.size());
    for (auto& f : found) {
        out.push_back(std::move(f.second));
    }
    return out;
}

bool matches_entry(const RuntimeFile& entry, std::uint64_t size, const Sha256Digest& digest) {
    // Constant-time compare: not secret here, but no reason to leak timing.
    return size == entry.size &&
           crypto_verify32(digest.data(), entry.sha256.data()) == 0;
}

}  // namespace floserver::core

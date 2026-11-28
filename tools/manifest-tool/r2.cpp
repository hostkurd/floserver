#include "r2.h"

#include "floserver/core/encoding.h"
#include "floserver/core/paths.h"
#include "floserver/core/sha256.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <regex>

namespace floserver::manifest_tool {

namespace {

namespace fs = std::filesystem;
using core::Sha256;
using core::Sha256Digest;

Sha256Digest hmac(std::span<const std::uint8_t> key, std::string_view data) {
    constexpr std::size_t kBlock = 64;
    std::array<std::uint8_t, kBlock> k{};
    if (key.size() > kBlock) {
        Sha256 h;
        h.update(key);
        const Sha256Digest d = h.finish();
        std::copy(d.begin(), d.end(), k.begin());
    } else {
        std::copy(key.begin(), key.end(), k.begin());
    }
    std::array<std::uint8_t, kBlock> ipad{};
    std::array<std::uint8_t, kBlock> opad{};
    for (std::size_t i = 0; i < kBlock; ++i) {
        ipad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36U);
        opad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5cU);
    }
    Sha256 inner;
    inner.update(ipad);
    inner.update(data);
    const Sha256Digest i = inner.finish();
    Sha256 outer;
    outer.update(opad);
    outer.update(i);
    return outer.finish();
}

Sha256Digest hmac(std::string_view key, std::string_view data) {
    return hmac(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(key.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                                              key.size()),
                data);
}

std::string hex_sha256(std::string_view text) { return core::to_hex(core::sha256(text)); }

std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

std::optional<std::string> xml_value(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const auto a = xml.find(open);
    if (a == std::string::npos) {
        return std::nullopt;
    }
    const auto b = xml.find("</" + tag + ">", a);
    if (b == std::string::npos) {
        return std::nullopt;
    }
    return xml.substr(a + open.size(), b - a - open.size());
}

std::string content_type(const std::string& object) {
    const auto ends = [&](std::string_view e) { return object.ends_with(e); };
    if (ends(".zip")) {
        return "application/zip";
    }
    if (ends(".tar.gz") || ends(".tgz")) {
        return "application/gzip";
    }
    if (ends(".json")) {
        return "application/json";
    }
    return "application/octet-stream";
}

// The SHA-256 of a part of a file, read once before it's sent.
std::optional<std::string> file_sha256(const fs::path& file, std::uint64_t offset, std::uint64_t size,
                                       const std::atomic<bool>* cancel) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    in.seekg(static_cast<std::streamoff>(offset));
    Sha256 h;
    std::vector<char> buffer(std::size_t{1024} * 1024);
    std::uint64_t left = size;
    while (left > 0) {
        if (cancel != nullptr && cancel->load()) {
            return std::nullopt;
        }
        const auto n = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), left));
        in.read(buffer.data(), n);
        if (in.gcount() != n) {
            return std::nullopt;
        }
        h.update(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
        left -= static_cast<std::uint64_t>(n);
    }
    return core::to_hex(h.finish());
}

}  // namespace

std::string r2_key_problem(const R2Key& key) {
    static const std::regex account("^[0-9a-f]{32}$");
    static const std::regex id("^[0-9a-f]{32}$");
    static const std::regex secret("^[0-9a-f]{64}$");
    if (!std::regex_match(key.account_id, account)) {
        return "the account ID is 32 characters, 0-9 and a-f (R2's overview page shows it)";
    }
    if (!std::regex_match(key.access_key_id, id)) {
        return "the Access Key ID is 32 characters, 0-9 and a-f (shown once when the API token is made)";
    }
    if (!std::regex_match(key.secret, secret)) {
        return "the Secret Access Key is 64 characters, 0-9 and a-f (shown once when the API token is made)";
    }
    return {};
}

std::string s3_encode(std::string_view text, bool keep_slash) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                           c == '.' || c == '_' || c == '~' || (keep_slash && c == '/');
        if (plain) {
            out += ch;
        } else {
            out += '%';
            out += kHex[c >> 4U];
            out += kHex[c & 0xFU];
        }
    }
    return out;
}

std::string amz_date(std::chrono::sys_seconds when) {
    const auto day = std::chrono::floor<std::chrono::days>(when);
    const std::chrono::year_month_day ymd(day);
    const std::chrono::hh_mm_ss hms(when - day);
    std::array<char, 20> text{};
    (void)std::snprintf(text.data(), text.size(), "%04d%02u%02uT%02d%02d%02dZ", static_cast<int>(ymd.year()),
                        static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                        static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()),
                        static_cast<int>(hms.seconds().count()));
    return text.data();
}

std::string sigv4_authorization(const SigV4Request& r, const std::string& date, const std::string& region,
                                const std::string& service, const std::string& access_key_id,
                                const std::string& secret) {
    std::vector<std::pair<std::string, std::string>> query;
    query.reserve(r.query.size());
    for (const auto& [k, v] : r.query) {
        query.emplace_back(s3_encode(k, false), s3_encode(v, false));
    }
    std::sort(query.begin(), query.end());
    std::string canonical_query;
    for (const auto& [k, v] : query) {
        if (!canonical_query.empty()) {
            canonical_query += '&';
        }
        canonical_query.append(k).append("=").append(v);
    }
    std::string canonical_headers;
    std::string signed_headers;
    for (const auto& [k, v] : r.headers) {  // a std::map: sorted by name
        canonical_headers.append(k).append(":").append(trim(v)).append("\n");
        if (!signed_headers.empty()) {
            signed_headers += ';';
        }
        signed_headers += k;
    }
    std::string canonical = r.method;
    canonical.append("\n").append(r.path.empty() ? "/" : r.path).append("\n").append(canonical_query);
    canonical.append("\n").append(canonical_headers).append("\n").append(signed_headers);
    canonical.append("\n").append(r.payload_sha256);

    const std::string day = date.substr(0, 8);
    std::string scope = day;
    scope.append("/").append(region).append("/").append(service).append("/aws4_request");
    std::string to_sign = "AWS4-HMAC-SHA256\n";
    to_sign.append(date).append("\n").append(scope).append("\n").append(hex_sha256(canonical));

    const Sha256Digest k_date = hmac("AWS4" + secret, day);
    const Sha256Digest k_region = hmac(k_date, region);
    const Sha256Digest k_service = hmac(k_region, service);
    const Sha256Digest k_signing = hmac(k_service, "aws4_request");
    const std::string signature = core::to_hex(hmac(k_signing, to_sign));
    std::string out = "AWS4-HMAC-SHA256 Credential=";
    out.append(access_key_id).append("/").append(scope).append(", SignedHeaders=").append(signed_headers);
    out.append(", Signature=").append(signature);
    return out;
}

std::string s3_error(const core::HttpResponse& response) {
    if (response.status == 0) {
        return response.error.empty() ? "no answer" : response.error;
    }
    std::string out = "HTTP " + std::to_string(response.status);
    const auto code = xml_value(response.body, "Code");
    const auto message = xml_value(response.body, "Message");
    if (code) {
        out += ", " + *code;
    }
    if (message) {
        out += ": " + *message;
    }
    if (response.status == 403 && (!code || *code != "SignatureDoesNotMatch")) {
        out += " (is the key allowed to write to floserver-cdn?)";
    } else if (code && *code == "SignatureDoesNotMatch") {
        out += " (is the Secret Access Key right?)";
    } else if (code && *code == "RequestTimeTooSkewed") {
        out += " (this PC's clock is wrong)";
    }
    return out;
}

R2Bucket::R2Bucket(core::HttpTransport& http, R2Key key, std::string bucket,
                   std::function<std::chrono::sys_seconds()> now)
    : http_(http), key_(std::move(key)), bucket_(std::move(bucket)), now_(std::move(now)) {
    if (!now_) {
        now_ = [] { return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()); };
    }
}

std::string R2Bucket::host() const { return key_.account_id + ".r2.cloudflarestorage.com"; }

core::HttpResponse R2Bucket::call(const Call& c, std::string& error, const std::atomic<bool>* cancel,
                                  const std::function<void(std::uint64_t)>& sent) {
    core::HttpResponse none;
    std::string payload;
    if (c.file != nullptr) {
        const auto h = file_sha256(*c.file, c.offset, c.size, cancel);
        if (!h) {
            error = cancel != nullptr && cancel->load() ? "stopped" : "can't read " + core::path_to_utf8(*c.file);
            return none;
        }
        payload = *h;
    } else {
        payload = hex_sha256(c.text);
    }
    SigV4Request s;
    s.method = c.method;
    s.path = "/" + s3_encode(bucket_, false) + "/" + s3_encode(c.object, true);
    s.query = c.query;
    const std::string date = amz_date(now_());
    s.headers = {{"host", host()}, {"x-amz-content-sha256", payload}, {"x-amz-date", date}};
    if (!c.content_type.empty()) {
        s.headers["content-type"] = c.content_type;
    }
    s.payload_sha256 = payload;

    core::HttpRequest req;
    req.method = c.method;
    std::string query;
    {
        std::vector<std::pair<std::string, std::string>> q;
        q.reserve(c.query.size());
        for (const auto& [k, v] : c.query) {
            q.emplace_back(s3_encode(k, false), s3_encode(v, false));
        }
        std::sort(q.begin(), q.end());
        for (const auto& [k, v] : q) {
            query += (query.empty() ? "?" : "&") + k + (v.empty() && k == "uploads" ? "" : "=" + v);
        }
    }
    req.url = "https://" + host() + s.path + query;
    req.headers = {{"x-amz-content-sha256", payload},
                   {"x-amz-date", date},
                   {"Authorization", sigv4_authorization(s, date, "auto", "s3", key_.access_key_id, key_.secret)}};
    if (!c.content_type.empty()) {
        req.headers.emplace_back("Content-Type", c.content_type);
    }
    std::ifstream in;
    std::uint64_t done = 0;
    if (c.file != nullptr) {
        in.open(*c.file, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(c.offset));
        req.body_size = c.size;
        req.source = [&](std::span<std::uint8_t> out) -> std::size_t {
            in.read(reinterpret_cast<char*>(out.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                    static_cast<std::streamsize>(out.size()));
            const auto n = static_cast<std::size_t>(in.gcount());
            done += n;
            if (sent) {
                sent(done);
            }
            return n;
        };
    } else {
        req.body_size = c.text.size();
        req.source = [&](std::span<std::uint8_t> out) -> std::size_t {
            const std::size_t n = std::min<std::size_t>(out.size(), c.text.size() - static_cast<std::size_t>(done));
            std::copy_n(c.text.begin() + static_cast<std::ptrdiff_t>(done), n, out.begin());
            done += n;
            return n;
        };
    }
    req.max_reply = c.max_reply;
    core::HttpLimits limits;
    limits.max_time = c.max_time;
    limits.cancel = cancel;
    core::HttpResponse r = http_.send(req, limits);
    if (r.status == 0) {
        error = cancel != nullptr && cancel->load() ? "stopped" : s3_error(r);
    }
    return r;
}

std::optional<std::uint64_t> R2Bucket::size_of(const std::string& object, std::string& error,
                                               const std::atomic<bool>* cancel) {
    error.clear();
    Call c;
    c.method = "HEAD";
    c.object = object;
    c.max_time = std::chrono::seconds{30};
    const core::HttpResponse r = call(c, error, cancel);
    if (r.status == 404) {
        return std::nullopt;
    }
    if (r.status != 200) {
        if (error.empty()) {
            error = "R2 couldn't say whether " + object + " is there: " + s3_error(r);
        }
        return std::nullopt;
    }
    const auto length = r.header("Content-Length");
    std::uint64_t size = 0;
    const char* end = length ? length->data() + length->size() : nullptr;
    if (!length || length->empty() || std::from_chars(length->data(), end, size).ptr != end) {
        error = "R2's answer for " + object + " has no size";
        return std::nullopt;
    }
    return size;
}

namespace {
// Worth sending again: no answer, or the server's own trouble.
bool retryable(const core::HttpResponse& r) { return r.status == 0 || r.status >= 500; }
}  // namespace

std::string R2Bucket::check(const std::atomic<bool>* cancel) {
    Call c;
    c.method = "GET";
    c.object = "";  // the bucket itself: "/floserver-cdn/"
    c.query = {{"list-type", "2"}, {"max-keys", "1"}};
    c.max_time = std::chrono::seconds{20};
    std::string error;
    const core::HttpResponse r = call(c, error, cancel);
    if (r.status == 200) {
        return {};
    }
    return error.empty() ? s3_error(r) : error;
}

namespace {

// The five entities S3 listings escape in keys.
std::string xml_unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        static constexpr std::array<std::pair<std::string_view, char>, 5> kEntities = {
            {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}}};
        bool done = false;
        for (const auto& [name, c] : kEntities) {
            if (s.substr(i, name.size()) == name) {
                out += c;
                i += name.size() - 1;
                done = true;
                break;
            }
        }
        if (!done) {
            out += '&';
        }
    }
    return out;
}

}  // namespace

std::optional<std::vector<R2Object>> R2Bucket::list_objects(std::string& error, const std::atomic<bool>* cancel,
                                                             std::size_t max_objects) {
    error.clear();
    std::vector<R2Object> out;
    std::string token;
    for (int page = 0; page < 1000; ++page) {
        Call c;
        c.method = "GET";
        c.object = "";  // the bucket itself
        c.query = {{"list-type", "2"}, {"max-keys", "1000"}};
        if (!token.empty()) {
            c.query.emplace_back("continuation-token", token);
        }
        c.max_time = std::chrono::seconds{60};
        c.max_reply = std::size_t{8} * 1024 * 1024;
        core::HttpResponse r = call(c, error, cancel);
        if (r.status != 200 && retryable(r) && (cancel == nullptr || !cancel->load())) {
            error.clear();
            r = call(c, error, cancel);
        }
        if (r.status != 200) {
            if (error.empty()) {
                error = "R2 couldn't list the bucket: " + s3_error(r);
            }
            return std::nullopt;
        }
        std::size_t at = 0;
        while (true) {
            const auto a = r.body.find("<Contents>", at);
            if (a == std::string::npos) {
                break;
            }
            const auto b = r.body.find("</Contents>", a);
            if (b == std::string::npos) {
                error = "R2's listing was cut short";
                return std::nullopt;
            }
            const std::string item = r.body.substr(a, b - a);
            at = b;
            R2Object o;
            o.key = xml_unescape(xml_value(item, "Key").value_or(""));
            const std::string size = xml_value(item, "Size").value_or("");
            const auto [end, ec] = std::from_chars(size.data(), size.data() + size.size(), o.size);
            if (o.key.empty() || ec != std::errc() || end != size.data() + size.size()) {
                error = "R2's listing has an entry without a key or size";
                return std::nullopt;
            }
            o.last_modified = xml_value(item, "LastModified").value_or("");
            if (out.size() >= max_objects) {
                error = "the bucket holds more than " + std::to_string(max_objects) + " objects";
                return std::nullopt;
            }
            out.push_back(std::move(o));
        }
        if (xml_value(r.body, "IsTruncated").value_or("false") != "true") {
            return out;
        }
        std::string next = xml_unescape(xml_value(r.body, "NextContinuationToken").value_or(""));
        if (next.empty() || next == token) {
            error = "R2 said the listing goes on but gave no new continuation token";
            return std::nullopt;
        }
        token = std::move(next);
    }
    error = "R2's listing didn't end";
    return std::nullopt;
}

bool R2Bucket::delete_object(const std::string& object, std::string& error, const std::atomic<bool>* cancel) {
    error.clear();
    if (object.empty() || object.starts_with('/')) {
        error = "not an object name: " + object;
        return false;
    }
    Call c;
    c.method = "DELETE";
    c.object = object;
    c.max_time = std::chrono::seconds{30};
    core::HttpResponse r = call(c, error, cancel);
    if (retryable(r) && (cancel == nullptr || !cancel->load())) {
        error.clear();
        r = call(c, error, cancel);
    }
    // S3 answers 204 whether it was there or not; 404 counts as gone too,
    // unless it's the bucket that isn't there.
    if (r.status == 204 || r.status == 200 ||
        (r.status == 404 && r.body.find("<Code>NoSuchBucket</Code>") == std::string::npos)) {
        error.clear();
        return true;
    }
    if (error.empty()) {
        error = "R2 didn't delete " + object + ": " + s3_error(r);
    }
    return false;
}

std::optional<std::string> R2Bucket::get_text(const std::string& object, std::size_t max_bytes,
                                               std::string& error, const std::atomic<bool>* cancel) {
    error.clear();
    Call c;
    c.method = "GET";
    c.object = object;
    c.max_time = std::chrono::seconds{30};
    // One byte more than allowed, so a bigger object is seen as too big.
    c.max_reply = max_bytes + 1;
    core::HttpResponse r = call(c, error, cancel);
    if (r.status == 404) {
        error.clear();
        return std::nullopt;
    }
    if (r.status != 200) {
        if (error.empty()) {
            error = "R2 couldn't give " + object + ": " + s3_error(r);
        }
        return std::nullopt;
    }
    if (r.body.size() > max_bytes) {
        error = object + " on R2 is bigger than " + std::to_string(max_bytes) + " bytes";
        return std::nullopt;
    }
    return std::move(r.body);
}

bool R2Bucket::put_text(const std::string& object, const std::string& text, std::string& error) {
    Call c;
    c.method = "PUT";
    c.object = object;
    c.content_type = content_type(object);
    c.text = text;
    c.max_time = std::chrono::seconds{30};
    core::HttpResponse r;
    for (int attempt = 0; attempt < 2; ++attempt) {
        error.clear();
        // Not stoppable: half of a list pair mustn't be left because of a click.
        r = call(c, error, nullptr);
        if (r.status == 200 || !retryable(r)) {
            break;
        }
    }
    if (r.status == 200) {
        return true;
    }
    if (error.empty()) {
        error = "R2 refused " + object + ": " + s3_error(r);
    }
    return false;
}

bool R2Bucket::put_file(const std::string& object, const fs::path& file, std::string& error,
                        const std::atomic<bool>* cancel, const std::function<void(const R2Progress&)>& progress) {
    error.clear();
    std::error_code ec;
    const std::uint64_t total = fs::file_size(file, ec);
    if (ec) {
        error = "can't read " + core::path_to_utf8(file);
        return false;
    }
    const auto tell = [&](std::uint64_t sent) {
        if (progress) {
            progress({sent, total});
        }
    };
    if (total <= multipart_from_) {
        Call c;
        c.method = "PUT";
        c.object = object;
        c.content_type = content_type(object);
        c.file = &file;
        c.size = total;
        c.max_time = std::chrono::hours{1};
        core::HttpResponse r;
        for (int attempt = 0; attempt < 2; ++attempt) {
            error.clear();
            r = call(c, error, cancel, tell);
            if (r.status == 200 || !retryable(r) || (cancel != nullptr && cancel->load())) {
                break;
            }
        }
        if (r.status == 200) {
            return true;
        }
        if (error.empty()) {
            error = "R2 refused " + object + ": " + s3_error(r);
        }
        return false;
    }

    // Multipart: start, parts, complete (or abort, so no half upload is billed).
    Call start;
    start.method = "POST";
    start.object = object;
    start.query = {{"uploads", ""}};
    start.content_type = content_type(object);
    const core::HttpResponse s = call(start, error, cancel);
    const auto upload_id = s.status == 200 ? xml_value(s.body, "UploadId") : std::nullopt;
    if (!upload_id || upload_id->empty()) {
        if (error.empty()) {
            error = "R2 didn't start the upload of " + object + ": " + s3_error(s);
        }
        return false;
    }
    const auto abort = [&] {
        Call a;
        a.method = "DELETE";
        a.object = object;
        a.query = {{"uploadId", *upload_id}};
        a.max_time = std::chrono::seconds{30};
        std::string ignored;
        (void)call(a, ignored, nullptr);
    };
    std::string complete = "<CompleteMultipartUpload>";
    std::uint64_t offset = 0;
    for (int part = 1; offset < total; ++part) {
        Call p;
        p.method = "PUT";
        p.object = object;
        p.query = {{"partNumber", std::to_string(part)}, {"uploadId", *upload_id}};
        p.file = &file;
        p.offset = offset;
        p.size = std::min(part_size_, total - offset);
        p.max_time = std::chrono::minutes{30};
        const std::uint64_t base = offset;
        // One retry: a part is 64 MiB, a dropped connection shouldn't lose the whole file.
        core::HttpResponse r;
        for (int attempt = 0; attempt < 2; ++attempt) {
            error.clear();
            r = call(p, error, cancel, [&](std::uint64_t sent) { tell(base + sent); });
            if (r.status == 200 || !retryable(r) || (cancel != nullptr && cancel->load())) {
                break;
            }
        }
        const auto etag = r.header("ETag");
        if (r.status != 200 || !etag) {
            if (error.empty()) {
                error = "R2 refused part " + std::to_string(part) + " of " + object + ": " + s3_error(r);
            }
            abort();
            return false;
        }
        complete += "<Part><PartNumber>" + std::to_string(part) + "</PartNumber><ETag>" + *etag + "</ETag></Part>";
        offset += p.size;
    }
    complete += "</CompleteMultipartUpload>";
    Call done;
    done.method = "POST";
    done.object = object;
    done.query = {{"uploadId", *upload_id}};
    done.content_type = "application/xml";
    done.text = complete;
    done.max_time = std::chrono::minutes{5};
    const core::HttpResponse r = call(done, error, cancel);
    // S3 can answer 200 with an <Error> body here.
    if (r.status != 200 || r.body.find("<Error>") != std::string::npos) {
        if (error.empty()) {
            core::HttpResponse shown = r;
            if (shown.status == 200) {
                shown.status = 500;
            }
            error = "R2 didn't finish the upload of " + object + ": " + s3_error(shown);
        }
        abort();
        return false;
    }
    return true;
}

UploadResult upload_files(R2Bucket& bucket, const std::vector<std::string>& cdn_paths, const fs::path& folder,
                          const std::atomic<bool>* cancel,
                          const std::function<void(const std::string&, const R2Progress&)>& progress) {
    UploadResult result;
    for (const std::string& path : cdn_paths) {
        if (cancel != nullptr && cancel->load()) {
            result.problem = "stopped";
            break;
        }
        const fs::path file = folder / core::path_from_utf8(path);
        std::error_code ec;
        const std::uint64_t size = fs::file_size(file, ec);
        if (ec) {
            result.problem = "can't read " + core::path_to_utf8(file);
            break;
        }
        std::string error;
        const auto there = bucket.size_of(path, error, cancel);
        if (!error.empty()) {
            result.problem = error;
            break;
        }
        if (there) {
            if (*there == size) {
                result.already.push_back(path);
                continue;
            }
            result.conflict = true;
            result.problem = path + " is already on R2 with a different size (" + std::to_string(*there) +
                             " bytes, this one " + std::to_string(size) +
                             "): not overwritten, as lists people have point at it";
            break;
        }
        if (!bucket.put_file(path, file, error, cancel, [&](const R2Progress& p) {
                if (progress) {
                    progress(path, p);
                }
            })) {
            result.problem = error;
            break;
        }
        result.uploaded.push_back(path);
    }
    return result;
}

}  // namespace floserver::manifest_tool

// D34.4: uploading to R2 with AWS Signature Version 4. The signing is checked
// against Amazon's published S3 examples; the bucket is a fake that keeps the
// objects in memory and checks each body against its signed SHA-256.

#include "r2.h"
#include "test_util.h"

#include "floserver/core/encoding.h"
#include "floserver/core/sha256.h"

#include <doctest/doctest.h>

#include <fstream>
#include <map>
#include <mutex>
#include <regex>

using namespace floserver::manifest_tool;
namespace core = floserver::core;
namespace fs = std::filesystem;

namespace {

// Amazon's example credentials (S3 API reference, "Signature Calculations").
constexpr const char* kId = "AKIAIOSFODNN7EXAMPLE";
constexpr const char* kSecret = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
constexpr const char* kEmptySha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

const R2Key kKey{std::string(32, 'a'), std::string(32, 'b'), std::string(64, 'c')};

std::string decode(std::string_view t) {
    std::string out;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '%' && i + 2 < t.size()) {
            out += static_cast<char>(std::stoi(std::string(t.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += t[i];
        }
    }
    return out;
}

class FakeR2 final : public core::HttpTransport {
public:
    std::map<std::string, std::string> objects;  // path ("/floserver-cdn/...") -> body
    std::vector<std::string> log;                // "METHOD path?query"
    int fail_parts = 0;                          // parts to answer with fail_status first
    int fail_status = 500;
    int fail_puts = 0;                           // single PUTs to answer 503 first
    bool no_bucket = false;
    std::atomic<bool>* cancel_flag = nullptr;  // set (and the part dropped) when a part is sent
    bool complete_with_error = false;

    core::HttpReply get(const std::string&, const core::HttpLimits&, const core::HttpBody&) override { return {}; }

    core::HttpResponse send(const core::HttpRequest& req, const core::HttpLimits&) override {
        core::HttpResponse r;
        std::string body;
        std::vector<std::uint8_t> buf(7000);  // odd-sized reads
        while (body.size() < req.body_size) {
            const std::size_t want = std::min<std::size_t>(buf.size(), req.body_size - body.size());
            const std::size_t n = req.source(std::span<std::uint8_t>(buf.data(), want));
            REQUIRE(n > 0);
            body.append(reinterpret_cast<const char*>(buf.data()), n);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        }
        const std::string prefix = "https://" + kKey.account_id + ".r2.cloudflarestorage.com";
        REQUIRE(req.url.starts_with(prefix));
        const std::string rest = req.url.substr(prefix.size());
        const std::string path = rest.substr(0, rest.find('?'));
        const std::string query = rest.find('?') == std::string::npos ? "" : rest.substr(rest.find('?') + 1);
        log.push_back(req.method + " " + rest);
        std::string sha;
        bool auth = false;
        for (const auto& [k, v] : req.headers) {
            sha = k == "x-amz-content-sha256" ? v : sha;
            auth = auth || (k == "Authorization" && v.starts_with("AWS4-HMAC-SHA256 Credential=" + kKey.access_key_id));
        }
        CHECK(auth);
        CHECK(sha == core::to_hex(core::sha256(body)));  // what was signed is what came
        // The signature again, from the request as sent: the URL must be what was signed.
        {
            SigV4Request again;
            again.method = req.method;
            again.path = path;
            std::string_view q = query;
            while (!q.empty()) {
                const auto amp = q.find('&');
                const std::string_view part = q.substr(0, amp);
                const auto eq = part.find('=');
                again.query.emplace_back(decode(part.substr(0, eq)),
                                         eq == std::string_view::npos ? "" : decode(part.substr(eq + 1)));
                q = amp == std::string_view::npos ? "" : q.substr(amp + 1);
            }
            again.headers["host"] = kKey.account_id + ".r2.cloudflarestorage.com";
            std::string authorization;
            std::string date;
            for (const auto& [k, v] : req.headers) {
                if (k == "Authorization") {
                    authorization = v;
                } else {
                    std::string lower = k;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
                    again.headers[lower] = v;
                    date = lower == "x-amz-date" ? v : date;
                }
            }
            again.payload_sha256 = sha;
            CHECK(authorization ==
                  sigv4_authorization(again, date, "auto", "s3", kKey.access_key_id, kKey.secret));
            CHECK(decode(path).starts_with("/floserver-cdn/"));
        }
        if (no_bucket) {
            r.status = 404;
            if (req.method != "HEAD") {  // a HEAD's answer has no body
                r.body =
                    "<Error><Code>NoSuchBucket</Code><Message>The specified bucket does not exist.</Message></Error>";
            }
            return r;
        }
        if (req.method == "GET" && path == "/floserver-cdn/" && query == "list-type=2&max-keys=1") {
            r.status = 200;
            r.body = "<ListBucketResult></ListBucketResult>";
            return r;
        }
        if (req.method == "GET" && query.empty()) {
            const auto it = objects.find(decode(path));
            r.status = it == objects.end() ? 404 : 200;
            r.body = it == objects.end() ? "<Error><Code>NoSuchKey</Code></Error>" : it->second;
            r.body.resize(std::min(r.body.size(), req.max_reply));
            return r;
        }
        if (req.method == "HEAD") {
            const auto it = objects.find(decode(path));
            r.status = it == objects.end() ? 404 : 200;
            if (it != objects.end()) {
                r.headers.emplace_back("Content-Length", std::to_string(it->second.size()));
            }
        } else if (req.method == "PUT" && query.empty()) {
            if (fail_puts > 0) {
                --fail_puts;
                r.status = 503;
                return r;
            }
            objects[decode(path)] = body;
            content_types[decode(path)] = content_type_of(req);
            r.status = 200;
        } else if (req.method == "POST" && query == "uploads") {
            r.status = 200;
            r.body = "<InitiateMultipartUploadResult><UploadId>up1</UploadId></InitiateMultipartUploadResult>";
        } else if (req.method == "PUT") {
            std::smatch m;
            REQUIRE(std::regex_match(query, m, std::regex("partNumber=(\\d+)&uploadId=up1")));
            ++part_tries;
            if (cancel_flag != nullptr) {
                *cancel_flag = true;
                r.status = 0;
                r.error = "cancelled";
                return r;
            }
            if (fail_parts > 0) {
                --fail_parts;
                r.status = fail_status;
                return r;
            }
            parts[std::stoi(m[1])] = body;
            r.status = 200;
            r.headers.emplace_back("ETag", "\"etag" + m[1].str() + "\"");
        } else if (req.method == "POST" && query == "uploadId=up1") {
            r.status = 200;
            if (complete_with_error) {
                r.body = "<Error><Code>InternalError</Code><Message>try again</Message></Error>";
                return r;
            }
            completed = body;
            std::string all;
            for (const auto& [n, p] : parts) {
                all += p;
            }
            objects[decode(path)] = all;
        } else if (req.method == "DELETE" && query == "uploadId=up1") {
            aborted = true;
            r.status = 204;
        } else {
            r.status = 400;
        }
        return r;
    }

    static std::string content_type_of(const core::HttpRequest& req) {
        for (const auto& [k, v] : req.headers) {
            if (k == "Content-Type") {
                return v;
            }
        }
        return {};
    }
    std::map<std::string, std::string> content_types;
    int part_tries = 0;
    std::map<int, std::string> parts;
    std::string completed;
    bool aborted = false;
};

R2Bucket bucket(FakeR2& r2) {
    return {r2, kKey, kR2Bucket, [] { return std::chrono::sys_seconds{std::chrono::seconds{1'700'000'000}}; }};
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

}  // namespace

TEST_CASE("r2: Signature Version 4 as in Amazon's S3 examples") {
    SUBCASE("GET Object") {
        SigV4Request r;
        r.method = "GET";
        r.path = "/test.txt";
        r.headers = {{"host", "examplebucket.s3.amazonaws.com"},
                     {"range", "bytes=0-9"},
                     {"x-amz-content-sha256", kEmptySha},
                     {"x-amz-date", "20130524T000000Z"}};
        r.payload_sha256 = kEmptySha;
        CHECK(sigv4_authorization(r, "20130524T000000Z", "us-east-1", "s3", kId, kSecret) ==
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
              "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
              "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    }
    SUBCASE("GET Bucket Lifecycle (a query with no value)") {
        SigV4Request r;
        r.method = "GET";
        r.path = "/";
        r.query = {{"lifecycle", ""}};
        r.headers = {{"host", "examplebucket.s3.amazonaws.com"},
                     {"x-amz-content-sha256", kEmptySha},
                     {"x-amz-date", "20130524T000000Z"}};
        r.payload_sha256 = kEmptySha;
        CHECK(sigv4_authorization(r, "20130524T000000Z", "us-east-1", "s3", kId, kSecret).ends_with(
            "Signature=fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543"));
    }
    SUBCASE("GET Bucket, list objects (encoded query)") {
        SigV4Request r;
        r.method = "GET";
        r.path = "/";
        r.query = {{"max-keys", "2"}, {"prefix", "J"}};
        r.headers = {{"host", "examplebucket.s3.amazonaws.com"},
                     {"x-amz-content-sha256", kEmptySha},
                     {"x-amz-date", "20130524T000000Z"}};
        r.payload_sha256 = kEmptySha;
        CHECK(sigv4_authorization(r, "20130524T000000Z", "us-east-1", "s3", kId, kSecret).ends_with(
            "Signature=34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7"));
    }
}

TEST_CASE("r2: encoding and dates") {
    CHECK(s3_encode("php/8.4.16/php-8.4.16 x+y.zip", true) == "php/8.4.16/php-8.4.16%20x%2By.zip");
    CHECK(s3_encode("a/b", false) == "a%2Fb");
    CHECK(amz_date(std::chrono::sys_seconds{std::chrono::seconds{1'369'353'600}}) == "20130524T000000Z");
}

TEST_CASE("r2: key check") {
    CHECK(r2_key_problem(kKey).empty());
    R2Key k = kKey;
    k.account_id = "not-an-id";
    CHECK(r2_key_problem(k).find("account ID") != std::string::npos);
    k = kKey;
    k.secret = std::string(63, 'c');
    CHECK(r2_key_problem(k).find("Secret") != std::string::npos);
}

TEST_CASE("r2: one PUT for an ordinary file, its size read back") {
    FakeR2 r2;
    R2Bucket b = bucket(r2);
    const test_util::TempDir dir;
    const fs::path f = dir.path() / "node.zip";
    write(f, std::string(12345, 'n'));
    std::string error;
    CHECK(!b.size_of("node/24.23.0/node-v24.23.0-win-x64.zip", error));
    CHECK(error.empty());
    std::uint64_t last = 0;
    REQUIRE(b.put_file("node/24.23.0/node-v24.23.0-win-x64.zip", f, error, nullptr,
                       [&](const R2Progress& p) { last = p.sent; }));
    CHECK(last == 12345);
    CHECK(r2.objects.at("/floserver-cdn/node/24.23.0/node-v24.23.0-win-x64.zip").size() == 12345);
    CHECK(b.size_of("node/24.23.0/node-v24.23.0-win-x64.zip", error) == std::optional<std::uint64_t>{12345});
}

TEST_CASE("r2: a big file goes in parts") {
    FakeR2 r2;
    R2Bucket b = bucket(r2);
    const test_util::TempDir dir;
    const fs::path f = dir.path() / "mysql.zip";
    // Sparse past the multipart size: two full parts and a short one.
    {
        std::ofstream out(f, std::ios::binary);
        out << "head";
    }
    const std::uint64_t size = 2 * R2Bucket::kPartSize + 1000;
    fs::resize_file(f, size);
    std::string error;

    SUBCASE("all parts, then complete") {
        r2.fail_parts = 1;  // the first try of part 1 fails: retried
        REQUIRE(b.put_file("mysql/8.4.8/mysql-8.4.8-winx64.zip", f, error));
        CHECK(r2.parts.size() == 3);
        CHECK(r2.parts.at(3).size() == 1000);
        CHECK(r2.completed.find("<PartNumber>3</PartNumber><ETag>\"etag3\"</ETag>") != std::string::npos);
        CHECK(r2.objects.at("/floserver-cdn/mysql/8.4.8/mysql-8.4.8-winx64.zip").size() == size);
        CHECK(!r2.aborted);
        CHECK(r2.log.front() == "POST /floserver-cdn/mysql/8.4.8/mysql-8.4.8-winx64.zip?uploads");
    }
    SUBCASE("a part that keeps failing: aborted") {
        r2.fail_parts = 2;
        CHECK(!b.put_file("mysql/8.4.8/mysql-8.4.8-winx64.zip", f, error));
        CHECK(error.find("part 1") != std::string::npos);
        CHECK(r2.aborted);
        CHECK(r2.objects.empty());
    }
    SUBCASE("an error inside a 200 at the end: aborted") {
        r2.complete_with_error = true;
        CHECK(!b.put_file("mysql/8.4.8/mysql-8.4.8-winx64.zip", f, error));
        CHECK(error.find("InternalError") != std::string::npos);
        CHECK(r2.aborted);
    }
}

TEST_CASE("r2: error answers explain themselves") {
    core::HttpResponse r;
    r.status = 403;
    r.body = "<Error><Code>SignatureDoesNotMatch</Code><Message>bad</Message></Error>";
    CHECK(s3_error(r) == "HTTP 403, SignatureDoesNotMatch: bad (is the Secret Access Key right?)");
    r.body = "<Error><Code>AccessDenied</Code><Message>Access Denied</Message></Error>";
    CHECK(s3_error(r).find("allowed to write") != std::string::npos);
}

TEST_CASE("r2: upload_files never overwrites, and counts a same-size file as there") {
    FakeR2 r2;
    R2Bucket b = bucket(r2);
    const test_util::TempDir dir;
    write(dir.path() / "deno/2.6.0/a.zip", "aaaa");
    write(dir.path() / "deno/2.6.0/b.zip", "bbbb");
    std::string seen;

    SUBCASE("new: uploaded") {
        const UploadResult u = upload_files(b, {"deno/2.6.0/a.zip", "deno/2.6.0/b.zip"}, dir.path(), nullptr,
                                            [&](const std::string& p, const R2Progress&) { seen = p; });
        CHECK(u.problem.empty());
        CHECK(u.uploaded.size() == 2);
        CHECK(seen == "deno/2.6.0/b.zip");
        CHECK(r2.objects.at("/floserver-cdn/deno/2.6.0/b.zip") == "bbbb");
    }
    SUBCASE("there with the same size: already") {
        r2.objects["/floserver-cdn/deno/2.6.0/a.zip"] = "AAAA";
        const UploadResult u = upload_files(b, {"deno/2.6.0/a.zip"}, dir.path());
        CHECK(u.problem.empty());
        CHECK(u.already == std::vector<std::string>{"deno/2.6.0/a.zip"});
        CHECK(r2.objects.at("/floserver-cdn/deno/2.6.0/a.zip") == "AAAA");  // untouched
    }
    SUBCASE("there with another size: refused, never overwritten") {
        r2.objects["/floserver-cdn/deno/2.6.0/a.zip"] = "different";
        const UploadResult u = upload_files(b, {"deno/2.6.0/a.zip", "deno/2.6.0/b.zip"}, dir.path());
        CHECK(u.problem.find("not overwritten") != std::string::npos);
        CHECK(u.uploaded.empty());
        CHECK(r2.objects.at("/floserver-cdn/deno/2.6.0/a.zip") == "different");
        CHECK(!r2.objects.contains("/floserver-cdn/deno/2.6.0/b.zip"));
    }
}

TEST_CASE("r2: a name with '+', a space and non-ASCII is signed as sent") {
    FakeR2 r2;
    R2Bucket b = bucket(r2);
    const test_util::TempDir dir;
    write(dir.path() / "f.zip", "x");
    std::string error;
    REQUIRE(b.put_file("test/1.0/a+b c\xc3\xa9.zip", dir.path() / "f.zip", error));
    CHECK(r2.objects.contains("/floserver-cdn/test/1.0/a+b c\xc3\xa9.zip"));
    CHECK(r2.log.back() == "PUT /floserver-cdn/test/1.0/a%2Bb%20c%C3%A9.zip");
    CHECK(r2.content_types.at("/floserver-cdn/test/1.0/a+b c\xc3\xa9.zip") == "application/zip");
}

TEST_CASE("r2: retries only what may go better") {
    FakeR2 r2;
    R2Bucket b = bucket(r2);
    const test_util::TempDir dir;
    write(dir.path() / "f.zip", "x");
    std::string error;
    SUBCASE("a single PUT after a 503") {
        r2.fail_puts = 1;
        CHECK(b.put_file("t/1/f.zip", dir.path() / "f.zip", error));
    }
    SUBCASE("a part refused with 403: not sent again") {
        const fs::path big = dir.path() / "big.zip";
        write(big, "h");
        fs::resize_file(big, R2Bucket::kMultipartFrom + 1);
        r2.fail_parts = 5;
        r2.fail_status = 403;
        CHECK(!b.put_file("t/1/big.zip", big, error));
        CHECK(r2.part_tries == 1);
        CHECK(r2.aborted);
    }
    SUBCASE("Stop during a part: aborted, said as stopped") {
        const fs::path big = dir.path() / "big.zip";
        write(big, "h");
        fs::resize_file(big, R2Bucket::kMultipartFrom + 1);
        std::atomic<bool> cancel{false};
        r2.cancel_flag = &cancel;
        CHECK(!b.put_file("t/1/big.zip", big, error, &cancel));
        CHECK(error == "stopped");
        CHECK(r2.part_tries == 1);
        CHECK(r2.aborted);
    }
}

TEST_CASE("r2: check() lists one item, so a missing bucket says so") {
    FakeR2 r2;
    R2Bucket b = bucket(r2);
    CHECK(b.check().empty());
    r2.no_bucket = true;
    CHECK(b.check().find("NoSuchBucket") != std::string::npos);
}

TEST_CASE("r2: upload_files stopping half way says what went up") {
    FakeR2 r2;
    R2Bucket b = bucket(r2);
    const test_util::TempDir dir;
    write(dir.path() / "bun/1.3.14/bun-windows-x64.zip", "regular");
    write(dir.path() / "bun/1.3.14/bun-windows-x64-baseline.zip", "baseline");
    r2.objects["/floserver-cdn/bun/1.3.14/bun-windows-x64-baseline.zip"] = "other size";
    const UploadResult u =
        upload_files(b, {"bun/1.3.14/bun-windows-x64.zip", "bun/1.3.14/bun-windows-x64-baseline.zip"}, dir.path());
    CHECK(u.uploaded == std::vector<std::string>{"bun/1.3.14/bun-windows-x64.zip"});
    CHECK(u.conflict);
    CHECK(!u.problem.empty());
}

TEST_CASE("r2: put_text replaces, get_text reads back, and says when it's missing or too big") {
    FakeR2 r2;
    R2Bucket b = bucket(r2);
    std::string error;
    CHECK_FALSE(b.get_text("manifest.json", 100, error));
    CHECK(error.empty());

    REQUIRE(b.put_text("manifest.json", "old", error));
    REQUIRE(b.put_text("manifest.json", "new list", error));  // replaced, unlike upload_files
    CHECK(r2.objects["/floserver-cdn/manifest.json"] == "new list");
    CHECK(r2.content_types["/floserver-cdn/manifest.json"] == "application/json");
    CHECK(b.get_text("manifest.json", 100, error) == "new list");
    CHECK_FALSE(b.get_text("manifest.json", 3, error));
    CHECK(error.find("bigger than 3 bytes") != std::string::npos);

    r2.fail_puts = 1;
    CHECK(b.put_text("manifest.json.sig", "sig", error));  // one retry
    r2.fail_puts = 2;
    CHECK_FALSE(b.put_text("manifest.json.sig", "sig2", error));
    CHECK(error.find("HTTP 503") != std::string::npos);
    CHECK(r2.objects["/floserver-cdn/manifest.json.sig"] == "sig");

    r2.no_bucket = true;
    CHECK_FALSE(b.get_text("manifest.json", 100, error));
    CHECK(error.empty());  // a 404 either way; go_live says R2 has no list
}

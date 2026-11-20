#pragma once

// D34.4: the Publisher uploads to Cloudflare R2 itself, through R2's
// S3-compatible API with a key limited to the floserver-cdn bucket. Requests
// are signed with AWS Signature Version 4 (region "auto", service "s3").
// An object already on R2 is never overwritten: lists people already have
// point at it.

#include "floserver/core/http.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace floserver::manifest_tool {

struct R2Key {
    std::string account_id;     // 32 hex characters, from the R2 overview page
    std::string access_key_id;  // from the R2 API token
    std::string secret;         // its Secret Access Key
};

// What's wrong with a key typed in, or empty.
[[nodiscard]] std::string r2_key_problem(const R2Key& key);

inline constexpr const char* kR2Bucket = "floserver-cdn";

// S3's URI encoding: every byte but A-Z a-z 0-9 - . _ ~ as %XX (upper case);
// '/' kept when `keep_slash`.
[[nodiscard]] std::string s3_encode(std::string_view text, bool keep_slash);

// The parts of one request that Signature Version 4 signs.
struct SigV4Request {
    std::string method;
    std::string path;  // already encoded (s3_encode with keep_slash)
    std::vector<std::pair<std::string, std::string>> query;  // not encoded
    std::map<std::string, std::string> headers;              // lower-case names, host included
    std::string payload_sha256;                              // hex, or "UNSIGNED-PAYLOAD"
};

// The Authorization header's value. `amz_date` is "20130524T000000Z".
[[nodiscard]] std::string sigv4_authorization(const SigV4Request& request, const std::string& amz_date,
                                              const std::string& region, const std::string& service,
                                              const std::string& access_key_id, const std::string& secret);

// "20130524T000000Z" for a moment.
[[nodiscard]] std::string amz_date(std::chrono::sys_seconds when);

struct R2Progress {
    std::uint64_t sent = 0;
    std::uint64_t total = 0;
};

class R2Bucket {
public:
    // `now` gives the signing time (tests fix it).
    R2Bucket(core::HttpTransport& http, R2Key key, std::string bucket = kR2Bucket,
             std::function<std::chrono::sys_seconds()> now = {});

    // The object's size if it's there; nullopt with `error` empty when it isn't.
    [[nodiscard]] std::optional<std::uint64_t> size_of(const std::string& object, std::string& error,
                                                       const std::atomic<bool>* cancel = nullptr);

    // Whether the key can reach the bucket: a one-item listing, whose error
    // answers say why (a HEAD's don't). Empty when it can.
    [[nodiscard]] std::string check(const std::atomic<bool>* cancel = nullptr);

    // Uploads `file` as `object`: one PUT up to kMultipartFrom, in parts above.
    [[nodiscard]] bool put_file(const std::string& object, const std::filesystem::path& file, std::string& error,
                                const std::atomic<bool>* cancel = nullptr,
                                const std::function<void(const R2Progress&)>& progress = {});

    // D34.6: a small object's contents; nullopt with `error` empty when it isn't there.
    [[nodiscard]] std::optional<std::string> get_text(const std::string& object, std::size_t max_bytes,
                                                      std::string& error, const std::atomic<bool>* cancel = nullptr);

    // D34.6: one PUT of `text`, with one retry on no answer or 5xx. Unlike
    // upload_files it replaces what's there: for the list and its signature only.
    [[nodiscard]] bool put_text(const std::string& object, const std::string& text, std::string& error);

    static constexpr std::uint64_t kMultipartFrom = std::uint64_t{100} * 1024 * 1024;
    static constexpr std::uint64_t kPartSize = std::uint64_t{64} * 1024 * 1024;  // R2: equal parts, 5 MiB+

private:
    struct Call {
        std::string method;
        std::string object;
        std::vector<std::pair<std::string, std::string>> query;
        std::string content_type;
        // The body: a part of a file, or text.
        const std::filesystem::path* file = nullptr;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::string text;
        std::chrono::seconds max_time{120};
        std::size_t max_reply = std::size_t{1024} * 1024;
    };
    core::HttpResponse call(const Call& c, std::string& error, const std::atomic<bool>* cancel,
                            const std::function<void(std::uint64_t)>& sent = {});
    [[nodiscard]] std::string host() const;

    core::HttpTransport& http_;
    R2Key key_;
    std::string bucket_;
    std::function<std::chrono::sys_seconds()> now_;
};

// Get selected's upload (D34.3 step 4): each file of a version, from
// `folder`/<cdn path> to the same path on R2. One already there with the
// same size counts as uploaded (Publish checks its contents); one with a
// different size is never overwritten. Stops at the first problem.
struct UploadResult {
    std::vector<std::string> uploaded;  // sent now
    std::vector<std::string> already;   // there before, same size
    std::string problem;
    // R2 has another file at one of these paths: never to be uploaded by
    // hand either (that would overwrite it).
    bool conflict = false;
};
[[nodiscard]] UploadResult upload_files(
    R2Bucket& bucket, const std::vector<std::string>& cdn_paths, const std::filesystem::path& folder,
    const std::atomic<bool>* cancel = nullptr,
    const std::function<void(const std::string& cdn_path, const R2Progress&)>& progress = {});

// The reason in an S3 error answer ("<Code>…</Code>: <Message>…"), for people.
[[nodiscard]] std::string s3_error(const core::HttpResponse& response);

}  // namespace floserver::manifest_tool

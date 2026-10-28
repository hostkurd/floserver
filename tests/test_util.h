#pragma once

// Helpers shared by the downloader, zip and runtime tests.

#include "floserver/core/encoding.h"
#include "floserver/core/http.h"
#include "floserver/core/manifest.h"
#include "floserver/core/sha256.h"

#include <monocypher-ed25519.h>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace test_util {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = fs::temp_directory_path() / ("floserver-test-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

inline void write_file(const fs::path& p, std::string_view bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline std::string read_file(const fs::path& p) {
    // Not istreambuf_iterator: GCC's release build warns inside it (null-dereference).
    std::ifstream in(p, std::ios::binary);
    std::string out;
    std::array<char, 4096> chunk{};
    while (in.read(chunk.data(), chunk.size()) || in.gcount() > 0) {
        out.append(chunk.data(), static_cast<std::size_t>(in.gcount()));
    }
    return out;
}

struct SigningKey {
    std::array<std::uint8_t, 64> secret{};
    floserver::core::Ed25519PublicKey pub{};
    explicit SigningKey(std::uint8_t fill) {
        std::array<std::uint8_t, 32> seed{};
        seed.fill(fill);
        crypto_ed25519_key_pair(secret.data(), pub.data(), seed.data());
    }
    [[nodiscard]] std::string sign(std::string_view bytes, const std::string& id) const {
        std::array<std::uint8_t, 64> sig{};
        crypto_ed25519_sign(sig.data(), secret.data(), reinterpret_cast<const std::uint8_t*>(bytes.data()),
                            bytes.size());
        return nlohmann::json{{"key", id}, {"signature", floserver::core::to_base64(sig)}}.dump();
    }
};

inline std::string sha256_hex(std::string_view bytes) {
    return floserver::core::to_hex(floserver::core::sha256(bytes));
}

// A transport that serves canned replies. Each URL holds a list of replies
// used in turn (the last one repeats). Bodies arrive in small chunks so the
// streaming code is exercised.
class FakeTransport final : public floserver::core::HttpTransport {
public:
    struct Reply {
        Reply() = default;
        Reply(int s, std::string b, std::string e = {},  // NOLINT(google-explicit-constructor)
              std::optional<std::chrono::sys_seconds> d = {})
            : status(s), body(std::move(b)), error(std::move(e)), date(d) {}
        int status = 200;
        std::string body;
        std::string error;  // with status 0
        std::optional<std::chrono::sys_seconds> date;
    };

    void serve(const std::string& url, Reply reply) {
        const std::lock_guard lock(mutex_);
        replies_[url] = {std::move(reply)};
    }
    void serve_sequence(const std::string& url, std::vector<Reply> replies) {
        const std::lock_guard lock(mutex_);
        replies_[url] = std::move(replies);
    }
    [[nodiscard]] int requests(const std::string& url) const {
        const std::lock_guard lock(mutex_);
        const auto it = counts_.find(url);
        return it == counts_.end() ? 0 : it->second;
    }
    [[nodiscard]] std::size_t bytes_sent() const {
        const std::lock_guard lock(mutex_);
        return bytes_sent_;
    }

    floserver::core::HttpReply get(const std::string& url, const floserver::core::HttpBody& body) override {
        Reply r;
        {
            const std::lock_guard lock(mutex_);
            const int n = counts_[url]++;
            const auto it = replies_.find(url);
            if (it == replies_.end() || it->second.empty()) {
                r.status = 0;
                r.error = "no route to " + url;
            } else {
                r = it->second[std::min<std::size_t>(static_cast<std::size_t>(n), it->second.size() - 1)];
            }
        }
        floserver::core::HttpReply out;
        out.status = r.status;
        out.error = r.error;
        out.server_date = r.date;
        out.content_length = r.body.size();
        if (r.status != 200) {
            return out;
        }
        constexpr std::size_t kChunk = 7;
        for (std::size_t at = 0; at < r.body.size(); at += kChunk) {
            const std::size_t n = std::min(kChunk, r.body.size() - at);
            {
                const std::lock_guard lock(mutex_);
                bytes_sent_ += n;
            }
            if (!body(std::span(reinterpret_cast<const std::uint8_t*>(r.body.data() + at), n))) {
                out.stopped = true;
                return out;
            }
        }
        return out;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<Reply>> replies_;
    std::map<std::string, int> counts_;
    std::size_t bytes_sent_ = 0;
};

}  // namespace test_util

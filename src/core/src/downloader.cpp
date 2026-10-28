#include "floserver/core/downloader.h"

#include "floserver/core/paths.h"
#include "floserver/core/sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <thread>

namespace floserver::core {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr auto kClockTolerance = std::chrono::hours(1);
constexpr std::size_t kMaxStateBytes = std::size_t{64} * 1024;
constexpr std::size_t kMaxKeyIdLength = 64;

struct Fetched {
    HttpReply reply;
    std::string body;
    bool too_big = false;
};

Fetched fetch_small(HttpTransport& transport, const std::string& url, std::uint64_t cap,
                    const std::atomic<bool>* cancel) {
    Fetched f;
    f.reply = transport.get(url, {kManifestMaxTime, cancel}, [&](std::span<const std::uint8_t> chunk) {
        if (f.body.size() + chunk.size() > cap) {
            f.too_big = true;
            return false;
        }
        f.body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        return true;
    });
    return f;
}

// Fills `out` with the transfer error, if any. True when `f` holds a body.
bool check_transfer(const Fetched& f, const std::string& url, ManifestFetch& out) {
    if (f.too_big) {
        out.error = FetchError::TooBig;
        out.message = url + " is larger than any runtime list could be";
        return false;
    }
    if (f.reply.status == 0) {
        out.error = FetchError::Network;
        out.message = "couldn't fetch the runtime list: " + f.reply.error;
        return false;
    }
    if (f.reply.status != 200) {
        out.error = FetchError::Http;
        out.message = "couldn't fetch the runtime list: the server answered " +
                      std::to_string(f.reply.status) + " for " + url;
        return false;
    }
    return true;
}

std::string utc_text(std::chrono::sys_seconds t) {
    const auto days = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day ymd{days};
    const std::chrono::hh_mm_ss hms{t - days};
    std::array<char, 32> text{};
    const int n = std::snprintf(text.data(), text.size(), "%04d-%02u-%02u %02d:%02d UTC",
                                static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                                static_cast<unsigned>(ymd.day()), static_cast<int>(hms.hours().count()),
                                static_cast<int>(hms.minutes().count()));
    return {text.data(), n > 0 ? static_cast<std::size_t>(n) : 0};
}

ManifestFetch attempt(HttpTransport& transport, const ManifestSource& source, const ManifestCheck& check,
                      const std::atomic<bool>* cancel, std::optional<std::chrono::sys_seconds>& server_date) {
    ManifestFetch out;
    // The signature first: if the pair changes between the two requests, the
    // retry below catches it either way.
    const Fetched sig = fetch_small(transport, source.signature_url, kMaxSignatureBytes, cancel);
    if (!check_transfer(sig, source.signature_url, out)) {
        return out;
    }
    const Fetched list = fetch_small(transport, source.manifest_url, kMaxManifestBytes, cancel);
    if (!check_transfer(list, source.manifest_url, out)) {
        return out;
    }
    server_date = list.reply.server_date ? list.reply.server_date : sig.reply.server_date;

    ManifestResult result = open_manifest(list.body, sig.body, check);
    out.manifest = std::move(result.manifest);
    out.key_id = std::move(result.key_id);
    out.manifest_error = result.error;
    out.message = std::move(result.message);
    out.warnings = std::move(result.warnings);
    if (out.manifest_error != ManifestError::None) {
        out.error = FetchError::Manifest;
        out.manifest.reset();
    }
    return out;
}

}  // namespace

ManifestFetch fetch_manifest(HttpTransport& transport, const ManifestSource& source,
                             const ManifestFetchOptions& options) {
    std::optional<std::chrono::sys_seconds> server_date;
    ManifestFetch out = attempt(transport, source, options.check, options.cancel, server_date);
    if (out.error == FetchError::Manifest && out.manifest_error == ManifestError::BadSignature &&
        (options.cancel == nullptr || !options.cancel->load())) {
        // Most likely a cache holding one new file and one old: wait and retry once.
        if (options.sleep) {
            options.sleep(options.retry_delay);
        } else {
            std::this_thread::sleep_for(options.retry_delay);
        }
        server_date.reset();
        out = attempt(transport, source, options.check, options.cancel, server_date);
    }
    if (out.error == FetchError::Manifest && out.manifest_error == ManifestError::Expired) {
        const auto now = options.check.now;
        if (server_date && (*server_date - now > kClockTolerance || now - *server_date > kClockTolerance)) {
            out.clock_wrong = true;
            out.message = "your computer's date or time looks wrong: it says " + utc_text(now) +
                          ", the server says " + utc_text(*server_date) +
                          ". Set the correct date and time, then try again";
        } else {
            out.message = "the runtime list on the server has expired. This is a problem on our side; "
                          "please try again later";
        }
    }
    return out;
}

std::map<std::string, std::int64_t, std::less<>> load_last_sequences(const fs::path& file) {
    std::map<std::string, std::int64_t, std::less<>> out;
    const auto text = read_small_file(file, kMaxStateBytes);
    if (!text) {
        return out;
    }
    const json doc = json::parse(*text, nullptr, false);
    if (!doc.is_object() || !doc.contains("last_sequences") || !doc["last_sequences"].is_object()) {
        return out;
    }
    for (const auto& [key, value] : doc["last_sequences"].items()) {
        if (!key.empty() && key.size() <= kMaxKeyIdLength && value.is_number_integer() &&
            value.get<std::int64_t>() >= 0) {
            out[key] = value.get<std::int64_t>();
        }
    }
    return out;
}

bool store_last_sequence(const fs::path& file, const std::string& key_id, std::int64_t sequence,
                         std::string& error) {
    auto all = load_last_sequences(file);
    if (const auto it = all.find(key_id); it != all.end() && it->second >= sequence) {
        return true;
    }
    all[key_id] = sequence;
    json doc = {{"schema", 1}, {"last_sequences", json::object()}};
    for (const auto& [key, value] : all) {
        doc["last_sequences"][key] = value;
    }
    return write_file_atomically(file, doc.dump(2) + "\n", error);
}

std::chrono::seconds download_time_limit(std::uint64_t size) {
    constexpr std::uint64_t kSlowestBytesPerSecond = std::uint64_t{16} * 1024;
    constexpr std::uint64_t kMaxSeconds = std::uint64_t{24} * 3600;
    return std::chrono::seconds(
        static_cast<std::int64_t>(std::min(kMaxSeconds, 120 + size / kSlowestBytesPerSecond)));
}

std::optional<FileDigest> digest_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Sha256 hash;
    FileDigest out;
    std::vector<char> chunk(std::size_t{64} * 1024);
    while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || in.gcount() > 0) {
        const auto got = static_cast<std::size_t>(in.gcount());
        hash.update(std::string_view(chunk.data(), got));
        out.size += got;
    }
    if (in.bad()) {
        return std::nullopt;
    }
    out.sha256 = hash.finish();
    return out;
}

DownloadResult download_file(HttpTransport& transport, const RuntimeFile& entry, const fs::path& destination,
                             const DownloadOptions& options) {
    DownloadResult result;
    std::error_code ec;
    if (fs::is_regular_file(destination, ec)) {
        if (const auto d = digest_file(destination); d && matches_entry(entry, d->size, d->sha256)) {
            result.ok = true;
            return result;
        }
        fs::remove(destination, ec);
    }
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        result.message = "can't create " + path_to_utf8(destination.parent_path()) + ": " + error_text(ec);
        return result;
    }
    fs::path part = destination;
    part += ".part";
    const std::string what = entry.name + " " + entry.version;

    for (std::size_t i = 0; i < entry.urls.size(); ++i) {
        const std::string& url = entry.urls[i];
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.message = "can't write " + path_to_utf8(part);
            return result;
        }
        Sha256 hash;
        std::uint64_t received = 0;
        bool too_much = false;
        bool write_failed = false;
        bool cancelled = false;
        const HttpReply reply = transport.get(url, {download_time_limit(entry.size), options.cancel},
                                              [&](std::span<const std::uint8_t> chunk) {
            if (chunk.size() > entry.size - received) {
                too_much = true;
                return false;
            }
            out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            if (!out) {
                write_failed = true;
                return false;
            }
            hash.update(chunk);
            received += chunk.size();
            if (options.progress && !options.progress({received, entry.size, i})) {
                cancelled = true;
                return false;
            }
            return true;
        });
        out.close();
        if (out.fail()) {
            write_failed = true;
        }

        std::string problem;
        if (options.cancel != nullptr && options.cancel->load()) {
            cancelled = true;
        }
        if (cancelled) {
            fs::remove(part, ec);
            result.cancelled = true;
            result.message = "the download was cancelled";
            return result;
        }
        if (write_failed) {
            fs::remove(part, ec);
            result.message = "can't write " + path_to_utf8(part) + " (is the disk full?)";
            return result;
        }
        if (too_much) {
            problem = "sent more than the " + std::to_string(entry.size) + " bytes in the list";
        } else if (reply.status == 0) {
            problem = reply.error;
        } else if (reply.status != 200) {
            problem = "the server answered " + std::to_string(reply.status);
        } else if (received != entry.size) {
            problem = "the download stopped after " + std::to_string(received) + " of " +
                      std::to_string(entry.size) + " bytes";
        } else if (!matches_entry(entry, received, hash.finish())) {
            problem = "the file's SHA-256 doesn't match the signed list";
        }
        if (problem.empty()) {
            fs::rename(part, destination, ec);
            if (ec) {
                problem = "can't move it into place: " + error_text(ec);
            } else {
                result.ok = true;
                result.attempts.push_back(url + ": ok");
                return result;
            }
        }
        fs::remove(part, ec);
        std::string attempt_line = url;
        attempt_line += ": ";
        attempt_line += problem;
        result.attempts.push_back(std::move(attempt_line));
        result.message = "couldn't download ";
        result.message += what;
        result.message += ": ";
        result.message += problem;
    }
    if (entry.urls.empty()) {
        result.message = "couldn't download " + what + ": the list gives no address for it";
    }
    return result;
}

}  // namespace floserver::core

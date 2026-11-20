#pragma once

// D34.3: getting the versions ticked in the Updates window. Each one's files
// are worked out (plan_fetch: asking the site where the name isn't fixed,
// such as PHP's rebuild number or EDB's build), downloaded from the official
// site, checked against its official checksum where one is published,
// prepared (PostgreSQL's server zip, Python's "+" -> "-") and written under
// a folder as they go on the CDN: <folder>/<runtime>/<version>/<file>.
// Uploading is the user's (Download only) or R2's (PR B2); Publish checks
// every file again from the CDN either way.

#include "publish.h"
#include "updates.h"

#include "floserver/core/http.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace floserver::manifest_tool {

struct FetchFile {
    std::string url;       // the official download
    std::string cdn_path;  // where it goes on the CDN: "postgresql/17.12/postgresql-17.12-1-windows-x64-server.zip"
    // What's done to it after the download.
    enum class Prepare : std::uint8_t { None, PostgresServerZip } prepare = Prepare::None;
};

struct FetchPlan {
    std::string title;  // "PostgreSQL 17.12"
    std::vector<FetchFile> files;
    std::string problem;  // empty when every file is known
    std::string note;     // something to say alongside (phpredis: a PHP series without a Windows build yet)
};

// The files of `row`'s newest version. `listed`: runtime -> versions in the
// next list (phpredis is fetched for the PHP series listed).
[[nodiscard]] FetchPlan plan_fetch(core::HttpTransport& http, const UpdateRow& row,
                                   const std::map<std::string, std::vector<std::string>>& listed,
                                   const std::atomic<bool>* cancel = nullptr);

struct FetchProgress {
    std::string title;     // "PostgreSQL 17.12"
    std::string step;      // "Downloading", "Checking", "Making the server zip"
    std::uint64_t bytes = 0;
};

struct FetchResult {
    std::vector<std::string> cdn_paths;  // written under the folder, ready to upload
    std::string problem;                 // empty when all were written
};

// Downloads `plan`'s files into `folder` (<folder>/<cdn path>), checking
// each against its official checksum where one is published. A file that
// doesn't match is deleted and named in the problem. All of a plan or none:
// on a problem, the files already written for it are deleted too.
[[nodiscard]] FetchResult fetch_files(core::HttpTransport& http, const FetchPlan& plan,
                                      const std::filesystem::path& folder, const std::atomic<bool>* cancel = nullptr,
                                      const std::function<void(const FetchProgress&)>& progress = {});

// FloServer's PostgreSQL zip from EDB's binaries zip (D29.1): the entries
// under pgsql/bin, lib, share and include, copied as they are (the same
// bytes, names and dates), as tools/postgresql/make-postgresql-server-zip.ps1
// does. The required server programs must be there.
[[nodiscard]] bool make_postgresql_server_zip(const std::filesystem::path& edb_zip, const std::filesystem::path& out,
                                              std::string& error, const std::atomic<bool>* cancel = nullptr);

// upload-steps.txt for a Download only folder: what to upload where, with
// the Wrangler commands for the bucket. A file in `folder` over Wrangler's
// 300 MiB is listed apart, for an S3 client.
[[nodiscard]] std::string upload_steps(const std::vector<std::string>& cdn_paths,
                                       const std::filesystem::path& folder = {});

// D34.5: the fetched files added to the Publisher's next list, each one
// replacing the older versions of its series there (the same runtime,
// series and variant: PHP 8.4.16 replaces 8.4.15, Bun's baseline build
// replaces the older baseline). A published one replaced is in `left_out`,
// for the Publisher's "left out" set.
struct ListChange {
    std::vector<PublishFile> files;
    std::vector<std::string> added;     // CDN paths
    std::vector<std::string> left_out;  // CDN paths of older versions taken out
    std::vector<std::string> problems;  // a path the list can't carry
};
[[nodiscard]] ListChange add_to_list(std::vector<PublishFile> files, const std::vector<std::string>& cdn_paths);

}  // namespace floserver::manifest_tool

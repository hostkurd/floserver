#pragma once

// The terminal shims (D10, D17): small php, node, npm, npx, composer,
// python and pip programs in one folder on the user's PATH. Each finds the
// nearest floserver.json from the current folder and runs the version it
// asks for; outside a project, the default (for now the newest installed;
// Settings will set it), or when FloServer has none, the program of that
// name next on the PATH, as if the shim weren't there. They run runtimes directly and don't go through the
// service.
//
//   php       php(.exe) of the project's PHP, with a php.ini FloServer writes
//             for that install and the project's extensions (the base ones
//             every project gets, as on the web), so Composer and artisan work
//   node      the project's Node
//   npm, npx  the project's Node running its own npm-cli.js / npx-cli.js
//   composer  the project's PHP running the newest installed composer.phar
//   python    the project's .venv when it has one, else its Python install
//   pip       the same python with -m pip

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace floserver::core {

// The names the shims answer to.
[[nodiscard]] const std::vector<std::string>& shim_names();

struct ShimTarget {
    std::filesystem::path program;     // absolute
    std::vector<std::string> args;     // before the user's own arguments
    std::vector<std::pair<std::string, std::string>> env;  // set for the program
    std::string version;               // the runtime's, for messages
};

// What `tool` ("php", "npm"...) runs in `cwd`. Nothing (with `error`, in plain
// English, saying what to install or fix) when it can't. May write the php.ini
// for a PHP install under data/cli/php.
[[nodiscard]] std::optional<ShimTarget> resolve_shim(const std::string& tool, const std::filesystem::path& cwd,
                                                     const std::filesystem::path& data_dir, std::string& error);

[[nodiscard]] std::filesystem::path shims_dir(const std::filesystem::path& data);

}  // namespace floserver::core

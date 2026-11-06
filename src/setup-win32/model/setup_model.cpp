#include "model/setup_model.h"

#include <array>
#include <charconv>
#include <cstdio>

namespace floserver::setup {

std::optional<std::vector<unsigned>> parse_version(std::string_view text) {
    std::vector<unsigned> parts;
    while (true) {
        unsigned value = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end == text.data()) {
            return std::nullopt;
        }
        parts.push_back(value);
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        if (text.empty()) {
            break;
        }
        if (text.front() != '.' || parts.size() == 4) {
            return std::nullopt;
        }
        text.remove_prefix(1);
    }
    return parts;
}

int compare_versions(std::string_view a, std::string_view b) {
    const auto x = parse_version(a);
    const auto y = parse_version(b);
    if (!x || !y) {
        return 0;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const unsigned p = i < x->size() ? (*x)[i] : 0;
        const unsigned q = i < y->size() ? (*y)[i] : 0;
        if (p != q) {
            return p < q ? -1 : 1;
        }
    }
    return 0;
}

SetupMode decide_setup_mode(const std::optional<Product>& installed, const Product& ours) {
    if (!installed) {
        return SetupMode::Install;
    }
    if (installed->code == ours.code) {
        return SetupMode::Reinstall;
    }
    // A different build of the same version replaces it, as the MSI's
    // AllowSameVersionUpgrades does.
    return compare_versions(installed->version, ours.version) > 0 ? SetupMode::Newer : SetupMode::Update;
}

bool licence_needed(SetupMode mode, std::string_view accepted_sha256, std::string_view ours_sha256) {
    if (mode == SetupMode::Install) {
        return true;
    }
    return accepted_sha256.empty() || accepted_sha256 != ours_sha256;
}

std::wstring msi_properties(SetupMode mode) {
    std::wstring out = L"REBOOT=ReallySuppress";
    if (mode == SetupMode::Reinstall) {
        // The same product again: every file and registry value written anew.
        out += L" REINSTALL=ALL REINSTALLMODE=vamus";
    }
    return out;
}

std::optional<InstallStep> step_for_action(std::string_view action) {
    struct Entry {
        std::string_view action;
        InstallStep step;
    };
    static constexpr std::array kEntries{
        Entry{"InstallValidate", InstallStep::Checked},
        Entry{"InstallInitialize", InstallStep::Checked},
        Entry{"RemoveExistingProducts", InstallStep::Copying},
        Entry{"RemoveFiles", InstallStep::Copying},
        Entry{"InstallFiles", InstallStep::Copying},
        Entry{"WriteRegistryValues", InstallStep::Copying},
        Entry{"WriteEnvironmentStrings", InstallStep::Path},
        Entry{"CreateShortcuts", InstallStep::StartMenu},
        Entry{"InstallFinalize", InstallStep::StartMenu},
    };
    for (const Entry& e : kEntries) {
        if (e.action == action) {
            return e.step;
        }
    }
    return std::nullopt;
}

std::string format_size(std::uint64_t bytes) {
    constexpr std::uint64_t kKb = 1024;
    constexpr std::uint64_t kMb = kKb * 1024;
    std::array<char, 32> buf{};
    if (bytes >= kMb) {
        (void)std::snprintf(buf.data(), buf.size(), "%.1f MB", static_cast<double>(bytes) / static_cast<double>(kMb));
    } else {
        (void)std::snprintf(buf.data(), buf.size(), "%llu KB",
                      static_cast<unsigned long long>((bytes + kKb - 1) / kKb));  // NOLINT(google-runtime-int): printf's %llu
    }
    return buf.data();
}

SetupArgs parse_setup_args(const std::vector<std::string>& args) {
    SetupArgs out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--quiet" || a == "/quiet" || a == "/q" || a == "/qn" || a == "/Q" || a == "/QN") {
            out.quiet = true;
        } else if ((a == "--log" || a == "/log" || a == "/l") && i + 1 < args.size()) {
            out.log = args[++i];
        }
    }
    return out;
}

}  // namespace floserver::setup

#pragma once

// The shims folder on the user's PATH (D10). On Windows: the user's own Path
// (HKCU\Environment, no admin rights), with other programs told it changed;
// new terminals see it. Elsewhere FloServer doesn't edit shell profiles: the
// caller says which line to add.

#include <cstdint>
#include <filesystem>
#include <string>

namespace floserver::core {

enum class PathChange : std::uint8_t { Done, AlreadyDone, Unsupported, Failed };

[[nodiscard]] PathChange add_to_user_path(const std::filesystem::path& dir, std::string& error);
[[nodiscard]] PathChange remove_from_user_path(const std::filesystem::path& dir, std::string& error);
// True when `dir` is on the user's saved Path (Windows) or this process's PATH.
[[nodiscard]] bool on_user_path(const std::filesystem::path& dir);

// A Path value with `dir` added at the front or taken out; entries compared
// without case and trailing '\' (Windows' rules). Exposed for tests.
[[nodiscard]] std::wstring path_with(const std::wstring& path, const std::wstring& dir);
[[nodiscard]] std::wstring path_without(const std::wstring& path, const std::wstring& dir);
[[nodiscard]] bool path_has(const std::wstring& path, const std::wstring& dir);

}  // namespace floserver::core

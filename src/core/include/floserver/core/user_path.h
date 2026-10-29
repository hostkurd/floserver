#pragma once

// The shims folder on the user's PATH (D10). On Windows: the user's own Path
// (HKCU\Environment, no admin rights), with other programs told it changed;
// new terminals see it. Windows puts the machine-wide Path before the user's,
// so a program installed for all users (C:\Program Files\nodejs) is still
// found first: first_on_path() says which one a new terminal finds.
// Elsewhere FloServer doesn't edit shell profiles: the caller says which
// line to add.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace floserver::core {

enum class PathChange : std::uint8_t { Done, AlreadyDone, Unsupported, Failed };

[[nodiscard]] PathChange add_to_user_path(const std::filesystem::path& dir, std::string& error);
[[nodiscard]] PathChange remove_from_user_path(const std::filesystem::path& dir, std::string& error);
// True when `dir` is on the user's saved Path (Windows) or this process's PATH.
[[nodiscard]] bool on_user_path(const std::filesystem::path& dir);

// The program a new terminal runs for `name` ("node"): the machine's Path,
// then the user's, each folder tried with PATHEXT's endings (Windows); this
// process's PATH elsewhere.
[[nodiscard]] std::optional<std::filesystem::path> first_on_path(const std::string& name);

// A Path value with `dir` added at the front or taken out; entries compared
// without case and trailing '\' (Windows' rules). Exposed for tests.
[[nodiscard]] std::wstring path_with(const std::wstring& path, const std::wstring& dir);
[[nodiscard]] std::wstring path_without(const std::wstring& path, const std::wstring& dir);
[[nodiscard]] bool path_has(const std::wstring& path, const std::wstring& dir);

}  // namespace floserver::core

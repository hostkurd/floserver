#pragma once

// The R2 upload key (D34.4, Q1): kept in Windows Credential Manager on this
// PC only ("FloServer Publisher: R2 upload key"), asked for once. It can
// write to the floserver-cdn bucket and nothing else; the signing key stays
// separate and offline.

#include "r2.h"

#include <windows.h>

#include <optional>
#include <string>

namespace floserver::publisher {

[[nodiscard]] std::optional<manifest_tool::R2Key> load_r2_key();
[[nodiscard]] bool save_r2_key(const manifest_tool::R2Key& key, std::string& error);
void forget_r2_key();

// The key window, modal over `owner`. Returns the key saved, or nothing
// when it was closed or the key forgotten.
std::optional<manifest_tool::R2Key> ask_r2_key(HWND owner);

}  // namespace floserver::publisher

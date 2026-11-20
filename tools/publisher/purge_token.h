#pragma once

// The Cloudflare cache-purge token (D34.6, Q2): kept in Windows Credential
// Manager on this PC only ("FloServer Publisher: Cloudflare purge token"),
// with the floserver.org zone's ID. It can purge that zone's cache and
// nothing else.

#include "golive.h"

#include <windows.h>

#include <optional>

namespace floserver::publisher {

[[nodiscard]] std::optional<manifest_tool::PurgeToken> load_purge_token();

// The token window, modal over `owner`. Returns the token saved, or nothing
// when it was closed or the token forgotten.
std::optional<manifest_tool::PurgeToken> ask_purge_token(HWND owner);

}  // namespace floserver::publisher

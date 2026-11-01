#pragma once

// Settings > General's Windows side: window.json in FloServer's data folder,
// Windows' display languages, the Run entry under HKCU, and FloServer.exe's
// own path. Each is read only when asked (startup, the section opening, a
// click): nothing here polls.

#include "model/general_page.h"

#include <optional>
#include <string>
#include <vector>

namespace floserver::ui {

// window.json, or the defaults when it isn't there or can't be read.
[[nodiscard]] WindowPrefs load_window_prefs();
// False with `error` when it couldn't be written. `kept_as`: where a damaged
// window.json was kept before the new one was written (empty when none was).
[[nodiscard]] bool save_window_prefs(const WindowPrefs& prefs, std::string& error, std::string& kept_as);

// Windows' display languages for this user, most wanted first ("ar-IQ", "en-US").
[[nodiscard]] std::vector<std::string> windows_display_languages();
// The language the window shows for `prefs` (see resolve_lang).
[[nodiscard]] Lang window_lang(const WindowPrefs& prefs);

// FloServer.exe's full path (UTF-8); empty when Windows won't say.
[[nodiscard]] std::string own_exe_path();

// The Run value for this service instance: nothing when there's none.
// `error` is set (and nothing returned) when the key couldn't be read.
[[nodiscard]] std::optional<RunEntry> read_run_entry(std::string& error);
// Writes it for this FloServer.exe (`tray`: start in the tray), or removes
// it when `on` is false. False with `error` when Windows refused.
[[nodiscard]] bool write_run_entry(bool on, bool tray, std::string& error);

}  // namespace floserver::ui

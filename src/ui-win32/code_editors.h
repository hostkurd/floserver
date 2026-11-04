#pragma once

// The code editors on this PC (approved 3 Nov), found without running
// anything: their usual install folders, App Paths and the Uninstall
// entries (per user, then the machine's, 64- and 32-bit). Asked only when
// Settings > General opens or the window first needs the editor's name; the
// window keeps the answer for its session (see model/code_editor.h).

#include "model/code_editor.h"

#include <optional>
#include <vector>

namespace floserver::ui {

// Each editor found once, in kEditors' order.
[[nodiscard]] std::vector<FoundEditor> detect_editors();

// What a project opens in now: pick_editor(), with a chosen program only
// while it's still on disk.
[[nodiscard]] std::optional<EditorTarget> resolve_editor(const EditorChoice& choice,
                                                         const std::vector<FoundEditor>& found);

}  // namespace floserver::ui

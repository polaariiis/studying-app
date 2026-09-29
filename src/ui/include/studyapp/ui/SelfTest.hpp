#pragma once

#include <studyapp/ui/MainWindow.hpp>

#include <functional>
#include <string>
#include <vector>

namespace studyapp::canvas {
class TextLayout;
}

namespace studyapp::ui {

/// `studyapp --self-test` (Phase 9): checks that an installed copy has everything it needs
/// at run time — the Qt platform and image-format plugins, fonts and text layout, SQLite
/// workspaces (create, edit, reopen, search), Qt PDF (write, inspect, import, render a
/// tile), export (PDF, PNG, SVG) and printing to a file — in a temporary directory, without
/// showing a window or touching the user's workspaces and settings. Used to smoke-test
/// packages on clean machines (CI) and to diagnose broken installations.
///
/// `log` receives one line per check ("ok: …" or "FAILED: …"). Returns the failures (empty:
/// every check passed). Needs a QApplication.
[[nodiscard]] std::vector<std::string>
runSelfTest(const ShellServices& services, canvas::TextLayout& textLayout,
            const std::function<void(const std::string&)>& log);

} // namespace studyapp::ui

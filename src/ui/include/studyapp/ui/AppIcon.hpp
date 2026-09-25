#pragma once

#include <QIcon>

namespace studyapp::ui {

/// The StudyBoard application icon, loaded from the compiled-in Qt resources
/// (`:/icons/app/studyboard-<size>.png`, 16–256 px). Generated from
/// resources/icons/app/studyboard-master.png by tools/generate_app_icons.py.
[[nodiscard]] QIcon applicationIcon();

} // namespace studyapp::ui

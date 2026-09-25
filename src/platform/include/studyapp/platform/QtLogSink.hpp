#pragma once

namespace studyapp::platform {

/// Routes the core logging facade (`studyapp::core::log`) into Qt's message handling
/// (`qDebug`/`qInfo`/`qWarning`/`qCritical`), so core and Qt messages share one output and
/// honour `QT_LOGGING_RULES` / `QT_MESSAGE_PATTERN`. Call once at start-up.
void installQtLogSink();

} // namespace studyapp::platform

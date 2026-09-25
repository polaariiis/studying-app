#include <studyapp/platform/QtLogSink.hpp>

#include <studyapp/core/Log.hpp>

#include <QByteArray>
#include <QLoggingCategory>
#include <QString>

namespace studyapp::platform {

namespace {

Q_LOGGING_CATEGORY(lcStudyApp, "studyapp")

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

void forwardToQt(core::LogLevel level, std::string_view category, std::string_view message) {
    const QString text = toQString(category) + QStringLiteral(": ") + toQString(message);
    switch (level) {
    case core::LogLevel::Debug:
        qCDebug(lcStudyApp).noquote() << text;
        break;
    case core::LogLevel::Info:
        qCInfo(lcStudyApp).noquote() << text;
        break;
    case core::LogLevel::Warning:
        qCWarning(lcStudyApp).noquote() << text;
        break;
    case core::LogLevel::Error:
        qCCritical(lcStudyApp).noquote() << text;
        break;
    }
}

} // namespace

void installQtLogSink() {
    core::setLogSink(&forwardToQt);
}

} // namespace studyapp::platform

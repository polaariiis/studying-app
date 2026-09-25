// StudyBoard composition root: creates the application object, the adapters and the main
// window, wires them together by constructor injection and runs the event loop.

#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/platform/QtLogSink.hpp>
#include <studyapp/ui/AppIcon.hpp>
#include <studyapp/ui/MainWindow.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QApplication>
#include <QSettings>
#include <QString>

#include <string_view>

namespace {

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv); // NOLINT(misc-const-correctness): configured via static APIs
    QApplication::setOrganizationName(toQString(studyapp::core::build::kProductName));
    QApplication::setApplicationName(toQString(studyapp::core::build::kProductName));
    QApplication::setApplicationVersion(toQString(studyapp::core::build::kVersion));
    QApplication::setWindowIcon(studyapp::ui::applicationIcon());

    studyapp::platform::installQtLogSink();

    QSettings settings; // per-user, per-machine UI state (window geometry, theme)
    studyapp::ui::ThemeManager themes;
    studyapp::ui::MainWindow window(themes, settings);
    window.show();

    studyapp::core::logInfo("app", "started");
    const int exitCode = QApplication::exec();
    studyapp::core::logInfo("app", "exited cleanly");
    return exitCode;
}

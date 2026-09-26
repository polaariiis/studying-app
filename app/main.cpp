// StudyBoard composition root: creates the application object, the adapters and the main
// window, wires them together by constructor injection, opens the start-up workspace and
// runs the event loop. The window owns the open workspace (docs/ARCHITECTURE.md §3.5).

#include <studyapp/application/WorkspaceSession.hpp>
#include <studyapp/core/BuildInfo.hpp>
#include <studyapp/core/Clock.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Log.hpp>
#include <studyapp/document/Commands.hpp>
#include <studyapp/platform/QtLogSink.hpp>
#include <studyapp/platform/QtWorkspaceLocker.hpp>
#include <studyapp/ui/AppIcon.hpp>
#include <studyapp/ui/MainWindow.hpp>
#include <studyapp/ui/ThemeManager.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTimer>

#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using studyapp::application::WorkspaceSession;

QString toQString(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::filesystem::path toPath(const QString& text) {
    return std::filesystem::path(text.toStdU16String());
}

std::filesystem::path defaultWorkspacePath() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return toPath(QDir(base).filePath(QStringLiteral("Default.studyws")));
}

/// Benchmark data: `count` hand-writing-like strokes (smooth random walks with varying
/// pressure) spread over a large area, written as one command. Deterministic seed.
void generateStrokes(WorkspaceSession& session, studyapp::core::PageId page, int count,
                     studyapp::core::IdGenerator& ids) {
    namespace commands = studyapp::document::commands;
    namespace document = studyapp::document;
    const auto layers = session.workspace().layersOf(page);
    if (layers.empty() || count <= 0) {
        return;
    }
    std::mt19937 random(20260925);
    std::uniform_real_distribution<double> position(0.0, 6000.0);
    std::normal_distribution<float> turn(0.0F, 0.35F);
    std::uniform_int_distribution<int> length(30, 90);
    document::Workspace scratch = session.workspace();
    document::Patch patch;
    for (int i = 0; i < count; ++i) {
        std::vector<document::StrokePoint> points;
        float x = 0.0F;
        float y = 0.0F;
        float heading = turn(random) * 4.0F;
        const int n = length(random);
        for (int k = 0; k < n; ++k) {
            points.push_back(
                {x, y, 0.4F + 0.6F * std::abs(std::sin(static_cast<float>(k) * 0.2F))});
            heading += turn(random);
            x += 3.0F * std::cos(heading);
            y += 3.0F * std::sin(heading);
        }
        auto created = commands::createElement(
            scratch, layers.back(),
            {.transform = {.position = {position(random), position(random) * 0.6}},
             .payload = document::Stroke{.baseWidth = 2.0F,
                                         .points = document::makeStrokePoints(std::move(points))}},
            ids);
        if (!created || !scratch.apply(created->command.patch)) {
            return;
        }
        for (const auto& change : created->command.patch.changes()) {
            patch.add(change);
        }
    }
    (void)session.execute({"Generate benchmark strokes", std::move(patch)});
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv); // NOLINT(misc-const-correctness): configured via static APIs
    QApplication::setOrganizationName(toQString(studyapp::core::build::kProductName));
    QApplication::setApplicationName(toQString(studyapp::core::build::kProductName));
    QApplication::setApplicationVersion(toQString(studyapp::core::build::kVersion));
    QApplication::setWindowIcon(studyapp::ui::applicationIcon());

    studyapp::platform::installQtLogSink();

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption workspaceOption(
        QStringLiteral("workspace"),
        QStringLiteral("Workspace directory to open (created if it does not exist)."),
        QStringLiteral("directory"));
    const QCommandLineOption generateOption(
        QStringLiteral("bench-generate"),
        QStringLiteral("Development: add <count> synthetic strokes to an empty start page."),
        QStringLiteral("count"));
    const QCommandLineOption panOption(
        QStringLiteral("bench-pan"),
        QStringLiteral("Development: pan for <frames> frames, print timings and quit."),
        QStringLiteral("frames"));
    const QCommandLineOption screenshotOption(
        QStringLiteral("screenshot"),
        QStringLiteral("Development: save the rendered canvas to <file> after start-up and quit."),
        QStringLiteral("file"));
    const QCommandLineOption benchZoomOption(
        QStringLiteral("bench-zoom"),
        QStringLiteral("Development: zoom factor applied before --bench-pan; 0 = zoom to fit "
                       "(the whole page in view)."),
        QStringLiteral("factor"), QStringLiteral("1"));
    parser.addOptions(
        {workspaceOption, generateOption, panOption, screenshotOption, benchZoomOption});
    parser.process(app);

    QSettings settings; // per-user, per-machine UI state (window geometry, theme)
    studyapp::ui::ThemeManager themes;

    const studyapp::core::SystemClock clock;
    studyapp::core::UuidV7Generator ids(clock);
    studyapp::platform::QtWorkspaceLocker locker;

    const studyapp::ui::ShellServices services{.clock = clock, .ids = ids, .locker = locker};
    auto window = std::make_unique<studyapp::ui::MainWindow>(themes, settings, services);
    // Development runs (benchmarks, screenshots) leave the recent/start-up workspace alone.
    window->setRememberWorkspaces(!parser.isSet(generateOption) && !parser.isSet(panOption) &&
                                  !parser.isSet(screenshotOption));
    window->show();

    // Start-up workspace: --workspace (created if missing), else the one open when the app
    // last quit, else — on first start — the default workspace in the per-user app data
    // (created with a start page). A workspace closed on purpose starts on the welcome
    // screen instead.
    using OpenMode = studyapp::ui::MainWindow::OpenMode;
    if (parser.isSet(workspaceOption)) {
        (void)window->openWorkspace(toPath(parser.value(workspaceOption)),
                                    OpenMode::CreateIfMissing);
    } else if (const auto last = window->lastWorkspace()) {
        (void)window->openWorkspace(*last);
    } else if (window->recentWorkspaces().isEmpty()) {
        (void)window->openWorkspace(defaultWorkspacePath(), OpenMode::CreateIfMissing);
    }

    if (parser.isSet(generateOption)) {
        WorkspaceSession* session = window->session();
        const auto page = window->activePage();
        if (session != nullptr && page) {
            const auto layers = session->workspace().layersOf(*page);
            const bool empty =
                layers.empty() || session->workspace().elementsOf(layers.back()).empty();
            if (empty) {
                generateStrokes(*session, *page, parser.value(generateOption).toInt(), ids);
            }
        }
    }

    if (parser.isSet(panOption)) {
        const int frames = parser.value(panOption).toInt();
        const double zoom = parser.value(benchZoomOption).toDouble();
        QTimer::singleShot(500, window.get(), [&window, frames, zoom] {
            window->runPanBenchmark(frames, zoom, [](const studyapp::ui::PanBenchmarkResult& r) {
                const auto n = [](double value) {
                    return QString::number(value, 'f', 2).toStdString();
                };
                studyapp::core::logInfo(
                    "bench", "pan: " + std::to_string(r.frames) + " frames, " +
                                 std::to_string(r.sceneElements) + " elements, " +
                                 std::to_string(r.averageDrawItems) +
                                 " draw items/frame; interval avg " + n(r.averageIntervalMs) +
                                 " median " + n(r.medianIntervalMs) + " p95 " + n(r.p95IntervalMs) +
                                 " p99 " + n(r.p99IntervalMs) + " worst " + n(r.worstIntervalMs) +
                                 " jitter " + n(r.intervalStdDevMs) + " ms; cpu avg " +
                                 n(r.averageCpuMs) + " p95 " + n(r.p95CpuMs) + " worst " +
                                 n(r.worstCpuMs) + " ms; gpu avg " + n(r.averageGpuMs) + " p95 " +
                                 n(r.p95GpuMs) + " ms");
                QApplication::quit();
            });
        });
    }

    if (parser.isSet(screenshotOption)) {
        const QString file = parser.value(screenshotOption);
        QTimer::singleShot(800, window.get(), [&window, file] {
            const bool saved = window->grabCanvas().save(file);
            studyapp::core::logInfo("app", std::string(saved ? "saved " : "could not save ") +
                                               file.toStdString());
            QApplication::quit();
        });
    }

    studyapp::core::logInfo("app", "started");
    const int exitCode = QApplication::exec();
    window.reset(); // closes the workspace (it was closed already if the window was)
    studyapp::core::logInfo("app", "exited cleanly");
    return exitCode;
}

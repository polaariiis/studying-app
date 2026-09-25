// StudyBoard composition root: creates the application object, the adapters, the open
// workspace and the main window, wires them together by constructor injection and runs
// the event loop.

#include <studyapp/application/StartPage.hpp>
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
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTimer>

#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using studyapp::application::AccessMode;
using studyapp::application::LockState;
using studyapp::application::OpenOptions;
using studyapp::application::SessionServices;
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

/// Opens (or creates) the workspace. Lock handling follows docs/ARCHITECTURE.md §11.1:
/// held by a live process → read-only; left behind by a crash → ask to recover.
std::unique_ptr<WorkspaceSession> openWorkspace(const std::filesystem::path& root,
                                                SessionServices services) {
    std::error_code ec;
    if (!std::filesystem::exists(root / "workspace.db", ec)) {
        auto created = WorkspaceSession::create(root, "My Workspace", services);
        if (!created) {
            QMessageBox::critical(nullptr, QObject::tr("StudyBoard"),
                                  QObject::tr("The workspace could not be created:\n%1")
                                      .arg(toQString(created.error().message)));
            return nullptr;
        }
        return std::move(*created);
    }
    OpenOptions options;
    if (auto lock = WorkspaceSession::inspectLock(root, services.locker)) {
        if (lock->state == LockState::Active) {
            options.mode = AccessMode::ReadOnly;
            studyapp::core::logWarning("app", "workspace is open elsewhere; opening read-only");
        } else if (lock->state == LockState::Stale) {
            const auto answer = QMessageBox::question(
                nullptr, QObject::tr("StudyBoard"),
                QObject::tr("The workspace was not closed properly. Recover it (checks the "
                            "database first), or open it read-only?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (answer == QMessageBox::Yes) {
                options.recoverStaleLock = true;
            } else {
                options.mode = AccessMode::ReadOnly;
            }
        }
    }
    auto opened = WorkspaceSession::open(root, options, services);
    if (!opened) {
        QMessageBox::critical(nullptr, QObject::tr("StudyBoard"),
                              QObject::tr("The workspace could not be opened:\n%1")
                                  .arg(toQString(opened.error().message)));
        return nullptr;
    }
    return std::move(*opened);
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
        QStringLiteral("workspace"), QStringLiteral("Workspace directory to open or create."),
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
    const std::filesystem::path root = parser.isSet(workspaceOption)
                                           ? toPath(parser.value(workspaceOption))
                                           : defaultWorkspacePath();

    std::unique_ptr<WorkspaceSession> session =
        openWorkspace(root, SessionServices{.clock = clock, .ids = ids, .locker = locker});
    std::optional<studyapp::core::PageId> page;
    if (session) {
        auto start = studyapp::application::ensureStartPage(*session, clock, ids);
        if (start) {
            page = *start;
            if (parser.isSet(generateOption)) {
                const auto layers = session->workspace().layersOf(*page);
                const bool empty =
                    layers.empty() || session->workspace().elementsOf(layers.back()).empty();
                if (empty) {
                    generateStrokes(*session, *page, parser.value(generateOption).toInt(), ids);
                }
            }
        } else {
            studyapp::core::logError("app", "no page to show: " + start.error().message);
        }
    }

    std::unique_ptr<studyapp::ui::MainWindow> window;
    if (session && page) {
        window = std::make_unique<studyapp::ui::MainWindow>(
            themes, settings,
            studyapp::ui::WorkspaceContext{
                .session = *session, .ids = ids, .clock = clock, .page = *page});
    } else {
        window = std::make_unique<studyapp::ui::MainWindow>(themes, settings);
    }
    window->show();

    if (parser.isSet(panOption)) {
        const int frames = parser.value(panOption).toInt();
        const double zoom = parser.value(benchZoomOption).toDouble();
        QTimer::singleShot(500, window.get(), [&window, frames, zoom] {
            window->runPanBenchmark(frames, zoom, [](const studyapp::ui::PanBenchmarkResult& r) {
                studyapp::core::logInfo(
                    "bench",
                    "pan: " + std::to_string(r.frames) + " frames, " +
                        std::to_string(r.sceneElements) + " elements, " +
                        std::to_string(r.averageDrawItems) + " draw items/frame; interval avg " +
                        QString::number(r.averageIntervalMs, 'f', 2).toStdString() + " ms, p95 " +
                        QString::number(r.p95IntervalMs, 'f', 2).toStdString() + " ms, worst " +
                        QString::number(r.worstIntervalMs, 'f', 2).toStdString() + " ms; cpu avg " +
                        QString::number(r.averageCpuMs, 'f', 2).toStdString() + " ms, worst " +
                        QString::number(r.worstCpuMs, 'f', 2).toStdString() + " ms; gpu avg " +
                        QString::number(r.averageGpuMs, 'f', 2).toStdString() + " ms");
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
    window.reset(); // the window refers to the session: destroy it first
    if (session) {
        if (auto closed = session->close(); !closed) {
            studyapp::core::logError("app",
                                     "closing the workspace failed: " + closed.error().message);
        }
    }
    studyapp::core::logInfo("app", "exited cleanly");
    return exitCode;
}

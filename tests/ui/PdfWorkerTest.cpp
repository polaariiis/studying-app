// The PDF inspection worker and its client (docs/PDF_WORKER.md §19, D53): real processes —
// the test helper (pdf_worker_helper, the real worker code or one specific misbehaviour)
// and the built `studyapp --pdf-worker` — real channels, real job directories.

#include "PdfInspectionClient.hpp"
#include "SessionDocumentRasterizer.hpp"

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/IdGenerator.hpp>

#include <QFile>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace std::chrono_literals;
using studyapp::core::JobId;
using studyapp::ui::PdfJobFailure;
using studyapp::ui::PdfJobResult;
using studyapp::ui::PdfJobState;
using studyapp::ui::PdfWorkerOptions;

class PdfWorkerTest : public QObject {
    Q_OBJECT

public:
    PdfWorkerTest() : ids_(clock_) {}

private:
    QTemporaryDir dir_;
    studyapp::core::SystemClock clock_;
    studyapp::core::UuidV7Generator ids_;
    std::atomic<bool> noStop_{false};

    std::filesystem::path path(const QString& name) const {
        return std::filesystem::path(dir_.filePath(name).toStdU16String());
    }
    std::filesystem::path tempRoot() const { return path(QStringLiteral("jobs")); }

    /// A PDF with `pages` pages of `sizePt` points.
    std::filesystem::path writePdf(const QString& name, int pages, QSizeF sizePt = {400, 300}) {
        const QString file = dir_.filePath(name);
        QPdfWriter writer(file);
        writer.setResolution(72);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setPageSize(QPageSize(sizePt, QPageSize::Point));
        QPainter painter(&writer);
        for (int i = 0; i < pages; ++i) {
            if (i > 0) {
                writer.newPage();
            }
            painter.drawText(20, 40, QStringLiteral("page %1").arg(i + 1));
        }
        return path(name);
    }

    std::filesystem::path writeFile(const QString& name, const QByteArray& content) {
        QFile out(dir_.filePath(name));
        if (!out.open(QIODevice::WriteOnly)) {
            qFatal("cannot write %s", qPrintable(name));
        }
        out.write(content);
        return path(name);
    }

    PdfWorkerOptions helper(const QString& mode) const {
        PdfWorkerOptions options;
        options.program = QString::fromUtf8(STUDYAPP_PDF_WORKER_HELPER);
        options.arguments = {mode};
        options.tempRoot = tempRoot();
        return options;
    }

    PdfJobResult run(const std::filesystem::path& pdf, const PdfWorkerOptions& options) {
        return studyapp::ui::runPdfInspectionJob(pdf, JobId::generate(ids_), noStop_, options);
    }

    /// Nothing of any job is left: the job directories are removed on every path.
    bool noJobsLeft() const {
        std::error_code ec;
        return std::filesystem::is_empty(tempRoot(), ec);
    }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        std::filesystem::create_directories(tempRoot());
    }

    // The real worker code answers with the page sizes, exactly as in-process inspection.
    void aRealWorkerInspectsAStagedPdf() {
        const auto pdf = writePdf(QStringLiteral("two pages.pdf"), 2);
        const PdfJobResult result = run(pdf, helper(QStringLiteral("real")));
        QCOMPARE(result.state, PdfJobState::Succeeded);
        QVERIFY(result.reply.has_value());
        QCOMPARE(result.reply->pages.size(), std::size_t{2});
        QCOMPARE(result.reply->pages[0].width, 400.0);
        QVERIFY(!std::filesystem::exists(result.directory));
        auto inWorker = studyapp::ui::inspectPdfInWorker(pdf, JobId::generate(ids_), noStop_,
                                                         helper(QStringLiteral("real")));
        auto local = studyapp::ui::inspectPdf(pdf);
        QVERIFY(inWorker.has_value() && local.has_value());
        QVERIFY(inWorker->pageSizes == local->pageSizes);
        QVERIFY(noJobsLeft());
    }

    // The built application in worker mode (QCoreApplication, no GUI platform).
    void studyappItselfIsTheWorker() {
        const auto pdf = writePdf(QStringLiteral("app.pdf"), 3, {595, 842});
        PdfWorkerOptions options;
        options.program = QString::fromUtf8(STUDYAPP_EXECUTABLE);
        options.tempRoot = tempRoot();
        const PdfJobResult result = run(pdf, options);
        QCOMPARE(result.state, PdfJobState::Succeeded);
        QCOMPARE(result.reply->pages.size(), std::size_t{3});
        QCOMPARE(result.reply->pages[2].height, 842.0);
        QVERIFY(noJobsLeft());
    }

    // PDFs the worker refuses, with today's messages (the same as in-process inspection).
    void theWorkerAppliesTheExistingRules() {
        const auto options = helper(QStringLiteral("real"));
        const auto check = [&](const std::filesystem::path& pdf, studyapp::core::ErrorCode code,
                               const char* message) {
            auto inWorker =
                studyapp::ui::inspectPdfInWorker(pdf, JobId::generate(ids_), noStop_, options);
            auto local = studyapp::ui::inspectPdf(pdf);
            QVERIFY(!inWorker.has_value() && !local.has_value());
            QVERIFY(inWorker.error().code == code);
            QCOMPARE(QString::fromStdString(inWorker.error().message),
                     QString::fromLatin1(message));
            QVERIFY(inWorker.error().message == local.error().message);
        };
        check(writeFile(QStringLiteral("text.pdf"), "just some text"),
              studyapp::core::ErrorCode::IoError, "the file is not a readable PDF document");
        check(writePdf(QStringLiteral("huge.pdf"), 1, {15000, 300}),
              studyapp::core::ErrorCode::InvalidArgument,
              "page 1 of the PDF has no supported size");
        // An encryption dictionary whose password check fails for the empty password.
        check(writeFile(QStringLiteral("protected.pdf"), protectedPdf()),
              studyapp::core::ErrorCode::Unsupported, "password-protected PDFs are not supported");
        check(writePdf(QStringLiteral("many.pdf"), studyapp::ui::kMaxPdfPages + 1, {20, 20}),
              studyapp::core::ErrorCode::InvalidArgument,
              "the PDF has 5001 pages; at most 5000 are supported");
        QVERIFY(noJobsLeft());
    }

    // Every way the worker can fail ends the job in a controlled failure, without using
    // anything it sent, and with the job directory removed.
    void failingWorkersFailTheJob() {
        const auto pdf = writePdf(QStringLiteral("one.pdf"), 1);
        PdfWorkerOptions missing = helper(QStringLiteral("ok"));
        missing.program = dir_.filePath(QStringLiteral("no such program"));
        QCOMPARE(run(pdf, missing).failure, PdfJobFailure::Start);
        QCOMPARE(run(pdf, helper(QStringLiteral("exit1"))).failure, PdfJobFailure::Exited);
        QCOMPARE(run(pdf, helper(QStringLiteral("noreply"))).failure, PdfJobFailure::Exited);
        for (const char* mode : {"crash", "replycrash"}) {
            const PdfJobResult crashed = run(pdf, helper(QString::fromLatin1(mode)));
            QCOMPARE(crashed.state, PdfJobState::Failed);
            // POSIX reports abort() as a crash, Windows as exit code 3.
            QVERIFY(crashed.failure == PdfJobFailure::Crashed ||
                    crashed.failure == PdfJobFailure::Exited);
            QVERIFY(!crashed.reply.has_value()); // a reply before the crash is not used
        }
        for (const char* mode : {"wrongjob", "garbage", "badstatus", "badsize"}) {
            const PdfJobResult bad = run(pdf, helper(QString::fromLatin1(mode)));
            QCOMPARE(bad.failure, PdfJobFailure::Protocol);
            QVERIFY(!bad.reply.has_value());
        }
        PdfWorkerOptions lingering = helper(QStringLiteral("replylinger"));
        lingering.exitTimeout = 300ms;
        QCOMPARE(run(pdf, lingering).failure, PdfJobFailure::Exited);
        QCOMPARE(run(pdf, helper(QStringLiteral("ok"))).state, PdfJobState::Succeeded);
        auto message = studyapp::ui::inspectPdfInWorker(pdf, JobId::generate(ids_), noStop_,
                                                        helper(QStringLiteral("crash")));
        QVERIFY(!message.has_value());
        QVERIFY(message.error().code == studyapp::core::ErrorCode::IoError);
        QVERIFY(noJobsLeft());
    }

    // A worker that hangs is killed at the deadline; a stop ends the job within one slice.
    void hangsTimeOutAndStopsCancel() {
        const auto pdf = writePdf(QStringLiteral("hang.pdf"), 1);
        PdfWorkerOptions options = helper(QStringLiteral("hang"));
        options.deadline = 1500ms;
        options.slice = 50ms;
        const auto started = std::chrono::steady_clock::now();
        const PdfJobResult timedOut = run(pdf, options);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        QCOMPARE(timedOut.failure, PdfJobFailure::Timeout);
        QVERIFY(elapsed >= 1500ms && elapsed < 5s);
        auto message =
            studyapp::ui::inspectPdfInWorker(pdf, JobId::generate(ids_), noStop_, options);
        QCOMPARE(QString::fromStdString(message.error().message),
                 QStringLiteral("the PDF could not be read in time"));

        options.deadline = 30s;
        std::atomic<bool> stop{false};
        std::thread stopper([&] {
            std::this_thread::sleep_for(500ms);
            stop.store(true);
        });
        const auto stopStarted = std::chrono::steady_clock::now();
        const PdfJobResult cancelled =
            studyapp::ui::runPdfInspectionJob(pdf, JobId::generate(ids_), stop, options);
        const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
        stopper.join();
        QCOMPARE(cancelled.state, PdfJobState::Cancelled);
        QVERIFY(stopElapsed < 3s); // the worker was killed, not waited for
        QVERIFY(noJobsLeft());
    }

    // The job directory stays short (MAX_PATH on Windows) or the job is refused.
    void jobDirectoriesAreShortAndPerJob() {
        const JobId job = JobId::generate(ids_);
        const auto directory = studyapp::ui::pdfJobDirectory(tempRoot(), job);
        QCOMPARE(directory.filename().string().size(), std::size_t{3 + 32});
        QVERIFY(directory.filename().string().starts_with("sb-"));
        PdfWorkerOptions deep = helper(QStringLiteral("ok"));
        deep.tempRoot = tempRoot() / std::string(190, 'd');
        const auto pdf = writePdf(QStringLiteral("deep.pdf"), 1);
        QCOMPARE(run(pdf, deep).failure, PdfJobFailure::Setup);
    }

    // Two imports at once: separate ids, directories, channels and answers.
    void concurrentJobsDoNotMix() {
        const auto two = writePdf(QStringLiteral("c2.pdf"), 2);
        const auto five = writePdf(QStringLiteral("c5.pdf"), 5);
        const JobId first = JobId::generate(ids_);
        const JobId second = JobId::generate(ids_);
        QVERIFY(first != second);
        QVERIFY(studyapp::ui::pdfJobDirectory(tempRoot(), first) !=
                studyapp::ui::pdfJobDirectory(tempRoot(), second));
        const auto options = helper(QStringLiteral("real"));
        PdfJobResult a;
        PdfJobResult b;
        std::thread one(
            [&] { a = studyapp::ui::runPdfInspectionJob(two, first, noStop_, options); });
        std::thread other(
            [&] { b = studyapp::ui::runPdfInspectionJob(five, second, noStop_, options); });
        one.join();
        other.join();
        QCOMPARE(a.state, PdfJobState::Succeeded);
        QCOMPARE(b.state, PdfJobState::Succeeded);
        QVERIFY(a.reply->job == first);
        QVERIFY(b.reply->job == second);
        QCOMPARE(a.reply->pages.size(), std::size_t{2});
        QCOMPARE(b.reply->pages.size(), std::size_t{5});
        QVERIFY(noJobsLeft());
    }

private:
    /// A one-page PDF with a standard security handler whose /U entry does not match the
    /// empty user password: PDFium reports it as needing a password.
    static QByteArray protectedPdf() {
        QByteArray pdf = "%PDF-1.4\n";
        QList<qsizetype> offsets;
        const auto object = [&](const QByteArray& body) {
            offsets.append(pdf.size());
            pdf += QByteArray::number(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
        };
        object("<< /Type /Catalog /Pages 2 0 R >>");
        object("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
        object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>");
        object("<< /Filter /Standard /V 1 /R 2 /P -4 /O <" + QByteArray(64, 'A') + "> /U <" +
               QByteArray(64, 'B') + "> >>");
        const qsizetype xref = pdf.size();
        pdf += "xref\n0 5\n0000000000 65535 f \n";
        for (const qsizetype offset : offsets) {
            pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        }
        pdf += "trailer\n<< /Size 5 /Root 1 0 R /Encrypt 4 0 R /ID [<" + QByteArray(32, 'C') +
               "> <" + QByteArray(32, 'C') + ">] >>\nstartxref\n" + QByteArray::number(xref) +
               "\n%%EOF\n";
        return pdf;
    }
};

QTEST_MAIN(PdfWorkerTest)
#include "PdfWorkerTest.moc"

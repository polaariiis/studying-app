// The PDF inspection worker and its client (docs/PDF_WORKER.md §19, D53): real processes —
// the test helper (pdf_worker_helper, the real worker code or one specific misbehaviour)
// and the built `studyapp --pdf-worker` — real channels, real job directories. Since
// 1.2-CMD-01 also text extraction (§23) with real PDFs whose text is known.

#include "PdfInspectionClient.hpp"
#include "PdfText.hpp"
#include "SessionDocumentRasterizer.hpp"

#include <studyapp/application/PdfTextSearch.hpp>
#include <studyapp/core/Clock.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/testing/MinimalPdf.hpp>

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

    // ---- text extraction (1.2-CMD-01, docs/PDF_WORKER.md §23)

private:
    /// A PDF whose pages hold these lines (real text in Helvetica, no fonts needed).
    std::filesystem::path writeTextPdf(const QString& name,
                                       const std::vector<std::vector<std::string>>& pages) {
        const std::string bytes = studyapp::testing::minimalPdf(pages);
        return writeFile(name, QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
    }

    studyapp::core::Result<studyapp::application::PdfDocumentText>
    extract(const std::filesystem::path& pdf, const PdfWorkerOptions& options) {
        return studyapp::ui::extractPdfTextInWorker(pdf, JobId::generate(ids_), noStop_, options);
    }

    const std::vector<std::vector<std::string>> lecture_{
        {"Linear Algebra - Lecture 7", "Every square matrix has a characteristic polynomial."},
        {"The eigenvalue of matrix A is 3.", "Eigenvectors belong to an Eigenvalue."},
        {"Signals (part 2): the Fourier", "transform maps f(x) to F(k)."},
    };

private Q_SLOTS:
    // The built application reads the text of a real PDF: every page, in order.
    void studyappExtractsTheTextOfARealPdf() {
        const auto pdf = writeTextPdf(QStringLiteral("Linear Algebra.pdf"), lecture_);
        PdfWorkerOptions options;
        options.program = QString::fromUtf8(STUDYAPP_EXECUTABLE);
        options.tempRoot = tempRoot();
        const JobId job = JobId::generate(ids_);
        const PdfJobResult result = studyapp::ui::runPdfTextJob(pdf, job, noStop_, options);
        QCOMPARE(result.state, PdfJobState::Succeeded);
        QVERIFY(result.text.has_value());
        QVERIFY(!result.reply.has_value());
        QVERIFY(result.text->job == job); // the reply belongs to this job
        QCOMPARE(result.text->pages.size(), std::size_t{3});
        QVERIFY(result.text->pages[0].find("characteristic polynomial") != std::string::npos);
        QVERIFY(result.text->pages[1].find("eigenvalue of matrix A") != std::string::npos);
        QVERIFY(result.text->pages[2].find("f(x)") != std::string::npos);
        QCOMPARE(result.text->detail, std::uint32_t{0});
        QVERIFY(!std::filesystem::exists(result.directory));
        QVERIFY(noJobsLeft());
    }

    // What `pdf search` finds in real extracted text: words, phrases across lines, any case,
    // punctuation, several pages, the right page numbers in order.
    void extractedTextIsSearchable() {
        const auto pdf = writeTextPdf(QStringLiteral("search.pdf"), lecture_);
        auto text = extract(pdf, helper(QStringLiteral("real")));
        QVERIFY2(text.has_value(), text ? "" : text.error().message.c_str());
        using studyapp::application::findInPdfText;
        const auto pages = [](const std::vector<studyapp::application::PdfTextMatch>& found) {
            std::vector<std::size_t> out;
            for (const auto& match : found) {
                out.push_back(match.page);
            }
            return out;
        };
        QVERIFY(pages(findInPdfText(*text, "eigenvalue")) == (std::vector<std::size_t>{1, 1}));
        QVERIFY(pages(findInPdfText(*text, "MATRIX")) == (std::vector<std::size_t>{0, 1}));
        QVERIFY(pages(findInPdfText(*text, "Fourier transform")) == std::vector<std::size_t>{2});
        QVERIFY(pages(findInPdfText(*text, "f(x)")) == std::vector<std::size_t>{2});
        QVERIFY(pages(findInPdfText(*text, "(part 2):")) == std::vector<std::size_t>{2});
        QVERIFY(findInPdfText(*text, "quaternion").empty());
        const auto first = findInPdfText(*text, "eigenvalue");
        QVERIFY(first[0].context.find("eigenvalue of matrix A") != std::string::npos);
        QVERIFY(first[0].context.size() < 120); // context, not the whole page
        // The same as reading it in this process (the worker adds isolation, not changes).
        const auto local = studyapp::ui::pdfTextFrom(studyapp::ui::extractPdfTextLocally(pdf));
        QVERIFY(local.has_value());
        QVERIFY(local->pages == text->pages);
        QVERIFY(noJobsLeft());
    }

    // Unreadable files fail with the import's wording; text over the limits is cut, marked.
    void textExtractionAppliesTheRules() {
        const auto options = helper(QStringLiteral("real"));
        auto notPdf = extract(writeFile(QStringLiteral("plain.pdf"), "just some text"), options);
        QVERIFY(!notPdf.has_value());
        QCOMPARE(QString::fromStdString(notPdf.error().message),
                 QStringLiteral("the file is not a readable PDF document"));
        auto missing = extract(path(QStringLiteral("does not exist.pdf")), options);
        QVERIFY(!missing.has_value());
        QVERIFY(missing.error().code == studyapp::core::ErrorCode::IoError);
        auto locked = extract(writeFile(QStringLiteral("locked.pdf"), protectedPdf()), options);
        QVERIFY(!locked.has_value());
        QVERIFY(locked.error().code == studyapp::core::ErrorCode::Unsupported);
        // A page without text (a scanned page) is an empty string, not an error.
        auto blank = extract(writePdf(QStringLiteral("blank.pdf"), 2), options);
        QVERIFY(blank.has_value());
        QCOMPARE(blank->pages.size(), std::size_t{2});
        // More than 64 KiB of text on one page is cut at a character boundary.
        std::vector<std::string> lines(1200, std::string(80, 'w'));
        auto big = extract(writeTextPdf(QStringLiteral("big.pdf"), {lines}), options);
        QVERIFY2(big.has_value(), big ? "" : big.error().message.c_str());
        QVERIFY(big->truncated);
        QVERIFY(big->pages[0].size() <= studyapp::ipc::pdf::kMaxPageTextBytes);
        QVERIFY(noJobsLeft());
    }

    // Text jobs fail like inspection jobs: crash, hang, cancel, another job's or another
    // kind's reply, garbage — never using what the worker sent, never leaving files.
    void failingTextJobsFailCleanly() {
        const auto pdf = writeTextPdf(QStringLiteral("fail.pdf"), lecture_);
        const auto runText = [&](const PdfWorkerOptions& options) {
            return studyapp::ui::runPdfTextJob(pdf, JobId::generate(ids_), noStop_, options);
        };
        const PdfJobResult ok = runText(helper(QStringLiteral("ok")));
        QCOMPARE(ok.state, PdfJobState::Succeeded);
        QCOMPARE(ok.text->pages.size(), std::size_t{2});
        const PdfJobResult crashed = runText(helper(QStringLiteral("crash")));
        QVERIFY(crashed.failure == PdfJobFailure::Crashed ||
                crashed.failure == PdfJobFailure::Exited);
        auto stopped = extract(pdf, helper(QStringLiteral("crash")));
        QCOMPARE(QString::fromStdString(stopped.error().message),
                 QStringLiteral("the PDF could not be read (the reader stopped)"));
        const PdfJobResult afterReply = runText(helper(QStringLiteral("replycrash")));
        QVERIFY(!afterReply.text.has_value());
        for (const char* mode : {"wrongjob", "otherkind", "garbage", "badstatus"}) {
            const PdfJobResult bad = runText(helper(QString::fromLatin1(mode)));
            QCOMPARE(bad.failure, PdfJobFailure::Protocol);
            QVERIFY(!bad.text.has_value());
        }
        // Inspection jobs refuse a text reply in the same way.
        QCOMPARE(run(pdf, helper(QStringLiteral("otherkind"))).failure, PdfJobFailure::Protocol);

        PdfWorkerOptions hanging = helper(QStringLiteral("hang"));
        hanging.deadline = 1000ms;
        hanging.slice = 50ms;
        auto late = extract(pdf, hanging);
        QCOMPARE(QString::fromStdString(late.error().message),
                 QStringLiteral("the PDF could not be read in time"));

        hanging.deadline = 30s;
        std::atomic<bool> stop{false};
        std::thread stopper([&] {
            std::this_thread::sleep_for(300ms);
            stop.store(true);
        });
        const auto started = std::chrono::steady_clock::now();
        auto cancelled =
            studyapp::ui::extractPdfTextInWorker(pdf, JobId::generate(ids_), stop, hanging);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        stopper.join();
        QVERIFY(!cancelled.has_value());
        QVERIFY(cancelled.error().code == studyapp::core::ErrorCode::Conflict);
        QVERIFY(elapsed < 3s);
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

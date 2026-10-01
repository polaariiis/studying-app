#include "PdfInspectionClient.hpp"

#include <studyapp/core/Log.hpp>
#include <studyapp/ipc/FileChannel.hpp>
#include <studyapp/ui/PdfWorker.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QProcess>

#include <algorithm>
#include <string>
#include <utility>

namespace studyapp::ui {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

/// Longest job directory path (characters): IPCFileLab limits a channel's directory to
/// MAX_PATH on Windows (GetTempFileNameW), D53 §6.
constexpr std::size_t kMaxDirectoryLength = 200;
/// The worker's stderr kept for the log.
constexpr qsizetype kStderrBytes = 4096;

constexpr const char* kLog = "pdf-worker";

const char* name(PdfJobState state) {
    switch (state) {
    case PdfJobState::Preparing:
        return "preparing";
    case PdfJobState::Starting:
        return "starting";
    case PdfJobState::Waiting:
        return "waiting";
    case PdfJobState::Exiting:
        return "exiting";
    case PdfJobState::Succeeded:
        return "succeeded";
    case PdfJobState::Failed:
        return "failed";
    case PdfJobState::Cancelled:
        return "cancelled";
    }
    return "?";
}

const char* name(PdfJobFailure failure) {
    switch (failure) {
    case PdfJobFailure::None:
        return "none";
    case PdfJobFailure::Setup:
        return "setup";
    case PdfJobFailure::Start:
        return "start";
    case PdfJobFailure::Crashed:
        return "crashed";
    case PdfJobFailure::Exited:
        return "exited";
    case PdfJobFailure::Protocol:
        return "protocol";
    case PdfJobFailure::Timeout:
        return "timeout";
    }
    return "?";
}

int toInt(milliseconds value) {
    return static_cast<int>(std::clamp<milliseconds::rep>(value.count(), 0, 1'000'000'000));
}

/// One job: its directory, channels and worker process. The destructor is the only cleanup
/// (D53 §13) — it kills a worker that is still running, closes the channels and removes the
/// directory, whichever state the job ended in.
class PdfJob {
public:
    PdfJob(const std::filesystem::path& staged, core::JobId job, const std::atomic<bool>& stop,
           const PdfWorkerOptions& options)
        : staged_(staged), job_(job), stop_(stop), options_(options) {}

    PdfJob(const PdfJob&) = delete;
    PdfJob& operator=(const PdfJob&) = delete;
    PdfJob(PdfJob&&) = delete;
    PdfJob& operator=(PdfJob&&) = delete;

    ~PdfJob() {
        if (process_ && process_->state() != QProcess::NotRunning) {
            process_->kill();
            (void)process_->waitForFinished(1000);
        }
        process_.reset();
        requests_.reset();
        replies_.reset();
        if (created_) {
            std::error_code ec;
            std::filesystem::remove_all(result_.directory, ec);
            if (ec) {
                core::logWarning(kLog,
                                 "job " + job_.toString() +
                                     ": the job directory could not be removed: " + ec.message());
            }
        }
    }

    PdfJobResult run() {
        const auto started = Clock::now();
        if (prepare() && start()) {
            if (const bool answered = wait(); answered) {
                finishExit();
            }
        }
        const auto elapsed =
            std::chrono::duration_cast<milliseconds>(Clock::now() - started).count();
        std::string line = "job " + job_.toString() + " " + name(result_.state);
        if (result_.state == PdfJobState::Failed) {
            line +=
                std::string(" (") + name(result_.failure) + " while " + name(result_.reached) + ")";
        }
        if (process_) {
            line +=
                ", worker exit " +
                std::string(process_->exitStatus() == QProcess::CrashExit ? "crash" : "normal") +
                " code " + std::to_string(process_->exitCode());
        }
        line += ", " + std::to_string(elapsed) + " ms";
        if (result_.reply) {
            line += ", status " + std::to_string(static_cast<int>(result_.reply->status)) + ", " +
                    std::to_string(result_.reply->pages.size()) + " pages";
        }
        if (result_.state == PdfJobState::Failed && process_) {
            const QByteArray err = process_->readAllStandardError().left(kStderrBytes);
            if (!err.isEmpty()) {
                line += "; worker stderr: " + err.toStdString();
            }
        }
        if (result_.state == PdfJobState::Failed) {
            core::logWarning(kLog, line);
        } else {
            core::logInfo(kLog, line);
        }
        return result_;
    }

private:
    void fail(PdfJobFailure failure) {
        result_.state = PdfJobState::Failed;
        result_.failure = failure;
    }

    bool prepare() {
        result_.reached = PdfJobState::Preparing;
        const std::filesystem::path root =
            options_.tempRoot.empty() ? std::filesystem::path(QDir::tempPath().toStdU16String())
                                      : options_.tempRoot;
        result_.directory = pdfJobDirectory(root, job_);
        core::logInfo(kLog, "job " + job_.toString() + " started for " +
                                staged_.filename().string()); // an asset id, not a user path
        if (result_.directory.u16string().size() > kMaxDirectoryLength) {
            fail(PdfJobFailure::Setup);
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::create_directory(result_.directory, ec) || ec) {
            fail(PdfJobFailure::Setup); // never an existing directory: one per job
            return false;
        }
        created_ = true;
        std::filesystem::permissions(result_.directory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        auto requests = ipc::FileChannel::open(result_.directory / "request.ipc");
        auto replies = ipc::FileChannel::open(result_.directory / "response.ipc");
        if (!requests || !replies) {
            fail(PdfJobFailure::Setup);
            return false;
        }
        requests_.emplace(std::move(*requests));
        replies_.emplace(std::move(*replies));
        // The request waits in its single slot until the worker reads it: no start-up race.
        const std::u8string path = staged_.u8string();
        const ipc::pdf::InspectRequest request{.job = job_,
                                               .path = std::string(path.begin(), path.end())};
        if (!requests_->send(ipc::pdf::encode(request), milliseconds(1000))) {
            fail(PdfJobFailure::Setup);
            return false;
        }
        return true;
    }

    bool start() {
        result_.reached = PdfJobState::Starting;
        deadline_ = Clock::now() + options_.deadline;
        process_.emplace();
        const QString program =
            options_.program.isEmpty() ? QCoreApplication::applicationFilePath() : options_.program;
        QStringList arguments = options_.arguments;
        arguments << QString::fromLatin1(kPdfWorkerArgument)
                  << QString::fromStdU16String(result_.directory.u16string());
        process_->setProgram(program);
        process_->setArguments(arguments);
        process_->setWorkingDirectory(QString::fromStdU16String(result_.directory.u16string()));
        process_->setStandardInputFile(QProcess::nullDevice());
        process_->setStandardOutputFile(QProcess::nullDevice());
        process_->start();
        if (!process_->waitForStarted(toInt(std::min(options_.startTimeout, remaining())))) {
            fail(PdfJobFailure::Start);
            return false;
        }
        return true;
    }

    milliseconds remaining() const {
        return std::max(milliseconds(0),
                        std::chrono::duration_cast<milliseconds>(deadline_ - Clock::now()));
    }

    /// Takes the reply out of the channel if one is there. False: the job failed.
    bool take(milliseconds timeout, bool& answered) {
        auto message = replies_->receive(timeout);
        if (!message) {
            fail(PdfJobFailure::Protocol);
            return false;
        }
        if (!*message) {
            return true; // nothing yet
        }
        auto reply = ipc::pdf::decodeReply(**message);
        if (!reply || reply->job != job_) {
            fail(PdfJobFailure::Protocol); // never partly used, never another job's
            return false;
        }
        result_.reply = std::move(*reply);
        answered = true;
        return true;
    }

    /// Waits for the reply. True: a valid reply is in result_.reply.
    bool wait() {
        result_.reached = PdfJobState::Waiting;
        for (;;) {
            if (stop_.load()) {
                result_.state = PdfJobState::Cancelled;
                result_.failure = PdfJobFailure::None;
                return false;
            }
            if (remaining() == milliseconds(0)) {
                fail(PdfJobFailure::Timeout);
                return false;
            }
            bool answered = false;
            if (!take(std::min(options_.slice, remaining()), answered)) {
                return false;
            }
            if (answered) {
                return true;
            }
            (void)process_->waitForFinished(0); // updates the process state
            if (process_->state() == QProcess::NotRunning) {
                // A reply written just before the worker exited is still in the channel.
                if (!take(milliseconds(0), answered)) {
                    return false;
                }
                if (answered) {
                    return true;
                }
                fail(process_->exitStatus() == QProcess::CrashExit ? PdfJobFailure::Crashed
                                                                   : PdfJobFailure::Exited);
                return false;
            }
        }
    }

    /// After a reply: the worker must end normally with code 0.
    void finishExit() {
        result_.reached = PdfJobState::Exiting;
        if (process_->state() != QProcess::NotRunning &&
            !process_->waitForFinished(toInt(options_.exitTimeout))) {
            result_.reply.reset();
            fail(PdfJobFailure::Exited);
            return;
        }
        if (process_->exitStatus() == QProcess::CrashExit || process_->exitCode() != 0) {
            result_.reply.reset();
            fail(process_->exitStatus() == QProcess::CrashExit ? PdfJobFailure::Crashed
                                                               : PdfJobFailure::Exited);
            return;
        }
        result_.state = PdfJobState::Succeeded;
        result_.failure = PdfJobFailure::None;
    }

    std::filesystem::path staged_;
    core::JobId job_;
    const std::atomic<bool>& stop_;
    const PdfWorkerOptions& options_;
    PdfJobResult result_;
    bool created_ = false;
    Clock::time_point deadline_{};
    std::optional<ipc::FileChannel> requests_;
    std::optional<ipc::FileChannel> replies_;
    std::optional<QProcess> process_; ///< last: destroyed (after the kill) before the channels
};

} // namespace

std::filesystem::path pdfJobDirectory(const std::filesystem::path& tempRoot, core::JobId job) {
    std::string hex = job.toString();
    std::erase(hex, '-');
    return tempRoot / ("sb-" + hex);
}

PdfJobResult runPdfInspectionJob(const std::filesystem::path& staged, core::JobId job,
                                 const std::atomic<bool>& stop, const PdfWorkerOptions& options) {
    PdfJob run(staged, job, stop, options);
    return run.run();
}

core::Result<PdfInfo> inspectPdfInWorker(const std::filesystem::path& staged, core::JobId job,
                                         const std::atomic<bool>& stop,
                                         const PdfWorkerOptions& options) {
    const PdfJobResult result = runPdfInspectionJob(staged, job, stop, options);
    switch (result.state) {
    case PdfJobState::Succeeded:
        return pdfInfoFrom(*result.reply);
    case PdfJobState::Cancelled:
        return core::makeError(core::ErrorCode::Conflict, "the import was cancelled");
    default:
        break;
    }
    switch (result.failure) {
    case PdfJobFailure::Crashed:
    case PdfJobFailure::Exited:
        return core::makeError(core::ErrorCode::IoError,
                               "the PDF could not be read (the reader stopped)");
    case PdfJobFailure::Timeout:
        return core::makeError(core::ErrorCode::IoError, "the PDF could not be read in time");
    default:
        return core::makeError(core::ErrorCode::IoError, "the PDF could not be read");
    }
}

} // namespace studyapp::ui

#include <studyapp/platform/QtWorkspaceLocker.hpp>

#include "os/ProcessInfo.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLockFile>
#include <QString>
#include <QSysInfo>

#include <string>
#include <utility>

namespace studyapp::platform {

using application::LockOwner;
using application::LockState;
using application::LockStatus;
using core::ErrorCode;
using core::makeError;
using core::Result;

namespace {

QString toQString(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                             static_cast<qsizetype>(text.size()));
}

std::string toStdString(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

class QtWorkspaceLock final : public application::WorkspaceLock {
public:
    explicit QtWorkspaceLock(std::unique_ptr<QLockFile> file) : file_(std::move(file)) {}
    ~QtWorkspaceLock() override { file_->unlock(); }
    QtWorkspaceLock(const QtWorkspaceLock&) = delete;
    QtWorkspaceLock& operator=(const QtWorkspaceLock&) = delete;
    QtWorkspaceLock(QtWorkspaceLock&&) = delete;
    QtWorkspaceLock& operator=(QtWorkspaceLock&&) = delete;

private:
    std::unique_ptr<QLockFile> file_;
};

std::string describe(const LockStatus& status) {
    if (!status.owner) {
        return "an unknown process";
    }
    return "pid " + std::to_string(status.owner->processId) + " on " + status.owner->hostName;
}

} // namespace

Result<LockStatus> QtWorkspaceLocker::inspect(const std::filesystem::path& lockFile) {
    const QString file = toQString(lockFile);
    if (!QFileInfo::exists(file)) {
        return LockStatus{.state = LockState::Free, .owner = std::nullopt};
    }
    QLockFile lock(file);
    qint64 pid = 0;
    QString host;
    QString application;
    if (!lock.getLockInfo(&pid, &host, &application)) {
        // Present but unreadable or malformed: no live QLockFile holder writes such a file.
        return LockStatus{.state = LockState::Stale, .owner = std::nullopt};
    }
    LockStatus status{.state = LockState::Active,
                      .owner = LockOwner{.processId = pid,
                                         .hostName = toStdString(host),
                                         .applicationName = toStdString(application)}};
    const bool sameHost = host.isEmpty() || host == QSysInfo::machineHostName();
    if (sameHost && pid != QCoreApplication::applicationPid() && !os::isProcessRunning(pid)) {
        status.state = LockState::Stale;
    }
    return status;
}

Result<std::unique_ptr<application::WorkspaceLock>>
QtWorkspaceLocker::acquire(const std::filesystem::path& lockFile, bool takeOverStale) {
    auto status = inspect(lockFile);
    if (!status) {
        return tl::unexpected<core::Error>(std::move(status.error()));
    }
    if (status->state == LockState::Active) {
        return makeError(ErrorCode::Conflict, "the workspace is locked by " + describe(*status));
    }
    auto file = std::make_unique<QLockFile>(toQString(lockFile));
    file->setStaleLockTime(0); // never stale by age; only by a vanished owner
    if (status->state == LockState::Stale) {
        if (!takeOverStale) {
            return makeError(ErrorCode::Conflict, "the workspace has a stale lock left by " +
                                                      describe(*status) +
                                                      "; recovery must be requested explicitly");
        }
        // A readable leftover lock is taken over by tryLock() itself: QLockFile re-checks that
        // the recorded owner is gone and removes the file under its own guard, so two
        // processes recovering at the same moment cannot both win (an unconditional
        // removeStaleLockFile() could delete the lock the other one just created). Only a
        // file without owner information — which tryLock() never treats as stale once
        // time-based staleness is off — is removed explicitly.
        if (!status->owner) {
            file->removeStaleLockFile();
        }
    }
    if (!file->tryLock(0)) {
        switch (file->error()) {
        case QLockFile::LockFailedError:
            return makeError(ErrorCode::Conflict, "the workspace is locked by another process");
        case QLockFile::PermissionError:
            return makeError(ErrorCode::IoError, "no permission to create the workspace lock file");
        case QLockFile::NoError:
        case QLockFile::UnknownError:
            break;
        }
        return makeError(ErrorCode::IoError, "the workspace lock could not be created");
    }
    return std::unique_ptr<application::WorkspaceLock>(
        std::make_unique<QtWorkspaceLock>(std::move(file)));
}

} // namespace studyapp::platform

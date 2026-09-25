// Tests the QLockFile-backed workspace lock (docs/ARCHITECTURE.md §11.1) with real lock
// files in a temporary directory.

#include <studyapp/platform/QtWorkspaceLocker.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QList>
#include <QTemporaryDir>
#include <QTest>

#include <filesystem>

using studyapp::application::LockState;
using studyapp::platform::QtWorkspaceLocker;

namespace {

std::filesystem::path lockPath(const QTemporaryDir& dir) {
    return std::filesystem::path(dir.filePath(QStringLiteral(".lock")).toStdU16String());
}

/// Replaces line `index` (0 = pid, 1 = application, 2 = host) of a QLockFile lock file.
QByteArray withLine(QByteArray content, qsizetype index, const QByteArray& value) {
    QList<QByteArray> lines = content.split('\n');
    if (index < lines.size()) {
        lines[index] = value;
    }
    return lines.join('\n');
}

/// Writes a lock file that looks like one left behind by another process.
QByteArray leftoverLock(const std::filesystem::path& path) {
    QtWorkspaceLocker locker;
    QByteArray content;
    {
        auto lock = locker.acquire(path, false);
        if (!lock) {
            return {};
        }
        QFile file(QString::fromStdU16String(path.u16string()));
        if (file.open(QIODevice::ReadOnly)) {
            content = file.readAll();
        }
    } // released: QLockFile removes the file
    return content;
}

void writeFile(const std::filesystem::path& path, const QByteArray& content) {
    QFile file(QString::fromStdU16String(path.u16string()));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(content), static_cast<qint64>(content.size()));
}

// Larger than any real process id on Windows (multiples of 4 below 2^32 are reused, but
// this one is odd), Linux (pid_max <= 2^22) and macOS (<= 99998).
const QByteArray kDeadPid = "2147483601";

} // namespace

class QtWorkspaceLockerTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void freeWithoutLockFile() {
        QTemporaryDir dir;
        QtWorkspaceLocker locker;
        auto status = locker.inspect(lockPath(dir));
        QVERIFY(status.has_value());
        QCOMPARE(status->state, LockState::Free);
    }

    void acquiredLockIsActiveAndExclusive() {
        QTemporaryDir dir;
        QtWorkspaceLocker locker;
        auto lock = locker.acquire(lockPath(dir), false);
        QVERIFY(lock.has_value());

        auto status = locker.inspect(lockPath(dir));
        QVERIFY(status.has_value());
        QCOMPARE(status->state, LockState::Active);
        QVERIFY(status->owner.has_value());
        QCOMPARE(static_cast<qint64>(status->owner->processId), QCoreApplication::applicationPid());

        // Neither a second acquire nor an explicit take-over succeeds against a live lock.
        QVERIFY(!locker.acquire(lockPath(dir), false).has_value());
        QVERIFY(!locker.acquire(lockPath(dir), true).has_value());
    }

    void releasingFreesTheLock() {
        QTemporaryDir dir;
        QtWorkspaceLocker locker;
        {
            auto lock = locker.acquire(lockPath(dir), false);
            QVERIFY(lock.has_value());
        }
        auto status = locker.inspect(lockPath(dir));
        QVERIFY(status.has_value());
        QCOMPARE(status->state, LockState::Free);
        QVERIFY(locker.acquire(lockPath(dir), false).has_value());
    }

    void leftoverLockOfDeadProcessIsStale() {
        QTemporaryDir dir;
        const auto path = lockPath(dir);
        const QByteArray content = leftoverLock(path);
        QVERIFY(!content.isEmpty());
        writeFile(path, withLine(content, 0, kDeadPid));

        QtWorkspaceLocker locker;
        auto status = locker.inspect(path);
        QVERIFY(status.has_value());
        QCOMPARE(status->state, LockState::Stale);
        QVERIFY(status->owner.has_value());
        QCOMPARE(static_cast<qint64>(status->owner->processId), kDeadPid.toLongLong());

        // Inspecting does not change anything; recovery must be requested explicitly.
        QVERIFY(QFile::exists(QString::fromStdU16String(path.u16string())));
        QVERIFY(!locker.acquire(path, false).has_value());
        auto recovered = locker.acquire(path, true);
        QVERIFY(recovered.has_value());
        QCOMPARE(locker.inspect(path)->state, LockState::Active);
    }

    void lockOfAnotherHostIsNeverStale() {
        QTemporaryDir dir;
        const auto path = lockPath(dir);
        const QByteArray content = leftoverLock(path);
        QVERIFY(!content.isEmpty());
        writeFile(path, withLine(withLine(content, 0, kDeadPid), 2, "another-host.example"));

        QtWorkspaceLocker locker;
        auto status = locker.inspect(path);
        QVERIFY(status.has_value());
        QCOMPARE(status->state, LockState::Active);
        QVERIFY(status->owner->hostName == "another-host.example");
        QVERIFY(!locker.acquire(path, true).has_value());
    }

    void unreadableLockFileIsStale() {
        QTemporaryDir dir;
        const auto path = lockPath(dir);
        writeFile(path, "garbage without owner information");
        QtWorkspaceLocker locker;
        auto status = locker.inspect(path);
        QVERIFY(status.has_value());
        QCOMPARE(status->state, LockState::Stale);
        QVERIFY(!status->owner.has_value());
        QVERIFY(locker.acquire(path, true).has_value());
    }
};

QTEST_GUILESS_MAIN(QtWorkspaceLockerTest)
#include "QtWorkspaceLockerTest.moc"

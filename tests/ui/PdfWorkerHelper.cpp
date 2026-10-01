// A stand-in for `studyapp --pdf-worker` (docs/PDF_WORKER.md §19): the client starts it as
// `pdf_worker_helper <mode> --pdf-worker <job-directory>`. Mode "real" runs the actual worker
// code (ui::runPdfWorker); the others misbehave in one specific way so the client's
// handling of crashes, hangs and bad replies can be tested.

#include <studyapp/core/Ids.hpp>
#include <studyapp/ipc/FileChannel.hpp>
#include <studyapp/ipc/PdfInspection.hpp>
#include <studyapp/ui/PdfWorker.hpp>

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace std::chrono_literals;
using studyapp::ipc::FileChannel;
namespace pdf = studyapp::ipc::pdf;

/// Dies like a crashing parser, without a crash dialog (Windows reports abort() as exit
/// code 3, POSIX as SIGABRT; the client treats both as a failed worker).
[[noreturn]] void crash() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
}

pdf::InspectReply okReply(studyapp::core::JobId job) {
    return {.job = job,
            .status = pdf::InspectStatus::Ok,
            .detail = 0,
            .pages = {{.width = 612.0, .height = 792.0}, {.width = 595.0, .height = 842.0}}};
}

} // namespace

int main(int argc, char* argv[]) {
    const QCoreApplication app(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() != 4 ||
        arguments.at(2) != QString::fromLatin1(studyapp::ui::kPdfWorkerArgument)) {
        return 64;
    }
    const QString mode = arguments.at(1);
    const QString directory = arguments.at(3);
    if (mode == QStringLiteral("real")) {
        return studyapp::ui::runPdfWorker(directory);
    }
    if (mode == QStringLiteral("exit1")) {
        return 1; // before reading anything
    }
    const std::filesystem::path dir(directory.toStdU16String());
    auto requests = FileChannel::open(dir / "request.ipc");
    auto replies = FileChannel::open(dir / "response.ipc");
    if (!requests || !replies) {
        return 1;
    }
    auto message = requests->receive(10s);
    if (!message || !*message) {
        return 1;
    }
    const auto request = pdf::decodeRequest(**message);
    if (!request) {
        return 2;
    }
    if (mode == QStringLiteral("crash")) {
        crash();
    }
    if (mode == QStringLiteral("noreply")) {
        return 0;
    }
    if (mode == QStringLiteral("hang")) {
        std::this_thread::sleep_for(120s); // killed by the client long before
        return 0;
    }
    std::vector<std::byte> bytes = pdf::encode(okReply(request->job));
    if (mode == QStringLiteral("wrongjob")) {
        auto job = request->job.value().bytes();
        job[15] = static_cast<std::uint8_t>(job[15] ^ 0xFFU);
        bytes = pdf::encode(okReply(studyapp::core::JobId{studyapp::core::Uuid(job)}));
    } else if (mode == QStringLiteral("garbage")) {
        const std::string text = "not a reply at all";
        bytes.assign(reinterpret_cast<const std::byte*>(text.data()),
                     reinterpret_cast<const std::byte*>(text.data() + text.size()));
    } else if (mode == QStringLiteral("badstatus")) {
        bytes[6] = std::byte{99}; // an unknown status, otherwise a valid reply
    } else if (mode == QStringLiteral("badsize")) {
        pdf::InspectReply reply = okReply(request->job);
        reply.pages[1].height = 20000.0; // larger than any supported page
        bytes = pdf::encode(reply);
    }
    if (!replies->send(bytes, 5s)) {
        return 1;
    }
    if (mode == QStringLiteral("replycrash")) {
        crash(); // after a valid reply: the client must not use it
    }
    if (mode == QStringLiteral("replylinger")) {
        std::this_thread::sleep_for(120s); // replies but never exits
    }
    return 0; // "ok", "wrongjob", "garbage", "badstatus", "badsize"
}

#include <studyapp/ui/PdfWorker.hpp>

#include "PdfText.hpp"
#include "SessionDocumentRasterizer.hpp"

#include <studyapp/ipc/FileChannel.hpp>
#include <studyapp/ipc/PdfInspection.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace studyapp::ui {

namespace {

using namespace std::chrono_literals;

constexpr auto kRequestWait = 10s;
constexpr auto kSelfLimit = 60s;
constexpr auto kReplySendWait = 5s;

/// Ends the process with code 3 if the worker is still busy after kSelfLimit (a PDF that
/// makes PDFium loop, or a parent that died and will never kill this process).
class SelfLimit {
public:
    SelfLimit()
        : thread_([this] {
              std::unique_lock lock(mutex_);
              if (!done_.wait_for(lock, kSelfLimit, [this] { return finished_; })) {
                  std::_Exit(3);
              }
          }) {}
    ~SelfLimit() {
        {
            const std::lock_guard lock(mutex_);
            finished_ = true;
        }
        done_.notify_all();
        thread_.join();
    }
    SelfLimit(const SelfLimit&) = delete;
    SelfLimit& operator=(const SelfLimit&) = delete;
    SelfLimit(SelfLimit&&) = delete;
    SelfLimit& operator=(SelfLimit&&) = delete;

private:
    std::mutex mutex_;
    std::condition_variable done_;
    bool finished_ = false;
    std::thread thread_; ///< last: started once the members it uses exist
};

} // namespace

int runPdfWorker(const QString& jobDirectory) {
    const SelfLimit limit;
    const std::filesystem::path directory(jobDirectory.toStdU16String());
    std::error_code ec;
    if (jobDirectory.isEmpty() || !std::filesystem::is_directory(directory, ec)) {
        return 1;
    }
    auto requests = ipc::FileChannel::open(directory / "request.ipc");
    auto replies = ipc::FileChannel::open(directory / "response.ipc");
    if (!requests || !replies) {
        return 1;
    }
    auto message = requests->receive(kRequestWait);
    if (!message) {
        return 1;
    }
    if (!*message) {
        return 3;
    }
    const std::span<const std::byte> bytes(**message);
    auto request = ipc::pdf::decodeRequest(bytes);
    if (!request) {
        const ipc::pdf::InspectReply refusal{
            .job = ipc::pdf::requestJob(bytes),
            .status = request.error().code == core::ErrorCode::Unsupported
                          ? ipc::pdf::InspectStatus::UnsupportedRequest
                          : ipc::pdf::InspectStatus::InvalidRequest,
            .detail = 0,
            .pages = {}};
        if (!refusal.job.isNull()) { // a reply without a job id would be malformed itself
            (void)replies->send(ipc::pdf::encode(refusal), kReplySendWait);
        }
        return 2;
    }
    // Read only: QtPdf opens the file (a staged import or a stored asset) for reading;
    // nothing else is touched.
    const std::u8string path(request->path.begin(), request->path.end());
    if (request->kind == ipc::pdf::RequestKind::ExtractText) {
        ipc::pdf::TextReply reply = extractPdfTextLocally(std::filesystem::path(path));
        reply.job = request->job;
        return replies->send(ipc::pdf::encode(reply), kReplySendWait) ? 0 : 1;
    }
    ipc::pdf::InspectReply reply = inspectPdfLocally(std::filesystem::path(path));
    reply.job = request->job;
    return replies->send(ipc::pdf::encode(reply), kReplySendWait) ? 0 : 1;
}

} // namespace studyapp::ui

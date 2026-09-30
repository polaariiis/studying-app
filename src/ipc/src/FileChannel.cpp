#include <studyapp/ipc/FileChannel.hpp>

#include <ipc.h>
#include <limits>
#include <string>
#include <utility>

namespace studyapp::ipc {

static_assert(FileChannel::kMaxMessageSize == IPC_MAX_PAYLOAD_SIZE);
static_assert(FileChannel::kWaitForever.count() == IPC_WAIT_FOREVER);

namespace {

using core::ErrorCode;

/// `sending`: the error of send(), where a timeout means the channel stayed full.
core::Error toError(ipc_status status, bool sending) {
    ErrorCode code = ErrorCode::IoError;
    switch (status) {
    case IPC_INVALID_ARGUMENT:
    case IPC_PAYLOAD_TOO_LARGE:
        code = ErrorCode::InvalidArgument;
        break;
    case IPC_CORRUPT_DATA:
        code = ErrorCode::ParseError;
        break;
    case IPC_TIMEOUT:
    case IPC_PROTOCOL_ERROR:
        code = ErrorCode::Conflict;
        break;
    case IPC_OUT_OF_MEMORY:
        code = ErrorCode::Internal;
        break;
    case IPC_OK:
    case IPC_IO_ERROR:
    case IPC_SYNC_ERROR:
        break;
    }
    std::string message = std::string("IPC channel: ") + ipc_status_string(status);
    if (status == IPC_TIMEOUT && sending) {
        message += " (the channel still holds an unread message)";
    }
    return core::Error{code, std::move(message)};
}

/// The C library's timeout: -1 waits forever, larger values are capped at INT_MAX ms.
core::Result<int> toTimeout(std::chrono::milliseconds timeout) {
    if (timeout == FileChannel::kWaitForever) {
        return IPC_WAIT_FOREVER;
    }
    if (timeout.count() < 0) {
        return core::makeError(ErrorCode::InvalidArgument, "IPC channel: negative timeout");
    }
    if (timeout.count() > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(timeout.count());
}

/// The path as the C library takes it: UTF-8 on every platform.
std::string utf8Path(const std::filesystem::path& file) {
    const std::u8string text = file.u8string();
    return {text.begin(), text.end()};
}

} // namespace

core::Result<FileChannel> FileChannel::open(const std::filesystem::path& file) {
    if (file.empty()) {
        return core::makeError(ErrorCode::InvalidArgument, "IPC channel: no file given");
    }
    ::ipc_channel* channel = nullptr;
    const ipc_status status = ipc_channel_open(utf8Path(file).c_str(), &channel);
    if (status != IPC_OK) {
        return tl::unexpected(toError(status, false));
    }
    return FileChannel(channel);
}

FileChannel::FileChannel(FileChannel&& other) noexcept
    : channel_(std::exchange(other.channel_, nullptr)) {}

FileChannel& FileChannel::operator=(FileChannel&& other) noexcept {
    if (this != &other) {
        ipc_channel_close(channel_);
        channel_ = std::exchange(other.channel_, nullptr);
    }
    return *this;
}

FileChannel::~FileChannel() {
    ipc_channel_close(channel_);
}

core::Result<void> FileChannel::send(std::span<const std::byte> message,
                                     std::chrono::milliseconds timeout) {
    auto timeoutMs = toTimeout(timeout);
    if (!timeoutMs) {
        return tl::unexpected(timeoutMs.error());
    }
    const ipc_status status =
        ipc_channel_send(channel_, message.data(), message.size(), *timeoutMs);
    if (status != IPC_OK) {
        return tl::unexpected(toError(status, true));
    }
    return {};
}

core::Result<std::optional<std::vector<std::byte>>>
FileChannel::receive(std::chrono::milliseconds timeout) {
    auto timeoutMs = toTimeout(timeout);
    if (!timeoutMs) {
        return tl::unexpected(timeoutMs.error());
    }
    void* data = nullptr;
    std::size_t size = 0;
    const ipc_status status = ipc_channel_receive(channel_, &data, &size, *timeoutMs);
    if (status == IPC_TIMEOUT) {
        return std::optional<std::vector<std::byte>>{};
    }
    if (status != IPC_OK) {
        return tl::unexpected(toError(status, false));
    }
    const auto* bytes = static_cast<const std::byte*>(data);
    std::vector<std::byte> message(bytes, bytes + size);
    ipc_free(data);
    return std::optional<std::vector<std::byte>>(std::move(message));
}

} // namespace studyapp::ipc

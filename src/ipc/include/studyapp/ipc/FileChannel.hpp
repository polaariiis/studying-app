#pragma once

#include <studyapp/core/Error.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

struct ipc_channel; // IPCFileLab's C handle (ipc.h); only FileChannel.cpp sees the C API

namespace studyapp::ipc {

/// A message channel between processes: IPCFileLab's file-backed single-slot channel
/// (docs/ARCHITECTURE.md D52). One message at a time: a sender waits while an unread
/// message is in the channel, a receiver waits while it is empty. Delivery is at most once;
/// a message survives the sender exiting. Any process on the machine that opens the same
/// file takes part.
///
/// No StudyBoard feature uses it yet; it is the boundary for a future helper process (for
/// example, parsing imported documents in isolation). Qt-free.
///
/// One FileChannel is used by one thread at a time; threads and processes each open their
/// own. The file lives where the caller decides (a directory the user can write to); any
/// path works (it is handed to the C library as UTF-8).
class FileChannel {
public:
    /// Largest message, in bytes (16 MiB).
    static constexpr std::size_t kMaxMessageSize = std::size_t{16} * 1024 * 1024;
    /// Wait without a limit.
    static constexpr std::chrono::milliseconds kWaitForever{-1};

    /// Opens the channel stored in `file`, creating it (empty) when missing. A message left
    /// by a sender that finished is kept. Errors: InvalidArgument (path), ParseError
    /// (malformed channel file), IoError.
    [[nodiscard]] static core::Result<FileChannel> open(const std::filesystem::path& file);

    FileChannel(FileChannel&& other) noexcept;
    FileChannel& operator=(FileChannel&& other) noexcept;
    FileChannel(const FileChannel&) = delete;
    FileChannel& operator=(const FileChannel&) = delete;
    ~FileChannel();

    /// Puts `message` into the channel, waiting up to `timeout` for an unread message to be
    /// taken. Errors: InvalidArgument (too large), Conflict (still full when the time ran
    /// out, or a stale channel state), ParseError (malformed file), IoError.
    [[nodiscard]] core::Result<void> send(std::span<const std::byte> message,
                                          std::chrono::milliseconds timeout);

    /// Takes the channel's message, waiting up to `timeout` for one. std::nullopt when none
    /// arrived in time. Errors as send().
    [[nodiscard]] core::Result<std::optional<std::vector<std::byte>>>
    receive(std::chrono::milliseconds timeout);

private:
    explicit FileChannel(::ipc_channel* channel) noexcept : channel_(channel) {}

    ::ipc_channel* channel_ = nullptr;
};

} // namespace studyapp::ipc

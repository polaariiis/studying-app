#include "FileIo.hpp"

#include "Utf8Path.hpp"

#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace studyapp::persistence::detail {

using core::ErrorCode;
using core::makeError;
using core::Result;

Result<OutputFile> OutputFile::create(const std::filesystem::path& path) {
#ifdef _WIN32
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wbx") != 0) {
        file = nullptr;
    }
#else
    std::FILE* file = std::fopen(path.c_str(), "wbx");
#endif
    if (file == nullptr) {
        return makeError(ErrorCode::IoError, "cannot create '" + utf8(path) + "'");
    }
    return OutputFile(file, path);
}

OutputFile::~OutputFile() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

OutputFile::OutputFile(OutputFile&& other) noexcept
    : file_(std::exchange(other.file_, nullptr)), path_(std::move(other.path_)) {}

Result<void> OutputFile::write(std::span<const std::uint8_t> data) {
    if (file_ == nullptr) {
        return makeError(ErrorCode::Internal, "write to a closed file");
    }
    if (!data.empty() && std::fwrite(data.data(), 1, data.size(), file_) != data.size()) {
        return makeError(ErrorCode::IoError, "writing '" + utf8(path_) + "' failed");
    }
    return {};
}

Result<void> OutputFile::syncAndClose() {
    if (file_ == nullptr) {
        return makeError(ErrorCode::Internal, "file is already closed");
    }
    bool ok = std::fflush(file_) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(file_)) == 0;
#else
    ok = ok && fsync(fileno(file_)) == 0;
#endif
    ok = std::fclose(std::exchange(file_, nullptr)) == 0 && ok;
    if (!ok) {
        return makeError(ErrorCode::IoError, "flushing '" + utf8(path_) + "' to disk failed");
    }
    return {};
}

} // namespace studyapp::persistence::detail

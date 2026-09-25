#pragma once

#include <studyapp/core/Error.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>

namespace studyapp::persistence::detail {

/// Binary output file that can be flushed to stable storage before it is renamed into
/// place (docs/DATABASE_SCHEMA.md §3.3: copy → fsync → rename → insert row).
///
/// std::ofstream cannot fsync, so this wraps a C stream: `_wfopen` + `_commit` on Windows
/// (Unicode paths), `fopen` + `fsync` elsewhere. This is the persistence module's only
/// platform-specific code; it uses C runtime functions only, no OS APIs.
class OutputFile {
public:
    [[nodiscard]] static core::Result<OutputFile> create(const std::filesystem::path& path);

    ~OutputFile();
    OutputFile(OutputFile&& other) noexcept;
    OutputFile& operator=(OutputFile&&) = delete;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    [[nodiscard]] core::Result<void> write(std::span<const std::uint8_t> data);
    /// Flushes user-space buffers, asks the OS to write the file to disk, and closes it.
    [[nodiscard]] core::Result<void> syncAndClose();

private:
    OutputFile(std::FILE* file, std::filesystem::path path) noexcept
        : file_(file), path_(std::move(path)) {}

    std::FILE* file_ = nullptr;
    std::filesystem::path path_;
};

} // namespace studyapp::persistence::detail

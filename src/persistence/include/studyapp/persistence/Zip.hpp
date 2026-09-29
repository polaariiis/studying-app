#pragma once

#include <studyapp/core/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace studyapp::persistence {

// Minimal zip archives for bundles (docs/DATABASE_SCHEMA.md §12): entries are *stored*
// (no compression — the database compresses poorly and assets are already compressed
// images/PDFs), no zip64 (an archive and its entries stay below 4 GiB, at most 65 535
// entries), UTF-8 names. Any zip tool can open the result. Reading accepts only such
// archives and treats them as untrusted: every offset and size is checked against the file,
// names are returned as they are (callers validate them before using them as paths), the
// entries together cannot be larger than the archive, and the CRC-32 of every extracted
// entry is verified.

/// CRC-32 (IEEE 802.3, as in zip) of `bytes`, continuing from `crc` (0 to start).
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes,
                                  std::uint32_t crc = 0) noexcept;

class ZipWriter {
public:
    /// Starts `target`; the archive is written to "<target>.part" and renamed over
    /// `target` by finish() (a failed or abandoned archive never looks complete).
    [[nodiscard]] static core::Result<ZipWriter> create(const std::filesystem::path& target);

    ZipWriter(ZipWriter&& other) noexcept;
    ZipWriter& operator=(ZipWriter&&) = delete;
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
    ~ZipWriter(); ///< removes the partial file unless finished

    [[nodiscard]] core::Result<void> addFile(std::string_view name,
                                             const std::filesystem::path& source);
    [[nodiscard]] core::Result<void> addBytes(std::string_view name,
                                              std::span<const std::uint8_t> bytes);
    /// Writes the central directory and moves the archive to its target.
    [[nodiscard]] core::Result<void> finish();

private:
    struct Entry {
        std::string name;
        std::uint32_t crc = 0;
        std::uint32_t size = 0;
        std::uint32_t offset = 0;
    };
    ZipWriter() = default;
    [[nodiscard]] core::Result<void> beginEntry(std::string_view name, Entry& entry);
    [[nodiscard]] core::Result<void> endEntry(Entry entry);

    std::filesystem::path target_;
    std::filesystem::path partial_;
    std::ofstream out_;
    std::vector<Entry> entries_;
    bool finished_ = false;
};

struct ZipEntry {
    std::string name;
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    std::uint64_t dataOffset = 0; ///< where the stored bytes start
};

class ZipReader {
public:
    /// Most entries accepted.
    static constexpr std::size_t kMaxEntries = 65535;

    /// Reads the central directory. Errors: IoError (unreadable), ParseError (not a zip
    /// archive of the supported kind, inconsistent or out-of-range offsets and sizes,
    /// duplicate or empty names, compressed or encrypted entries).
    [[nodiscard]] static core::Result<ZipReader> open(const std::filesystem::path& file);

    [[nodiscard]] const std::vector<ZipEntry>& entries() const noexcept { return entries_; }
    /// Copies the entry to `target` (created; must not exist) and checks its CRC-32; on
    /// failure the file is removed.
    [[nodiscard]] core::Result<void> extract(const ZipEntry& entry,
                                             const std::filesystem::path& target);
    /// The entry's bytes, if it has at most `maxBytes` (ParseError otherwise).
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> read(const ZipEntry& entry,
                                                               std::size_t maxBytes);

private:
    std::ifstream in_;
    std::vector<ZipEntry> entries_;
};

} // namespace studyapp::persistence

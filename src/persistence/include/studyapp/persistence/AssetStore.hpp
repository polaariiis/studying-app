#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/persistence/Sha256.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace studyapp::persistence {

/// Metadata of an imported asset (`asset` row).
struct AssetInfo {
    core::AssetId id;
    Sha256Digest sha256{};
    std::string mediaType;
    std::uint64_t byteSize = 0;
    std::optional<std::string> originalName;
    core::Timestamp created{};

    [[nodiscard]] friend bool operator==(const AssetInfo&, const AssetInfo&) = default;
};

struct AssetGcReport {
    std::size_t rowsDeleted = 0;
    std::size_t filesDeleted = 0;
};

struct AssetProblem {
    enum class Kind {
        MissingFile, ///< a row whose file does not exist
        WrongSize,   ///< the file's size differs from the row
        WrongHash,   ///< the file's content does not hash to its name
    };
    Kind kind;
    core::AssetId asset;
    std::filesystem::path file;
};

/// Content-addressed storage of large binaries (images, PDFs) outside the database
/// (docs/DATABASE_SCHEMA.md §3):
///
///   <workspace>/assets/<h0h1>/<h2h3>/<sha256-hex>.<ext>
///
/// Files are immutable and named by the SHA-256 of their content, so identical content is
/// stored once. Files and rows cannot share a transaction; ordering keeps them consistent:
///
///   import: hash while copying to temporary/ → fsync → rename into assets/ → insert the
///           row (a crash leaves at worst an orphan *file*, never a row without a file);
///   GC:     delete unreferenced rows first (committed), then files without rows.
///
/// Image elements and page backgrounds reference assets with ON DELETE RESTRICT, so the
/// database itself refuses to drop a referenced asset.
class AssetStore {
public:
    AssetStore(Database& database, std::filesystem::path workspaceRoot);

    /// Imports a file. Returns the existing asset if identical content was imported
    /// before (dedupe by hash). `mediaType` is e.g. "image/png" or "application/pdf".
    /// Fails with NotFound if `source` does not exist and with InvalidArgument if it is not
    /// a regular file (e.g. a directory).
    [[nodiscard]] core::Result<core::AssetId> import(const std::filesystem::path& source,
                                                     std::string_view mediaType,
                                                     core::IdGenerator& ids,
                                                     const core::Clock& clock);

    [[nodiscard]] core::Result<std::optional<AssetInfo>> find(core::AssetId id);
    [[nodiscard]] core::Result<bool> exists(core::AssetId id);

    /// Absolute path of the asset's file.
    [[nodiscard]] core::Result<std::filesystem::path> pathOf(core::AssetId id);

    /// Path of content with this hash, relative to the workspace root.
    [[nodiscard]] static std::filesystem::path relativePath(const Sha256Digest& sha256,
                                                            std::string_view mediaType);

    /// Deletes asset rows that nothing references and that are older than `grace`, then
    /// files under assets/ that no row names. The grace period protects assets that only
    /// the in-memory undo history references (docs/DATABASE_SCHEMA.md §3.3).
    [[nodiscard]] core::Result<AssetGcReport> collectGarbage(core::Timestamp now,
                                                             std::chrono::milliseconds grace);

    /// Checks every asset row against its file (existence, size, hash).
    [[nodiscard]] core::Result<std::vector<AssetProblem>> verify();

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    Database* database_;
    std::filesystem::path root_;
};

/// File extension (without dot) used for a media type; "bin" for unknown types.
[[nodiscard]] std::string_view extensionFor(std::string_view mediaType) noexcept;

} // namespace studyapp::persistence

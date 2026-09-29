#pragma once

#include <studyapp/core/Error.hpp>

#include <filesystem>
#include <span>
#include <string_view>

namespace studyapp::persistence {

// Workspace and notebook bundles (Phase 8; docs/DATABASE_SCHEMA.md §12, docs/ARCHITECTURE.md
// §11 "Export/backup"). A bundle is a zip archive (see Zip.hpp) holding a workspace in the
// workspace directory layout:
//
//   studyboard-bundle.txt   manifest: format, bundle version, kind, schema version
//   workspace.db            a consistent database snapshot (VACUUM INTO)
//   assets/ab/cd/<sha256>.<ext>   the files of the database's asset rows
//
// A workspace bundle holds a whole workspace; a notebook bundle is the same format holding a
// workspace with just the exported notebook (and the tags and assets it uses). Bundles are
// untrusted input when read: see extractBundle().

enum class BundleKind {
    Workspace,
    Notebook,
};

struct BundleManifest {
    BundleKind kind = BundleKind::Workspace;
    int schemaVersion = 0;
};

/// A bundle file's extension (the files are zip archives).
inline constexpr std::string_view kBundleExtension = ".studybundle";

struct BundleAsset {
    std::filesystem::path relative; ///< in the workspace layout: assets/ab/cd/<sha256>.<ext>
    std::filesystem::path file;     ///< the file to store
};

/// Writes a bundle of the database file `database` (a snapshot, schema `schemaVersion`) and
/// `assets` to `target` (replaced only once complete).
[[nodiscard]] core::Result<void> writeBundle(const std::filesystem::path& target, BundleKind kind,
                                             int schemaVersion,
                                             const std::filesystem::path& database,
                                             std::span<const BundleAsset> assets);

/// Extracts `bundle` into `root` as a workspace directory (`root` must be usable by
/// WorkspaceFile::create: missing or empty). Everything is checked before the result is
/// kept, and on any failure `root` is removed again:
///
///   * the archive (Zip.hpp: offsets, sizes, CRCs, stored entries only);
///   * the manifest (format, bundle version, kind, schema version this app can read);
///   * entry names: only the manifest, workspace.db and asset paths of the exact layout —
///     nothing can be written outside `root` or anywhere else inside it;
///   * the database: a StudyBoard database of the manifest's schema version that passes
///     `PRAGMA integrity_check`, whose schema objects (tables, indexes, triggers, views)
///     are exactly those the built-in migrations create — no foreign triggers or views;
///   * every asset row has its file, of the right size and SHA-256.
///
/// Errors: ParseError (not a valid bundle), Unsupported (made by a newer version),
/// AlreadyExists (`root` is not empty), IoError.
[[nodiscard]] core::Result<BundleManifest> extractBundle(const std::filesystem::path& bundle,
                                                         const std::filesystem::path& root);

} // namespace studyapp::persistence

#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Records.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/persistence/Transaction.hpp>

#include <string_view>
#include <vector>

namespace studyapp::persistence {

/// The always-loaded part of a workspace: its metadata and the notebook → section → page
/// structure (docs/DATABASE_SCHEMA.md §7.2 "Catalog"). Unordered; the Workspace derives
/// the order from the records' keys.
struct CatalogData {
    std::vector<document::NotebookInfo> notebooks;
    std::vector<document::SectionInfo> sections;
    std::vector<document::PageInfo> pages;
};

/// Reads and writes `workspace_meta`, `notebook`, `section` and `page`.
///
/// Writes are driven by the same record changes the in-memory Workspace applied
/// (create → INSERT, update → UPDATE, remove → DELETE); there is no per-command SQL.
/// Columns the Phase 2 model does not have (colours, icons, course links, nested sections,
/// page background assets, trash timestamps) keep their schema defaults and are never
/// overwritten by updates.
class CatalogStore {
public:
    explicit CatalogStore(Database& database) noexcept : database_(&database) {}

    /// Writes the metadata of a new workspace.
    [[nodiscard]] core::Result<void> createInfo(Transaction& transaction,
                                                const document::WorkspaceInfo& info,
                                                std::string_view appVersion);
    /// Records the application version that last opened the workspace for writing.
    [[nodiscard]] core::Result<void> recordWriterVersion(Transaction& transaction,
                                                         std::string_view appVersion);
    [[nodiscard]] core::Result<document::WorkspaceInfo> loadInfo();

    /// All notebooks, sections and pages. Fails with ParseError on undecodable rows and
    /// with Unsupported on data this version cannot represent (trashed items, nested
    /// sections).
    [[nodiscard]] core::Result<CatalogData> load();

    [[nodiscard]] core::Result<void> apply(Transaction& transaction,
                                           const document::NotebookChange& change);
    [[nodiscard]] core::Result<void> apply(Transaction& transaction,
                                           const document::SectionChange& change);
    [[nodiscard]] core::Result<void> apply(Transaction& transaction,
                                           const document::PageChange& change);

private:
    Database* database_;
};

} // namespace studyapp::persistence

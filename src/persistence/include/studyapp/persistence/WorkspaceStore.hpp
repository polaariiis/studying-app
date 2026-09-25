#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Workspace.hpp>
#include <studyapp/persistence/CatalogStore.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/persistence/PageStore.hpp>

namespace studyapp::persistence {

/// Patch-driven persistence of the document model (docs/DATABASE_SCHEMA.md §7).
///
/// write() stores exactly the Patch that the in-memory Workspace applied — the same value
/// the undo history keeps — in one `BEGIN IMMEDIATE … COMMIT` transaction. Foreign keys
/// are checked at commit (`defer_foreign_keys`), matching Workspace::apply, which checks
/// cross-record invariants per patch. If any statement or the commit fails, the whole
/// patch is rolled back and the error is returned; the database never holds half a patch.
///
/// load() rebuilds a Workspace from the database *through* Workspace::apply: the rows are
/// turned into one creating patch and applied to an empty workspace, so every document
/// invariant (parents, names, numeric ranges, connector rules, at least one layer per
/// page) is enforced exactly as for edits, followed by Workspace::validate(). Invalid or
/// unreachable persisted data fails the load; an invalid Workspace is never returned.
class WorkspaceStore {
public:
    WorkspaceStore(Database& database, const core::Clock& clock) noexcept
        : database_(&database), clock_(&clock), catalog_(database), pages_(database) {}

    [[nodiscard]] core::Result<document::Workspace> load();

    [[nodiscard]] core::Result<void> write(const document::Patch& patch);

    [[nodiscard]] CatalogStore& catalog() noexcept { return catalog_; }
    [[nodiscard]] PageStore& pages() noexcept { return pages_; }

private:
    Database* database_;
    const core::Clock* clock_;
    CatalogStore catalog_;
    PageStore pages_;
};

} // namespace studyapp::persistence

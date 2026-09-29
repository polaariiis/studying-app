#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/core/Uuid.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/persistence/Transaction.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace studyapp::persistence {

/// What a search hit is (`search_doc.owner_kind`, docs/DATABASE_SCHEMA.md §6).
enum class SearchKind : std::uint8_t {
    PageTitle = 1,
    TextBox = 2,
    Task = 3
};

struct SearchHit {
    SearchKind kind = SearchKind::PageTitle;
    core::Uuid owner;   ///< the page, text box element or task
    double score = 0.0; ///< bm25: lower is better
};

/// Version of what the index contains; a workspace whose `workspace_meta.search_index_version`
/// differs is rebuilt once when opened read-write (decision D28).
inline constexpr int kSearchIndexVersion = 1;

/// The full-text index over page titles, text boxes and tasks (title and notes), kept in the
/// contentless FTS5 table `search_index` with `search_doc` mapping each indexed record to its
/// FTS rowid (docs/DATABASE_SCHEMA.md §6).
///
/// Maintained incrementally by the WorkspaceStore in the same transaction as each patch:
/// only changes to indexed text touch it (a moved text box or stroke costs nothing). Queries
/// are FTS5 MATCH lookups ordered by bm25 (page and task titles weigh more than bodies).
class SearchIndex {
public:
    explicit SearchIndex(Database& database) noexcept : database_(&database) {}

    /// Updates the index for one applied change (no-op for changes to unindexed text).
    [[nodiscard]] core::Result<void> apply(Transaction& transaction,
                                           const document::AnyChange& change);
    /// Whether the index matches kSearchIndexVersion.
    [[nodiscard]] core::Result<bool> isCurrent();
    /// Regenerates the index from the source tables and records the version.
    [[nodiscard]] core::Result<void> rebuild(Transaction& transaction);
    /// Up to `limit` hits for `text` (words, each matching a prefix for the last one: "cel"
    /// finds "cells"; case and diacritics are ignored). Empty for text without words.
    [[nodiscard]] core::Result<std::vector<SearchHit>> query(std::string_view text,
                                                             std::size_t limit);

    /// The FTS5 query for user text: each word quoted (so operators and punctuation are
    /// literal), the last one as a prefix; "" if there is no word.
    [[nodiscard]] static std::string matchExpression(std::string_view text);

private:
    core::Result<void> upsert(SearchKind kind, const core::Uuid& owner, std::string_view title,
                              std::string_view body);
    core::Result<void> remove(const core::Uuid& owner);

    Database* database_;
};

} // namespace studyapp::persistence

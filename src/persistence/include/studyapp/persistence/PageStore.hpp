#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/Ids.hpp>
#include <studyapp/document/Element.hpp>
#include <studyapp/document/Patch.hpp>
#include <studyapp/document/Records.hpp>
#include <studyapp/persistence/Database.hpp>
#include <studyapp/persistence/Transaction.hpp>

#include <vector>

namespace studyapp::persistence {

/// The content of one page: its layers and their elements (unordered; the Workspace
/// derives order from the keys).
struct PageData {
    core::PageId page;
    std::vector<document::Layer> layers;
    std::vector<document::Element> elements;
};

/// Reads and writes `layer`, `element` and the per-kind element tables (`stroke`,
/// `text_box`, `shape`, `image`, `connector`).
///
/// Element writes follow docs/DATABASE_SCHEMA.md §7.1:
///   * create → INSERT header + kind row;
///   * update → UPDATE header; the kind row is rewritten only if the payload changed, and
///     a stroke's point blob only if its points changed (a move never rewrites points);
///     a kind change replaces the kind row;
///   * remove → DELETE header (the kind row cascades).
/// `element.page_id` (denormalised for page loading) is derived from the layer in SQL.
/// `element.min_x … max_y` caches document::worldBounds(). Element timestamps exist only
/// in the database and come from the `now` argument.
class PageStore {
public:
    explicit PageStore(Database& database) noexcept : database_(&database) {}

    /// Layers and elements of `page`. Fails with ParseError on undecodable or inconsistent
    /// rows (e.g. an element without its kind row, or whose layer is on another page).
    [[nodiscard]] core::Result<PageData> load(core::PageId page);

    [[nodiscard]] core::Result<void> apply(Transaction& transaction,
                                           const document::LayerChange& change);
    [[nodiscard]] core::Result<void>
    apply(Transaction& transaction, const document::ElementChange& change, core::Timestamp now);

private:
    Database* database_;
};

} // namespace studyapp::persistence

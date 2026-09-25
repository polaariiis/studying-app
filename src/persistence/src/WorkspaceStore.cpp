#include <studyapp/persistence/WorkspaceStore.hpp>

#include "StoreSupport.hpp"

#include <studyapp/persistence/Transaction.hpp>

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

namespace studyapp::persistence {

using core::ErrorCode;
using core::makeError;
using core::Result;
using detail::forward;

namespace {

core::Error invalidData(const core::Error& error) {
    return core::Error{ErrorCode::ParseError, "invalid workspace data: " + error.message};
}

/// Rows of `table` that the hierarchy walk did not reach (their parent is missing): the
/// load must account for every row, or it would silently drop data.
Result<void> checkAllRowsLoaded(Database& database, std::string_view table, std::size_t loaded) {
    auto count = database.queryInt("SELECT count(*) FROM " + std::string(table));
    if (!count) {
        return forward(count);
    }
    if (*count != static_cast<std::int64_t>(loaded)) {
        return makeError(ErrorCode::ParseError,
                         "invalid workspace data: " +
                             std::to_string(*count - static_cast<std::int64_t>(loaded)) + " " +
                             std::string(table) +
                             " row(s) are not reachable from the workspace hierarchy");
    }
    return {};
}

} // namespace

Result<document::Workspace> WorkspaceStore::load() {
    auto info = catalog_.loadInfo();
    if (!info) {
        return forward(info);
    }
    auto catalog = catalog_.load();
    if (!catalog) {
        return forward(catalog);
    }

    // Parents before children; within a page, connectors after the elements they attach to.
    document::Patch patch;
    for (auto& notebook : catalog->notebooks) {
        patch.add(document::created(std::move(notebook)));
    }
    for (auto& section : catalog->sections) {
        patch.add(document::created(std::move(section)));
    }
    std::size_t layerCount = 0;
    std::size_t elementCount = 0;
    std::vector<PageData> pages;
    pages.reserve(catalog->pages.size());
    for (const auto& page : catalog->pages) {
        auto content = pages_.load(page.id);
        if (!content) {
            return forward(content);
        }
        pages.push_back(std::move(*content));
    }
    for (auto& page : catalog->pages) {
        patch.add(document::created(std::move(page)));
    }
    for (auto& content : pages) {
        layerCount += content.layers.size();
        elementCount += content.elements.size();
        for (auto& layer : content.layers) {
            patch.add(document::created(std::move(layer)));
        }
        std::stable_partition(content.elements.begin(), content.elements.end(),
                              [](const document::Element& e) {
                                  return e.kind() != document::ElementKind::Connector;
                              });
        for (auto& element : content.elements) {
            patch.add(document::created(std::move(element)));
        }
    }

    for (const auto& [table, loaded] :
         {std::pair<std::string_view, std::size_t>{"layer", layerCount},
          std::pair<std::string_view, std::size_t>{"element", elementCount}}) {
        if (auto complete = checkAllRowsLoaded(*database_, table, loaded); !complete) {
            return forward(complete);
        }
    }

    document::Workspace workspace(std::move(*info));
    if (auto applied = workspace.apply(patch); !applied) {
        return tl::unexpected<core::Error>(invalidData(applied.error()));
    }
    if (auto valid = workspace.validate(); !valid) {
        return tl::unexpected<core::Error>(invalidData(valid.error()));
    }
    return workspace;
}

Result<void> WorkspaceStore::write(const document::Patch& patch) {
    if (patch.empty()) {
        return {};
    }
    auto transaction = Transaction::begin(*database_, Transaction::Kind::Immediate);
    if (!transaction) {
        return forward(transaction);
    }
    // Reset automatically at COMMIT/ROLLBACK.
    if (auto deferred = database_->execute("PRAGMA defer_foreign_keys = ON"); !deferred) {
        return deferred;
    }

    const core::Timestamp now = clock_->now();
    std::vector<core::LayerId> touchedLayers;
    std::vector<core::PageId> touchedPages;

    const auto& changes = patch.changes();
    for (std::size_t i = 0; i < changes.size(); ++i) {
        auto written = std::visit(
            [&](const auto& change) -> Result<void> {
                using Change = std::decay_t<decltype(change)>;
                if constexpr (std::is_same_v<Change, document::ElementChange>) {
                    for (const auto* element : {&change.before, &change.after}) {
                        if (*element) {
                            touchedLayers.push_back((*element)->layer);
                        }
                    }
                    return pages_.apply(*transaction, change, now);
                } else if constexpr (std::is_same_v<Change, document::LayerChange>) {
                    for (const auto* layer : {&change.before, &change.after}) {
                        if (*layer) {
                            touchedPages.push_back((*layer)->page);
                        }
                    }
                    return pages_.apply(*transaction, change);
                } else {
                    return catalog_.apply(*transaction, change);
                }
            },
            changes[i]);
        if (!written) {
            return makeError(written.error().code,
                             "writing change " + std::to_string(i) +
                                 " of the patch failed: " + written.error().message);
        }
    }

    // Bump content_version of every page whose content changed (thumbnail/cache
    // invalidation). Not a domain field, so it does not affect the loaded model.
    std::sort(touchedLayers.begin(), touchedLayers.end());
    touchedLayers.erase(std::unique(touchedLayers.begin(), touchedLayers.end()),
                        touchedLayers.end());
    for (const core::LayerId layer : touchedLayers) {
        auto page = database_->cached("SELECT page_id FROM layer WHERE id = ?1");
        if (!page) {
            return forward(page);
        }
        (*page)->bindId(1, layer);
        auto row = (*page)->step();
        if (!row) {
            return forward(row);
        }
        if (*row && (*page)->columnType(0) == ColumnType::Blob &&
            (*page)->columnBlob(0).size() == 16) {
            core::Uuid::Bytes bytes{};
            std::copy_n((*page)->columnBlob(0).begin(), bytes.size(), bytes.begin());
            touchedPages.push_back(core::PageId{core::Uuid{bytes}});
        }
    }
    std::sort(touchedPages.begin(), touchedPages.end());
    touchedPages.erase(std::unique(touchedPages.begin(), touchedPages.end()), touchedPages.end());
    for (const core::PageId page : touchedPages) {
        auto bump = database_->cached(
            "UPDATE page SET content_version = content_version + 1 WHERE id = ?1");
        if (!bump) {
            return forward(bump);
        }
        if (auto bumped = (*bump)->bindId(1, page).run(); !bumped) {
            return bumped;
        }
    }

    if (auto committed = transaction->commit(); !committed) {
        return makeError(committed.error().code,
                         "committing the patch failed: " + committed.error().message);
    }
    return {};
}

} // namespace studyapp::persistence

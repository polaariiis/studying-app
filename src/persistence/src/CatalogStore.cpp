#include <studyapp/persistence/CatalogStore.hpp>

#include "RowDecoder.hpp"
#include "StoreSupport.hpp"

#include <cassert>
#include <charconv>
#include <map>
#include <string>
#include <system_error>

namespace studyapp::persistence {

using core::ErrorCode;
using core::makeError;
using core::Result;
using detail::forward;
using detail::millis;
using detail::RowDecoder;

namespace {

// workspace_meta keys (docs/DATABASE_SCHEMA.md §5).
constexpr std::string_view kWorkspaceId = "workspace_id";
constexpr std::string_view kName = "name";
constexpr std::string_view kCreatedAt = "created_at";
constexpr std::string_view kCreatedByVersion = "created_by_version";
constexpr std::string_view kLastWrittenByVersion = "last_written_by_version";

Result<void> putMeta(Database& database, std::string_view key, std::string_view value) {
    auto statement =
        database.cached("INSERT OR REPLACE INTO workspace_meta (key, value) VALUES (?1, ?2)");
    if (!statement) {
        return forward(statement);
    }
    return (*statement)->bindText(1, key).bindText(2, value).run();
}

Result<void> unsupportedIfTrashed(const Statement& row, int column, std::string_view what) {
    if (!row.columnIsNull(column)) {
        return makeError(ErrorCode::Unsupported,
                         "the workspace contains a trashed " + std::string(what) +
                             "; Trash is not supported by this version of StudyBoard");
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------- metadata

Result<void> CatalogStore::createInfo(Transaction& transaction, const document::WorkspaceInfo& info,
                                      std::string_view appVersion) {
    assert(&transaction.database() == database_);
    (void)transaction;
    const std::pair<std::string_view, std::string> entries[] = {
        {kWorkspaceId, info.id.toString()},
        {kName, info.name},
        {kCreatedAt, std::to_string(millis(info.created))},
        {kCreatedByVersion, std::string(appVersion)},
        {kLastWrittenByVersion, std::string(appVersion)},
    };
    for (const auto& [key, value] : entries) {
        if (auto put = putMeta(*database_, key, value); !put) {
            return put;
        }
    }
    return {};
}

Result<void> CatalogStore::recordWriterVersion(Transaction& transaction,
                                               std::string_view appVersion) {
    assert(&transaction.database() == database_);
    (void)transaction;
    return putMeta(*database_, kLastWrittenByVersion, appVersion);
}

Result<document::WorkspaceInfo> CatalogStore::loadInfo() {
    auto statement = database_->cached("SELECT key, value FROM workspace_meta");
    if (!statement) {
        return forward(statement);
    }
    std::map<std::string, std::string, std::less<>> meta;
    while (true) {
        auto row = (*statement)->step();
        if (!row) {
            return forward(row);
        }
        if (!*row) {
            break;
        }
        RowDecoder decode(**statement, "workspace_meta");
        std::string key = decode.text(0);
        std::string value = decode.text(1);
        if (auto status = decode.status(); !status) {
            return forward(status);
        }
        meta.emplace(std::move(key), std::move(value));
    }

    const auto get = [&](std::string_view key) -> Result<std::string> {
        const auto it = meta.find(key);
        if (it == meta.end()) {
            return makeError(ErrorCode::ParseError,
                             "corrupt workspace metadata: '" + std::string(key) + "' is missing");
        }
        return it->second;
    };
    auto idText = get(kWorkspaceId);
    auto name = get(kName);
    auto createdText = get(kCreatedAt);
    for (auto* value : {&idText, &name, &createdText}) {
        if (!*value) {
            return forward(*value);
        }
    }
    auto id = core::WorkspaceId::parse(*idText);
    if (!id || id->isNull()) {
        return makeError(ErrorCode::ParseError, "corrupt workspace metadata: invalid workspace_id");
    }
    std::int64_t created = 0;
    const auto* begin = createdText->data();
    const auto* end = begin + createdText->size();
    const auto [parsedEnd, ec] = std::from_chars(begin, end, created);
    if (ec != std::errc{} || parsedEnd != end) {
        return makeError(ErrorCode::ParseError, "corrupt workspace metadata: invalid created_at");
    }
    return document::WorkspaceInfo{.id = *id,
                                   .name = std::move(*name),
                                   .created = core::Timestamp(std::chrono::milliseconds(created))};
}

// ---------------------------------------------------------------------------- load

Result<CatalogData> CatalogStore::load() {
    CatalogData data;

    // ---- notebooks
    {
        auto statement = database_->cached(
            "SELECT id, title, sort_key, created_at, updated_at, deleted_at FROM notebook");
        if (!statement) {
            return forward(statement);
        }
        while (true) {
            auto row = (*statement)->step();
            if (!row) {
                return forward(row);
            }
            if (!*row) {
                break;
            }
            if (auto trashed = unsupportedIfTrashed(**statement, 5, "notebook"); !trashed) {
                return forward(trashed);
            }
            RowDecoder decode(**statement, "notebook");
            document::NotebookInfo notebook{.id = decode.id<core::NotebookId>(0),
                                            .title = decode.text(1),
                                            .order = decode.key(2),
                                            .created = decode.timestamp(3),
                                            .modified = decode.timestamp(4)};
            if (auto status = decode.status(); !status) {
                return forward(status);
            }
            data.notebooks.push_back(std::move(notebook));
        }
    }

    // ---- sections
    {
        auto statement =
            database_->cached("SELECT id, notebook_id, parent_id, title, sort_key, created_at, "
                              "updated_at, deleted_at FROM section");
        if (!statement) {
            return forward(statement);
        }
        while (true) {
            auto row = (*statement)->step();
            if (!row) {
                return forward(row);
            }
            if (!*row) {
                break;
            }
            if (auto trashed = unsupportedIfTrashed(**statement, 7, "section"); !trashed) {
                return forward(trashed);
            }
            if (!(*statement)->columnIsNull(2)) {
                return makeError(ErrorCode::Unsupported,
                                 "the workspace contains nested sections, which this version of "
                                 "StudyBoard does not support");
            }
            RowDecoder decode(**statement, "section");
            document::SectionInfo section{.id = decode.id<core::SectionId>(0),
                                          .notebook = decode.id<core::NotebookId>(1),
                                          .title = decode.text(3),
                                          .order = decode.key(4),
                                          .created = decode.timestamp(5),
                                          .modified = decode.timestamp(6)};
            if (auto status = decode.status(); !status) {
                return forward(status);
            }
            data.sections.push_back(std::move(section));
        }
    }

    // ---- pages
    {
        auto statement = database_->cached(
            "SELECT id, section_id, title, sort_key, extent, width, height, bg_color, bg_pattern, "
            "bg_spacing, created_at, updated_at, deleted_at FROM page");
        if (!statement) {
            return forward(statement);
        }
        while (true) {
            auto row = (*statement)->step();
            if (!row) {
                return forward(row);
            }
            if (!*row) {
                break;
            }
            if (auto trashed = unsupportedIfTrashed(**statement, 12, "page"); !trashed) {
                return forward(trashed);
            }
            RowDecoder decode(**statement, "page");
            const Statement& s = **statement;
            document::PageInfo page{
                .id = decode.id<core::PageId>(0),
                .section = decode.id<core::SectionId>(1),
                .title = decode.text(2),
                .order = decode.key(3),
                .extent = decode.enumeration<document::PageExtent>(4, 0, 1),
                // Width/height are written for every page (lossless); NULL means "no size".
                .size = {s.columnIsNull(5) ? 0.0 : decode.real(5),
                         s.columnIsNull(6) ? 0.0 : decode.real(6)},
                .background = {.color = decode.color(7),
                               .pattern = decode.enumeration<document::BackgroundPattern>(8, 0, 3),
                               .spacing = decode.real32(9)},
                .created = decode.timestamp(10),
                .modified = decode.timestamp(11)};
            if (auto status = decode.status(); !status) {
                return forward(status);
            }
            data.pages.push_back(std::move(page));
        }
    }
    return data;
}

// ---------------------------------------------------------------------------- apply

Result<void> CatalogStore::apply(Transaction& transaction, const document::NotebookChange& change) {
    assert(&transaction.database() == database_);
    (void)transaction;
    if (change.isRemove()) {
        auto statement = database_->cached("DELETE FROM notebook WHERE id = ?1");
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindId(1, change.before->id);
        return detail::runOnOneRow(*database_, **statement, "notebook",
                                   change.before->id.toString());
    }
    const document::NotebookInfo& n = *change.after;
    auto statement = database_->cached(
        change.isCreate()
            ? "INSERT INTO notebook (id, title, sort_key, created_at, updated_at) "
              "VALUES (?1, ?2, ?3, ?4, ?5)"
            : "UPDATE notebook SET title = ?2, sort_key = ?3, created_at = ?4, updated_at = ?5 "
              "WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)
        ->bindId(1, n.id)
        .bindText(2, n.title)
        .bindText(3, n.order.value())
        .bindInt(4, millis(n.created))
        .bindInt(5, millis(n.modified));
    return detail::runOnOneRow(*database_, **statement, "notebook", n.id.toString());
}

Result<void> CatalogStore::apply(Transaction& transaction, const document::SectionChange& change) {
    assert(&transaction.database() == database_);
    (void)transaction;
    if (change.isRemove()) {
        auto statement = database_->cached("DELETE FROM section WHERE id = ?1");
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindId(1, change.before->id);
        return detail::runOnOneRow(*database_, **statement, "section",
                                   change.before->id.toString());
    }
    const document::SectionInfo& s = *change.after;
    auto statement = database_->cached(
        change.isCreate()
            ? "INSERT INTO section (id, notebook_id, title, sort_key, created_at, updated_at) "
              "VALUES (?1, ?2, ?3, ?4, ?5, ?6)"
            : "UPDATE section SET notebook_id = ?2, title = ?3, sort_key = ?4, created_at = ?5, "
              "updated_at = ?6 WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)
        ->bindId(1, s.id)
        .bindId(2, s.notebook)
        .bindText(3, s.title)
        .bindText(4, s.order.value())
        .bindInt(5, millis(s.created))
        .bindInt(6, millis(s.modified));
    return detail::runOnOneRow(*database_, **statement, "section", s.id.toString());
}

Result<void> CatalogStore::apply(Transaction& transaction, const document::PageChange& change) {
    assert(&transaction.database() == database_);
    (void)transaction;
    if (change.isRemove()) {
        auto statement = database_->cached("DELETE FROM page WHERE id = ?1");
        if (!statement) {
            return forward(statement);
        }
        (*statement)->bindId(1, change.before->id);
        return detail::runOnOneRow(*database_, **statement, "page", change.before->id.toString());
    }
    const document::PageInfo& p = *change.after;
    auto statement = database_->cached(
        change.isCreate()
            ? "INSERT INTO page (id, section_id, title, sort_key, extent, width, height, bg_color, "
              "bg_pattern, bg_spacing, created_at, updated_at) "
              "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)"
            : "UPDATE page SET section_id = ?2, title = ?3, sort_key = ?4, extent = ?5, width = "
              "?6, "
              "height = ?7, bg_color = ?8, bg_pattern = ?9, bg_spacing = ?10, created_at = ?11, "
              "updated_at = ?12 WHERE id = ?1");
    if (!statement) {
        return forward(statement);
    }
    (*statement)
        ->bindId(1, p.id)
        .bindId(2, p.section)
        .bindText(3, p.title)
        .bindText(4, p.order.value())
        .bindInt(5, static_cast<std::int64_t>(p.extent))
        .bindReal(6, p.size.x)
        .bindReal(7, p.size.y)
        .bindInt(8, static_cast<std::int64_t>(p.background.color.toArgb32()))
        .bindInt(9, static_cast<std::int64_t>(p.background.pattern))
        .bindReal(10, static_cast<double>(p.background.spacing))
        .bindInt(11, millis(p.created))
        .bindInt(12, millis(p.modified));
    return detail::runOnOneRow(*database_, **statement, "page", p.id.toString());
}

} // namespace studyapp::persistence

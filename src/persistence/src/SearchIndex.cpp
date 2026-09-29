#include <studyapp/persistence/SearchIndex.hpp>

#include "StoreSupport.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include <type_traits>
#include <variant>

namespace studyapp::persistence {

using core::Result;
using detail::forward;

namespace {

/// A word has at least one letter or digit (any non-ASCII byte counts: UTF-8 text).
bool isWordByte(unsigned char c) noexcept {
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

const document::TextBox* textOf(const std::optional<document::Element>& element) {
    return element ? std::get_if<document::TextBox>(&element->payload) : nullptr;
}

} // namespace

std::string SearchIndex::matchExpression(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && isSpace(text[at])) {
            ++at;
        }
        const std::size_t start = at;
        while (at < text.size() && !isSpace(text[at])) {
            ++at;
        }
        const std::string_view word = text.substr(start, at - start);
        bool hasWordByte = false;
        for (const char c : word) {
            hasWordByte = hasWordByte || isWordByte(static_cast<unsigned char>(c));
        }
        if (hasWordByte) {
            words.push_back(word);
        }
    }
    std::string expression;
    for (std::size_t i = 0; i < words.size(); ++i) {
        expression += i == 0 ? "\"" : " \"";
        for (const char c : words[i]) {
            expression += c;
            if (c == '"') {
                expression += '"'; // FTS5 string: a quote is doubled
            }
        }
        expression += '"';
        if (i + 1 == words.size()) {
            expression += '*'; // the word being typed matches as a prefix
        }
    }
    return expression;
}

Result<void> SearchIndex::upsert(SearchKind kind, const core::Uuid& owner, std::string_view title,
                                 std::string_view body) {
    auto find = database_->cached("SELECT rowid FROM search_doc WHERE owner_id = ?1");
    if (!find) {
        return forward(find);
    }
    (*find)->bindUuid(1, owner);
    auto row = (*find)->step();
    if (!row) {
        return forward(row);
    }
    std::int64_t rowid = 0;
    if (*row) {
        rowid = (*find)->columnInt(0);
        auto drop = database_->cached("DELETE FROM search_index WHERE rowid = ?1");
        if (!drop) {
            return forward(drop);
        }
        if (auto done = (*drop)->bindInt(1, rowid).run(); !done) {
            return done;
        }
    } else {
        auto insert = database_->cached(
            "INSERT INTO search_doc (owner_kind, owner_id, page_id) VALUES (?1, ?2, ?3) "
            "RETURNING rowid");
        if (!insert) {
            return forward(insert);
        }
        (*insert)->bindInt(1, static_cast<std::int64_t>(kind)).bindUuid(2, owner);
        if (kind == SearchKind::PageTitle) {
            (*insert)->bindUuid(3, owner);
        } else {
            (*insert)->bindNull(3);
        }
        auto inserted = (*insert)->step();
        if (!inserted) {
            return forward(inserted);
        }
        rowid = (*insert)->columnInt(0);
    }
    auto add =
        database_->cached("INSERT INTO search_index (rowid, title, body) VALUES (?1, ?2, ?3)");
    if (!add) {
        return forward(add);
    }
    return (*add)->bindInt(1, rowid).bindText(2, title).bindText(3, body).run();
}

Result<void> SearchIndex::remove(const core::Uuid& owner) {
    auto find = database_->cached("SELECT rowid FROM search_doc WHERE owner_id = ?1");
    if (!find) {
        return forward(find);
    }
    (*find)->bindUuid(1, owner);
    auto row = (*find)->step();
    if (!row) {
        return forward(row);
    }
    if (!*row) {
        return {}; // never indexed (e.g. empty text): nothing to remove
    }
    const std::int64_t rowid = (*find)->columnInt(0);
    for (const char* sql :
         {"DELETE FROM search_index WHERE rowid = ?1", "DELETE FROM search_doc WHERE rowid = ?1"}) {
        auto statement = database_->cached(sql);
        if (!statement) {
            return forward(statement);
        }
        if (auto done = (*statement)->bindInt(1, rowid).run(); !done) {
            return done;
        }
    }
    return {};
}

Result<void> SearchIndex::apply(Transaction& transaction, const document::AnyChange& change) {
    assert(&transaction.database() == database_);
    (void)transaction;
    return std::visit(
        [&](const auto& c) -> Result<void> {
            using Change = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<Change, document::PageChange>) {
                if (c.isRemove()) {
                    return remove(c.before->id.value());
                }
                if (c.isCreate() || c.before->title != c.after->title) {
                    return upsert(SearchKind::PageTitle, c.after->id.value(), c.after->title, {});
                }
            } else if constexpr (std::is_same_v<Change, document::ElementChange>) {
                const document::TextBox* before = textOf(c.before);
                const document::TextBox* after = textOf(c.after);
                if (after != nullptr && (before == nullptr || before->text != after->text)) {
                    return upsert(SearchKind::TextBox, c.after->id.value(), {}, after->text);
                }
                if (before != nullptr && after == nullptr) {
                    return remove(c.before->id.value());
                }
            } else if constexpr (std::is_same_v<Change, document::TaskChange>) {
                if (c.isRemove()) {
                    return remove(c.before->id.value());
                }
                if (c.isCreate() || c.before->title != c.after->title ||
                    c.before->notes != c.after->notes) {
                    return upsert(SearchKind::Task, c.after->id.value(), c.after->title,
                                  c.after->notes);
                }
            }
            return {};
        },
        change);
}

Result<bool> SearchIndex::isCurrent() {
    auto version =
        database_->cached("SELECT value FROM workspace_meta WHERE key = 'search_index_version'");
    if (!version) {
        return forward(version);
    }
    auto row = (*version)->step();
    if (!row) {
        return forward(row);
    }
    return *row && (*version)->columnText(0) == std::to_string(kSearchIndexVersion);
}

Result<void> SearchIndex::rebuild(Transaction& transaction) {
    assert(&transaction.database() == database_);
    (void)transaction;
    return database_->execute(
        "INSERT INTO search_index (search_index) VALUES ('delete-all');"
        "DELETE FROM search_doc;"
        "INSERT INTO search_doc (owner_kind, owner_id, page_id) SELECT 1, id, id FROM page;"
        "INSERT INTO search_doc (owner_kind, owner_id, page_id) "
        "  SELECT 2, element_id, NULL FROM text_box;"
        "INSERT INTO search_doc (owner_kind, owner_id, page_id) SELECT 3, id, NULL FROM task;"
        "INSERT INTO search_index (rowid, title, body) SELECT d.rowid, p.title, '' "
        "  FROM search_doc d JOIN page p ON p.id = d.owner_id WHERE d.owner_kind = 1;"
        "INSERT INTO search_index (rowid, title, body) SELECT d.rowid, '', t.plain_text "
        "  FROM search_doc d JOIN text_box t ON t.element_id = d.owner_id WHERE d.owner_kind = 2;"
        "INSERT INTO search_index (rowid, title, body) SELECT d.rowid, k.title, k.notes "
        "  FROM search_doc d JOIN task k ON k.id = d.owner_id WHERE d.owner_kind = 3;"
        "INSERT INTO workspace_meta (key, value) VALUES ('search_index_version', '" +
        std::to_string(kSearchIndexVersion) +
        "') ON CONFLICT (key) DO UPDATE SET value = excluded.value;");
}

Result<std::vector<SearchHit>> SearchIndex::query(std::string_view text, std::size_t limit) {
    std::vector<SearchHit> hits;
    const std::string expression = matchExpression(text);
    if (expression.empty() || limit == 0) {
        return hits;
    }
    // Titles weigh five times the body text; bm25 is lower for better matches.
    auto statement =
        database_->cached("SELECT d.owner_kind, d.owner_id, bm25(search_index, 5.0, 1.0) AS score "
                          "FROM search_index JOIN search_doc d ON d.rowid = search_index.rowid "
                          "WHERE search_index MATCH ?1 ORDER BY score LIMIT ?2");
    if (!statement) {
        return forward(statement);
    }
    (*statement)->bindText(1, expression).bindInt(2, static_cast<std::int64_t>(limit));
    while (true) {
        auto row = (*statement)->step();
        if (!row) {
            return forward(row);
        }
        if (!*row) {
            break;
        }
        const Statement& s = **statement;
        const std::int64_t kind = s.columnInt(0);
        const auto blob = s.columnBlob(1);
        if (kind < 1 || kind > 3 || blob.size() != 16) {
            continue; // not ours (a later version's kind): skipped, not an error
        }
        core::Uuid::Bytes bytes{};
        std::copy_n(blob.begin(), bytes.size(), bytes.begin());
        hits.push_back({.kind = static_cast<SearchKind>(kind),
                        .owner = core::Uuid{bytes},
                        .score = s.columnReal(2)});
    }
    return hits;
}

} // namespace studyapp::persistence

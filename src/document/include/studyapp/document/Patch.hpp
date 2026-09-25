#pragma once

#include <studyapp/document/Element.hpp>
#include <studyapp/document/Records.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace studyapp::document {

/// Change of one record: its complete state before and after.
///
///   before only  -> the record is removed
///   after only   -> the record is created
///   both         -> the record is replaced (ids must match)
///
/// Storing full before/after states (not deltas or callbacks) makes a change
/// self-describing: it can be validated against the current state, inverted for undo,
/// written to SQLite (Phase 3) and, later, sent to other devices.
template <class Record>
struct Change {
    std::optional<Record> before;
    std::optional<Record> after;

    [[nodiscard]] bool isCreate() const noexcept { return !before && after; }
    [[nodiscard]] bool isRemove() const noexcept { return before && !after; }
    [[nodiscard]] bool isUpdate() const noexcept { return before && after; }

    [[nodiscard]] Change inverted() const { return Change{after, before}; }

    [[nodiscard]] friend bool operator==(const Change&, const Change&) = default;
};

using NotebookChange = Change<NotebookInfo>;
using SectionChange = Change<SectionInfo>;
using PageChange = Change<PageInfo>;
using LayerChange = Change<Layer>;
using ElementChange = Change<Element>;

using AnyChange =
    std::variant<NotebookChange, SectionChange, PageChange, LayerChange, ElementChange>;

template <class Record>
[[nodiscard]] AnyChange created(Record record) {
    return Change<Record>{std::nullopt, std::move(record)};
}
template <class Record>
[[nodiscard]] AnyChange removed(Record record) {
    return Change<Record>{std::move(record), std::nullopt};
}
template <class Record>
[[nodiscard]] AnyChange updated(Record before, Record after) {
    return Change<Record>{std::move(before), std::move(after)};
}

/// An ordered list of record changes that together form one logical edit.
///
/// A Patch is the single description of a document mutation (docs/DATA_MODEL.md §6):
/// the Workspace applies it atomically, the undo history stores it, and later phases
/// persist it and invalidate canvas caches from it. It contains data only: no callbacks,
/// no pointers into the document.
class Patch {
public:
    Patch() = default;
    explicit Patch(std::vector<AnyChange> changes) : changes_(std::move(changes)) {}

    void add(AnyChange change) { changes_.push_back(std::move(change)); }

    [[nodiscard]] const std::vector<AnyChange>& changes() const noexcept { return changes_; }
    [[nodiscard]] bool empty() const noexcept { return changes_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return changes_.size(); }

    /// The patch that undoes this one: changes in reverse order, before/after swapped.
    [[nodiscard]] Patch inverted() const;

    [[nodiscard]] friend bool operator==(const Patch&, const Patch&) = default;

private:
    std::vector<AnyChange> changes_;
};

/// A named, user-level edit: what the undo menu shows ("Rename page") plus the patch
/// that performs it.
struct Command {
    std::string label;
    Patch patch;

    [[nodiscard]] friend bool operator==(const Command&, const Command&) = default;
};

} // namespace studyapp::document

#pragma once

#include <studyapp/core/Error.hpp>

#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace studyapp::core {

/// Ordering key for sibling collections (notebooks, sections, pages, layers, elements).
///
/// A key is a string over the 62 digits `0-9A-Za-z` (ASCII order) made of a
/// variable-length base-62 *integer part* followed by an optional *fraction*
/// (the "fractional indexing" scheme). Keys compare bytewise, so they sort identically in
/// C++ and in SQLite (`COLLATE BINARY`).
///
/// * Appending/prepending increments/decrements the integer part: append-heavy
///   collections (new pages, new strokes) keep keys of 2-4 characters.
/// * Inserting between two neighbours extends the fraction; only the new element gets a
///   key, so reordering is a single-record change (see docs/DATA_MODEL.md).
/// * A fraction never ends in `0`, so there is always room for another key.
///
/// Keys are generated deterministically: the same neighbours always produce the same key.
/// Two independent insertions at the same position therefore produce equal keys; ties are
/// broken by entity id wherever siblings are sorted.
class FractionalIndex {
public:
    /// Key for the first element of an empty collection ("a0", integer zero).
    [[nodiscard]] static FractionalIndex first();

    /// Validates `text` as a key.
    [[nodiscard]] static Result<FractionalIndex> parse(std::string_view text);

    /// Key strictly between `lower` and `upper`. `nullopt` means "no bound" on that side.
    /// Fails with InvalidArgument if `lower >= upper`.
    [[nodiscard]] static Result<FractionalIndex>
    between(const std::optional<FractionalIndex>& lower,
            const std::optional<FractionalIndex>& upper);

    /// Key strictly greater than `key` (append after the last sibling).
    [[nodiscard]] static FractionalIndex after(const FractionalIndex& key);

    /// Key strictly less than `key` (insert before the first sibling).
    [[nodiscard]] static FractionalIndex before(const FractionalIndex& key);

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    [[nodiscard]] friend bool operator==(const FractionalIndex&, const FractionalIndex&) = default;
    [[nodiscard]] friend std::strong_ordering operator<=>(const FractionalIndex& lhs,
                                                          const FractionalIndex& rhs) noexcept {
        const int c = lhs.value_.compare(rhs.value_);
        return c < 0 ? std::strong_ordering::less
                     : (c > 0 ? std::strong_ordering::greater : std::strong_ordering::equal);
    }

private:
    explicit FractionalIndex(std::string value) : value_(std::move(value)) {}

    std::string value_;
};

} // namespace studyapp::core

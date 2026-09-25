#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Color.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/core/FractionalIndex.hpp>
#include <studyapp/core/Id.hpp>
#include <studyapp/core/Uuid.hpp>
#include <studyapp/persistence/Database.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace studyapp::persistence::detail {

/// Decodes the columns of one result row into domain values, validating storage classes
/// and encodings (16-byte UUIDs, fractional-index keys, 0xAARRGGBB colours, enum ranges).
///
/// Accessors never fail individually: the first problem is recorded, a neutral value is
/// returned, and status() reports it, so a record can be decoded in one expression and
/// checked once. Persisted data is untrusted: a corrupt row must produce an error, never an
/// invalid domain object. Value invariants that the document model owns (finite numbers,
/// ranges, names) are checked later by Workspace::apply.
class RowDecoder {
public:
    RowDecoder(const Statement& statement, std::string_view what) noexcept
        : statement_(&statement), what_(what) {}

    [[nodiscard]] core::Uuid uuid(int column) {
        if (!expect(column, ColumnType::Blob)) {
            return {};
        }
        const auto blob = statement_->columnBlob(column);
        if (blob.size() != 16) {
            fail(column, "is not a 16-byte UUID");
            return {};
        }
        core::Uuid::Bytes bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = blob[i];
        }
        const core::Uuid value(bytes);
        if (value.isNil()) {
            fail(column, "is the nil UUID");
        }
        return value;
    }

    template <class Id>
    [[nodiscard]] Id id(int column) {
        return Id{uuid(column)};
    }

    template <class Id>
    [[nodiscard]] std::optional<Id> optionalId(int column) {
        if (statement_->columnIsNull(column)) {
            return std::nullopt;
        }
        return id<Id>(column);
    }

    [[nodiscard]] std::int64_t integer(int column) {
        return expect(column, ColumnType::Integer) ? statement_->columnInt(column) : 0;
    }

    [[nodiscard]] bool boolean(int column) {
        const std::int64_t value = integer(column);
        if (value != 0 && value != 1) {
            fail(column, "is not a boolean (0 or 1)");
        }
        return value == 1;
    }

    /// REAL column; integral values are accepted (SQLite may store them as INTEGER).
    [[nodiscard]] double real(int column) {
        const ColumnType type = statement_->columnType(column);
        if (type == ColumnType::Integer || type == ColumnType::Real) {
            return statement_->columnReal(column);
        }
        fail(column, "is not a number");
        return 0.0;
    }

    /// Single-precision model field. Values outside float range become infinite and are
    /// then rejected by the document invariants.
    [[nodiscard]] float real32(int column) { return static_cast<float>(real(column)); }

    [[nodiscard]] std::string text(int column) {
        return expect(column, ColumnType::Text) ? std::string(statement_->columnText(column))
                                                : std::string();
    }

    [[nodiscard]] core::FractionalIndex key(int column) {
        const std::string value = text(column);
        if (!ok()) {
            return core::FractionalIndex::first();
        }
        auto parsed = core::FractionalIndex::parse(value);
        if (!parsed) {
            fail(column, "is not a valid ordering key ('" + value + "')");
            return core::FractionalIndex::first();
        }
        return *parsed;
    }

    [[nodiscard]] core::Timestamp timestamp(int column) {
        return core::Timestamp(std::chrono::milliseconds(integer(column)));
    }

    [[nodiscard]] core::Color color(int column) {
        const std::int64_t value = integer(column);
        if (value < 0 || value > std::int64_t{0xFFFFFFFF}) {
            fail(column, "is not a 0xAARRGGBB colour");
            return {};
        }
        return core::Color::fromArgb32(static_cast<std::uint32_t>(value));
    }

    [[nodiscard]] std::optional<core::Color> optionalColor(int column) {
        if (statement_->columnIsNull(column)) {
            return std::nullopt;
        }
        return color(column);
    }

    /// Enum stored as INTEGER with values 0..maxValue (or first..last).
    template <class Enum>
    [[nodiscard]] Enum enumeration(int column, int first, int last) {
        const std::int64_t value = integer(column);
        if (value < first || value > last) {
            fail(column, "has unknown value " + std::to_string(value));
            return static_cast<Enum>(first);
        }
        return static_cast<Enum>(value);
    }

    [[nodiscard]] bool ok() const noexcept { return !error_; }

    [[nodiscard]] core::Result<void> status() const {
        if (error_) {
            return tl::unexpected<core::Error>(*error_);
        }
        return {};
    }

    /// Records a problem that is not tied to one column.
    void failRow(std::string message) {
        if (!error_) {
            error_ = core::Error{core::ErrorCode::ParseError,
                                 "corrupt " + std::string(what_) + " row: " + std::move(message)};
        }
    }

private:
    bool expect(int column, ColumnType type) {
        if (statement_->columnType(column) == type) {
            return true;
        }
        fail(column, statement_->columnIsNull(column) ? "is NULL" : "has the wrong type");
        return false;
    }

    void fail(int column, const std::string& problem) {
        failRow("column '" + std::string(statement_->columnName(column)) + "' " + problem);
    }

    const Statement* statement_;
    std::string_view what_;
    std::optional<core::Error> error_;
};

} // namespace studyapp::persistence::detail

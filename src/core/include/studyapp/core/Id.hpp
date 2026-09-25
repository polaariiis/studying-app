#pragma once

#include <studyapp/core/IdGenerator.hpp>
#include <studyapp/core/Uuid.hpp>

#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace studyapp::core {

/// Strongly typed identifier. `Tag` is an empty marker type; `Id<PageTag>` and
/// `Id<ElementTag>` are distinct, non-convertible types sharing one representation.
template <class Tag>
class Id {
public:
    using tag_type = Tag;

    /// Null id (nil UUID).
    constexpr Id() noexcept = default;
    constexpr explicit Id(const Uuid& value) noexcept : value_(value) {}

    [[nodiscard]] static Id generate(IdGenerator& generator) { return Id{generator.next()}; }

    [[nodiscard]] static Result<Id> parse(std::string_view text) {
        auto uuid = Uuid::parse(text);
        if (!uuid) {
            return tl::unexpected<Error>(std::move(uuid.error()));
        }
        return Id{*uuid};
    }

    [[nodiscard]] constexpr const Uuid& value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool isNull() const noexcept { return value_.isNil(); }
    [[nodiscard]] std::string toString() const { return value_.toString(); }

    [[nodiscard]] friend constexpr bool operator==(const Id&, const Id&) noexcept = default;
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const Id&,
                                                                    const Id&) noexcept = default;

private:
    Uuid value_{};
};

} // namespace studyapp::core

template <class Tag>
struct std::hash<studyapp::core::Id<Tag>> {
    [[nodiscard]] std::size_t operator()(const studyapp::core::Id<Tag>& id) const noexcept {
        return std::hash<studyapp::core::Uuid>{}(id.value());
    }
};

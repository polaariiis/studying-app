#pragma once

// Helpers shared by the command implementations (Commands.cpp, StudyCommands.cpp).

#include <studyapp/core/Error.hpp>
#include <studyapp/core/FractionalIndex.hpp>
#include <studyapp/document/Patch.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace studyapp::document::commands::detail {

/// Key after the last of `siblings` (or the first key).
template <class Id, class Lookup>
core::FractionalIndex appendKey(std::span<const Id> siblings, Lookup orderOf) {
    return siblings.empty() ? core::FractionalIndex::first()
                            : core::FractionalIndex::after(orderOf(siblings.back()));
}

template <class Id>
core::Error notFound(std::string_view what, const Id& id) {
    return core::Error{core::ErrorCode::NotFound,
                       std::string(what) + " " + id.toString() + " does not exist"};
}

inline Command makeCommand(std::string label, std::vector<AnyChange> changes) {
    return Command{std::move(label), Patch(std::move(changes))};
}

/// Order key that places `moved` at `index` among `siblings` (the destination's children,
/// which may include `moved` itself). nullopt: it is already there (no-op).
template <class Id, class OrderOf>
core::Result<std::optional<core::FractionalIndex>> keyAt(std::span<const Id> siblings, Id moved,
                                                         std::size_t index, OrderOf orderOf) {
    std::vector<Id> others;
    others.reserve(siblings.size());
    std::optional<std::size_t> current;
    for (const Id id : siblings) {
        if (id == moved) {
            current = others.size();
        } else {
            others.push_back(id);
        }
    }
    index = std::min(index, others.size());
    if (current == index) {
        return std::optional<core::FractionalIndex>{};
    }
    const std::optional<core::FractionalIndex> lower =
        index > 0 ? std::optional(orderOf(others[index - 1])) : std::nullopt;
    const std::optional<core::FractionalIndex> upper =
        index < others.size() ? std::optional(orderOf(others[index])) : std::nullopt;
    auto key = core::FractionalIndex::between(lower, upper);
    if (!key) {
        return tl::unexpected(key.error());
    }
    return std::optional(std::move(*key));
}

} // namespace studyapp::document::commands::detail

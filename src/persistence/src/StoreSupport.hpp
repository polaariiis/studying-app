#pragma once

#include <studyapp/core/Clock.hpp>
#include <studyapp/core/Error.hpp>
#include <studyapp/persistence/Database.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace studyapp::persistence::detail {

[[nodiscard]] inline std::int64_t millis(core::Timestamp time) noexcept {
    return time.time_since_epoch().count();
}

/// Forwards the error of a failed Result as the error of another Result type.
template <class T>
[[nodiscard]] tl::unexpected<core::Error> forward(core::Result<T>& failed) {
    return tl::unexpected<core::Error>(std::move(failed.error()));
}

/// Runs a cached, bound INSERT/UPDATE/DELETE and checks that it affected exactly one row.
/// A missing row means the database and the in-memory document disagree: Conflict.
[[nodiscard]] inline core::Result<void> runOnOneRow(Database& database, Statement& statement,
                                                    std::string_view what, const std::string& id) {
    if (auto done = statement.run(); !done) {
        return done;
    }
    if (database.changes() != 1) {
        return core::makeError(core::ErrorCode::Conflict,
                               std::string(what) + " " + id +
                                   " is not in the database as the change expects");
    }
    return {};
}

} // namespace studyapp::persistence::detail

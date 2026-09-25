#include <studyapp/persistence/Transaction.hpp>

#include <utility>

namespace studyapp::persistence {

using core::ErrorCode;
using core::makeError;
using core::Result;

Result<Transaction> Transaction::begin(Database& database, Kind kind) {
    if (database.inTransaction()) {
        return makeError(ErrorCode::Internal, "transactions do not nest");
    }
    auto begun = database.execute(kind == Kind::Immediate ? "BEGIN IMMEDIATE" : "BEGIN DEFERRED");
    if (!begun) {
        return tl::unexpected<core::Error>(std::move(begun.error()));
    }
    return Transaction(database);
}

Transaction::~Transaction() {
    rollback();
}

Transaction::Transaction(Transaction&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)),
      active_(std::exchange(other.active_, false)) {}

Result<void> Transaction::commit() {
    if (!isActive()) {
        return makeError(ErrorCode::Internal, "transaction is not active");
    }
    if (auto committed = database_->execute("COMMIT"); !committed) {
        rollback(); // a failed COMMIT leaves the transaction open; never leave it dangling
        return committed;
    }
    active_ = false;
    return {};
}

void Transaction::rollback() noexcept {
    if (!isActive()) {
        return;
    }
    active_ = false;
    // SQLite may already have rolled back automatically (e.g. after SQLITE_FULL); only
    // issue ROLLBACK while a transaction is actually open.
    if (database_->inTransaction()) {
        (void)database_->execute("ROLLBACK");
    }
}

} // namespace studyapp::persistence

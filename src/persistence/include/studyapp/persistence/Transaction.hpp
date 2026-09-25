#pragma once

#include <studyapp/core/Error.hpp>
#include <studyapp/persistence/Database.hpp>

namespace studyapp::persistence {

/// RAII transaction: BEGIN on creation, COMMIT on commit(), ROLLBACK if destroyed without
/// a successful commit (including after a failed commit). Transactions do not nest.
class Transaction {
public:
    enum class Kind {
        Deferred,  ///< BEGIN DEFERRED: reads first, lock taken on the first write
        Immediate, ///< BEGIN IMMEDIATE: takes the write lock up front (all writers)
    };

    [[nodiscard]] static core::Result<Transaction> begin(Database& database,
                                                         Kind kind = Kind::Immediate);

    ~Transaction();
    Transaction(Transaction&& other) noexcept;
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    [[nodiscard]] Database& database() const noexcept { return *database_; }
    [[nodiscard]] bool isActive() const noexcept { return database_ != nullptr && active_; }

    [[nodiscard]] core::Result<void> commit();
    /// Explicit rollback; also what the destructor does for an uncommitted transaction.
    void rollback() noexcept;

private:
    explicit Transaction(Database& database) noexcept : database_(&database) {}

    Database* database_ = nullptr;
    bool active_ = true;
};

} // namespace studyapp::persistence

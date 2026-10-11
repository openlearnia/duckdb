//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/transaction_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/parser/parsed_data/transaction_info.hpp"

namespace duckdb {

class ClientContext;
class MetaTransaction;
class Transaction;
class TransactionManager;

//! The transaction context keeps track of all the information relating to the
//! current transaction
class TransactionContext {
public:
	explicit TransactionContext(ClientContext &context);
	~TransactionContext();

	MetaTransaction &ActiveTransaction() {
		auto transaction = CurrentTransaction();
		if (!transaction) {
			throw InternalException("TransactionContext::ActiveTransaction called without active transaction");
		}
		return *transaction;
	}

	bool HasActiveTransaction() const {
		return current_transaction || borrowed_transaction;
	}

	void BeginTransaction();
	//! Run this context's statements in another context's open transaction without owning it (procedure SQL
	//! joining its caller's transaction). The owner keeps the transaction and must not use it, commit it or roll it
	//! back until EndBorrowTransaction; this context cannot commit or roll it back.
	void BorrowTransaction(MetaTransaction &transaction);
	void EndBorrowTransaction();
	bool IsBorrowedTransaction() const {
		return borrowed_transaction != nullptr;
	}
	void Commit();
	void Rollback(optional_ptr<ErrorData>);
	void ClearTransaction();
	void SetAutocheckpointError(ErrorData error);

	void SetAutoCommit(bool value);
	bool IsAutoCommit() const {
		return auto_commit;
	}

	void SetReadOnly();

	idx_t GetActiveQuery();
	void ResetActiveQuery();
	void SetActiveQuery(transaction_t query_number);

	void SetInvalidationPolicy(TransactionInvalidationPolicy new_invalidation_policy);
	TransactionInvalidationPolicy GetInvalidationPolicy() {
		return invalidation_policy;
	};
	void SetAutoRollback(bool new_auto_rollback) {
		auto_rollback = new_auto_rollback;
	};
	bool GetAutoRollback() {
		return auto_rollback;
	};

private:
	optional_ptr<MetaTransaction> CurrentTransaction() const {
		if (borrowed_transaction) {
			return borrowed_transaction;
		}
		return current_transaction.get();
	}

	ClientContext &context;
	bool auto_commit;
	TransactionInvalidationPolicy invalidation_policy = TransactionInvalidationPolicy::STANDARD_POLICY;
	bool auto_rollback = false;

	unique_ptr<MetaTransaction> current_transaction;
	//! A transaction owned by another context, see BorrowTransaction
	optional_ptr<MetaTransaction> borrowed_transaction;
	ErrorData autocheckpoint_error;

	TransactionContext(const TransactionContext &) = delete;
};

} // namespace duckdb

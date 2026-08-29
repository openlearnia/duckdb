#include "duckdb/execution/operator/helper/physical_call_procedure.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/parser.hpp"
#include "quickjs.h"

#include <condition_variable>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>

namespace duckdb {

//! Maximum number of JavaScript procedures allowed on the call stack of one thread at
//! once. Guards against unbounded mutual recursion through `duckdb.execute("CALL ...")`.
constexpr int kMaxProcedureNesting = 16;
constexpr const char *kNestingErrorText = "nesting limit exceeded";

static bool IsNestingLimitError(const string &message) {
	return strstr(message.c_str(), kNestingErrorText) != nullptr;
}

//! When the limit trips deep in a nested CALL chain every intermediate boundary
//! (C++ exception formatting, QuickJS error labelling) prepends its own tag. Reduce the
//! collapsed message back to its core sentence before surfacing it.
static string StripExceptionLabels(string message) {
	static const char *const kLabels[] = {"InternalError: ", "Invalid Input Error: ", "Invalid Input Exception: ",
	                                      "Catalog Error: ", "Parser Error: "};
	bool stripped_any = true;
	while (stripped_any) {
		stripped_any = false;
		for (auto *label : kLabels) {
			const auto length = strlen(label);
			if (StringUtil::StartsWith(message, string(label))) {
				message = message.substr(length);
				stripped_any = true;
			}
		}
	}
	return message;
}

thread_local int javascript_procedure_depth = 0;

struct ProcedureNestingGuard {
	ProcedureNestingGuard() {
		++javascript_procedure_depth;
	}
	~ProcedureNestingGuard() {
		--javascript_procedure_depth;
	}
};

struct ProcedureSqlOperation {
	string sql;
	vector<Value> bind_values;
	int magic = 0;
	//! Promise resolving functions. These are only touched on the QuickJS owner thread.
	JSValue resolve = JS_UNDEFINED;
	JSValue reject = JS_UNDEFINED;
};

struct ProcedureSqlCompletion {
	shared_ptr<ProcedureSqlOperation> operation;
	unique_ptr<QueryResult> result;
	string error;
};

enum class ProcedureTransactionPhase : uint8_t { IDLE, ACTIVE, DRAINING, COMMITTED, ROLLED_BACK };

//! State reachable from every QuickJS callback of one procedure invocation through
//! JS_SetRuntimeOpaque. The connection is declared before the worker so the worker is
//! joined before the connection is destroyed.
struct ProcedureSqlApi {
	ClientContext &client;
	//! Depth of the procedure that owns this invocation. Worker threads must propagate it
	//! across nested SQL CALLs because the recursion guard is thread-local.
	int nesting_depth = 0;
	//! True while the explicit duckdb.transaction callback owns the connection transaction.
	bool transaction_active = false;
	//! SQL execution errors invalidate the DuckDB transaction even if JavaScript catches them.
	bool transaction_rollback_only = false;
	string transaction_error;
	Connection connection;

	//! Promise-backed SQL operations are serialized per procedure connection. QuickJS values
	//! are rooted here until the owning thread settles the corresponding Promise.
	std::mutex async_mutex;
	std::condition_variable async_cv;
	std::deque<shared_ptr<ProcedureSqlOperation>> pending_operations;
	std::deque<ProcedureSqlCompletion> completed_operations;
	bool close_requested = false;
	bool worker_active = false;
	bool worker_finished = false;
	idx_t outstanding_operations = 0;
	std::thread worker;

	//! The transaction callback may remain suspended at await. Its callback and outer Promise
	//! values are inspected and settled by the QuickJS owner thread.
	ProcedureTransactionPhase transaction_phase = ProcedureTransactionPhase::IDLE;
	JSValue transaction_callback_promise = JS_UNDEFINED;
	JSValue transaction_callback_value = JS_UNDEFINED;
	JSValue transaction_callback_rejection = JS_UNDEFINED;
	JSValue transaction_resolve = JS_UNDEFINED;
	JSValue transaction_reject = JS_UNDEFINED;
	bool transaction_callback_is_promise = false;
	bool transaction_callback_rejected = false;

	explicit ProcedureSqlApi(ClientContext &client_p)
	    : client(client_p), nesting_depth(javascript_procedure_depth), transaction_active(false),
	      transaction_rollback_only(false), connection(*client_p.db) {
		worker = std::thread(&ProcedureSqlApi::WorkerLoop, this);
	}

	~ProcedureSqlApi() {
		RequestClose();
		if (worker.joinable()) {
			worker.join();
		}
	}

	void WorkerLoop();
	unique_ptr<QueryResult> ExecuteOperation(ProcedureSqlOperation &operation);
	bool EnqueueOperation(shared_ptr<ProcedureSqlOperation> operation);
	vector<ProcedureSqlCompletion> DrainCompletions();
	void RequestClose();
	bool HasOutstandingOperations();
	void WaitForActivity();
};

static JSValue ToJSValue(JSContext *js_context, const Value &value) {
	if (value.IsNull()) {
		return JS_NULL;
	}
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return JS_NewBool(js_context, value.GetValue<bool>());
	case LogicalTypeId::FLOAT:
		return JS_NewFloat64(js_context, value.GetValue<float>());
	case LogicalTypeId::DOUBLE:
		return JS_NewFloat64(js_context, value.GetValue<double>());
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
		return JS_NewInt64(js_context, value.GetValue<int64_t>());
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return JS_NewInt64(js_context, NumericCast<int64_t>(value.GetValue<uint64_t>()));
	default: {
		auto string_value = value.ToString();
		return JS_NewStringLen(js_context, string_value.c_str(), string_value.size());
	}
	}
}

static string GetJSError(JSContext *js_context) {
	auto exception = JS_GetException(js_context);
	auto message = JS_ToCString(js_context, exception);
	string result = message ? message : "JavaScript exception";
	if (message) {
		JS_FreeCString(js_context, message);
	}
	JS_FreeValue(js_context, exception);
	return result;
}

//! Return the JavaScript error's message field when it is an Error object. This keeps
//! duckdb.transaction from adding a second `Error:`/`InternalError:` prefix when it
//! rethrows a callback failure into JavaScript. Primitive throws fall back to their
//! string representation.
static string GetJSErrorMessage(JSContext *js_context) {
	auto exception = JS_GetException(js_context);
	string result;
	auto message_value = JS_GetPropertyStr(js_context, exception, "message");
	if (!JS_IsException(message_value)) {
		auto message = JS_ToCString(js_context, message_value);
		if (message) {
			result = message;
			JS_FreeCString(js_context, message);
		}
	}
	JS_FreeValue(js_context, message_value);
	if (result.empty()) {
		auto fallback = JS_ToCString(js_context, exception);
		if (fallback) {
			result = fallback;
			JS_FreeCString(js_context, fallback);
		} else {
			result = "JavaScript exception";
		}
	}
	JS_FreeValue(js_context, exception);
	return result;
}

static string GetJSValueErrorMessage(JSContext *js_context, JSValueConst exception) {
	string result;
	auto message_value = JS_GetPropertyStr(js_context, exception, "message");
	if (!JS_IsException(message_value)) {
		auto message = JS_ToCString(js_context, message_value);
		if (message) {
			result = message;
			JS_FreeCString(js_context, message);
		}
	}
	JS_FreeValue(js_context, message_value);
	if (result.empty()) {
		auto fallback = JS_ToCString(js_context, exception);
		if (fallback) {
			result = fallback;
			JS_FreeCString(js_context, fallback);
		} else {
			result = "JavaScript exception";
		}
	}
	return result;
}

static Value FromJSValue(JSContext *js_context, JSValueConst value, ClientContext &context,
                         const LogicalType &return_type) {
	if (JS_IsNull(value) || JS_IsUndefined(value)) {
		return Value(return_type);
	}
	if (return_type.id() == LogicalTypeId::BOOLEAN) {
		auto result = JS_ToBool(js_context, value);
		if (result < 0) {
			throw InvalidInputException("JavaScript procedure returned a value that cannot be cast to BOOLEAN: %s",
			                            GetJSError(js_context));
		}
		return Value::BOOLEAN(result != 0);
	}
	if (return_type.IsNumeric()) {
		double result;
		if (JS_ToFloat64(js_context, &result, value) < 0) {
			throw InvalidInputException("JavaScript procedure returned a value that cannot be cast to %s: %s",
			                            return_type.ToString(), GetJSError(js_context));
		}
		return Value::DOUBLE(result).CastAs(context, return_type);
	}
	auto result = JS_ToCString(js_context, value);
	if (!result) {
		throw InvalidInputException("JavaScript procedure returned a value that cannot be cast to %s: %s",
		                            return_type.ToString(), GetJSError(js_context));
	}
	Value duckdb_value(result);
	JS_FreeCString(js_context, result);
	return duckdb_value.CastAs(context, return_type);
}

//! Converts a JavaScript parameter into an (untyped-by-intent) SQL value. Type coercion
//! against the statement's expected parameter types happens during binding, so scalars
//! are carried as their natural DuckDB types here.
static bool JsToBindValue(JSContext *js_context, JSValueConst js_value, Value &result, string &error) {
	if (JS_IsBool(js_value)) {
		result = Value::BOOLEAN(JS_ToBool(js_context, js_value) != 0);
		return true;
	}
	if (JS_IsNumber(js_value)) {
		double double_result;
		if (JS_ToFloat64(js_context, &double_result, js_value) < 0) {
			error = GetJSError(js_context);
			return false;
		}
		int64_t integer_result;
		if (double_result >= static_cast<double>(NumericLimits<int64_t>::Minimum()) &&
		    double_result <= static_cast<double>(NumericLimits<int64_t>::Maximum()) &&
		    double_result == std::floor(double_result)) {
			result = Value::BIGINT(static_cast<int64_t>(double_result));
		} else {
			result = Value::DOUBLE(double_result);
		}
		return true;
	}
	if (JS_IsBigInt(js_value)) {
		uint64_t big_result;
		if (JS_ToBigUint64(js_context, &big_result, js_value) < 0) {
			error = GetJSError(js_context);
			return false;
		}
		result = Value::UBIGINT(big_result);
		return true;
	}
	if (JS_IsString(js_value)) {
		size_t length = 0;
		auto chars = JS_ToCStringLen(js_context, &length, js_value);
		if (!chars) {
			error = GetJSError(js_context);
			return false;
		}
		result = Value(string(chars, length));
		JS_FreeCString(js_context, chars);
		return true;
	}
	if (JS_IsNull(js_value) || JS_IsUndefined(js_value)) {
		result = Value();
		return true;
	}
	error = "unsupported parameter type; supported: boolean, number, bigint, string, null";
	return false;
}

//! Result-row values are exposed losslessly where practical: integral families become
//! numbers, wide numerics (DECIMAL/HUGEINT/UBIGINT) become doubles, temporal/complex
//! types become their string representation.
static JSValue JsValueFromDuckValue(JSContext *js_context, const Value &value) {
	if (value.IsNull()) {
		return JS_NULL;
	}
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return JS_NewBool(js_context, value.GetValue<bool>());
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
		return JS_NewInt64(js_context, value.GetValue<int64_t>());
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		return JS_NewInt64(js_context, static_cast<int64_t>(value.GetValue<uint32_t>()));
	case LogicalTypeId::FLOAT:
		return JS_NewFloat64(js_context, value.GetValue<float>());
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::UBIGINT:
		return JS_NewFloat64(js_context, value.GetValue<double>());
	default: {
		auto string_value = value.ToString();
		return JS_NewStringLen(js_context, string_value.c_str(), string_value.size());
	}
	}
}

static JSValue ThrowSqlError(JSContext *js_context, const string &message) {
	return JS_ThrowInternalError(js_context, "%s", message.c_str());
}

unique_ptr<QueryResult> ProcedureSqlApi::ExecuteOperation(ProcedureSqlOperation &operation) {
	if (!operation.bind_values.empty()) {
		auto prepared = connection.Prepare(operation.sql);
		if (!prepared) {
			throw InvalidInputException("failed to prepare SQL operation");
		}
		if (prepared->HasError()) {
			throw InvalidInputException("%s", prepared->GetError());
		}
		if (operation.bind_values.size() > prepared->GetExpectedParameterTypes().size()) {
			throw InvalidInputException("too many bound parameters supplied");
		}
		return prepared->Execute(operation.bind_values, /*allow_stream_result=*/false);
	}
	return connection.Query(operation.sql);
}

void ProcedureSqlApi::WorkerLoop() {
	for (;;) {
		shared_ptr<ProcedureSqlOperation> operation;
		{
			std::unique_lock<std::mutex> lock(async_mutex);
			async_cv.wait(lock, [&]() { return close_requested || !pending_operations.empty(); });
			if (pending_operations.empty()) {
				if (close_requested) {
					break;
				}
				continue;
			}
			operation = std::move(pending_operations.front());
			pending_operations.pop_front();
			worker_active = true;
		}

		ProcedureSqlCompletion completion;
		completion.operation = operation;
		auto previous_depth = javascript_procedure_depth;
		javascript_procedure_depth = nesting_depth;
		try {
			completion.result = ExecuteOperation(*operation);
			if (!completion.result) {
				completion.error = "query did not produce a result";
			} else if (completion.result->HasError()) {
				completion.error = completion.result->GetError();
			}
		} catch (const std::exception &ex) {
			completion.error = ex.what();
		} catch (...) {
			completion.error = "unknown error while executing SQL";
		}
		javascript_procedure_depth = previous_depth;

		{
			std::lock_guard<std::mutex> lock(async_mutex);
			worker_active = false;
			completed_operations.push_back(std::move(completion));
		}
		async_cv.notify_all();
	}

	{
		std::lock_guard<std::mutex> lock(async_mutex);
		worker_finished = true;
	}
	async_cv.notify_all();
}

bool ProcedureSqlApi::EnqueueOperation(shared_ptr<ProcedureSqlOperation> operation) {
	std::lock_guard<std::mutex> lock(async_mutex);
	if (close_requested) {
		return false;
	}
	pending_operations.push_back(std::move(operation));
	++outstanding_operations;
	async_cv.notify_one();
	return true;
}

vector<ProcedureSqlCompletion> ProcedureSqlApi::DrainCompletions() {
	vector<ProcedureSqlCompletion> result;
	std::lock_guard<std::mutex> lock(async_mutex);
	while (!completed_operations.empty()) {
		result.push_back(std::move(completed_operations.front()));
		completed_operations.pop_front();
	}
	return result;
}

void ProcedureSqlApi::RequestClose() {
	{
		std::lock_guard<std::mutex> lock(async_mutex);
		if (close_requested) {
			return;
		}
		close_requested = true;
		while (!pending_operations.empty()) {
			ProcedureSqlCompletion completion;
			completion.operation = std::move(pending_operations.front());
			pending_operations.pop_front();
			completion.error = "procedure SQL session is closing";
			completed_operations.push_back(std::move(completion));
		}
	}
	async_cv.notify_all();
}

bool ProcedureSqlApi::HasOutstandingOperations() {
	std::lock_guard<std::mutex> lock(async_mutex);
	return outstanding_operations != 0;
}

void ProcedureSqlApi::WaitForActivity() {
	std::unique_lock<std::mutex> lock(async_mutex);
	async_cv.wait_for(lock, std::chrono::milliseconds(1));
}

//! Roll back an explicit procedure transaction without allowing cleanup failures to
//! cross the QuickJS C stack. The caller's error is preserved and augmented when the
//! rollback itself reports an error.
static string RollbackProcedureTransaction(ProcedureSqlApi &api) {
	if (!api.transaction_active) {
		return string();
	}
	string rollback_error;
	try {
		if (api.connection.HasActiveTransaction()) {
			api.connection.Rollback();
		}
	} catch (const std::exception &ex) {
		rollback_error = ex.what();
	} catch (...) {
		rollback_error = "unknown error while rolling back transaction";
	}
	api.transaction_active = false;
	api.transaction_rollback_only = false;
	api.transaction_error.clear();
	return rollback_error;
}

static string AppendRollbackError(string error, const string &rollback_error) {
	if (!rollback_error.empty()) {
		error += StringUtil::Format("; transaction rollback failed: %s", rollback_error);
	}
	return error;
}

static void MarkProcedureTransactionRollbackOnly(ProcedureSqlApi &api, const string &error) {
	if (!api.transaction_active || api.transaction_rollback_only) {
		return;
	}
	api.transaction_rollback_only = true;
	api.transaction_error = error;
}

static JSValue NewProcedureError(JSContext *js_context, const string &message) {
	JS_ThrowInternalError(js_context, "%s", message.c_str());
	return JS_GetException(js_context);
}

static bool CallPromiseResolver(JSContext *js_context, JSValueConst resolver, JSValue value, string &error) {
	JSValue argv[] = {value};
	auto result = JS_Call(js_context, resolver, JS_UNDEFINED, 1, argv);
	if (JS_IsException(result)) {
		error = GetJSError(js_context);
		return false;
	}
	JS_FreeValue(js_context, result);
	return true;
}

static JSValue QueryResultToJS(JSContext *js_context, QueryResult &result) {
	if (result.GetResultType() != QueryResultType::MATERIALIZED_RESULT) {
		throw InvalidInputException("SQL query did not produce a materialized result");
	}
	auto &materialized = static_cast<MaterializedQueryResult &>(result);
	auto row_array = JS_NewArray(js_context);
	if (JS_IsException(row_array)) {
		throw InvalidInputException("%s", GetJSError(js_context));
	}
	const idx_t column_count = materialized.ColumnCount();
	for (idx_t row_index = 0; row_index < materialized.RowCount(); row_index++) {
		auto row_object = JS_NewObject(js_context);
		if (JS_IsException(row_object)) {
			JS_FreeValue(js_context, row_array);
			throw InvalidInputException("%s", GetJSError(js_context));
		}
		for (idx_t col_index = 0; col_index < column_count; col_index++) {
			auto cell = JsValueFromDuckValue(js_context, materialized.GetValue(col_index, row_index));
			if (JS_IsException(cell)) {
				JS_FreeValue(js_context, row_object);
				JS_FreeValue(js_context, row_array);
				throw InvalidInputException("%s", GetJSError(js_context));
			}
			auto status = JS_SetPropertyStr(js_context, row_object,
			                                materialized.GetNames()[col_index].GetIdentifierName().c_str(), cell);
			if (status < 0) {
				JS_FreeValue(js_context, row_object);
				JS_FreeValue(js_context, row_array);
				throw InvalidInputException("%s", GetJSError(js_context));
			}
		}
		if (JS_DefinePropertyValueUint32(js_context, row_array, NumericCast<uint32_t>(row_index), row_object,
		                                 JS_PROP_C_W_E) < 0) {
			JS_FreeValue(js_context, row_array);
			throw InvalidInputException("%s", GetJSError(js_context));
		}
	}
	return row_array;
}

static void ReleaseOperationResolvers(JSContext *js_context, ProcedureSqlOperation &operation) {
	JS_FreeValue(js_context, operation.resolve);
	JS_FreeValue(js_context, operation.reject);
	operation.resolve = JS_UNDEFINED;
	operation.reject = JS_UNDEFINED;
}

static void CompleteOperationCount(ProcedureSqlApi &api) {
	std::lock_guard<std::mutex> lock(api.async_mutex);
	D_ASSERT(api.outstanding_operations > 0);
	if (api.outstanding_operations > 0) {
		--api.outstanding_operations;
	}
}

//! Transaction control is owned by duckdb.transaction(callback). Inspect the lexer tokens
//! at statement boundaries so comments and all SQL literal forms are ignored without
//! reparsing or executing the helper query.
static bool IsTransactionControlStatement(const string &sql) {
	auto tokens = Parser::Tokenize(sql);
	bool statement_start = true;
	for (idx_t index = 0; index < tokens.size(); index++) {
		const auto &token = tokens[index];
		if (token.type == SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT) {
			continue;
		}
		if (statement_start) {
			if (token.type == SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR && token.start < sql.size() &&
			    sql[token.start] == ';') {
				continue;
			}
			statement_start = false;
			if (token.type != SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD) {
				continue;
			}
			idx_t keyword_end = token.start;
			while (keyword_end < sql.size() && StringUtil::CharacterIsAlpha(sql[keyword_end])) {
				keyword_end++;
			}
			string keyword;
			keyword.reserve(keyword_end - token.start);
			for (auto position = token.start; position < keyword_end; position++) {
				keyword.push_back(StringUtil::CharacterToUpper(sql[position]));
			}
			if (keyword == "BEGIN" || keyword == "START" || keyword == "ABORT" || keyword == "ROLLBACK" ||
			    keyword == "COMMIT" || keyword == "END") {
				return true;
			}
		} else if (token.type == SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR && token.start < sql.size() &&
		           sql[token.start] == ';') {
			statement_start = true;
		}
	}
	return false;
}

static void FreeTransactionValues(JSContext *js_context, ProcedureSqlApi &api) {
	JS_FreeValue(js_context, api.transaction_callback_promise);
	JS_FreeValue(js_context, api.transaction_callback_value);
	JS_FreeValue(js_context, api.transaction_callback_rejection);
	JS_FreeValue(js_context, api.transaction_resolve);
	JS_FreeValue(js_context, api.transaction_reject);
	api.transaction_callback_promise = JS_UNDEFINED;
	api.transaction_callback_value = JS_UNDEFINED;
	api.transaction_callback_rejection = JS_UNDEFINED;
	api.transaction_resolve = JS_UNDEFINED;
	api.transaction_reject = JS_UNDEFINED;
	api.transaction_callback_is_promise = false;
	api.transaction_callback_rejected = false;
}

static string RejectTransactionPromise(JSContext *js_context, ProcedureSqlApi &api, JSValue value) {
	string error;
	if (!CallPromiseResolver(js_context, api.transaction_reject, value, error) && error.empty()) {
		error = "failed to reject transaction Promise";
	}
	return error;
}

static string ResolveTransactionPromise(JSContext *js_context, ProcedureSqlApi &api, JSValue value) {
	string error;
	if (!CallPromiseResolver(js_context, api.transaction_resolve, value, error) && error.empty()) {
		error = "failed to resolve transaction Promise";
	}
	return error;
}

//! Advance the asynchronous transaction state machine. This function is called only from
//! the QuickJS owner thread, after SQL completions have been settled.
static string AdvanceProcedureTransaction(JSContext *js_context, ProcedureSqlApi &api) {
	if (api.transaction_phase == ProcedureTransactionPhase::ACTIVE && api.transaction_callback_is_promise) {
		auto state = JS_PromiseState(js_context, api.transaction_callback_promise);
		if (state == JS_PROMISE_PENDING) {
			return string();
		}
		if (state != JS_PROMISE_FULFILLED && state != JS_PROMISE_REJECTED) {
			return "invalid transaction callback Promise state";
		}
		if (state == JS_PROMISE_REJECTED) {
			api.transaction_callback_rejected = true;
			api.transaction_callback_rejection = JS_PromiseResult(js_context, api.transaction_callback_promise);
		} else {
			api.transaction_callback_value = JS_PromiseResult(js_context, api.transaction_callback_promise);
		}
		JS_FreeValue(js_context, api.transaction_callback_promise);
		api.transaction_callback_promise = JS_UNDEFINED;
		api.transaction_callback_is_promise = false;
		api.transaction_phase = ProcedureTransactionPhase::DRAINING;
	}

	if (api.transaction_phase != ProcedureTransactionPhase::DRAINING || api.HasOutstandingOperations()) {
		return string();
	}

	if (api.transaction_callback_rejected || api.transaction_rollback_only) {
		auto transaction_reason = api.transaction_error.empty() ? string("transaction is aborted") : api.transaction_error;
		string rollback_error = RollbackProcedureTransaction(api);
		api.transaction_phase = ProcedureTransactionPhase::ROLLED_BACK;
		JSValue rejection = JS_UNDEFINED;
		bool generated_rejection = false;
		if (api.transaction_callback_rejected && !JS_IsUndefined(api.transaction_callback_rejection)) {
			rejection = api.transaction_callback_rejection;
		} else {
			if (!rollback_error.empty()) {
				transaction_reason = AppendRollbackError(transaction_reason, rollback_error);
			}
			rejection = NewProcedureError(js_context, transaction_reason);
			generated_rejection = true;
		}
		auto resolver_error = RejectTransactionPromise(js_context, api, rejection);
		if (generated_rejection) {
			JS_FreeValue(js_context, rejection);
		}
		FreeTransactionValues(js_context, api);
		return resolver_error;
	}

	try {
		api.connection.Commit();
		api.transaction_active = false;
		api.transaction_rollback_only = false;
		api.transaction_error.clear();
		api.transaction_phase = ProcedureTransactionPhase::COMMITTED;
		auto resolver_error = ResolveTransactionPromise(js_context, api, api.transaction_callback_value);
		FreeTransactionValues(js_context, api);
		return resolver_error;
	} catch (const std::exception &ex) {
		auto error = AppendRollbackError(ex.what(), RollbackProcedureTransaction(api));
		api.transaction_phase = ProcedureTransactionPhase::ROLLED_BACK;
		auto rejection = NewProcedureError(js_context, error);
		auto resolver_error = RejectTransactionPromise(js_context, api, rejection);
		JS_FreeValue(js_context, rejection);
		FreeTransactionValues(js_context, api);
		return resolver_error;
	} catch (...) {
		auto error = AppendRollbackError("unknown error while committing transaction", RollbackProcedureTransaction(api));
		api.transaction_phase = ProcedureTransactionPhase::ROLLED_BACK;
		auto rejection = NewProcedureError(js_context, error);
		auto resolver_error = RejectTransactionPromise(js_context, api, rejection);
		JS_FreeValue(js_context, rejection);
		FreeTransactionValues(js_context, api);
		return resolver_error;
	}
}

//! Implementation behind duckdb.transaction(callback). The callback may suspend at await;
//! its outer Promise is settled by AdvanceProcedureTransaction after callback and SQL work
//! have both completed.
static JSValue JsCallTransaction(JSContext *js_context, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                                 JSValue *func_data) {
	auto *api = static_cast<ProcedureSqlApi *>(JS_GetRuntimeOpaque(JS_GetRuntime(js_context)));
	if (!api) {
		return ThrowSqlError(js_context, "procedure SQL session unavailable");
	}
	if (argc != 1 || !JS_IsFunction(js_context, argv[0])) {
		return JS_ThrowTypeError(js_context, "duckdb.transaction expects a callback function");
	}
	if (api->transaction_active || api->transaction_phase == ProcedureTransactionPhase::ACTIVE ||
	    api->transaction_phase == ProcedureTransactionPhase::DRAINING) {
		return JS_ThrowInternalError(js_context, "duckdb.transaction cannot be nested");
	}

	JSValue resolving_funcs[2];
	auto promise = JS_NewPromiseCapability(js_context, resolving_funcs);
	if (JS_IsException(promise)) {
		return promise;
	}
	api->transaction_resolve = resolving_funcs[0];
	api->transaction_reject = resolving_funcs[1];
	api->transaction_phase = ProcedureTransactionPhase::ACTIVE;
	api->transaction_callback_is_promise = false;
	api->transaction_callback_rejected = false;
	api->transaction_callback_promise = JS_UNDEFINED;
	api->transaction_callback_value = JS_UNDEFINED;
	api->transaction_callback_rejection = JS_UNDEFINED;

	try {
		api->connection.BeginTransaction();
		api->transaction_active = true;
		api->transaction_rollback_only = false;
		api->transaction_error.clear();

		auto callback_result = JS_Call(js_context, argv[0], JS_UNDEFINED, 0, nullptr);
		if (JS_IsException(callback_result)) {
			api->transaction_callback_rejected = true;
			api->transaction_callback_rejection = JS_GetException(js_context);
			api->transaction_phase = ProcedureTransactionPhase::DRAINING;
		} else if (JS_IsPromise(callback_result)) {
			api->transaction_callback_promise = callback_result;
			api->transaction_callback_is_promise = true;
		} else {
			api->transaction_callback_value = callback_result;
			api->transaction_phase = ProcedureTransactionPhase::DRAINING;
		}
		return promise;
	} catch (const std::exception &ex) {
		auto error = AppendRollbackError(ex.what(), RollbackProcedureTransaction(*api));
		api->transaction_phase = ProcedureTransactionPhase::ROLLED_BACK;
		auto rejection = NewProcedureError(js_context, error);
		string ignored;
		RejectTransactionPromise(js_context, *api, rejection);
		JS_FreeValue(js_context, rejection);
		FreeTransactionValues(js_context, *api);
		return promise;
	} catch (...) {
		auto error = AppendRollbackError("unknown error while executing transaction", RollbackProcedureTransaction(*api));
		api->transaction_phase = ProcedureTransactionPhase::ROLLED_BACK;
		auto rejection = NewProcedureError(js_context, error);
		string ignored;
		RejectTransactionPromise(js_context, *api, rejection);
		JS_FreeValue(js_context, rejection);
		FreeTransactionValues(js_context, *api);
		return promise;
	}
}

//! Implementation behind both duckdb.execute(sql[, params]) and duckdb.query(sql[, params]).
//! magic 0 = execute (side effects only, resolves to undefined)
//! magic 1 = query (resolves to Array<Object>, one object per row keyed by column name)
//!
//! Argument conversion and transaction-control validation run on the QuickJS thread. The SQL
//! operation itself is queued for the invocation worker and its completion is settled by the
//! procedure host loop.
static JSValue JsCallSqlApi(JSContext *js_context, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                            JSValue *func_data) {
	auto *api = static_cast<ProcedureSqlApi *>(JS_GetRuntimeOpaque(JS_GetRuntime(js_context)));
	if (!api) {
		return ThrowSqlError(js_context, "procedure SQL session unavailable");
	}
	const char *sql_chars = nullptr;
	try {
		if (argc < 1 || !JS_IsString(argv[0])) {
			return JS_ThrowTypeError(js_context, "%s expects a SQL string as its first argument",
			                         magic == 1 ? "duckdb.query" : "duckdb.execute");
		}
		size_t sql_length = 0;
		sql_chars = JS_ToCStringLen(js_context, &sql_length, argv[0]);
		if (!sql_chars) {
			return JS_ThrowInternalError(js_context, "%s", GetJSError(js_context).c_str());
		}

		vector<Value> bind_values;
		if (argc >= 2 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
			if (!JS_IsArray(argv[1])) {
				JS_FreeCString(js_context, sql_chars);
				return JS_ThrowTypeError(js_context,
				                         "the parameters argument of %s must be an array",
				                         magic == 1 ? "duckdb.query" : "duckdb.execute");
			}
			auto length_value = JS_GetPropertyStr(js_context, argv[1], "length");
			int64_t parameter_count = 0;
			if (JS_ToInt64(js_context, &parameter_count, length_value) < 0) {
				JS_FreeValue(js_context, length_value);
				JS_FreeCString(js_context, sql_chars);
				return JS_ThrowInternalError(js_context, "%s", GetJSError(js_context).c_str());
			}
			JS_FreeValue(js_context, length_value);
			if (parameter_count < 0 || static_cast<uint64_t>(parameter_count) > NumericLimits<idx_t>::Maximum()) {
				JS_FreeCString(js_context, sql_chars);
				return ThrowSqlError(js_context, "parameter array is too large");
			}
			bind_values.reserve(NumericCast<idx_t>(parameter_count));
			for (int64_t i = 0; i < parameter_count; i++) {
				auto element = JS_GetPropertyUint32(js_context, argv[1], NumericCast<uint32_t>(i));
				if (JS_IsException(element)) {
					JS_FreeCString(js_context, sql_chars);
					return JS_ThrowInternalError(js_context, "%s", GetJSError(js_context).c_str());
				}
				Value bind_value;
				string error;
				auto success = JsToBindValue(js_context, element, bind_value, error);
				JS_FreeValue(js_context, element);
				if (!success) {
					JS_FreeCString(js_context, sql_chars);
					return ThrowSqlError(js_context, StringUtil::Format(
					    "failed to convert parameter %lld: %s", static_cast<long long>(i), error));
				}
				bind_values.push_back(std::move(bind_value));
			}
		}

		string sql(sql_chars, sql_length);
		JS_FreeCString(js_context, sql_chars);
		sql_chars = nullptr;
		if (IsTransactionControlStatement(sql)) {
			return ThrowSqlError(js_context, "transaction control statements must use duckdb.transaction");
		}

		JSValue resolving_funcs[2];
		auto promise = JS_NewPromiseCapability(js_context, resolving_funcs);
		if (JS_IsException(promise)) {
			return promise;
		}
		auto operation = make_shared_ptr<ProcedureSqlOperation>();
		operation->sql = std::move(sql);
		operation->bind_values = std::move(bind_values);
		operation->magic = magic;
		operation->resolve = resolving_funcs[0];
		operation->reject = resolving_funcs[1];
		if (!api->EnqueueOperation(operation)) {
			auto error = NewProcedureError(js_context, "procedure SQL session is closing");
			string ignored;
			CallPromiseResolver(js_context, operation->reject, error, ignored);
			JS_FreeValue(js_context, error);
			ReleaseOperationResolvers(js_context, *operation);
			return promise;
		}
		return promise;
	} catch (const std::exception &ex) {
		if (sql_chars) {
			JS_FreeCString(js_context, sql_chars);
		}
		MarkProcedureTransactionRollbackOnly(*api, ex.what());
		return ThrowSqlError(js_context, ex.what());
	} catch (...) {
		if (sql_chars) {
			JS_FreeCString(js_context, sql_chars);
		}
		MarkProcedureTransactionRollbackOnly(*api, "unknown error while queuing SQL");
		return ThrowSqlError(js_context, "unknown error while queuing SQL");
	}
}

//! Settle one worker completion on the QuickJS owner thread. Query-result conversion and
//! transaction rollback bookkeeping intentionally happen here rather than in WorkerLoop.
static string SettleSqlCompletion(JSContext *js_context, ProcedureSqlApi &api,
                                  ProcedureSqlCompletion &completion) {
	auto &operation = *completion.operation;
	string error = completion.error;
	JSValue value = JS_UNDEFINED;
	if (error.empty() && (!completion.result || completion.result->HasError())) {
		error = completion.result ? completion.result->GetError() : "query did not produce a result";
	}
	if (error.empty() && operation.magic == 1) {
		try {
			value = QueryResultToJS(js_context, *completion.result);
		} catch (const std::exception &ex) {
			error = ex.what();
		} catch (...) {
			error = "unknown error while converting SQL result";
		}
	}

	if (!error.empty()) {
		MarkProcedureTransactionRollbackOnly(api, error);
		value = NewProcedureError(js_context, error);
		string resolver_error;
		if (!CallPromiseResolver(js_context, operation.reject, value, resolver_error) && resolver_error.empty()) {
			resolver_error = "failed to reject SQL Promise";
		}
		JS_FreeValue(js_context, value);
		ReleaseOperationResolvers(js_context, operation);
		CompleteOperationCount(api);
		return resolver_error;
	}

	string resolver_error;
	if (operation.magic == 0) {
		value = JS_UNDEFINED;
	}
	if (!CallPromiseResolver(js_context, operation.resolve, value, resolver_error) && resolver_error.empty()) {
		resolver_error = "failed to resolve SQL Promise";
	}
	JS_FreeValue(js_context, value);
	ReleaseOperationResolvers(js_context, operation);
	CompleteOperationCount(api);
	return resolver_error;
}

static string DrainAndSettleSqlCompletions(JSContext *js_context, ProcedureSqlApi &api) {
	string error;
	for (auto &completion : api.DrainCompletions()) {
		auto completion_error = SettleSqlCompletion(js_context, api, completion);
		if (error.empty() && !completion_error.empty()) {
			error = completion_error;
		}
	}
	return error;
}

//! Stop the invocation worker and release any completion records that cannot be settled
//! because the host loop itself failed. This must run while the QuickJS context is alive:
//! operation records own Promise resolver handles even when their SQL never completes.
static void CloseProcedureSqlApi(JSContext *js_context, ProcedureSqlApi &api) {
	api.RequestClose();
	if (api.worker.joinable()) {
		api.worker.join();
	}
	for (auto &completion : api.DrainCompletions()) {
		if (completion.operation) {
			ReleaseOperationResolvers(js_context, *completion.operation);
			CompleteOperationCount(api);
		}
	}
	D_ASSERT(!api.HasOutstandingOperations());
}

static bool IsProcedureTransactionPending(const ProcedureSqlApi &api) {
	return api.transaction_phase == ProcedureTransactionPhase::ACTIVE ||
	       api.transaction_phase == ProcedureTransactionPhase::DRAINING;
}

//! Drive the QuickJS Promise graph and the invocation SQL queue until the root async
//! procedure has settled. The returned value is owned by the caller and must be freed in
//! the same QuickJS context.
static JSValue RunProcedurePromiseLoop(JSContext *js_context, ProcedureSqlApi &api, JSValueConst root_promise,
                                       bool &rejected) {
	auto runtime = JS_GetRuntime(js_context);
	bool root_rejection_handled = false;
	for (;;) {
		auto completion_error = DrainAndSettleSqlCompletions(js_context, api);
		if (!completion_error.empty()) {
			api.RequestClose();
			throw InvalidInputException("JavaScript procedure SQL Promise settlement failed: %s", completion_error);
		}
		auto transaction_error = AdvanceProcedureTransaction(js_context, api);
		if (!transaction_error.empty()) {
			api.RequestClose();
			throw InvalidInputException("JavaScript procedure transaction Promise settlement failed: %s", transaction_error);
		}

		JSContext *job_context = nullptr;
		bool executed_job = false;
		for (;;) {
			auto job_result = JS_ExecutePendingJob(runtime, &job_context);
			if (job_result < 0) {
				auto error = GetJSError(job_context ? job_context : js_context);
				api.RequestClose();
				throw InvalidInputException("JavaScript procedure Promise job failed: %s", error);
			}
			if (job_result == 0) {
				break;
			}
			executed_job = true;
		}

		auto root_state = JS_PromiseState(js_context, root_promise);
		if (root_state != JS_PROMISE_PENDING && root_state != JS_PROMISE_FULFILLED &&
		    root_state != JS_PROMISE_REJECTED) {
			api.RequestClose();
			throw InvalidInputException("JavaScript procedure did not return a Promise");
		}
		if (root_state == JS_PROMISE_REJECTED && !root_rejection_handled) {
			// A procedure can reject without awaiting a transaction callback. Abort that
			// transaction and stop accepting queued work rather than leaving invocation
			// state open indefinitely. Outside a transaction this still drains/cancels the
			// queue before the QuickJS runtime is destroyed.
			if (api.transaction_active) {
				api.transaction_callback_rejected = true;
				api.transaction_callback_rejection = JS_PromiseResult(js_context, root_promise);
				if (api.transaction_callback_is_promise) {
					JS_FreeValue(js_context, api.transaction_callback_promise);
					api.transaction_callback_promise = JS_UNDEFINED;
					api.transaction_callback_is_promise = false;
				}
				api.transaction_phase = ProcedureTransactionPhase::DRAINING;
			}
			root_rejection_handled = true;
			api.RequestClose();
		}

		if (root_state != JS_PROMISE_PENDING && !api.HasOutstandingOperations() &&
		    !IsProcedureTransactionPending(api)) {
			rejected = root_state == JS_PROMISE_REJECTED;
			auto result = JS_PromiseResult(js_context, root_promise);
			api.RequestClose();
			return result;
		}
		if (!executed_job) {
			api.WaitForActivity();
		}
	}
}

//! Installs the `duckdb` global: { execute(sql[, params]), query(sql[, params]), transaction(callback) }.
static void RegisterSqlApi(JSContext *js_context) {
	auto global_object = JS_GetGlobalObject(js_context);
	auto api_object = JS_NewObject(js_context);
	if (!JS_IsObject(api_object)) {
		throw OutOfMemoryException("Unable to allocate JavaScript SQL API object");
	}
	// Parameters:
	// - magic 0 → execute(), magic 1 → query()
	// - the connection is reached via the runtime opaque; no func_data refs required
	auto execute_fn = JS_NewCFunctionData(js_context, JsCallSqlApi, 2, 0, 0, nullptr);
	auto query_fn = JS_NewCFunctionData(js_context, JsCallSqlApi, 2, 1, 0, nullptr);
	auto transaction_fn = JS_NewCFunctionData(js_context, JsCallTransaction, 1, 0, 0, nullptr);
	JS_SetPropertyStr(js_context, api_object, "execute", execute_fn);
	JS_SetPropertyStr(js_context, api_object, "query", query_fn);
	JS_SetPropertyStr(js_context, api_object, "transaction", transaction_fn);
	JS_SetPropertyStr(js_context, global_object, "duckdb", api_object);
	JS_FreeValue(js_context, global_object);
}

PhysicalCallProcedure::PhysicalCallProcedure(PhysicalPlan &physical_plan, ProcedureCatalogEntry &procedure,
                                             vector<unique_ptr<Expression>> arguments, idx_t estimated_cardinality)
	: PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, {procedure.return_type}, estimated_cardinality),
	  procedure(procedure), arguments(std::move(arguments)) {
}

SourceResultType PhysicalCallProcedure::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                        OperatorSourceInput &) const {
	if (chunk.size() != 0) {
		return SourceResultType::FINISHED;
	}
	if (javascript_procedure_depth >= kMaxProcedureNesting) {
		throw InvalidInputException(
		    "JavaScript procedure %s (%d): procedures calling procedures "
		    "(including indirectly through SQL) may not nest more than %d levels deep",
		    kNestingErrorText, kMaxProcedureNesting, kMaxProcedureNesting);
	}
	ProcedureNestingGuard nesting_guard;

	vector<JSValue> js_arguments;
	vector<Value> values;
	values.reserve(arguments.size());
	for (auto &argument : arguments) {
		values.push_back(ExpressionExecutor::EvaluateScalar(context.client, *argument));
	}

	// Owns the nested SQL connection for the duration of this invocation; the QuickJS
	// runtime points at it through its opaque handle so every `duckdb.*` call reaches it.
	ProcedureSqlApi sql_api(context.client);

	auto runtime = JS_NewRuntime();
	if (!runtime) {
		throw OutOfMemoryException("Unable to allocate JavaScript procedure runtime");
	}
	JS_SetMemoryLimit(runtime, 64ULL * 1024 * 1024);
	JS_SetMaxStackSize(runtime, 8ULL * 1024 * 1024);
	JS_SetRuntimeOpaque(runtime, &sql_api);
	auto js_context = JS_NewContext(runtime);
	if (!js_context) {
		JS_FreeRuntime(runtime);
		throw OutOfMemoryException("Unable to allocate JavaScript procedure context");
	}

	JSValue function = JS_UNDEFINED;
	JSValue root_promise = JS_UNDEFINED;
	JSValue settled_value = JS_UNDEFINED;
	try {
		RegisterSqlApi(js_context);
		auto source = "(async function(" + StringUtil::Join(procedure.parameter_names, ", ") + ") {\n" + procedure.body +
		              "\n})";
		function = JS_Eval(js_context, source.c_str(), source.size(), "<procedure>", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(function)) {
			auto error = GetJSError(js_context);
			throw InvalidInputException("JavaScript procedure '%s' could not be compiled: %s", procedure.name.GetIdentifierName(), error);
		}
		for (auto &value : values) {
			js_arguments.push_back(ToJSValue(js_context, value));
		}
		root_promise =
		    JS_Call(js_context, function, JS_UNDEFINED, NumericCast<int>(js_arguments.size()), js_arguments.data());
		for (auto &argument : js_arguments) {
			JS_FreeValue(js_context, argument);
		}
		js_arguments.clear();
		JS_FreeValue(js_context, function);
		function = JS_UNDEFINED;
		if (JS_IsException(root_promise)) {
			auto error = GetJSError(js_context);
			if (IsNestingLimitError(error)) {
				// already the collapsed root cause from a deeper invocation: propagate the
				// core message instead of accumulating one label per recursion level
				throw InvalidInputException(
				    StripExceptionLabels(error.substr(0, error.find('\n'))));
			}
			throw InvalidInputException("JavaScript procedure '%s' failed: %s", procedure.name.GetIdentifierName(), error);
		}
		bool rejected = false;
		settled_value = RunProcedurePromiseLoop(js_context, sql_api, root_promise, rejected);
		JS_FreeValue(js_context, root_promise);
		root_promise = JS_UNDEFINED;
		if (rejected) {
			auto error = GetJSValueErrorMessage(js_context, settled_value);
			JS_FreeValue(js_context, settled_value);
			settled_value = JS_UNDEFINED;
			if (IsNestingLimitError(error)) {
				throw InvalidInputException(StripExceptionLabels(error.substr(0, error.find('\n'))));
			}
			throw InvalidInputException("JavaScript procedure '%s' failed: %s", procedure.name.GetIdentifierName(), error);
		}
		auto duckdb_result = FromJSValue(js_context, settled_value, context.client, procedure.return_type);
		JS_FreeValue(js_context, settled_value);
		settled_value = JS_UNDEFINED;
		CloseProcedureSqlApi(js_context, sql_api);
		JS_FreeContext(js_context);
		JS_FreeRuntime(runtime);

		chunk.SetValue(0, 0, duckdb_result);
		chunk.SetCardinality(1);
		return SourceResultType::FINISHED;
	} catch (...) {
		for (auto &argument : js_arguments) {
			JS_FreeValue(js_context, argument);
		}
		JS_FreeValue(js_context, function);
		JS_FreeValue(js_context, root_promise);
		JS_FreeValue(js_context, settled_value);
		CloseProcedureSqlApi(js_context, sql_api);
		if (sql_api.transaction_active) {
			RollbackProcedureTransaction(sql_api);
		}
		FreeTransactionValues(js_context, sql_api);
		JS_FreeContext(js_context);
		JS_FreeRuntime(runtime);
		throw;
	}
}

} // namespace duckdb

#include "duckdb/execution/operator/helper/physical_call_procedure.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "quickjs.h"

#include <cmath>

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

//! State reachable from every QuickJS callback of one procedure invocation through
//! JS_SetRuntimeOpaque. 'connection' outlives the QuickJS context (declared last).
struct ProcedureSqlApi {
	ClientContext &client;
	Connection connection;

	explicit ProcedureSqlApi(ClientContext &client_p)
	    : client(client_p), connection(*client_p.db) {
	}
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

//! Implementation behind both duckdb.execute(sql[, params]) and duckdb.query(sql[, params]).
//! magic 0 = execute (side effects only, returns undefined)
//! magic 1 = query (returns Array<Object>, one object per row keyed by column name)
//!
//! Runs on a dedicated Connection so procedures can always execute regardless of what
//! statement state the caller's session is in; each statement runs in its own auto-commit
//! transaction, meaning writes from an earlier call are visible to subsequent calls within
//! the same procedure body.
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
			JS_ToInt64(js_context, &parameter_count, length_value);
			JS_FreeValue(js_context, length_value);
			bind_values.reserve(NumericCast<idx_t>(parameter_count));
			for (int64_t i = 0; i < parameter_count; i++) {
				auto element = JS_GetPropertyUint32(js_context, argv[1], NumericCast<uint32_t>(i));
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

		unique_ptr<QueryResult> result;
		if (!bind_values.empty()) {
			auto prepared = api->connection.Prepare(string(sql_chars, sql_length));
			JS_FreeCString(js_context, sql_chars);
			sql_chars = nullptr;
			if (prepared->HasError()) {
				return ThrowSqlError(js_context, prepared->GetError());
			}
			if (bind_values.size() > prepared->GetExpectedParameterTypes().size()) {
				return ThrowSqlError(js_context, "too many bound parameters supplied");
			}
			result = prepared->Execute(bind_values, /*allow_stream_result=*/false);
		} else {
			string sql(sql_chars, sql_length);
			JS_FreeCString(js_context, sql_chars);
			sql_chars = nullptr;
			result = api->connection.Query(sql);
		}
		if (!result) {
			return ThrowSqlError(js_context, "query did not produce a result");
		}
		if (result->HasError()) {
			return ThrowSqlError(js_context, result->GetError());
		}

		if (magic == 0) {
			// execute(): side-effect only
			return JS_UNDEFINED;
		}

		D_ASSERT(result->type == QueryResultType::MATERIALIZED_RESULT);
		auto &materialized = static_cast<MaterializedQueryResult &>(*result);
		auto row_array = JS_NewArray(js_context);
		if (JS_IsException(row_array)) {
			return ThrowSqlError(js_context, GetJSError(js_context));
		}
		const idx_t column_count = materialized.ColumnCount();
		for (idx_t row_index = 0; row_index < materialized.RowCount(); row_index++) {
			auto row_object = JS_NewObject(js_context);
			if (JS_IsException(row_object)) {
				JS_FreeValue(js_context, row_array);
				return ThrowSqlError(js_context, GetJSError(js_context));
			}
			for (idx_t col_index = 0; col_index < column_count; col_index++) {
				auto cell = JsValueFromDuckValue(js_context, materialized.GetValue(col_index, row_index));
				auto status =
				    JS_SetPropertyStr(js_context, row_object, materialized.GetNames()[col_index].GetIdentifierName().c_str(), cell);
				if (status < 0) {
					JS_FreeValue(js_context, row_object);
					JS_FreeValue(js_context, row_array);
					return ThrowSqlError(js_context, GetJSError(js_context));
				}
			}
			if (JS_DefinePropertyValueUint32(js_context, row_array, NumericCast<uint32_t>(row_index),
			                                 row_object, JS_PROP_C_W_E) < 0) {
				JS_FreeValue(js_context, row_array);
				return ThrowSqlError(js_context, GetJSError(js_context));
			}
		}
		return row_array;
	} catch (const std::exception &ex) {
		if (sql_chars) {
			// callbacks run inside QuickJS' C stack: never let C++ unwinding cross into it
			JS_FreeCString(js_context, sql_chars);
		}
		return ThrowSqlError(js_context, ex.what());
	} catch (...) {
		if (sql_chars) {
			JS_FreeCString(js_context, sql_chars);
		}
		return ThrowSqlError(js_context, "unknown error while executing SQL");
	}
}

//! Installs the `duckdb` global: { execute(sql[, params]), query(sql[, params]) }.
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
	JS_SetPropertyStr(js_context, api_object, "execute", execute_fn);
	JS_SetPropertyStr(js_context, api_object, "query", query_fn);
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

	try {
		RegisterSqlApi(js_context);
		auto source = "(function(" + StringUtil::Join(procedure.parameter_names, ", ") + ") {\n" + procedure.body +
		              "\n})";
		auto function = JS_Eval(js_context, source.c_str(), source.size(), "<procedure>", JS_EVAL_TYPE_GLOBAL);
		if (JS_IsException(function)) {
			auto error = GetJSError(js_context);
			throw InvalidInputException("JavaScript procedure '%s' could not be compiled: %s", procedure.name.GetIdentifierName(), error);
		}
		for (auto &value : values) {
			js_arguments.push_back(ToJSValue(js_context, value));
		}
		auto result =
		    JS_Call(js_context, function, JS_UNDEFINED, NumericCast<int>(js_arguments.size()), js_arguments.data());
		for (auto &argument : js_arguments) {
			JS_FreeValue(js_context, argument);
		}
		JS_FreeValue(js_context, function);
		if (JS_IsException(result)) {
			auto error = GetJSError(js_context);
			if (IsNestingLimitError(error)) {
				// already the collapsed root cause from a deeper invocation: propagate the
				// core message instead of accumulating one label per recursion level
				throw InvalidInputException(
				    StripExceptionLabels(error.substr(0, error.find('\n'))));
			}
			throw InvalidInputException("JavaScript procedure '%s' failed: %s", procedure.name.GetIdentifierName(), error);
		}
		auto duckdb_result = FromJSValue(js_context, result, context.client, procedure.return_type);
		JS_FreeValue(js_context, result);
		JS_FreeContext(js_context);
		JS_FreeRuntime(runtime);

		chunk.SetValue(0, 0, duckdb_result);
		chunk.SetCardinality(1);
		return SourceResultType::FINISHED;
	} catch (...) {
		JS_FreeContext(js_context);
		JS_FreeRuntime(runtime);
		throw;
	}
}

} // namespace duckdb

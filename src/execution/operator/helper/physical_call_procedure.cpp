#include "duckdb/execution/operator/helper/physical_call_procedure.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "quickjs.h"

namespace duckdb {

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
	vector<JSValue> js_arguments;
	vector<Value> values;
	values.reserve(arguments.size());
	for (auto &argument : arguments) {
		values.push_back(ExpressionExecutor::EvaluateScalar(context.client, *argument));
	}

	auto runtime = JS_NewRuntime();
	if (!runtime) {
		throw OutOfMemoryException("Unable to allocate JavaScript procedure runtime");
	}
	JS_SetMemoryLimit(runtime, 64ULL * 1024 * 1024);
	JS_SetMaxStackSize(runtime, 8ULL * 1024 * 1024);
	auto js_context = JS_NewContext(runtime);
	if (!js_context) {
		JS_FreeRuntime(runtime);
		throw OutOfMemoryException("Unable to allocate JavaScript procedure context");
	}

	auto source = "(function(" + StringUtil::Join(procedure.parameter_names, ", ") + ") {\n" + procedure.body + "\n})";
	auto function = JS_Eval(js_context, source.c_str(), source.size(), "<procedure>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(function)) {
		auto error = GetJSError(js_context);
		JS_FreeContext(js_context);
		JS_FreeRuntime(runtime);
		throw InvalidInputException("JavaScript procedure '%s' could not be compiled: %s", procedure.name, error);
	}
	for (auto &value : values) {
		js_arguments.push_back(ToJSValue(js_context, value));
	}
	auto result = JS_Call(js_context, function, JS_UNDEFINED, NumericCast<int>(js_arguments.size()), js_arguments.data());
	for (auto &argument : js_arguments) {
		JS_FreeValue(js_context, argument);
	}
	JS_FreeValue(js_context, function);
	if (JS_IsException(result)) {
		auto error = GetJSError(js_context);
		JS_FreeContext(js_context);
		JS_FreeRuntime(runtime);
		throw InvalidInputException("JavaScript procedure '%s' failed: %s", procedure.name, error);
	}
	auto duckdb_result = FromJSValue(js_context, result, context.client, procedure.return_type);
	JS_FreeValue(js_context, result);
	JS_FreeContext(js_context);
	JS_FreeRuntime(runtime);

	chunk.SetValue(0, 0, duckdb_result);
	chunk.SetCardinality(1);
	return SourceResultType::FINISHED;
}

} // namespace duckdb

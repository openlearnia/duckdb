#include "duckdb/catalog/catalog_entry/procedure_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/call_statement.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/operator/logical_call_procedure.hpp"

namespace duckdb {

BoundStatement Binder::Bind(CallStatement &stmt) {
	auto &function = stmt.function->Cast<FunctionExpression>();
	auto qualified_name = function.GetQualifiedName();
	BindSchemaOrCatalog(context, qualified_name);
	EntryLookupInfo procedure_lookup(CatalogType::PROCEDURE_ENTRY, qualified_name,
	                                 QueryErrorContext(function.GetQueryLocation()));
	auto procedure_entry = GetCatalogEntry(procedure_lookup, OnEntryNotFound::RETURN_NULL);
	if (procedure_entry) {
		auto &procedure = procedure_entry->Cast<ProcedureCatalogEntry>();
		auto &call_arguments = function.GetArguments();
		if (call_arguments.size() != procedure.parameter_types.size()) {
			throw BinderException("Procedure '%s' expects %llu arguments but %llu were provided",
			                      procedure.name.GetIdentifierName(), procedure.parameter_types.size(),
			                      call_arguments.size());
		}

		vector<unique_ptr<Expression>> arguments;
		ConstantBinder argument_binder(*this, context, "CALL argument");
		for (idx_t i = 0; i < call_arguments.size(); i++) {
			auto argument = call_arguments[i].GetExpression().Copy();
			auto bound_argument = argument_binder.Bind(argument);
			arguments.push_back(BoundCastExpression::AddCastToType(context, std::move(bound_argument),
			                                                       procedure.parameter_types[i]));
		}

		BoundStatement result;
		result.names = {procedure.name};
		result.types = {procedure.return_type};
		result.plan = make_uniq<LogicalCallProcedure>(procedure, std::move(arguments));
		GetStatementProperties().output_type = QueryResultOutputType::FORCE_MATERIALIZED;
		return result;
	}

	SelectStatement select_statement;
	auto select_node = make_uniq<SelectNode>();
	auto table_function = make_uniq<TableFunctionRef>();
	table_function->function = std::move(stmt.function);
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(table_function);
	select_statement.node = std::move(select_node);

	// CALL is `SELECT * FROM func()` (which already propagates call_return_type) forced to materialize.
	auto result = Bind(select_statement);
	GetStatementProperties().output_type = QueryResultOutputType::FORCE_MATERIALIZED;
	return result;
}

} // namespace duckdb

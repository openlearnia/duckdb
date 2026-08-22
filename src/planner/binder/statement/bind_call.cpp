#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/procedure_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/parser/expression/bound_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/operator/logical_call_procedure.hpp"
#include "duckdb/parser/statement/call_statement.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"

namespace duckdb {

BoundStatement Binder::Bind(CallStatement &stmt) {
	auto &function = stmt.function->Cast<FunctionExpression>();
	BindSchemaOrCatalog(function.catalog, function.schema);
	EntryLookupInfo procedure_lookup(CatalogType::PROCEDURE_ENTRY, function.function_name,
	                                 QueryErrorContext(function.GetQueryLocation()));
	auto procedure_entry = GetCatalogEntry(function.catalog, function.schema, procedure_lookup,
	                                       OnEntryNotFound::RETURN_NULL);
	if (procedure_entry) {
		auto &procedure = procedure_entry->Cast<ProcedureCatalogEntry>();
		if (function.children.size() != procedure.parameter_types.size()) {
			throw BinderException("Procedure '%s' expects %llu arguments but %llu were provided", procedure.name,
			                      procedure.parameter_types.size(), function.children.size());
		}

		vector<unique_ptr<Expression>> arguments;
		ConstantBinder argument_binder(*this, context, "CALL argument");
		for (idx_t i = 0; i < function.children.size(); i++) {
			auto argument = function.children[i]->Copy();
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

	auto result = Bind(select_statement);
	auto &properties = GetStatementProperties();
	properties.output_type = QueryResultOutputType::FORCE_MATERIALIZED;
	return result;
}

} // namespace duckdb

#include "duckdb/planner/materialized_view_incremental.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"

namespace duckdb {

enum class NativeMVAggregate : uint8_t { SUM, COUNT, MIN, MAX };

struct NativeMVAggregateInfo {
	NativeMVAggregate kind;
	idx_t select_index;
	string delta_sql;
};

static string MVIdentifier(const string &input) {
	return KeywordHelper::WriteQuoted(input, '"');
}

static string ExpressionSQL(const ParsedExpression &expression) {
	auto copy = expression.Copy();
	copy->SetAlias(Identifier());
	return copy->ToString();
}

static bool IsAppendOnlySinceRefresh(ClientContext &context, TableCatalogEntry &view, TableCatalogEntry *&dependency,
                                     idx_t &row_watermark) {
	auto &catalogs = view.GetMaterializedViewDependencyCatalogs();
	auto &schemas = view.GetMaterializedViewDependencySchemas();
	auto &tables = view.GetMaterializedViewDependencyTables();
	auto &append_generations = view.GetMaterializedViewDependencyAppendGenerations();
	auto &delete_generations = view.GetMaterializedViewDependencyDeleteGenerations();
	auto &update_generations = view.GetMaterializedViewDependencyUpdateGenerations();
	auto &row_counts = view.GetMaterializedViewDependencyRowCounts();
	if (catalogs.size() != 1 || schemas.size() != 1 || tables.size() != 1 || append_generations.size() != 1 ||
	    delete_generations.size() != 1 || update_generations.size() != 1 || row_counts.size() != 1) {
		return false;
	}
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY,
	                       QualifiedName(Identifier(catalogs[0]), Identifier(schemas[0]), Identifier(tables[0])));
	auto entry = Catalog::GetEntry(context, lookup, OnEntryNotFound::RETURN_NULL);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY || !entry->Cast<TableCatalogEntry>().IsDuckTable()) {
		return false;
	}
	dependency = &entry->Cast<TableCatalogEntry>();
	auto &storage = dependency->GetStorage();
	auto append_generation = storage.GetAppendGeneration();
	auto delete_generation = storage.GetDeleteGeneration();
	auto update_generation = storage.GetUpdateGeneration();
	auto &transaction = DuckTransaction::Get(context, dependency->ParentCatalog());
	append_generation += transaction.GetTableMutationCount(storage, TableModificationType::APPEND);
	delete_generation += transaction.GetTableMutationCount(storage, TableModificationType::DELETE);
	update_generation += transaction.GetTableMutationCount(storage, TableModificationType::UPDATE);
	auto local_appends = transaction.GetLocalStorage().AddedRows(storage);
	auto current_appended_rows = storage.GetAppendedRows() + local_appends;
	auto current_physical_rows = storage.GetTotalRows() + local_appends;
	if (append_generation <= append_generations[0] || delete_generation != delete_generations[0] ||
	    update_generation != update_generations[0] || current_appended_rows <= row_counts[0]) {
		return false;
	}
	auto newly_appended_rows = current_appended_rows - row_counts[0];
	if (newly_appended_rows > current_physical_rows) {
		return false;
	}
	row_watermark = current_physical_rows - newly_appended_rows;
	return true;
}

//! True when the expression contains a function that resolves to an aggregate function entry
//! (AVG, DISTINCT aggregates, string_agg, ...). Such expressions collapse rows: appending new
//! rows or re-deriving per-row values for them produces wrong results, so views containing
//! them must fall back to a full refresh.
static bool ContainsAggregateExpression(ClientContext &context, const ParsedExpression &expression) {
	if (expression.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &function = expression.Cast<const FunctionExpression>();
		EntryLookupInfo fn_entry(CatalogType::AGGREGATE_FUNCTION_ENTRY, function.GetQualifiedName());
		auto entry = Catalog::GetEntry(context, fn_entry, OnEntryNotFound::RETURN_NULL);
		if (entry && entry->type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
			return true;
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expression, [&](const ParsedExpression &child) {
		if (!found && ContainsAggregateExpression(context, child)) {
			found = true;
		}
	});
	return found;
}

static bool AnalyzeAggregate(const ParsedExpression &expression, idx_t select_index, NativeMVAggregateInfo &result) {
	if (expression.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &function = expression.Cast<const FunctionExpression>();
	if (function.Distinct() || function.ExportState()) {
		return false;
	}
	auto name = StringUtil::Lower(function.FunctionName().GetIdentifierName());
	if (name == "sum") {
		result.kind = NativeMVAggregate::SUM;
	} else if (name == "count" || name == "count_star") {
		result.kind = NativeMVAggregate::COUNT;
	} else if (name == "min") {
		result.kind = NativeMVAggregate::MIN;
	} else if (name == "max") {
		result.kind = NativeMVAggregate::MAX;
	} else {
		return false;
	}
	result.select_index = select_index;
	result.delta_sql = ExpressionSQL(expression);
	return true;
}

static string MergeAggregateSQL(const NativeMVAggregateInfo &aggregate, const string &column_name) {
	auto current = "m." + MVIdentifier(column_name);
	auto delta = "d.__d" + to_string(aggregate.select_index);
	switch (aggregate.kind) {
	case NativeMVAggregate::COUNT:
		return StringUtil::Format("(COALESCE(%s, 0) + COALESCE(%s, 0))", current, delta);
	case NativeMVAggregate::SUM:
		return StringUtil::Format("CASE WHEN %s IS NULL THEN %s WHEN %s IS NULL THEN %s ELSE %s + %s END", current,
		                          delta, delta, current, current, delta);
	case NativeMVAggregate::MIN:
		return StringUtil::Format("CASE WHEN %s IS NULL THEN %s WHEN %s IS NULL THEN %s ELSE LEAST(%s, %s) END",
		                          current, delta, delta, current, current, delta);
	case NativeMVAggregate::MAX:
		return StringUtil::Format("CASE WHEN %s IS NULL THEN %s WHEN %s IS NULL THEN %s ELSE GREATEST(%s, %s) END",
		                          current, delta, delta, current, current, delta);
	default:
		throw InternalException("Unsupported native materialized-view aggregate");
	}
}

static unique_ptr<SelectStatement> ParseIncrementalQuery(const string &sql) {
	Parser parser;
	parser.ParseQuery(sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw InternalException("Generated native materialized-view incremental query is not a SELECT");
	}
	return unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
}

vector<idx_t> GetMaterializedViewLogicalDiffKeyPositions(const string &original_sql,
                                                         const vector<string> &column_names) {
	Parser parser;
	parser.ParseQuery(original_sql);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		return vector<idx_t>();
	}
	auto statement = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	if (!statement->node || statement->node->type != QueryNodeType::SELECT_NODE) {
		return vector<idx_t>();
	}
	auto &node = statement->node->Cast<SelectNode>();
	if (node.groups.group_expressions.empty() || column_names.size() != node.select_list.size()) {
		return vector<idx_t>();
	}
	vector<idx_t> key_positions;
	for (auto &group : node.groups.group_expressions) {
		if (group->GetExpressionClass() == ExpressionClass::CONSTANT) {
			auto &constant = group->Cast<ConstantExpression>();
			int64_t select_index = 0;
			if (constant.GetLiteral().TryGetInt64(select_index)) {
				if (select_index >= 1 && NumericCast<idx_t>(select_index) <= node.select_list.size()) {
					key_positions.push_back(NumericCast<idx_t>(select_index - 1));
					continue;
				}
			}
		}
		bool found = false;
		for (idx_t i = 0; i < node.select_list.size(); i++) {
			if (ExpressionSQL(*group) == ExpressionSQL(*node.select_list[i])) {
				key_positions.push_back(i);
				found = true;
				break;
			}
		}
		if (!found) {
			return vector<idx_t>();
		}
	}
	return key_positions;
}

bool TryBuildMaterializedViewIncrementalQuery(ClientContext &context, TableCatalogEntry &view,
                                              unique_ptr<SelectStatement> &query, string &refresh_mode) {
	// Incremental refresh reads the stale backing view. Under the error policy,
	// use the original definition instead so refresh can repair it without
	// relaxing protection for any stale views referenced by that definition.
	if (MaterializedViewStaleReadSetting::GetSetting(context).GetValue<string>() == "error") {
		return false;
	}
	TableCatalogEntry *dependency = nullptr;
	idx_t row_watermark;
	if (!IsAppendOnlySinceRefresh(context, view, dependency, row_watermark)) {
		return false;
	}
	if (query->node->type != QueryNodeType::SELECT_NODE || !query->node->cte_map.map.empty()) {
		return false;
	}
	auto &node = query->node->Cast<SelectNode>();
	if (!node.from_table || node.from_table->type != TableReferenceType::BASE_TABLE || node.having || node.qualify ||
	    node.sample || !node.modifiers.empty() || (node.where_clause && node.where_clause->HasSubquery())) {
		return false;
	}
	auto &base = node.from_table->Cast<BaseTableRef>();
	auto qualified_base = base.GetQualifiedName();
	auto base_schema =
	    qualified_base.Schema().empty() ? string(DEFAULT_SCHEMA) : qualified_base.Schema().GetIdentifierName();
	auto base_catalog = qualified_base.Catalog().empty() ? dependency->ParentCatalog().GetName().GetIdentifierName()
	                                                     : qualified_base.Catalog().GetIdentifierName();
	if (!StringUtil::CIEquals(base_catalog, dependency->ParentCatalog().GetName().GetIdentifierName()) ||
	    !StringUtil::CIEquals(base_schema, dependency->schema.name.GetIdentifierName()) ||
	    !StringUtil::CIEquals(qualified_base.Name().GetIdentifierName(), dependency->name.GetIdentifierName())) {
		return false;
	}

	string appended_filter = StringUtil::Format("rowid >= %llu", row_watermark);
	if (node.where_clause) {
		appended_filter = "(" + node.where_clause->ToString() + ") AND " + appended_filter;
	}
	string view_sql = MVIdentifier(view.catalog.GetName().GetIdentifierName()) + "." +
	                  MVIdentifier(view.schema.name.GetIdentifierName()) + "." +
	                  MVIdentifier(view.name.GetIdentifierName());
	string from_sql = base.ToString();
	auto &groups = node.groups.group_expressions;

	vector<NativeMVAggregateInfo> aggregates;
	vector<idx_t> key_positions;
	vector<string> key_sql;
	for (idx_t select_idx = 0; select_idx < node.select_list.size(); select_idx++) {
		NativeMVAggregateInfo aggregate;
		if (AnalyzeAggregate(*node.select_list[select_idx], select_idx, aggregate)) {
			aggregates.push_back(std::move(aggregate));
			continue;
		}
		bool is_key = false;
		for (idx_t key_idx = 0; key_idx < groups.size(); key_idx++) {
			if (ExpressionSQL(*node.select_list[select_idx]) == ExpressionSQL(*groups[key_idx])) {
				key_positions.push_back(select_idx);
				key_sql.push_back(ExpressionSQL(*groups[key_idx]));
				is_key = true;
				break;
			}
		}
		if (!is_key && ContainsAggregateExpression(context, *node.select_list[select_idx])) {
			// a non-mergeable aggregate (AVG, DISTINCT aggregates, nested aggregates, ...):
			// appending rows or evaluating it per appended row is wrong - full refresh instead
			return false;
		}
		if (!is_key && !groups.empty()) {
			return false;
		}
	}

	if (aggregates.empty()) {
		if (!groups.empty()) {
			return false;
		}
		string select_sql;
		for (auto &item : node.select_list) {
			if (!select_sql.empty()) {
				select_sql += ", ";
			}
			select_sql += item->ToString();
		}
		auto sql = StringUtil::Format("SELECT * FROM %s UNION ALL SELECT %s FROM %s WHERE %s", view_sql, select_sql,
		                              from_sql, appended_filter);
		query = ParseIncrementalQuery(sql);
		refresh_mode = "append";
		return true;
	}

	if (key_positions.size() != groups.size() || node.select_list.size() != key_positions.size() + aggregates.size()) {
		return false;
	}
	string delta_select;
	for (idx_t key_idx = 0; key_idx < key_sql.size(); key_idx++) {
		if (!delta_select.empty()) {
			delta_select += ", ";
		}
		delta_select += key_sql[key_idx] + " AS __k" + to_string(key_idx);
	}
	for (auto &aggregate : aggregates) {
		if (!delta_select.empty()) {
			delta_select += ", ";
		}
		delta_select += aggregate.delta_sql + " AS __d" + to_string(aggregate.select_index);
	}
	string group_by;
	if (!key_sql.empty()) {
		group_by = " GROUP BY " + StringUtil::Join(key_sql, ", ");
	}
	string result_select;
	for (idx_t select_idx = 0; select_idx < node.select_list.size(); select_idx++) {
		if (!result_select.empty()) {
			result_select += ", ";
		}
		auto &column_name = view.GetColumns().GetColumn(LogicalIndex(select_idx)).Name();
		auto key_it = std::find(key_positions.begin(), key_positions.end(), select_idx);
		if (key_it != key_positions.end()) {
			auto key_idx = NumericCast<idx_t>(key_it - key_positions.begin());
			result_select +=
			    StringUtil::Format("COALESCE(m.%s, d.__k%llu) AS %s", MVIdentifier(column_name.GetIdentifierName()),
			                       key_idx, MVIdentifier(column_name.GetIdentifierName()));
			continue;
		}
		auto aggregate_it = std::find_if(aggregates.begin(), aggregates.end(), [&](const NativeMVAggregateInfo &entry) {
			return entry.select_index == select_idx;
		});
		if (aggregate_it == aggregates.end()) {
			return false;
		}
		result_select += MergeAggregateSQL(*aggregate_it, column_name.GetIdentifierName()) + " AS " +
		                 MVIdentifier(column_name.GetIdentifierName());
	}

	string merge_from;
	if (key_sql.empty()) {
		merge_from = view_sql + " m CROSS JOIN __delta d";
	} else {
		string join_condition;
		for (idx_t key_idx = 0; key_idx < key_positions.size(); key_idx++) {
			if (!join_condition.empty()) {
				join_condition += " AND ";
			}
			auto &column_name = view.GetColumns().GetColumn(LogicalIndex(key_positions[key_idx])).Name();
			join_condition += StringUtil::Format("m.%s IS NOT DISTINCT FROM d.__k%llu",
			                                     MVIdentifier(column_name.GetIdentifierName()), key_idx);
		}
		merge_from = view_sql + " m FULL OUTER JOIN __delta d ON " + join_condition;
	}
	auto sql = StringUtil::Format("WITH __delta AS (SELECT %s FROM %s WHERE %s%s) SELECT %s FROM %s", delta_select,
	                              from_sql, appended_filter, group_by, result_select, merge_from);
	query = ParseIncrementalQuery(sql);
	refresh_mode = "delta";
	return true;
}

//! Functions that read session state at bind time and are folded into constants, so the bound plan cannot show them
static bool IsSessionDependentFunction(const string &name) {
	return StringUtil::CIEquals(name, "getvariable") || StringUtil::CIEquals(name, "current_setting");
}

static void FindSessionDependentFunction(QueryNode &node, string &found);

static void FindSessionDependentFunction(ParsedExpression &expr, string &found) {
	if (!found.empty()) {
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &name = expr.Cast<FunctionExpression>().FunctionName().GetIdentifierName();
		if (IsSessionDependentFunction(name)) {
			found = name;
			return;
		}
	} else if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery = expr.Cast<SubqueryExpression>().Subquery();
		if (subquery && subquery->node) {
			FindSessionDependentFunction(*subquery->node, found);
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](ParsedExpression &child) { FindSessionDependentFunction(child, found); });
}

static void FindSessionDependentFunction(QueryNode &node, string &found) {
	ParsedExpressionIterator::EnumerateQueryNodeChildren(node, [&](unique_ptr<ParsedExpression> &child) {
		if (child) {
			FindSessionDependentFunction(*child, found);
		}
	});
}

static void FindUnstableFunction(const LogicalOperator &op, string &found);

static void FindUnstableFunction(const Expression &expr, string &found) {
	if (!found.empty()) {
		return;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_FUNCTION: {
		auto &function = expr.Cast<BoundFunctionExpression>().Function();
		if (function.GetStability() != FunctionStability::CONSISTENT) {
			found = function.GetName().GetIdentifierName();
			return;
		}
		break;
	}
	case ExpressionClass::BOUND_AGGREGATE: {
		auto &function = expr.Cast<BoundAggregateExpression>().Function();
		if (function.GetStability() != FunctionStability::CONSISTENT) {
			found = function.GetName().GetIdentifierName();
			return;
		}
		break;
	}
	case ExpressionClass::BOUND_WINDOW: {
		auto &window = expr.Cast<BoundWindowExpression>();
		auto &aggregate = window.AggregateFunction();
		auto stability = aggregate ? aggregate->GetStability() : window.WindowFunction()->GetStability();
		if (stability != FunctionStability::CONSISTENT) {
			found = aggregate ? aggregate->GetName().GetIdentifierName()
			                  : window.WindowFunction()->GetName().GetIdentifierName();
			return;
		}
		break;
	}
	case ExpressionClass::BOUND_SUBQUERY: {
		auto &subquery = expr.Cast<BoundSubqueryExpression>().Subquery();
		if (subquery.plan) {
			FindUnstableFunction(*subquery.plan, found);
		}
		break;
	}
	default:
		break;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { FindUnstableFunction(child, found); });
}

static void FindUnstableFunction(const LogicalOperator &op, string &found) {
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](const unique_ptr<Expression> *child) {
		if (*child) {
			FindUnstableFunction(**child, found);
		}
	});
	for (auto &child : op.children) {
		if (!found.empty()) {
			return;
		}
		FindUnstableFunction(*child, found);
	}
}

void VerifyMaterializedViewDeterministic(SelectStatement &definition, const LogicalOperator &plan) {
	string found;
	if (definition.node) {
		FindSessionDependentFunction(*definition.node, found);
	}
	if (found.empty()) {
		FindUnstableFunction(plan, found);
	}
	if (found.empty()) {
		return;
	}
	throw BinderException("Materialized view definitions cannot use \"%s\": a materialized view is a snapshot of its "
	                      "base tables, so its contents cannot depend on the time, random values or session state. "
	                      "Filter on stored columns instead, or use a view",
	                      found);
}

} // namespace duckdb

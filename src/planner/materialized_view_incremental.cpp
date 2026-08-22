#include "duckdb/planner/materialized_view_incremental.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
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
	copy->SetAlias(string());
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
	EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, tables[0]);
	auto entry = Catalog::GetEntry(context, catalogs[0], schemas[0], lookup, OnEntryNotFound::RETURN_NULL);
	if (!entry || entry->type != CatalogType::TABLE_ENTRY || !entry->Cast<TableCatalogEntry>().IsDuckTable()) {
		return false;
	}
	dependency = &entry->Cast<TableCatalogEntry>();
	auto &storage = dependency->GetStorage();
	auto append_generation = storage.GetAppendGeneration();
	auto delete_generation = storage.GetDeleteGeneration();
	auto update_generation = storage.GetUpdateGeneration();
	auto &transaction = DuckTransaction::Get(context, dependency->ParentCatalog());
	auto modification_type = transaction.GetTableModificationType(storage);
	if (modification_type & static_cast<uint8_t>(TableModificationType::APPEND)) {
		append_generation++;
	}
	if (modification_type & static_cast<uint8_t>(TableModificationType::DELETE)) {
		delete_generation++;
	}
	if (modification_type & static_cast<uint8_t>(TableModificationType::UPDATE)) {
		update_generation++;
	}
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

static bool AnalyzeAggregate(const ParsedExpression &expression, idx_t select_index, NativeMVAggregateInfo &result) {
	if (expression.expression_class != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &function = expression.Cast<const FunctionExpression>();
	if (function.distinct || function.export_state) {
		return false;
	}
	auto name = StringUtil::Lower(function.function_name);
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

bool TryBuildMaterializedViewIncrementalQuery(ClientContext &context, TableCatalogEntry &view,
                                              unique_ptr<SelectStatement> &query, string &refresh_mode) {
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
	auto base_schema = base.schema_name.empty() ? DEFAULT_SCHEMA : base.schema_name;
	auto base_catalog = base.catalog_name.empty() ? dependency->ParentCatalog().GetName() : base.catalog_name;
	if (!StringUtil::CIEquals(base_catalog, dependency->ParentCatalog().GetName()) ||
	    !StringUtil::CIEquals(base_schema, dependency->schema.name) ||
	    !StringUtil::CIEquals(base.table_name, dependency->name)) {
		return false;
	}

	string appended_filter = StringUtil::Format("rowid >= %llu", row_watermark);
	if (node.where_clause) {
		appended_filter = "(" + node.where_clause->ToString() + ") AND " + appended_filter;
	}
	string view_sql = MVIdentifier(view.catalog.GetName()) + "." + MVIdentifier(view.schema.name) + "." +
	                  MVIdentifier(view.name);
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
			result_select += StringUtil::Format("COALESCE(m.%s, d.__k%llu) AS %s", MVIdentifier(column_name), key_idx,
			                                        MVIdentifier(column_name));
			continue;
		}
		auto aggregate_it = std::find_if(aggregates.begin(), aggregates.end(), [&](const NativeMVAggregateInfo &entry) {
			return entry.select_index == select_idx;
		});
		if (aggregate_it == aggregates.end()) {
			return false;
		}
		result_select += MergeAggregateSQL(*aggregate_it, column_name) + " AS " + MVIdentifier(column_name);
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
			join_condition += StringUtil::Format("m.%s IS NOT DISTINCT FROM d.__k%llu", MVIdentifier(column_name), key_idx);
		}
		merge_from = view_sql + " m FULL OUTER JOIN __delta d ON " + join_condition;
	}
	auto sql = StringUtil::Format("WITH __delta AS (SELECT %s FROM %s WHERE %s%s) SELECT %s FROM %s", delta_select,
	                              from_sql, appended_filter, group_by, result_select, merge_from);
	query = ParseIncrementalQuery(sql);
	refresh_mode = "delta";
	return true;
}

} // namespace duckdb

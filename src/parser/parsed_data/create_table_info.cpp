#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

CreateTableInfo::CreateTableInfo() : CreateInfo(CatalogType::TABLE_ENTRY, Identifier::InvalidSchema()) {
}

CreateTableInfo::CreateTableInfo(QualifiedName qualified_name_p) : CreateInfo(CatalogType::TABLE_ENTRY) {
	SetQualifiedName(std::move(qualified_name_p));
}

CreateTableInfo::CreateTableInfo(SchemaCatalogEntry &schema, const Identifier &name_p)
    : CreateTableInfo(schema.GetQualifiedName(name_p)) {
}

unique_ptr<CreateInfo> CreateTableInfo::Copy() const {
	auto result = make_uniq<CreateTableInfo>(GetQualifiedName());
	CopyProperties(*result);
	result->columns = columns.Copy();
	for (auto &constraint : constraints) {
		result->constraints.push_back(constraint->Copy());
	}
	for (auto &partition : partition_keys) {
		result->partition_keys.push_back(partition->Copy());
	}
	for (auto &order : sort_keys) {
		result->sort_keys.push_back(order->Copy());
	}
	for (auto &option : options) {
		result->options.emplace(option.first, option.second->Copy());
	}
	if (query) {
		result->query = unique_ptr_cast<SQLStatement, SelectStatement>(query->Copy());
	}
	result->materialized_view = materialized_view;
	result->materialized_view_query = materialized_view_query;
	result->catalog_materialized_view = catalog_materialized_view;
	result->materialized_view_refresh = materialized_view_refresh;
	result->materialized_view_if_stale = materialized_view_if_stale;
	result->materialized_view_skip_refresh = materialized_view_skip_refresh;
	result->materialized_view_dependency_catalogs = materialized_view_dependency_catalogs;
	result->materialized_view_dependency_schemas = materialized_view_dependency_schemas;
	result->materialized_view_dependency_tables = materialized_view_dependency_tables;
	result->materialized_view_dependency_generations = materialized_view_dependency_generations;
	result->materialized_view_dependency_append_generations = materialized_view_dependency_append_generations;
	result->materialized_view_dependency_delete_generations = materialized_view_dependency_delete_generations;
	result->materialized_view_dependency_update_generations = materialized_view_dependency_update_generations;
	result->materialized_view_dependency_row_counts = materialized_view_dependency_row_counts;
	result->materialized_view_refresh_mode = materialized_view_refresh_mode;
	result->materialized_view_refresh_times = materialized_view_refresh_times;
	result->materialized_view_refresh_modes = materialized_view_refresh_modes;
	result->materialized_view_refresh_durations = materialized_view_refresh_durations;
	result->materialized_view_refresh_rows_written = materialized_view_refresh_rows_written;
	result->materialized_view_refresh_rows_added = materialized_view_refresh_rows_added;
	result->materialized_view_refresh_rows_removed = materialized_view_refresh_rows_removed;
	result->materialized_view_refresh_rows_changed = materialized_view_refresh_rows_changed;
	result->materialized_view_refresh_key_positions = materialized_view_refresh_key_positions;
	return std::move(result);
}

string CreateTableInfo::ExtraOptionsToString() const {
	string ret;
	if (!partition_keys.empty()) {
		ret += " PARTITIONED BY (";
		for (auto &partition : partition_keys) {
			ret += partition->ToString() + ",";
		}
		ret.pop_back();
		ret += ")";
	}
	if (!sort_keys.empty()) {
		ret += " SORTED BY (";
		for (auto &order : sort_keys) {
			ret += order->ToString() + ",";
		}
		ret.pop_back();
		ret += ")";
	}
	if (!options.empty()) {
		ret += " WITH (";
		for (auto &entry : options) {
			ret += "'" + entry.first + "'=" + entry.second->ToString() + ",";
		}
		ret.pop_back();
		ret += ")";
	}
	return ret;
}

string CreateTableInfo::ToString() const {
	string ret = GetCreatePrefix(materialized_view ? "MATERIALIZED VIEW" : "TABLE");
	ret += QualifiedNameToString();
	if (materialized_view && query == nullptr && !materialized_view_query.empty()) {
		return ret + " AS " + materialized_view_query + ";";
	}

	if (query != nullptr) {
		ret += TableCatalogEntry::ColumnNamesToSQL(columns);
		ret += ExtraOptionsToString();
		ret += " AS " + query->ToString();
	} else {
		ret += TableCatalogEntry::ColumnsToSQL(columns, constraints);
		ret += ExtraOptionsToString();
		ret += ";";
	}
	return ret;
}

} // namespace duckdb

//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_table_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/column_list.hpp"

namespace duckdb {
class SchemaCatalogEntry;

struct CreateTableInfo : public CreateInfo {
	DUCKDB_API CreateTableInfo();
	DUCKDB_API CreateTableInfo(string catalog, string schema, string name);
	DUCKDB_API CreateTableInfo(SchemaCatalogEntry &schema, string name);

	//! Table name to insert to
	string table;
	//! List of columns of the table
	ColumnList columns;
	//! List of constraints on the table
	vector<unique_ptr<Constraint>> constraints;
	//! CREATE TABLE as QUERY
	unique_ptr<SelectStatement> query;
	//! Table Partition definitions
	vector<unique_ptr<ParsedExpression>> partition_keys;
	//! Table Sort definitions
	vector<unique_ptr<ParsedExpression>> sort_keys;
	//! Extra Table options if any
	case_insensitive_map_t<unique_ptr<ParsedExpression>> options;
	//! Whether this physical table is a native materialized view
	bool materialized_view = false;
	//! Canonical defining SELECT for a native materialized view
	string materialized_view_query;
	//! Stable dependency identities and their table-change generations at the last refresh
	vector<string> materialized_view_dependency_catalogs;
	vector<string> materialized_view_dependency_schemas;
	vector<string> materialized_view_dependency_tables;
	vector<idx_t> materialized_view_dependency_generations;
	vector<idx_t> materialized_view_dependency_append_generations;
	vector<idx_t> materialized_view_dependency_delete_generations;
	vector<idx_t> materialized_view_dependency_update_generations;
	vector<idx_t> materialized_view_dependency_row_counts;
	string materialized_view_refresh_mode = "full";
	//! Transient flags used to bind REFRESH MATERIALIZED VIEW through the CTAS path
	bool materialized_view_refresh = false;
	bool materialized_view_if_stale = false;
	bool materialized_view_skip_refresh = false;

public:
	DUCKDB_API unique_ptr<CreateInfo> Copy() const override;

	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	string ExtraOptionsToString() const;
	string ToString() const override;
};

} // namespace duckdb

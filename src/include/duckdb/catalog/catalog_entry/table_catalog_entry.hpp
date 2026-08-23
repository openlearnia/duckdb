//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/table_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/planner/bound_constraint.hpp"
#include "duckdb/storage/table/table_statistics.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/catalog/catalog_entry/table_column_type.hpp"
#include "duckdb/catalog/catalog_entry/column_dependency_manager.hpp"
#include "duckdb/common/table_column.hpp"

namespace duckdb {

class DataTable;

struct RenameColumnInfo;
struct RenameFieldInfo;
struct AddColumnInfo;
struct AddFieldInfo;
struct RemoveColumnInfo;
struct RemoveFieldInfo;
struct SetDefaultInfo;
struct ChangeColumnTypeInfo;
struct AlterForeignKeyInfo;
struct SetNotNullInfo;
struct DropNotNullInfo;
struct SetColumnCommentInfo;
struct CreateTableInfo;
struct BoundCreateTableInfo;

class TableFunction;
struct FunctionData;
struct EntryLookupInfo;

class Binder;
struct ColumnSegmentInfo;
class TableStorageInfo;

class LogicalGet;
class LogicalProjection;
class LogicalUpdate;

//! A table catalog entry
class TableCatalogEntry : public StandardEntry {
public:
	static constexpr const CatalogType Type = CatalogType::TABLE_ENTRY;
	static constexpr const char *Name = "table";

public:
	//! Create a TableCatalogEntry and initialize storage for it
	DUCKDB_API TableCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);

public:
	DUCKDB_API unique_ptr<CreateInfo> GetInfo() const override;

	DUCKDB_API bool IsMaterializedView() const {
		return materialized_view;
	}
	//! True when catalog discovery should expose this relation as a materialized view.
	//! This is separate from IsMaterializedView() for extension-managed MVs.
	DUCKDB_API bool IsMaterializedViewForCatalog() const {
		return materialized_view || catalog_materialized_view;
	}

	DUCKDB_API const string &GetMaterializedViewQuery() const {
		return materialized_view_query;
	}
	DUCKDB_API const vector<string> &GetMaterializedViewDependencyCatalogs() const {
		return materialized_view_dependency_catalogs;
	}
	DUCKDB_API const vector<string> &GetMaterializedViewDependencySchemas() const {
		return materialized_view_dependency_schemas;
	}
	DUCKDB_API const vector<string> &GetMaterializedViewDependencyTables() const {
		return materialized_view_dependency_tables;
	}
	DUCKDB_API const vector<idx_t> &GetMaterializedViewDependencyGenerations() const {
		return materialized_view_dependency_generations;
	}
	DUCKDB_API const vector<idx_t> &GetMaterializedViewDependencyAppendGenerations() const {
		return materialized_view_dependency_append_generations;
	}
	DUCKDB_API const vector<idx_t> &GetMaterializedViewDependencyDeleteGenerations() const {
		return materialized_view_dependency_delete_generations;
	}
	DUCKDB_API const vector<idx_t> &GetMaterializedViewDependencyUpdateGenerations() const {
		return materialized_view_dependency_update_generations;
	}
	DUCKDB_API const vector<idx_t> &GetMaterializedViewDependencyRowCounts() const {
		return materialized_view_dependency_row_counts;
	}
	DUCKDB_API const string &GetMaterializedViewRefreshMode() const {
		return materialized_view_refresh_mode;
	}
	DUCKDB_API bool MaterializedViewIsStale(ClientContext &context);

	DUCKDB_API bool HasGeneratedColumns() const;

	//! Returns whether or not a column with the given name exists
	DUCKDB_API bool ColumnExists(const string &name) const;
	//! Returns a reference to the column of the specified name. Throws an
	//! exception if the column does not exist.
	DUCKDB_API const ColumnDefinition &GetColumn(const string &name) const;
	//! Returns a reference to the column of the specified logical index. Throws an
	//! exception if the column does not exist.
	DUCKDB_API const ColumnDefinition &GetColumn(LogicalIndex idx) const;
	//! Returns a list of types of the table, excluding generated columns
	DUCKDB_API vector<LogicalType> GetTypes() const;
	//! Returns a list of the columns of the table
	DUCKDB_API const ColumnList &GetColumns() const;
	//! Returns the underlying storage of the table
	virtual DataTable &GetStorage();

	//! Returns a list of the constraints of the table
	DUCKDB_API const vector<unique_ptr<Constraint>> &GetConstraints() const;
	DUCKDB_API string ToSQL() const override;

	//! Get statistics of a column (physical or virtual) within the table
	virtual unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) = 0;

	virtual unique_ptr<BlockingSample> GetSample();

	//! Returns the column index of the specified column name.
	//! If the column does not exist:
	//! If if_column_exists is true, returns DConstants::INVALID_INDEX
	//! If if_column_exists is false, throws an exception
	DUCKDB_API LogicalIndex GetColumnIndex(string &name, bool if_exists = false) const;
	DUCKDB_API StorageIndex GetStorageIndex(const ColumnIndex &column_index) const;

	//! Returns the scan function that can be used to scan the given table
	virtual TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) = 0;
	virtual TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
	                                      const EntryLookupInfo &lookup_info);

	virtual bool IsDuckTable() const {
		return false;
	}

	DUCKDB_API static string ColumnsToSQL(const ColumnList &columns, const vector<unique_ptr<Constraint>> &constraints);

	//! Returns the expression string list of the column names e.g. (col1, col2, col3)
	static string ColumnNamesToSQL(const ColumnList &columns);

	//! Returns a list of segment information for this table, if exists
	virtual vector<ColumnSegmentInfo> GetColumnSegmentInfo(const QueryContext &context);

	//! Returns the storage info of this table
	virtual TableStorageInfo GetStorageInfo(ClientContext &context) = 0;

	virtual void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                                   ClientContext &context);

	//! Returns a pointer to the table's primary key, if exists, else nullptr.
	optional_ptr<Constraint> GetPrimaryKey() const;
	//! Returns true, if the table has a primary key, else false.
	bool HasPrimaryKey() const;

	//! Returns the virtual columns for this table
	virtual virtual_column_map_t GetVirtualColumns() const;

	virtual vector<column_t> GetRowIdColumns() const;

protected:
	//! Native MV marker and persisted defining query
	bool materialized_view;
	bool catalog_materialized_view;
	string materialized_view_query;
	vector<string> materialized_view_dependency_catalogs;
	vector<string> materialized_view_dependency_schemas;
	vector<string> materialized_view_dependency_tables;
	vector<idx_t> materialized_view_dependency_generations;
	vector<idx_t> materialized_view_dependency_append_generations;
	vector<idx_t> materialized_view_dependency_delete_generations;
	vector<idx_t> materialized_view_dependency_update_generations;
	vector<idx_t> materialized_view_dependency_row_counts;
	string materialized_view_refresh_mode;
	//! A list of columns that are part of this table
	ColumnList columns;
	//! A list of constraints that are part of this table
	vector<unique_ptr<Constraint>> constraints;
};
} // namespace duckdb

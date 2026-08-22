#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

struct DuckDBMaterializedViewsData : public GlobalTableFunctionState {
	vector<reference<CatalogEntry>> entries;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> DuckDBMaterializedViewsBind(ClientContext &context, TableFunctionBindInput &input,
                                                            vector<LogicalType> &return_types,
                                                            vector<string> &names) {
	names = {"database_name", "schema_name", "view_name", "definition", "dependencies", "dependency_generations",
	         "current_dependency_generations", "is_stale"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::BIGINT),
	                LogicalType::LIST(LogicalType::BIGINT), LogicalType::BOOLEAN};
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBMaterializedViewsInit(ClientContext &context,
                                                                       TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBMaterializedViewsData>();
	for (auto &schema : Catalog::GetAllSchemas(context)) {
		schema.get().Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.Cast<TableCatalogEntry>().IsMaterializedView()) {
				result->entries.push_back(entry);
			}
		});
	}
	return std::move(result);
}

static void DuckDBMaterializedViewsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBMaterializedViewsData>();
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &table = data.entries[data.offset++].get().Cast<TableCatalogEntry>();
		idx_t col = 0;
		output.SetValue(col++, count, table.catalog.GetName());
		output.SetValue(col++, count, table.schema.name);
		output.SetValue(col++, count, table.name);
		output.SetValue(col++, count, table.GetMaterializedViewQuery());
		vector<Value> dependencies;
		vector<Value> dependency_generations;
		vector<Value> current_dependency_generations;
		for (idx_t i = 0; i < table.GetMaterializedViewDependencyTables().size(); i++) {
			dependencies.push_back(Value(table.GetMaterializedViewDependencyCatalogs()[i] + "." +
			                             table.GetMaterializedViewDependencySchemas()[i] + "." +
			                             table.GetMaterializedViewDependencyTables()[i]));
			dependency_generations.push_back(
			    Value::BIGINT(NumericCast<int64_t>(table.GetMaterializedViewDependencyGenerations()[i])));
			EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, table.GetMaterializedViewDependencyTables()[i]);
			auto dependency = Catalog::GetEntry(context, table.GetMaterializedViewDependencyCatalogs()[i],
			                                    table.GetMaterializedViewDependencySchemas()[i], lookup,
			                                    OnEntryNotFound::RETURN_NULL);
			if (!dependency || dependency->type != CatalogType::TABLE_ENTRY ||
			    !dependency->Cast<TableCatalogEntry>().IsDuckTable()) {
				current_dependency_generations.push_back(Value());
			} else {
				auto &storage = dependency->Cast<TableCatalogEntry>().GetStorage();
				auto generation = storage.GetModificationGeneration();
				auto &transaction = DuckTransaction::Get(context, dependency->ParentCatalog());
				if (transaction.HasModifiedTable(storage)) {
					generation++;
				}
				current_dependency_generations.push_back(Value::BIGINT(NumericCast<int64_t>(generation)));
			}
		}
		output.SetValue(col++, count, Value::LIST(LogicalType::VARCHAR, std::move(dependencies)));
		output.SetValue(col++, count, Value::LIST(LogicalType::BIGINT, std::move(dependency_generations)));
		output.SetValue(col++, count, Value::LIST(LogicalType::BIGINT, std::move(current_dependency_generations)));
		output.SetValue(col++, count, table.MaterializedViewIsStale(context));
		count++;
	}
	output.SetCardinality(count);
}

void DuckDBMaterializedViewsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_materialized_views", {}, DuckDBMaterializedViewsFunction,
	                              DuckDBMaterializedViewsBind, DuckDBMaterializedViewsInit));
}

} // namespace duckdb

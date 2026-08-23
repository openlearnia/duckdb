#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

struct DuckDBMaterializedViewsData : public GlobalTableFunctionState {
	vector<reference<CatalogEntry>> entries;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> DuckDBMaterializedViewsBind(ClientContext &, TableFunctionBindInput &,
	                                                         vector<LogicalType> &return_types,
	                                                         vector<Identifier> &names) {
	names.emplace_back("database_name");
	names.emplace_back("schema_name");
	names.emplace_back("view_name");
	names.emplace_back("definition");
	names.emplace_back("dependencies");
	names.emplace_back("dependency_generations");
	names.emplace_back("current_dependency_generations");
	names.emplace_back("is_stale");
	names.emplace_back("last_refresh_mode");
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::BIGINT),
	                LogicalType::LIST(LogicalType::BIGINT), LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBMaterializedViewsInit(ClientContext &context,
	                                                                       TableFunctionInitInput &) {
	auto result = make_uniq<DuckDBMaterializedViewsData>();
	for (auto &schema : Catalog::GetAllSchemas(context)) {
		schema.get().Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			// This function describes native DuckDB materialized views. DuckLake
			// exposes its managed views through ducklake_materialized_views();
			// mixing the two catalog implementations here leaves DuckLake entries
			// with native MV metadata assumptions and can crash after an attached
			// DuckLake test detaches its catalog.
			if (entry.catalog.GetCatalogType() == "duckdb" && entry.Cast<TableCatalogEntry>().IsMaterializedView()) {
				result->entries.push_back(entry);
			}
		});
	}
	return std::move(result);
}

static void DuckDBMaterializedViewsFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.global_state->Cast<DuckDBMaterializedViewsData>();
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &table = data.entries[data.offset++].get().Cast<TableCatalogEntry>();
		idx_t col = 0;
		output.data[col++].Append(Value(table.catalog.GetName()));
		output.data[col++].Append(Value(table.schema.name));
		output.data[col++].Append(Value(table.name));
		output.data[col++].Append(Value(table.GetMaterializedViewQuery()));
		vector<Value> dependencies;
		vector<Value> dependency_generations;
		vector<Value> current_dependency_generations;
		const auto &catalogs = table.GetMaterializedViewDependencyCatalogs();
		const auto &schemas = table.GetMaterializedViewDependencySchemas();
		const auto &tables = table.GetMaterializedViewDependencyTables();
		const auto &generations = table.GetMaterializedViewDependencyGenerations();
		for (idx_t i = 0; i < tables.size(); i++) {
			dependencies.push_back(Value(catalogs[i] + "." + schemas[i] + "." + tables[i]));
			dependency_generations.push_back(Value::BIGINT(NumericCast<int64_t>(generations[i])));
			EntryLookupInfo lookup(CatalogType::TABLE_ENTRY,
			                       QualifiedName(Identifier(catalogs[i]), Identifier(schemas[i]), Identifier(tables[i])));
			auto dependency = Catalog::GetEntry(context, lookup, OnEntryNotFound::RETURN_NULL);
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
		output.data[col++].Append(Value::LIST(LogicalType::VARCHAR, std::move(dependencies)));
		output.data[col++].Append(Value::LIST(LogicalType::BIGINT, std::move(dependency_generations)));
		output.data[col++].Append(Value::LIST(LogicalType::BIGINT, std::move(current_dependency_generations)));
		output.data[col++].Append(Value::BOOLEAN(table.MaterializedViewIsStale(context)));
		output.data[col++].Append(Value(table.GetMaterializedViewRefreshMode()));
		count++;
	}
	output.SetCardinality(count);
}

void DuckDBMaterializedViewsFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_materialized_views", {}, DuckDBMaterializedViewsFunction,
	                              DuckDBMaterializedViewsBind, DuckDBMaterializedViewsInit));
}

} // namespace duckdb

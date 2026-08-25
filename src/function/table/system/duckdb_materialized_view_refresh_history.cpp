#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

struct DuckDBMaterializedViewRefreshHistoryData : public GlobalTableFunctionState {
	vector<reference<CatalogEntry>> entries;
	idx_t entry_index = 0;
	idx_t refresh_index = 0;
};

static unique_ptr<FunctionData> DuckDBMaterializedViewRefreshHistoryBind(
    ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {Identifier("database_name"), Identifier("schema_name"), Identifier("view_name"),
	         Identifier("refresh_ordinal"), Identifier("refresh_time"), Identifier("refresh_mode")};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT,
	                LogicalType::TIMESTAMP, LogicalType::VARCHAR};
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBMaterializedViewRefreshHistoryInit(
    ClientContext &context, TableFunctionInitInput &) {
	auto result = make_uniq<DuckDBMaterializedViewRefreshHistoryData>();
	auto database_name = DatabaseManager::GetDefaultDatabase(context);
	auto database = DatabaseManager::Get(context).GetDatabase(database_name);
	if (!database || database->GetCatalog().GetCatalogType() != "duckdb") {
		return std::move(result);
	}
	for (auto &schema : database->GetCatalog().GetSchemas(context)) {
		schema.get().Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			if (entry.Cast<TableCatalogEntry>().IsMaterializedView()) {
				result->entries.push_back(entry);
			}
		});
	}
	return std::move(result);
}

static void DuckDBMaterializedViewRefreshHistoryFunction(ClientContext &, TableFunctionInput &input,
                                                          DataChunk &output) {
	auto &data = input.global_state->Cast<DuckDBMaterializedViewRefreshHistoryData>();
	idx_t count = 0;
	while (data.entry_index < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &view = data.entries[data.entry_index].get().Cast<TableCatalogEntry>();
		auto &times = view.GetMaterializedViewRefreshTimes();
		auto &modes = view.GetMaterializedViewRefreshModes();
		auto history_size = MinValue(times.size(), modes.size());
		if (data.refresh_index >= history_size) {
			data.entry_index++;
			data.refresh_index = 0;
			continue;
		}
		idx_t col = 0;
		output.data[col++].Append(Value(view.catalog.GetName()));
		output.data[col++].Append(Value(view.schema.name));
		output.data[col++].Append(Value(view.name));
		output.data[col++].Append(Value::BIGINT(NumericCast<int64_t>(data.refresh_index + 1)));
		output.data[col++].Append(Value::TIMESTAMP(timestamp_t(times[data.refresh_index])));
		output.data[col++].Append(Value(modes[data.refresh_index]));
		data.refresh_index++;
		count++;
	}
	output.SetCardinality(count);
}

void DuckDBMaterializedViewRefreshHistoryFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_materialized_view_refresh_history", {},
	                              DuckDBMaterializedViewRefreshHistoryFunction,
	                              DuckDBMaterializedViewRefreshHistoryBind,
	                              DuckDBMaterializedViewRefreshHistoryInit));
}

} // namespace duckdb

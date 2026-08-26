//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/procedure_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/function_entry.hpp"
#include "duckdb/parser/parsed_data/create_procedure_info.hpp"

namespace duckdb {

class ProcedureCatalogEntry : public FunctionEntry {
public:
	static constexpr const CatalogType Type = CatalogType::PROCEDURE_ENTRY;
	static constexpr const char *Name = "procedure";

public:
	ProcedureCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateProcedureInfo &info);

	vector<LogicalType> parameter_types;
	vector<string> parameter_names;
	LogicalType return_type;
	string language;
	string body;

public:
	unique_ptr<CatalogEntry> Copy(ClientContext &context) const override;
	unique_ptr<CreateInfo> GetInfo() const override;
	string ToSQL() const override;
};

} // namespace duckdb

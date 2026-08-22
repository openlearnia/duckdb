#pragma once

#include "duckdb/parser/parsed_data/create_function_info.hpp"

namespace duckdb {

struct CreateProcedureInfo : public CreateFunctionInfo {
	CreateProcedureInfo() : CreateFunctionInfo(CatalogType::PROCEDURE_ENTRY) {
	}

	vector<LogicalType> parameter_types;
	vector<string> parameter_names;
	LogicalType return_type;
	string language;
	string body;

	unique_ptr<CreateInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb

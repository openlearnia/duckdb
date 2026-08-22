#include "duckdb/parser/parsed_data/create_procedure_info.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

unique_ptr<CreateInfo> CreateProcedureInfo::Copy() const {
	auto result = make_uniq<CreateProcedureInfo>();
	result->parameter_types = parameter_types;
	result->parameter_names = parameter_names;
	result->return_type = return_type;
	result->language = language;
	result->body = body;
	result->name = name;
	CopyFunctionProperties(*result);
	return std::move(result);
}

string CreateProcedureInfo::ToString() const {
	auto result = GetCreatePrefix("PROCEDURE") + QualifierToString(catalog, schema, name) + "(";
	for (idx_t i = 0; i < parameter_names.size(); i++) {
		if (i > 0) result += ", ";
		result += KeywordHelper::WriteQuoted(parameter_names[i]) + " " + parameter_types[i].ToString();
	}
	return result + ") RETURNS " + return_type.ToString() + " LANGUAGE " + language + " AS $$" + body + "$$;";
}

} // namespace duckdb

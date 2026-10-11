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
	result->security_definer = security_definer;
	result->SetFunctionName(GetFunctionName());
	CopyFunctionProperties(*result);
	return std::move(result);
}

string CreateProcedureInfo::ToString() const {
	auto result = GetCreatePrefix("PROCEDURE") + QualifiedNameToString() + "(";
	for (idx_t i = 0; i < parameter_names.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += KeywordHelper::WriteQuoted(parameter_names[i], '"') + " " + parameter_types[i].ToString();
	}
	result += ") RETURNS " + return_type.ToString() + " LANGUAGE " + language;
	if (security_definer) {
		result += " SECURITY DEFINER";
	}
	// pick a dollar-quote tag that does not occur in the body
	string tag = "$$";
	for (idx_t i = 0; body.find(tag) != string::npos; i++) {
		tag = "$body" + to_string(i) + "$";
	}
	return result + " AS " + tag + body + tag + ";";
}

} // namespace duckdb

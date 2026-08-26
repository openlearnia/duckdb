#include "duckdb/parser/peg/ast/macro_parameter.hpp"
#include "duckdb/parser/parsed_data/create_procedure_info.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/create_statement.hpp"

#include <algorithm>

namespace duckdb {

static bool IsJavaScriptIdentifier(const string &name) {
	if (name.empty()) {
		return false;
	}
	auto is_identifier_start = [](char character) {
		return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
		       character == '_' || character == '$';
	};
	auto is_identifier_part = [&](char character) {
		return is_identifier_start(character) || (character >= '0' && character <= '9');
	};
	if (!is_identifier_start(name[0])) {
		return false;
	}
	return std::all_of(name.begin() + 1, name.end(), is_identifier_part);
}

unique_ptr<CreateStatement> PEGTransformerFactory::TransformCreateProcedureStmt(
    PEGTransformer &transformer, const optional<bool> &if_not_exists, const QualifiedName &qualified_name,
    optional<vector<MacroParameter>> procedure_parameters, const LogicalType &type, const Identifier &identifier,
    const string &string_literal) {
	auto result = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateProcedureInfo>();

	info->SetQualifiedName(qualified_name);
	info->on_conflict = if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	info->return_type = type;
	info->language = StringUtil::Lower(identifier.GetIdentifierName());
	info->body = string_literal;

	if (info->language != "javascript") {
		throw ParserException("Unsupported procedure language '%s'", identifier.GetIdentifierName());
	}

	case_insensitive_set_t parameter_names;
	if (procedure_parameters) {
		for (auto &parameter : *procedure_parameters) {
			auto parameter_name = parameter.name.GetIdentifierName();
			if (parameter.type == LogicalType::UNKNOWN) {
				throw ParserException("Procedure parameter '%s' requires a type", parameter_name);
			}
			if (parameter.is_default) {
				throw ParserException("Procedure parameters with defaults are not supported");
			}
			if (!parameter_names.insert(parameter_name).second) {
				throw ParserException("Duplicate parameter '%s' in procedure definition", parameter_name);
			}
			if (!IsJavaScriptIdentifier(parameter_name)) {
				throw ParserException("Procedure parameter '%s' is not a valid JavaScript identifier", parameter_name);
			}
			info->parameter_names.push_back(parameter_name);
			info->parameter_types.push_back(parameter.type);
		}
	}

	result->info = std::move(info);
	transformer.PivotEntryCheck("procedure");
	return result;
}

MacroParameter PEGTransformerFactory::TransformProcedureParameter(PEGTransformer &transformer,
                                                                  const Identifier &col_id, const LogicalType &type) {
	MacroParameter result;
	result.name = col_id;
	result.type = type;
	result.is_default = false;
	return result;
}

vector<MacroParameter> PEGTransformerFactory::TransformProcedureParameters(PEGTransformer &transformer,
                                                                           vector<MacroParameter> procedure_parameter) {
	return procedure_parameter;
}

} // namespace duckdb

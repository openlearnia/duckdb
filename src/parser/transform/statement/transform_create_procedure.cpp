#include "duckdb/parser/parsed_data/create_procedure_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/transformer.hpp"

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

unique_ptr<CreateStatement> Transformer::TransformCreateProcedure(duckdb_libpgquery::PGCreateProcedureStmt &stmt) {
	D_ASSERT(stmt.type == duckdb_libpgquery::T_PGCreateProcedureStmt);

	auto result = make_uniq<CreateStatement>();
	auto qname = TransformQualifiedName(*stmt.name);
	auto info = make_uniq<CreateProcedureInfo>();
	info->catalog = qname.catalog;
	info->schema = qname.schema;
	info->name = qname.name;
	info->return_type = TransformTypeName(*stmt.returnType);
	info->language = StringUtil::Lower(stmt.language);
	info->body = stmt.body;
	info->on_conflict = TransformOnConflict(stmt.onconflict);

	if (info->language != "javascript") {
		throw ParserException("Unsupported procedure language '%s'", stmt.language);
	}

	case_insensitive_set_t parameter_names;
	for (auto node = stmt.params ? stmt.params->head : nullptr; node; node = node->next) {
		auto &parameter = PGCast<duckdb_libpgquery::PGFunctionParameter>(
		    *PGPointerCast<duckdb_libpgquery::PGNode>(node->data.ptr_value));
		if (!parameter.typeName) {
			throw ParserException("Procedure parameter '%s' requires a type", parameter.name);
		}
		if (parameter.defaultValue) {
			throw ParserException("Procedure parameters with defaults are not supported");
		}
		if (!parameter_names.insert(parameter.name).second) {
			throw ParserException("Duplicate parameter '%s' in procedure definition", parameter.name);
		}
		if (!IsJavaScriptIdentifier(parameter.name)) {
			throw ParserException("Procedure parameter '%s' is not a valid JavaScript identifier", parameter.name);
		}
		info->parameter_names.emplace_back(parameter.name);
		info->parameter_types.emplace_back(TransformTypeName(*parameter.typeName));
	}

	result->info = std::move(info);
	return result;
}

} // namespace duckdb

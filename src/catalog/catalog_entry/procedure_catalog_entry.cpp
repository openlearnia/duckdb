#include "duckdb/catalog/catalog_entry/procedure_catalog_entry.hpp"

namespace duckdb {

ProcedureCatalogEntry::ProcedureCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateProcedureInfo &info)
    : FunctionEntry(CatalogType::PROCEDURE_ENTRY, catalog, schema, info),
      parameter_types(std::move(info.parameter_types)), parameter_names(std::move(info.parameter_names)),
      return_type(std::move(info.return_type)), language(std::move(info.language)), body(std::move(info.body)) {
	this->temporary = info.temporary;
	this->internal = info.internal;
	this->dependencies = info.dependencies;
}

unique_ptr<CatalogEntry> ProcedureCatalogEntry::Copy(ClientContext &context) const {
	auto info_copy = GetInfo();
	return make_uniq<ProcedureCatalogEntry>(catalog, schema, info_copy->Cast<CreateProcedureInfo>());
}

unique_ptr<CreateInfo> ProcedureCatalogEntry::GetInfo() const {
	auto info = make_uniq<CreateProcedureInfo>();
	info->SetQualifiedName(schema.GetQualifiedName(name));
	info->parameter_types = parameter_types;
	info->parameter_names = parameter_names;
	info->return_type = return_type;
	info->language = language;
	info->body = body;
	info->dependencies = dependencies;
	info->temporary = temporary;
	info->internal = internal;
	info->comment = comment;
	info->tags = tags;
	return std::move(info);
}

string ProcedureCatalogEntry::ToSQL() const {
	return GetInfo()->ToString();
}

} // namespace duckdb

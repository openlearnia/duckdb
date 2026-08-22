#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

unique_ptr<CreateStatement>
Transformer::TransformRefreshMaterializedView(duckdb_libpgquery::PGRefreshMatViewStmt &stmt) {
	auto qname = TransformQualifiedName(*stmt.relation);
	auto result = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateTableInfo>(qname.catalog, qname.schema, qname.name);
	info->materialized_view = true;
	info->materialized_view_refresh = true;
	info->materialized_view_if_stale = stmt.if_stale;
	result->info = std::move(info);
	return result;
}

} // namespace duckdb

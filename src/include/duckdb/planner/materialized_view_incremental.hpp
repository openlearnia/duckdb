#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class ClientContext;
class TableCatalogEntry;
class SelectStatement;

//! Rewrites an append-only native MV refresh to an exact delta query when the definition is eligible.
//! Returns false when the refresh must use the stored full definition.
bool TryBuildMaterializedViewIncrementalQuery(ClientContext &context, TableCatalogEntry &view,
                                              unique_ptr<SelectStatement> &query, string &refresh_mode);

//! Builds a key-aware logical diff query for grouped native materialized views.
//! Returns an empty string when the definition has no stable GROUP BY identity.
string BuildMaterializedViewLogicalDiffQuery(const string &view_relation_sql, const string &original_sql,
                                             const string &candidate_sql, const vector<string> &column_names,
                                             bool has_previous_refresh);

} // namespace duckdb

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

vector<idx_t> GetMaterializedViewLogicalDiffKeyPositions(const string &original_sql,
                                                         const vector<string> &column_names);

} // namespace duckdb

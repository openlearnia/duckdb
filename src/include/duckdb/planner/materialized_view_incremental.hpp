#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class ClientContext;
class TableCatalogEntry;
class SelectStatement;
class LogicalOperator;

//! Rewrites an append-only native MV refresh to an exact delta query when the definition is eligible.
//! Returns false when the refresh must use the stored full definition.
bool TryBuildMaterializedViewIncrementalQuery(ClientContext &context, TableCatalogEntry &view,
                                              unique_ptr<SelectStatement> &query, string &refresh_mode);

vector<idx_t> GetMaterializedViewLogicalDiffKeyPositions(const string &original_sql,
                                                         const vector<string> &column_names);

//! A materialized view is a snapshot of its base tables: its contents must be a function of the table data alone.
//! Throws when the definition uses a function whose result depends on time, randomness or session state (now(),
//! current_date, random(), getvariable(), current_setting(), ...). `plan` is the bound, unoptimized definition.
DUCKDB_API void VerifyMaterializedViewDeterministic(SelectStatement &definition, const LogicalOperator &plan);

} // namespace duckdb

# Native Materialized Views Implementation Plan

## Phase 1: SQL and catalog vertical slice

1. Add parser tests for create, refresh, `IF STALE`, drop, qualification, `IF NOT EXISTS`, `OR REPLACE`, and `WITH NO DATA`.
2. Add native grammar and AST transformation for materialized-view statements.
3. Add the optional persisted MV payload to `CreateTableInfo`, bound create info, and `DuckTableEntry`.
4. Bind and execute `CREATE MATERIALIZED VIEW` as atomic physical result creation.
5. Reject direct writes and incompatible table DDL against MV entries.
6. Implement full `REFRESH MATERIALIZED VIEW` and `DROP MATERIALIZED VIEW`.
7. Add logical-name discovery through `information_schema.tables` and `duckdb_materialized_views()`.

## Phase 2: Freshness and durability

1. Add committed table-change generations and transaction-local dirty detection.
2. Persist and replay table generations and MV dependency-generation snapshots.
3. Implement `IF STALE` and `duckdb_mv_stale_read=allow|warn|error`.
4. Test rollback, reopen, checkpoint, WAL replay, rename, alter/drop dependency rules, and concurrent readers.

## Phase 3: Exact incremental maintenance

1. Port the DuckLake definition analyzer behind a storage-neutral interface.
2. Create hidden shadow snapshots for eligible base dependencies.
3. Implement exact changed-row and changed-group derivation with bag semantics.
4. Implement projection/filter and grouped `count`/`sum` maintenance.
5. Add derived `avg`, conditional `min`/`max`, aggregate filters, NULL handling, and group liveness.
6. Implement fact-side INNER equijoin maintenance and dimension-change full fallback.
7. Mirror DuckLake's incremental, safety, dependency, transaction, and parser tests for native DuckDB.

## Phase 4: Parity and release

1. Run focused native MV tests, the complete DuckDB SQL suite, and DuckLake's complete MV suite.
2. Run SF10 full-versus-incremental benchmarks and require incremental elapsed time at or below 0.5x full refresh for the supported aggregate workload.
3. Build the pinned DuckDB runtime and DuckLake extension on Blacksmith.
4. Publish paired release assets and verify their checksums and runtime loading.
5. Run an end-to-end catalog check showing only logical MV names and successful reads in both native DuckDB and DuckLake catalogs.


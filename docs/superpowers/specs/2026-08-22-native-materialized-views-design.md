# Native Materialized Views Design

## Objective

Add first-class materialized views to DuckDB with the same user-facing lifecycle and maintenance behavior as the DuckLake implementation: logical names, ordinary SQL reads, create/refresh/drop syntax, stale detection and stale-read policy, exact incremental maintenance for eligible queries, and safe full-refresh fallback.

## SQL contract

```sql
CREATE [OR REPLACE] MATERIALIZED VIEW [IF NOT EXISTS] [catalog.]schema.name AS <select> [WITH [NO] DATA];
REFRESH MATERIALIZED VIEW [IF STALE] [catalog.]schema.name;
DROP MATERIALIZED VIEW [IF EXISTS] [catalog.]schema.name;
```

The logical name is the only user-visible relation name. A materialized view can be selected from anywhere a table can be selected from. `information_schema.tables` reports it as `MATERIALIZED VIEW`, and `duckdb_materialized_views()` exposes its definition, dependencies, freshness, last refresh, and last refresh mode.

## Catalog and storage model

A native materialized view is stored by a `DuckTableEntry` because its result is physical DuckDB table storage. `CreateTableInfo` and the resulting table entry carry an optional `MaterializedViewInfo` payload containing:

- the canonical defining SELECT;
- bound output names and types;
- stable dependency catalog/schema/table identities;
- maintenance analysis and any generated state-column metadata;
- last successful refresh version and refresh mode;
- internal shadow relation identities used by exact incremental maintenance.

This avoids a second backing table and therefore avoids the DuckLake internal-name discovery problem. DML against a materialized view is rejected unless it is an internal refresh operation. Ordinary tables remain byte-for-byte behaviorally unchanged when the optional payload is absent.

The payload is serialized with `CreateTableInfo`, written to the WAL/checkpoint, copied on catalog version changes, and restored on reopen. Older database files deserialize with no payload.

## Freshness model

Every persistent DuckDB table receives a monotonically increasing committed-change generation. Append, delete, update, truncate, and replace operations advance it on commit. The generation is persisted with table catalog metadata and restored during WAL replay/checkpoint load.

An MV stores the dependency generations observed by its last successful refresh. It is stale when any current dependency generation differs, a dependency was dropped/replaced, or the MV has never been populated. The comparison includes the current transaction's local changes, so `IF STALE` and stale-read enforcement are transactionally correct.

The session setting `duckdb_mv_stale_read` accepts `allow`, `warn`, or `error`, matching DuckLake's policy. The check runs when binding/scanning the MV, not only in the discovery function.

## Refresh execution

Refresh is one transactionally atomic operation. Readers see either the previous committed contents or the new contents; failure preserves the previous contents and freshness metadata.

Full refresh executes the stored definition into replacement table storage and swaps it into the logical entry at commit.

Exact incremental refresh initially uses per-MV shadow snapshots for eligible dependencies. The shadow snapshot represents the rows seen at the prior refresh. Changed rows are derived with bag semantics (`EXCEPT ALL` in both directions), changed group keys are identified from old and new rows, and only affected result groups are removed and recomputed. The shadow is updated in the same transaction. This is true incremental result maintenance and is exact for duplicates, NULLs, inserts, deletes, and updates. It deliberately trades change-detection scan cost for a durable, implementation-independent first version; the catalog contract permits replacing shadows with commit-time change journals later without changing SQL behavior.

Eligibility matches DuckLake:

- single-table projection/filter and grouped aggregates;
- `count`, `sum`, safe/derived `avg`, and conditional recomputation for `min`/`max`;
- aggregate `FILTER`, qualified columns, NULL and group-liveness semantics;
- one INNER equijoin with fact-side changes using join-incremental maintenance;
- dimension-side changes and unsupported shapes use full refresh.

The refresh result reports `full`, `incremental`, `join_incremental`, or `skipped` for `IF STALE`.

## Dependency and DDL rules

- Creating an MV registers dependencies so a referenced table cannot be silently dropped or altered incompatibly.
- `DROP ... CASCADE` removes dependent MVs and their internal shadow relations.
- Renames preserve stable dependency identity and regenerate display names.
- `CREATE OR REPLACE` validates and builds the replacement before swapping it in.
- Direct `INSERT`, `UPDATE`, `DELETE`, `MERGE`, `COPY TO <mv>`, and `ALTER TABLE` against an MV are rejected.
- Internal shadow relations are marked internal and excluded from ordinary catalog discovery.

## Compatibility and release gates

- Existing databases and SQL without materialized views retain existing behavior.
- Database reopen, checkpoint, WAL replay, rollback, and concurrent-reader tests are mandatory.
- DuckLake's MV semantic test matrix is mirrored for the native catalog where applicable.
- The paired DuckDB runtime and DuckLake extension are built through Blacksmith and released together.
- The SF10 incremental refresh gate remains no worse than 0.5x full-refresh elapsed time for the supported aggregate workload before declaring full parity.


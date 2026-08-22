//===----------------------------------------------------------------------===//
// DuckDB
//
// duckdb/main/authorization_provider.hpp
//
// Pluggable authorization for DuckDB statements. The core is unopinionated:
// if no provider is installed (the default) every statement is allowed. An
// installed provider is consulted from the binder choke points for reads,
// writes and DDL, and before ATTACH/DETACH execution.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

class ClientContext;
class Catalog;

//! Privileges requested from an AuthorizationProvider. Kept as a bitmask so
//! multi-privilege statements (e.g. MERGE) can request several at once.
enum AuthorizationPrivilege : uint8_t {
	AUTH_NONE = 0,
	AUTH_SELECT = 1 << 0,
	AUTH_INSERT = 1 << 1,
	AUTH_UPDATE = 1 << 2,
	AUTH_DELETE = 1 << 3,
	AUTH_CREATE = 1 << 4,
	AUTH_DROP = 1 << 5,
	AUTH_ALTER = 1 << 6,
	//! engine-management operations: ATTACH / DETACH / USE
	AUTH_ADMIN = 1 << 7,
};

class AuthorizationProvider {
public:
	virtual ~AuthorizationProvider() {
	}

	//! Called when a statement binds a table or view for reading. `catalog`
	//! is the catalog the entry was resolved in. Providers should throw
	//! (e.g. PermissionException) to deny access.
	virtual void CheckReadObject(ClientContext &context, Catalog &catalog, bool is_view,
	                             const string &schema_name, const string &object_name) = 0;

	//! Called when a statement modifies a table (INSERT / UPDATE / DELETE /
	//! MERGE). `privileges` is a bitmask of AuthorizationPrivilege.
	virtual void CheckModifyTable(ClientContext &context, Catalog &catalog, uint8_t privileges,
	                              const string &schema_name, const string &table_name) = 0;

	//! Called for catalog-object DDL (CREATE / DROP / ALTER of tables, views,
	//! schemas, macros). For schemas, `object_name` is the schema name.
	virtual void CheckModifySchema(ClientContext &context, Catalog &catalog, uint8_t privileges,
	                              const string &schema_name, const string &object_name) = 0;

	//! Called before ATTACH / DETACH statements are executed.
	virtual void CheckEngineManagement(ClientContext &context) = 0;

	//! Internal catalogs that never route through the provider (system,
	//! temp, and hidden DuckLake metadata databases).
	static bool ShouldSkipCatalog(const string &catalog_name) {
		return catalog_name == "system" || catalog_name == "temp" ||
		       catalog_name.rfind("__ducklake_metadata_", 0) == 0;
	}
};

//! The default provider: allows everything. Returned when no provider is
//! installed so call sites never need null checks.
class NullAuthorizationProvider : public AuthorizationProvider {
public:
	void CheckReadObject(ClientContext &context, Catalog &catalog, bool is_view, const string &schema_name,
	                     const string &object_name) override {
	}
	void CheckModifyTable(ClientContext &context, Catalog &catalog, uint8_t privileges, const string &schema_name,
	                      const string &table_name) override {
	}
	void CheckModifySchema(ClientContext &context, Catalog &catalog, uint8_t privileges, const string &schema_name,
	                       const string &object_name) override {
	}
	void CheckEngineManagement(ClientContext &context) override {
	}
};

} // namespace duckdb

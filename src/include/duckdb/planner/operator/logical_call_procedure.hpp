//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_call_procedure.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/procedure_catalog_entry.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

class LogicalCallProcedure : public LogicalExtensionOperator {
public:
	LogicalCallProcedure(ProcedureCatalogEntry &procedure, vector<unique_ptr<Expression>> arguments)
	    : LogicalExtensionOperator(std::move(arguments)), procedure(procedure) {
	}

	ProcedureCatalogEntry &procedure;

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;
	bool SupportSerialization() const override {
		return false;
	}
	string GetName() const override {
		return "CALL_PROCEDURE";
	}

protected:
	void ResolveTypes() override {
		types = {procedure.return_type};
	}
};

} // namespace duckdb

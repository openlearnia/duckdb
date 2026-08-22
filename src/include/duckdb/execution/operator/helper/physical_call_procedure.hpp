#pragma once

#include "duckdb/catalog/catalog_entry/procedure_catalog_entry.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class PhysicalCallProcedure : public PhysicalOperator {
public:
	PhysicalCallProcedure(PhysicalPlan &physical_plan, ProcedureCatalogEntry &procedure,
	                      vector<unique_ptr<Expression>> arguments, idx_t estimated_cardinality);

	ProcedureCatalogEntry &procedure;
	vector<unique_ptr<Expression>> arguments;

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}
};

} // namespace duckdb

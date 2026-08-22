#include "duckdb/planner/operator/logical_call_procedure.hpp"

#include "duckdb/execution/operator/helper/physical_call_procedure.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"

namespace duckdb {

PhysicalOperator &LogicalCallProcedure::CreatePlan(ClientContext &, PhysicalPlanGenerator &planner) {
	return planner.Make<PhysicalCallProcedure>(procedure, std::move(expressions), estimated_cardinality);
}

} // namespace duckdb

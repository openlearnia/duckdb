//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/persistent_table_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/table_statistics.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"

namespace duckdb {
class BaseStatistics;

class PersistentTableData {
public:
	explicit PersistentTableData(idx_t column_count);
	~PersistentTableData();

	MetaBlockPointer base_table_pointer;
	vector<MetaBlockPointer> read_metadata_pointers;
	TableStatistics table_stats;
	idx_t total_rows;
	idx_t row_group_count;
	idx_t modification_generation;
	idx_t append_generation;
	idx_t delete_generation;
	idx_t update_generation;
	idx_t appended_rows;
	MetaBlockPointer block_pointer;
};

} // namespace duckdb

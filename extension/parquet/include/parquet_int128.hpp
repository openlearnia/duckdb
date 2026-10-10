//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_int128.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstring>

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uhugeint.hpp"

namespace duckdb {

struct ParquetInt128TargetType {
	data_t bytes[16];
};

struct ParquetInt128 {
	static string MetadataKey(idx_t schema_index) {
		return StringUtil::Format("duckdb.int128.%llu", schema_index);
	}

	template <class T>
	static ParquetInt128TargetType Encode(T input) {
		ParquetInt128TargetType result;
		auto upper = static_cast<uint64_t>(input.upper);
		for (idx_t i = 0; i < 8; i++) {
			result.bytes[i] = static_cast<data_t>(upper >> (56 - 8 * i));
			result.bytes[i + 8] = static_cast<data_t>(input.lower >> (56 - 8 * i));
		}
		return result;
	}

	template <class T>
	static T Decode(const_data_ptr_t input) {
		uint64_t upper = 0;
		T result;
		result.lower = 0;
		for (idx_t i = 0; i < 8; i++) {
			upper = (upper << 8) | input[i];
			result.lower = (result.lower << 8) | input[i + 8];
		}
		memcpy(&result.upper, &upper, sizeof(upper));
		return result;
	}
};

} // namespace duckdb

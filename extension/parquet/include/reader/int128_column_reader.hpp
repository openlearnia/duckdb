//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/int128_column_reader.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "parquet_int128.hpp"
#include "templated_column_reader.hpp"

namespace duckdb {

template <class T>
struct Int128ValueConversion {
	template <bool CHECKED>
	static T PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			plain_data.available(16);
		}
		auto result = ParquetInt128::Decode<T>(const_data_ptr_cast(plain_data.ptr));
		plain_data.unsafe_inc(16);
		return result;
	}

	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		if (CHECKED) {
			plain_data.inc(16);
		} else {
			plain_data.unsafe_inc(16);
		}
	}

	static bool PlainAvailable(const ByteBuffer &plain_data, idx_t count) {
		return plain_data.check_available(count * 16);
	}

	static idx_t PlainConstantSize() {
		return 0;
	}
};

template <class T>
using Int128ColumnReader = TemplatedColumnReader<T, Int128ValueConversion<T>>;

} // namespace duckdb

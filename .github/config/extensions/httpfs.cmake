duckdb_extension_load(httpfs
    LOAD_TESTS
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    # Build the latest HTTPFS source, then patch multipart sizing for
    # S3-compatible stores such as Cloudflare R2.
    GIT_TAG c942cee64bb1bc848168d4ad74fcd9eff2c616e7
    APPLY_PATCHES
)

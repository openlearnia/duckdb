duckdb_extension_load(httpfs
    LOAD_TESTS
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    # Keep constant-sized non-trailing parts for S3-compatible stores such as
    # Cloudflare R2. The adaptive part-size tiers introduced in a7e4a05 can
    # produce unequal non-trailing parts, which R2 rejects at completion.
    GIT_TAG 5998e7be67b107cfca8898196fd845d3e24356a9
    APPLY_PATCHES
)

duckdb_extension_load(httpfs
    LOAD_TESTS
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    # Upstream pin (SigV4 moved into core). The R2 multipart-sizing patch in
    # .github/patches/extensions/httpfs/ must be kept applied until upstream
    # fixes constant part sizes for S3-compatible stores such as Cloudflare R2.
    GIT_TAG fafb14f2c899ddfd1998f8adf2e07fbbfd28b3fd
    APPLY_PATCHES
)

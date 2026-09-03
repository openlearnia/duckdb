duckdb_extension_load(httpfs
    LOAD_TESTS
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    # Upstream pin (SigV4 moved into core). The multipart reimplementation on
    # this pin emits constant-size non-trailing parts, which is what the R2
    # multipart patch used to enforce; the patch was dropped in the
    # v2.0-cyanoptera sync. Run a live R2 multipart write test before shipping.
    GIT_TAG fafb14f2c899ddfd1998f8adf2e07fbbfd28b3fd
    APPLY_PATCHES
)

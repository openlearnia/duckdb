duckdb_extension_load(httpfs
    LOAD_TESTS
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    # Upstream pin (SigV4 moved into core). Re-pinned to upstream's
    # v2.0-cyanoptera bump; the dropped R2 multipart patch stayed dropped, so
    # run a live R2 multipart write test before shipping.
    GIT_TAG 96a2f2e88e5dd075facbc5a65dc3afd67aa2bb44
    APPLY_PATCHES
)

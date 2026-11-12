# Connectivity CPU research harness

Run in Linux Docker, not as evidence of CUDA or PostgreSQL query performance.
The container needs GCC, PostgreSQL16 development libraries, Python, NumPy and
SciPy. The phase2-style data/graph directory is mounted at `/out/inputs` with
`syn`, `sift`, `far` subdirectories containing original #203 resident adjacency,
row IDs, reach mask, graph.json, base.fbin, queries.fbin and optional canonical GT.
Original source inputs must remain read-only; control graphs are copies in RAM.

```
gcc -O2 -std=c11 -shared -fPIC -Isrc test/experimental/conn203/kernel.c -o /tmp/conn203-kernel.so
python3 test/experimental/conn203/compare.py
python3 test/experimental/conn203/verify_results.py
make test-connectivity test-hnsw-converter
python3 test/experimental/conn203/mutations.py
python3 test/experimental/conn203/page_mutation.py
```

`postgres.sh` assumes the CPU shim extension/daemon are built/installed at
`/tmp/conn203-product`. It creates its own temporary cluster, exercises a real
SQL/NOTICE snapshot and INSERT, and uses immediate stop/restart to test WAL
recovery. It does not execute cuVS. `file_cost.py` constructs hnswlib-layout
fixtures with original adjacency and zero vectors (the check does not read
vectors); `final_cost.py` replays both heap and file-backed scratch paths.
The `--connectivity-file*` modes of the converter fixture invoke the actual
product file-check function with mocked PostgreSQL services.

Recall uses a fixed small prefix of original natural queries and one bundle
query per control. Every result has 10 unique in-range IDs and finite distances.
Original graph replays and CPU-created duplicate/induced/connected controls are
separately labeled. This is not a 10k-query product benchmark. `exact_all` is
the float64 reference, measured once per query and repeated as an ef-independent
reference in both ef tables. Replayed original GT sets are cross-checked.

Raw `storage` fields are compact payload estimates, not Python RSS or measured
PostgreSQL on-disk sizes. The final report uses a uniform float32/u32 model,
including both endpoints for sparse reverse edges. Real PG locators, visibility,
sidecar epochs, WAL, and headers add costs. Reverse-edge prototypes do not prune
to PG's fixed degree limit; production needs a sidecar or a capacity/pruning design.

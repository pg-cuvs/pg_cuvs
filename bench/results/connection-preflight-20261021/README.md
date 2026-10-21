# Benchmark connection preflight verification

Source: `b9f641b14692586cb3d0cfaee189fc8ff57e6f1c` (application change). This directory is a separate evidence commit.

The actual standalone `pg_engine.py` CLI ran to completion against an isolated
PostgreSQL 16.4 with the real pg_cuvs CPU-shim extension preloaded. The positive
fixture selected `pgvector_hnsw --require-gpu-environment`: 256 normalized
16-dimensional rows, eight queries, seven parameter points. It wrote seven CSV
rows and executed 112 real search SQL statements (56 warmup + 56 measured).
Connections, SQL, preload and schema status were not mocked. No GPU daemon ran.

The same CLI rejected missing PGHOST, a resolved pg_catalog search_path,
PGSERVICE overriding the explicit host, and missing pg_cuvs preload. A pgcuvs_cagra
invocation without the opt-in flag also rejected missing PGHOST before connecting.
The local PostgreSQL stopped successfully and its postmaster PID file was absent.

## Reproduce the consumer path

With a disposable UTF8 PostgreSQL, pgvector and pg_cuvs CPU shim installed:

```sh
export PGHOST=/path/to/fixture/socket
python bench/cuvs_bench_backend/pg_engine.py \
  --corpus corpus.fbin --queries queries.fbin --gt gt.npy --n 256 \
  --out fixture.csv --algos pgvector_hnsw --ks 10 --max-queries 8 \
  --require-gpu-environment
```

Keep argv and cwd fixed; remove PGHOST, set PGOPTIONS='-c search_path=pg_catalog',
use a fixture-only PGSERVICE with another socket alias, or restart only that
fixture without preload to check the failure paths. No new GPU is needed.

Related Python checks: 73 passed. Source byte hashes, case exits and the
SQL counting method are in [verification.json](verification.json).

## Limits and lesson

This verifies connection consumption and real CPU SQL through the benchmark
entry. It does not verify CUDA, GPU quality/performance, deployment freshness,
or the RMM native acceptance gate. Raw SQL logs, generated arrays and build
trees are local only. Metadata preservation does not back up those artifacts.

A default SQL_ASCII fixture returned bytes and failed the new status parsing;
the successful fixture explicitly used UTF8, matching benchmark provisioning.
Docker storage I/O prevented the container route. A PG17 API mismatch and
Linux-only daemon APIs prevented those local alternatives. No production C/CUDA
code was changed to work around these fixture limits.

Validate the final consumer with its actual argv/env/cwd. Parsing or launch-only
checks cannot establish which server or schema the workload will use.

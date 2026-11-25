"""Race a canceled REINDEX against a subsequent EXTEND on the CPU shim."""

from contextlib import contextmanager
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("pg_fixture", HERE.parent / "page_format/run.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)

ROWS = 2048
TID_SQL = "SELECT id FROM {table} ORDER BY v <-> '[-1,0,0,0]'::vector LIMIT 1"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def wait_for(description, ready):
    until = time.monotonic() + 12
    while time.monotonic() < until:
        result = ready()
        if result:
            return result
        time.sleep(0.025)
    raise RuntimeError(f"timed out waiting for {description}")


def psql(cluster, query, **kwargs):
    return subprocess.run(
        ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-v", "VERBOSITY=verbose", "-c", query],
        env=cluster.env, text=True, capture_output=True, timeout=12, **kwargs
    )


def sql(cluster, query):
    run = psql(cluster, query)
    require(run.returncode == 0, run.stderr)
    return run.stdout.strip()


@contextmanager
def session(cluster, name, query, options=""):
    with subprocess.Popen(
        ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-v", "VERBOSITY=verbose", "-c", query],
        env={**cluster.env, "PGAPPNAME": name, "PGOPTIONS": options},
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    ) as run:
        try:
            yield run
        finally:
            if run.poll() is None:
                run.kill()


def cancel(cluster, reindex):
    require(sql(cluster, """
        SELECT pg_cancel_backend(pid) FROM pg_stat_activity
        WHERE application_name = 'cancel-build' AND state = 'active'
    """) == "t", "failed to send cancel to active REINDEX")
    _, err = reindex.communicate(timeout=5)
    require(reindex.returncode != 0 and "57014" in err and "due to user request" in err, err)


def files(cluster, key):
    # The inode witnesses publication even if the replacement has identical
    # bytes. Include all authoritative files and reject leaked staging files.
    return {p.name: (p.stat().st_ino, p.read_bytes())
            for p in cluster.indexdir.glob(f"{key}.*") if p.is_file()}


def stats(cluster, index):
    return sql(cluster, f"""
        SELECT n_vecs, extend_count FROM pg_stat_gpu_search
        WHERE index_oid = '{index}'::regclass
    """)


def assert_extended(cluster, table, index):
    seen = stats(cluster, index)
    require(seen == f"{ROWS + 1}|1", f"EXTEND lost from daemon registry: {seen!r}")
    query = TID_SQL.format(table=table)
    plan = sql(cluster, f"SET enable_seqscan=off; EXPLAIN (COSTS OFF) {query}")
    require(f"Index Scan using {index}" in plan, plan)
    counter = f"SELECT search_count FROM pg_stat_gpu_search WHERE index_oid = '{index}'::regclass"
    before = int(sql(cluster, counter))
    require(sql(cluster, f"SET enable_seqscan=off; {query}") == "-1",
            "GPU index scan did not return the row added by EXTEND")
    require(int(sql(cluster, counter)) > before, "nearest-neighbor query did not reach the daemon")


def run_case(cluster, gate, label, phase, am, *, shards=1, workers=0):
    table, index = f"rows_{label}", f"idx_{label}"
    pg_options = (f"-c cuvs.shard_count={shards} -c max_parallel_maintenance_workers={workers} "
                  "-c min_parallel_table_scan_size=0 -c cuvs.cpu_hnsw_fallback=off")
    ivf_options = "WITH (n_lists=4, pq_bits=8, pq_dim=2)" if am == "ivfpq" else ""
    sql(cluster, f"""
        CREATE TABLE {table} (id int, v vector(4)) WITH (parallel_workers=2);
        INSERT INTO {table} SELECT g, format('[%s,0,0,0]', g)::vector
            FROM generate_series(1, {ROWS}) g;
        ANALYZE {table};
        SET max_parallel_maintenance_workers=0;
        CREATE INDEX {index} ON {table} USING {am} (v vector_l2_ops) {ivf_options};
    """)
    key = sql(cluster, f"""
        SELECT oid || '_' || '{index}'::regclass::oid FROM pg_database
        WHERE datname = current_database()
    """)
    before = files(cluster, key)
    require(f"{key}.tids" in before, f"{label}: initial BUILD did not publish .tids")
    log = cluster.logs / "daemon-publication.log"
    log_offset = log.stat().st_size
    for name in ("entered", "done"):
        (gate / name).unlink(missing_ok=True)
    arm = gate / "arm"
    arm.write_text(f"{phase} {key}\n")
    insert = f"INSERT INTO {table} VALUES (-1, '[-1,0,0,0]')"
    try:
        with session(cluster, "cancel-build", f"REINDEX INDEX {index}", pg_options) as reindex:
            wait_for(f"{label} to reach {phase}", lambda: (gate / "entered").exists())
            if workers or shards > 1:
                path = "multi" if workers else "sharded"
                require(re.search(r"\[admit\].*path=" + path + r"\b", log.read_text()[log_offset:]),
                        f"{label}: REINDEX did not exercise {path}")
            cancel(cluster, reindex)
            if phase == "gpu":
                # The daemon is outside L0. Success here establishes the exact
                # ordering that used to permit a stale corpus to overwrite it.
                require(sql(cluster, f"{insert} RETURNING id") == "-1", "INSERT failed")
                assert_extended(cluster, table, index)
            if phase == "published":
                require(files(cluster, key)[f"{key}.tids"][0] != before[f"{key}.tids"][0],
                        "fixture did not stop after the first final rename")
                with session(cluster, "after-cancel-insert", f"{insert} RETURNING id") as writer:
                    wait_for("INSERT to hold the heap write lock", lambda: sql(cluster, f"""
                        SELECT EXISTS (
                            SELECT 1 FROM pg_stat_activity a JOIN pg_locks l USING (pid)
                            WHERE a.application_name = 'after-cancel-insert'
                              AND a.state = 'active' AND l.relation = '{table}'::regclass
                              AND l.mode = 'RowExclusiveLock' AND l.granted
                        )
                    """) == "t")
                    require(writer.poll() is None, "EXTEND passed an unfinished publication")
                    arm.unlink()
                    row, err = writer.communicate(timeout=8)
                    require(writer.returncode == 0 and row.strip() == "-1", err)
            else:
                arm.unlink()
            wait_for(f"{label} BUILD thread to leave", lambda: (gate / "done").exists())
    finally:
        arm.unlink(missing_ok=True)

    if phase == "gpu" or phase == "published":
        assert_extended(cluster, table, index)
    if phase != "published":
        require(files(cluster, key) == before, f"{label}: canceled REINDEX changed prior artifacts")
    print(f"{label}: PASS", flush=True)


def main():
    pg_config, daemon, cc = sys.argv[1:]
    bindir = subprocess.check_output([pg_config, "--bindir"], text=True).strip()
    output = os.environ.get("BUILD_PUBLICATION_OUTPUT")
    with tempfile.TemporaryDirectory(prefix="pgcuvs-build-publish.", dir="/tmp") as tmp:
        work = Path(tmp)
        logs = Path(output).resolve() if output else work / "logs"
        logs.mkdir(parents=True, exist_ok=True)
        gate = work / "gate"
        gate.mkdir()
        preload = work / "gate.so"
        fixture.command(
            [*cc.split(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
             HERE / "build_publication_gate.c", "-o", preload, "-ldl"],
            logs / "compile-gate.log",
        )
        cluster = fixture.Cluster(work / "pg", bindir, logs)
        cluster.env.update(
            LD_PRELOAD=str(preload), CUVS_SHIM_BUILD_DELAY_MS="137",
            PGCUVS_BUILD_GATE_DIR=str(gate), PGCUVS_BUILD_GATE_SOCKET=str(cluster.socket),
        )
        # Set the preload only for the daemon child. PostgreSQL and psql use
        # their normal libc calls.
        pg_env = cluster.env.copy()
        for key in ("LD_PRELOAD", "CUVS_SHIM_BUILD_DELAY_MS", "PGCUVS_BUILD_GATE_DIR",
                    "PGCUVS_BUILD_GATE_SOCKET"):
            pg_env.pop(key)
        try:
            with cluster.daemon(Path(daemon).resolve(), "publication", vram_mib=512):
                cluster.env = pg_env
                cluster.start()
                sql(cluster, "CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs;")
                run_case(cluster, gate, "extend_before_publish", "gpu", "cagra")
                run_case(cluster, gate, "extend_after_publish", "published", "cagra")
                run_case(cluster, gate, "single_staged", "staged", "cagra")
                run_case(cluster, gate, "multi_staged", "staged", "cagra", workers=2)
                run_case(cluster, gate, "sharded_staged", "staged", "cagra", shards=2)
                run_case(cluster, gate, "flat_staged", "staged", "flat")
                run_case(cluster, gate, "ivfpq_staged", "staged", "ivfpq")
        except Exception:
            daemon_log = logs / "daemon-publication.log"
            if daemon_log.exists():
                print(daemon_log.read_text(), file=sys.stderr)
            raise
        finally:
            cluster.stop()


if __name__ == "__main__":
    main()

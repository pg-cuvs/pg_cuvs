"""Run U-13 on an isolated page-format primary using only Python 3 and psql.

Usage: python3 test/page_format/u13-concurrent.py DISPOSABLE_DB [--observe-page-hits]

For --observe-page-hits, provision a resident page BF float32 copy at BUILD and
REINDEX. The pinned daemon provides this via its existing
CUVS_FAULT_PAGE_BF_FIXTURE=1 hook in pg_cuvs_server_test. A CPU-shim run with
that fixture covers page IPC. For physical GPU evidence, use a native CUDA/cuVS
test daemon on an isolated A100 with the float32 BF fixture. Trace CUDA BF search
kernels in the daemon with Nsight Systems during the labeled calls. Record the
daemon binary identity, device, backend PID, database and index OIDs, and hit
deltas by index incarnation. A hit records preparation of a successful daemon
reply, including calls that subsequently use the heap SQL path. To trace the
backend receiving and accepting candidates, also trace the return values of
cuvs_ipc_search_pages and page_search_sql with symbols available.
"""

import argparse
import json
import os
import selectors
import subprocess
import time
from pathlib import Path
from uuid import uuid4


def psql(dbname):
    return ["psql", "-XwqAt", "--pset=pager=off", "--dbname", dbname]


class Session:
    def __init__(self, dbname, appname):
        self.proc = subprocess.Popen(
            psql(dbname) + ["-v", "ON_ERROR_STOP=off"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            env={**os.environ, "PGAPPNAME": appname, "PGCONNECT_TIMEOUT": "10"},
        )
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.proc.stdout, selectors.EVENT_READ)
        self.pending = b""
        self.command_number = 0
        self.appname = appname
        self.pid = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        finally:
            self.selector.close()
            self.proc.stdout.close()

    def execute(self, statement):
        self.command_number += 1
        marker = f"__u13_{self.command_number}__".encode()
        # Keep the session alive after a SQL error so an open transaction can
        # be rolled back. psql writes the original server error to stderr.
        command = statement.rstrip().rstrip(";").encode()
        payload = command + b";\n\\echo " + marker + b" :SQLSTATE\n"
        self.proc.stdin.write(payload)
        self.proc.stdin.flush()
        deadline = time.monotonic() + 75
        lines = []
        while True:
            line, sep, rest = self.pending.partition(b"\n")
            if sep:
                self.pending = rest
                if line.startswith(marker + b" "):
                    state = line.split()[1]
                    if state != b"00000":
                        raise RuntimeError(f"{self.appname}: SQLSTATE {state.decode()} ({statement.strip()})")
                    return lines
                lines.append(line.decode("utf-8"))
                continue

            remaining = deadline - time.monotonic()
            if remaining <= 0 or not self.selector.select(remaining):
                raise TimeoutError(f"{self.appname}: did not finish SQL within 75s")
            data = os.read(self.proc.stdout.fileno(), 65536)
            if not data:
                raise RuntimeError(f"{self.appname}: psql exited before returning SQLSTATE")
            self.pending += data


def one(conn, statement):
    rows = conn.execute(f"""
        WITH u13_value(value) AS ({statement})
        SELECT coalesce(to_json(value), 'null'::json) FROM u13_value
    """)
    if len(rows) != 1:
        raise RuntimeError(f"expected one SQL row, got {len(rows)} ({statement})")
    return json.loads(rows[0])


def literal(value):
    return "'" + value.replace("'", "''") + "'"


def identifier(value):
    return '"' + value.replace('"', '""') + '"'


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def configure(conn, schema=None):
    conn.execute("SET statement_timeout = '60s'")
    conn.execute("SET lock_timeout = '10s'")
    conn.execute("SET standard_conforming_strings = on")
    conn.execute("SET client_encoding = 'UTF8'")
    conn.pid = one(conn, "SELECT pg_backend_pid()")
    if schema is not None:
        conn.execute(f"SET search_path TO {identifier(schema)}, public")
        conn.execute("SELECT use_bf_oracle_settings(false)")


def page_hits(conn, index_oid):
    rows = one(conn, f"""
        SELECT coalesce(json_agg(json_build_array(warmup_state, hits)), '[]'::json)
        FROM public.pg_cuvs_index_status()
        WHERE db_oid = (SELECT oid FROM pg_database WHERE datname = current_database())
          AND index_oid = {index_oid}::oid AND kind = 'page_cagra' AND state = 'resident'
    """)
    hits = dict(rows)
    require(
        rows and len(hits) == len(rows)
        and all(name and name.startswith("incarnation:") and count is not None
                for name, count in rows),
        f"expected identifiable resident page_cagra entries for index {index_oid}: {rows}",
    )
    return hits


def check_with_hits(conn, observer, index_oid, stage, statement, expected_hits, incarnation=None):
    before = page_hits(observer, index_oid) if expected_hits is not None else None
    print(f"{stage} BEGIN unix_ns={time.time_ns()} backend={conn.pid}", flush=True)
    try:
        conn.execute(statement)
    finally:
        print(f"{stage} END unix_ns={time.time_ns()}", flush=True)
        if before is not None:
            after = page_hits(observer, index_oid)
            print(f"{stage} page hits expected_delta={expected_hits} {before} -> {after}", flush=True)
    if before is None:
        return True
    if before.keys() != after.keys():
        return False
    deltas = {key: after[key] - before[key] for key in before}
    if incarnation is not None:
        return incarnation in deltas and all(
            delta == (expected_hits if key == incarnation else 0)
            for key, delta in deltas.items()
        )
    return sorted(deltas.values()) == [0] * (len(before) - 1) + [expected_hits]


def compare(conn, observer, index_oid, stage, api, expected_hits, allowed=None, *,
            heap="base", index="base_idx", incarnation=None):
    tid_array = "NULL::tid[]" if allowed is None else (
        "ARRAY[" + ", ".join(literal(tid) for tid in allowed) + "]::tid[]"
    )
    label = f"{stage}/{api}"
    hits_match = check_with_hits(
        conn, observer, index_oid, label,
        f"SELECT assert_base({literal(stage)}, {literal(api)}, 3, {tid_array}, "
        f"{literal(heap)}::regclass, {literal(index)}::regclass)",
        expected_hits, incarnation,
    )
    print(f"{label} heap SQL comparison PASS", flush=True)
    require(hits_match, f"{label}: unexpected per-incarnation page search hit delta")


def ids(conn, heap="base"):
    return one(conn, f"SELECT ARRAY(SELECT id FROM ONLY {identifier(heap)} ORDER BY id LIMIT 3)")


def catalog_version(conn, index_oid):
    return one(conn, f"""
        SELECT json_build_array(c.relfilenode, c.xmin::text, i.indisvalid, i.indcheckxmin)
        FROM pg_class c JOIN pg_index i ON i.indexrelid = c.oid
        WHERE c.oid = {index_oid}::oid
    """)


def run(reader, writer, observer, observe):
    index_oid = int(one(writer, "SELECT 'base_idx'::regclass::oid"))
    db_oid = int(one(writer, "SELECT oid FROM pg_database WHERE datname = current_database()"))
    print(f"U-13 db_oid={db_oid} index_oid={index_oid}", flush=True)
    hit, no_hit = (1, 0) if observe else (None, None)
    apis = ("batch", "tid", "bigint")
    whitelist = one(reader, "SELECT ARRAY(SELECT ctid::text FROM base WHERE id BETWEEN 1 AND 4)")
    require(len(set(whitelist)) == 4, "baseline whitelist must have four distinct rows")
    compare(reader, observer, index_oid, "committed baseline", "batch", hit)
    compare(reader, observer, index_oid, "committed explicit filter", "tid", hit, whitelist)
    compare(reader, observer, index_oid, "committed explicit filter", "bigint", hit, whitelist)

    writer.execute("BEGIN")
    root = one(writer, "SELECT ctid::text FROM base WHERE id = 1")
    child = one(writer, "UPDATE base SET note = 'HOT' WHERE id = 1 RETURNING ctid::text")
    writer.execute(f"SELECT assert_hot('base', {literal(root)}::tid, {literal(child)}::tid)")
    writer.execute("COMMIT")
    hot_filter = [*whitelist, child, child]
    compare(reader, observer, index_oid, "committed HOT", "batch", hit)
    compare(reader, observer, index_oid, "committed HOT whitelist", "tid", hit, hot_filter)
    compare(reader, observer, index_oid, "committed HOT whitelist", "bigint", hit, hot_filter)

    # No VACUUM intervenes. The three base BF candidates include id 1 and omit
    # id 4. The happy-path daemon reply and the refilled result are separate checks.
    writer.execute("BEGIN")
    try:
        require(one(writer, "DELETE FROM base WHERE id = 1 RETURNING id") == 1, "missing id 1")
        require(ids(writer) == [2, 3, 4], "DELETE fixture must remove the nearest result")
        for api in apis:
            compare(writer, observer, index_oid, "deleted nearest, short shortlist", api, hit)
    finally:
        writer.execute("ROLLBACK")

    # Check the predicate lock after the standalone product call, before the
    # oracle has an opportunity to acquire a relation-level SIReadLock.
    raw_calls = {
        "batch": """
            SELECT count(*) FROM public.pg_cuvs_batch_search(
                'base', ARRAY['[0,0,0,0,0,0,0,0]'::vector,
                              '[256.25,0,0,0,0,0,0,0]'::vector], 3)
        """,
        "tid": """
            SELECT count(*) FROM public.cuvs_filtered_knn(
                'base_idx', '[0,0,0,0,0,0,0,0]'::vector, NULL::tid[], 3)
        """,
        "bigint": """
            SELECT count(*) FROM public.cuvs_filtered_knn(
                'base_idx', '[0,0,0,0,0,0,0,0]'::vector, NULL::bigint[], 3)
        """,
    }
    lock_sql = """
        SELECT EXISTS (
            SELECT FROM pg_locks WHERE pid = pg_backend_pid()
              AND locktype = 'relation' AND relation = 'base'::regclass
              AND mode = 'SIReadLock' AND granted
        )
    """
    for api in apis:
        reader.execute("BEGIN ISOLATION LEVEL SERIALIZABLE READ WRITE NOT DEFERRABLE")
        try:
            require(not one(reader, lock_sql), f"SERIALIZABLE/{api}: preexisting heap SIReadLock")
            hits_match = check_with_hits(
                reader, observer, index_oid, f"SERIALIZABLE/{api} product only",
                raw_calls[api], no_hit,
            )
            has_lock = one(reader, lock_sql)
            require(has_lock and hits_match,
                    f"SERIALIZABLE/{api}: base relation SIReadLock={has_lock}, hit check={hits_match}")
            print(f"SERIALIZABLE/{api} base SIReadLock PASS", flush=True)
            compare(reader, observer, index_oid, "SERIALIZABLE", api, no_hit)
        finally:
            reader.execute("ROLLBACK")

    # The INSERT statement has returned but the writer has not committed.
    # The reader uses the same snapshot on both sides of the writer's COMMIT.
    reader.execute("BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY")
    baseline_snapshot = one(reader, "SELECT pg_current_snapshot()::text")
    try:
        writer.execute("BEGIN")
        one(writer, "INSERT INTO base (id, v) VALUES (0, '[0,0,0,0,0,0,0,0]') RETURNING id")
        require(ids(writer) == [0, 1, 2], "writer must see the pending row")
        require(ids(reader) == [1, 2, 3], "reader must not see the uncommitted INSERT")
        for api in apis:
            compare(writer, observer, index_oid, "writer sees pending INSERT", api, no_hit)
            compare(reader, observer, index_oid, "reader before INSERT commit", api, no_hit)
        writer.execute("COMMIT")
        require(one(reader, "SELECT pg_current_snapshot()::text") == baseline_snapshot,
                "reader snapshot changed across INSERT commit")
        require(ids(reader) == [1, 2, 3], "old snapshot must still exclude the INSERT")
        require(ids(writer) == [0, 1, 2], "fresh snapshot must include the committed INSERT")
        for api in apis:
            compare(reader, observer, index_oid, "old snapshot after INSERT commit", api, no_hit)
            compare(writer, observer, index_oid, "fresh snapshot after INSERT commit", api, no_hit)
    finally:
        reader.execute("ROLLBACK")

    # A clean heap isolates the catalog guard from earlier HOT and pending activity.
    heap, index = "reindex_base", "reindex_idx"
    writer.execute("""
        CREATE TABLE reindex_base (id integer NOT NULL, v vector(8) NOT NULL)
        WITH (autovacuum_enabled = false)
    """)
    writer.execute("INSERT INTO reindex_base SELECT id, v FROM ONLY base ORDER BY id")
    writer.execute("CREATE INDEX reindex_idx ON reindex_base USING cagra (v vector_l2_ops)")
    writer.execute("SELECT assert_page('reindex_idx')")
    index_oid = int(one(writer, "SELECT 'reindex_idx'::regclass::oid"))
    print(f"U-13 REINDEX db_oid={db_oid} index_oid={index_oid}", flush=True)

    old_catalog = catalog_version(writer, index_oid)
    old_entries = page_hits(observer, index_oid) if observe else {}
    reader.execute("BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY")
    try:
        # Pin before taking a lock on the heap/index so plain REINDEX can finish.
        old_snapshot = one(reader, "SELECT pg_current_snapshot()::text")
        require(one(writer, "DELETE FROM reindex_base WHERE id = 0 RETURNING id") == 0, "missing id 0")
        writer.execute("REINDEX INDEX reindex_idx")
        writer.execute("SELECT assert_page('reindex_idx')")
        new_catalog = catalog_version(writer, index_oid)
        require(int(one(writer, "SELECT 'reindex_idx'::regclass::oid")) == index_oid
                and new_catalog[0] != old_catalog[0] and new_catalog[1] != old_catalog[1]
                and new_catalog[2:] == [True, False],
                f"expected a new valid relfilenode without indcheckxmin: {old_catalog} -> {new_catalog}")
        incarnation = None
        if observe:
            new_entries = page_hits(observer, index_oid).keys() - old_entries.keys()
            require(len(new_entries) == 1, f"expected one new REINDEX incarnation: {new_entries}")
            incarnation = new_entries.pop()
        require(one(reader, "SELECT pg_current_snapshot()::text") == old_snapshot
                and catalog_version(reader, index_oid) == old_catalog,
                "old snapshot must see the previous index catalog version")
        require(ids(reader, heap) == [0, 1, 2] and ids(writer, heap) == [1, 2, 3],
                "REINDEX fixture must distinguish old and fresh nearest rows")
        # The rebuilt corpus omits id 0. Both snapshots can fill k with ids
        # 1, 2, 3 from the new BF corpus. Only the old oracle requires id 0.
        for api in apis:
            compare(writer, observer, index_oid, "fresh snapshot after REINDEX", api, hit,
                    heap=heap, index=index, incarnation=incarnation)
            compare(reader, observer, index_oid, "old snapshot after REINDEX", api, no_hit,
                    heap=heap, index=index, incarnation=incarnation)
    finally:
        reader.execute("ROLLBACK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dbname", help="disposable database name; use libpq environment for connection settings")
    parser.add_argument("--observe-page-hits", action="store_true")
    args = parser.parse_args()

    schema = f"u13_{uuid4().hex}"
    fixture = Path(__file__).with_name("u13.sql")
    with Session(args.dbname, "u13-observer") as observer:
        configure(observer)
        try:
            subprocess.run(
                psql(args.dbname) + ["-v", f"u13_schema={schema}", "-v", "u13_setup_only=1",
                                    "-f", str(fixture)],
                check=True, timeout=120,
                env={**os.environ, "PGCONNECT_TIMEOUT": "10"},
            )
            with Session(args.dbname, "u13-reader") as reader, Session(args.dbname, "u13-writer") as writer:
                configure(reader, schema)
                configure(writer, schema)
                run(reader, writer, observer, args.observe_page_hits)
        finally:
            observer.execute(f"DROP SCHEMA IF EXISTS {identifier(schema)} CASCADE")

    print("U-13 committed-fixture SQL correctness PASS", flush=True)
    print(f"U-13 page IPC hit checks {'PASS' if args.observe_page_hits else 'NOT RUN'}", flush=True)


if __name__ == "__main__":
    main()

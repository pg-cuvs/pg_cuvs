"""Check resident-overlay KNN against a same-snapshot PostgreSQL heap sort."""

import argparse
from contextlib import contextmanager
import errno
import itertools
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import tempfile
import time

from run import Cluster


TABLE = "resident_overlay"
INDEX = TABLE + "_idx"
DB = "postgres"
DIM = 32
K = 3
BASE_ROWS = 256
SESSIONS = itertools.count()
QUERY = f"array_fill(0::real, ARRAY[{DIM}])::vector"
DISTANCE = f"v <-> {QUERY}"
INDEX_QUERY = (
    f"SELECT id, note, ctid::text, ({DISTANCE})::float8 FROM {TABLE} "
    f"ORDER BY {DISTANCE} LIMIT {K}"
)
HEAP_QUERY = (
    f"SELECT id, note, ctid::text, ({DISTANCE})::float8 FROM {TABLE} "
    f"ORDER BY ({DISTANCE}) + 0::float8, id LIMIT {K}"
)
PLAN_SETTINGS = ("indexscan", "indexonlyscan", "bitmapscan")


class Invalid(RuntimeError):
    """The intended fixture or execution gate could not be established."""


def require(condition, message):
    if not condition:
        raise Invalid(message)


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def emit(**record):
    print(json.dumps(record, sort_keys=True), flush=True)


class Session:
    def __init__(self, process, error_log):
        self.process = process
        self.error_log = error_log
        self.buffer = bytearray()
        self.marker = 0

    def send(self, sql):
        self.marker += 1
        marker = f"__resident_overlay_{self.marker}__"
        self.process.stdin.write(f"{sql.rstrip().rstrip(';')};\nSELECT '{marker}';\n".encode())
        return marker.encode()

    def receive(self, marker):
        deadline = time.monotonic() + 190
        while True:
            end = self.buffer.find(b"\n" + marker + b"\n")
            if end >= 0 or self.buffer.startswith(marker + b"\n"):
                length = end + 1
                result = self.buffer[:length].decode().strip()
                del self.buffer[:length + len(marker) + 1]
                return result
            seconds = deadline - time.monotonic()
            if seconds <= 0 or not select.select([self.process.stdout], [], [], seconds)[0]:
                raise Invalid(f"psql timed out waiting for {marker!r}: {self.error_log}")
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise Invalid(f"psql exited before {marker!r}: {self.error_log.read_text()}")
            self.buffer.extend(chunk)

    def sql(self, sql):
        return self.receive(self.send(sql))


@contextmanager
def session(cluster, label):
    error_log = cluster.logs / f"{cluster.data.parent.name}-{label}-{next(SESSIONS)}.stderr"
    with error_log.open("w") as errors, subprocess.Popen(
        ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", DB],
        env=cluster.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=errors, bufsize=0,
    ) as process:
        client = Session(process, error_log)
        try:
            client.sql(f"""
                SET statement_timeout = '180s';
                SET max_parallel_workers_per_gather = 0;
                SET cuvs.gpu_bruteforce = off;
                SET cuvs.cpu_hnsw_fallback = off;
                SET cuvs.search_mode = cagra;
                SET cuvs.k = {K}
            """)
            yield client
        finally:
            if process.poll() is None:
                process.stdin.close()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def poll_for(check, message):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.05)
    raise Invalid(message)


def begin(reader):
    return reader.sql("BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_current_snapshot()")


def rows(output):
    parsed = []
    for line in output.splitlines():
        row_id, note, tid, distance = line.split("|")
        parsed.append((int(row_id), note, tid, float(distance)))
    return parsed


def raw(client, block):
    return bytes.fromhex(client.sql(f"SELECT encode(get_raw_page('{INDEX}', {block}), 'hex')"))


def state(client):
    # PG16's MAXALIGN(8) metapage layout and ordinary-column pending page.
    meta = raw(client, 0)
    require(struct.unpack_from("=I", meta, 24)[0] == 0x43565047,
            "fixture does not have a CAGRA page-format metapage")
    nonce, base, graph, slots, folded, base_slots, _, dead = struct.unpack_from("=8Q", meta, 48)
    pending_block, = struct.unpack_from("=I", meta, 116)
    pending = raw(client, pending_block)
    special, = struct.unpack_from("=H", pending, 16)
    kind, flags, count, next_block, vector_dim, pending_nonce, first = struct.unpack_from(
        "=HHIIIQQ", pending, special
    )
    require((kind, flags, next_block, vector_dim, pending_nonce) == (2, 0, 0xffffffff, 0, nonce),
            f"unexpected pending tail page at block {pending_block}")
    tail = first + count
    require(graph and nonce and base and tail >= folded and slots == base_slots + folded,
            "invalid fixture graph identity or physical extent")
    return dict(index_nonce=nonce, base_nonce=base, graph_nonce=graph, folded=folded,
                tail=tail, dead=dead, n_slots=slots)


def setup(cluster, settings, nearest_base=False, base_rows=BASE_ROWS):
    with session(cluster, "setup") as client:
        client.sql("CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs; CREATE EXTENSION pageinspect")
        for name in settings:
            value = client.sql(f"SELECT setting FROM pg_settings WHERE name = '{name}'")
            require(value, f"unrecognized PostgreSQL setting {name}")
            emit(gate="pg-setting", name=name, value=value)
        first = "CASE WHEN i = 1 THEN 0 ELSE 10000 + i END" if nearest_base else "10000 + i"
        client.sql(f"""
            CREATE TABLE {TABLE} (id integer NOT NULL, v vector({DIM}) NOT NULL, note text NOT NULL)
                WITH (fillfactor=60, autovacuum_enabled=false);
            INSERT INTO {TABLE}
                SELECT i, (ARRAY[({first})::real] || array_fill(0::real, ARRAY[{DIM - 1}]))::vector, 'base'
                FROM generate_series(1, {base_rows}) AS i;
            CREATE INDEX {INDEX} ON {TABLE} USING cagra(v vector_l2_ops)
        """)
        before = state(client)
        require(before["tail"] == before["folded"] == 0 and before["n_slots"] == base_rows,
                f"unexpected initial extent {before}")


def insert(client, row_id, x, note="pending"):
    client.sql(f"""
        INSERT INTO {TABLE}
        VALUES ({row_id}, (ARRAY[{x}::real] || array_fill(0::real, ARRAY[{DIM - 1}]))::vector, '{note}')
    """)


def insert_range(client, first, count, x):
    client.sql(f"""
        INSERT INTO {TABLE}
            SELECT {first} + i, (ARRAY[({x} + i)::real] || array_fill(0::real, ARRAY[{DIM - 1}]))::vector,
                   'pending'
            FROM generate_series(0, {count - 1}) AS i
    """)


def stats(client, reset):
    return json.loads(client.sql(
        f"SELECT pg_temp.page_stats('{INDEX}', {'true' if reset else 'false'})"
    ))


def install_trace(client):
    client.sql("""
        CREATE OR REPLACE FUNCTION pg_temp.page_stats(regclass, boolean) RETURNS jsonb
        AS '$libdir/pg_cuvs', 'pg_cuvs_test_page_exact_stats' LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE
    """)


def start_probe(reader, label, ids, trace):
    snapshot = reader.sql("SELECT pg_current_snapshot()")
    reader.sql("SET LOCAL enable_seqscan=on; " +
               "; ".join(f"SET LOCAL enable_{setting}=off" for setting in PLAN_SETTINGS))
    heap_plan = json.loads(reader.sql("EXPLAIN (FORMAT JSON, COSTS OFF) " + HEAP_QUERY))[0]["Plan"]
    sort, = heap_plan.get("Plans", [{}])
    heap_scan, = sort.get("Plans", [{}])
    require(heap_plan["Node Type"] == "Limit" and sort.get("Node Type") == "Sort" and
            heap_scan.get("Node Type") == "Seq Scan" and heap_scan.get("Relation Name") == TABLE,
            f"{label}: wrong heap plan {heap_plan}")
    oracle = rows(reader.sql(HEAP_QUERY))
    require([row[0] for row in oracle] == ids and
            all(a[3] < b[3] for a, b in zip(oracle, oracle[1:])),
            f"{label}: heap fixture did not have the specified unique ranks {oracle}")

    reader.sql("SET LOCAL enable_seqscan=off; " +
               "; ".join(f"SET LOCAL enable_{setting}=on" for setting in PLAN_SETTINGS))
    if trace:
        install_trace(reader)
        stats(reader, True)
    index_plan = json.loads(reader.sql("EXPLAIN (FORMAT JSON, COSTS OFF) " + INDEX_QUERY))[0]["Plan"]
    node, = index_plan.get("Plans", [{}])
    require(index_plan["Node Type"] == "Limit" and node.get("Node Type") == "Index Scan" and
            node.get("Index Name") == INDEX and node.get("Relation Name") == TABLE and
            "<->" in str(node.get("Order By", "")), f"{label}: wrong KNN plan {index_plan}")
    emit(gate="plans", probe=label, heap_plan=heap_plan, index_plan=index_plan)
    return snapshot, oracle, reader.send(INDEX_QUERY)


def finish_probe(reader, label, pending, trace, *, exact=False, reason=None):
    snapshot, oracle, marker = pending
    try:
        actual = rows(reader.receive(marker))
    except Invalid as err:
        raise AssertionError(f"{label}: KNN execution failed: {err}") from err
    n_results, last_oid, index_oid = json.loads(reader.sql(f"""
        SELECT json_build_array(pg_cuvs_last_search_n_results(),
                                pg_cuvs_last_search_index(), '{INDEX}'::regclass::oid)
    """))
    sample = stats(reader, False) if trace else {}
    emit(probe=label, snapshot=snapshot, index_oid=index_oid, index_scan=actual, heap=oracle,
         last_n_results=n_results, last_index_oid=last_oid, stats=sample)
    require(reader.sql("SELECT pg_current_snapshot()") == snapshot,
            f"{label}: heap and index statements did not share a snapshot")
    expect(actual == oracle, f"{label}: index rank differs from heap rank")
    expect((n_results is None and last_oid is None) if exact else last_oid == index_oid and
           n_results is not None, f"{label}: expected {'exact heap' if exact else 'daemon'} route")
    if trace:
        require(sample.get("scan_starts") == 1, f"{label}: missing one-scan executor witness")
        expect(sample.get("exact_entries") == int(exact), f"{label}: unexpected exact-entry count")
        if exact:
            require(reason in sample.get("reasons", {}), f"{label}: missing reason {reason}")
            expect(sample["reasons"][reason] == 1, f"{label}: unexpected exact-entry reason")
        else:
            witness = sample.get("last_page_search")
            require(isinstance(witness, dict) and witness.get("scan_start") == sample["scan_starts"],
                    f"{label}: missing search witness for this scan")
            return witness
    return None


def probe(reader, label, ids, trace, **route):
    return finish_probe(reader, label, start_probe(reader, label, ids, trace), trace, **route)


def fresh(cluster, label, ids, trace, **route):
    with session(cluster, label) as reader:
        begin(reader)
        result = probe(reader, label, ids, trace, **route)
        reader.sql("ROLLBACK")
        return result


def extent(label, witness, physical, covered, wire, local, *, resident=None):
    resident = covered - physical["folded"] if resident is None else resident
    expected = {key: physical[key] for key in ("index_nonce", "base_nonce", "graph_nonce", "folded", "tail")}
    expected.update(covered_upto=covered, resident_positions=resident,
                    wire_positions=wire, local_positions=local)
    fields = (*expected, "postmaster_boot_id", "daemon_id")
    require(all(witness.get(key) is not None for key in fields),
            f"{label}: missing range/identity witness fields {fields}")
    expect(all(int(witness[key]) == value for key, value in expected.items()),
           f"{label}: range/identity mismatch, expected {expected}, actual {witness}")
    expect(int(witness["daemon_id"]) > 0 and int(witness["postmaster_boot_id"]) > 0,
           f"{label}: invalid daemon or boot identity")


def check_heap_visibility(reader, row_id, visible):
    value = reader.sql(f"SELECT count(*) FROM {TABLE} WHERE id = {row_id}")
    require(value == str(int(visible)), f"row {row_id} has unexpected visibility {value}")


def check_hot(client, row_id, parent):
    child = client.sql(f"SELECT ctid FROM {TABLE} WHERE id = {row_id}")
    block, root_off = map(int, parent.strip("()").split(","))
    child_block, child_off = map(int, child.strip("()").split(","))
    require(child_block == block and child_off != root_off, "tuple update did not stay on root page")
    found = client.sql(f"""
        SELECT count(*) FROM heap_page_items(get_raw_page('{TABLE}', {block}))
         WHERE (lp = {root_off} AND t_ctid = '{child}'::tid AND (t_infomask2 & 16384) <> 0)
            OR (lp = {child_off} AND (t_infomask2 & 32768) <> 0)
    """)
    require(found == "2", "pageinspect did not find both HOT flags and the root link")
    emit(gate="hot", row_id=row_id, root=parent, child=child)
    return child


def supply(writer, target, physical):
    # Each invocation uses the product sender in a fresh statement snapshot.
    writer.sql("""
        CREATE OR REPLACE FUNCTION pg_temp.supply_step(regclass, bigint) RETURNS jsonb
        AS '$libdir/pg_cuvs', 'pg_cuvs_test_page_supply_step' LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE
    """)
    prior = None
    for _ in range((target - physical["folded"] + 4095) // 4096 + 1):
        result = json.loads(writer.sql(f"SELECT pg_temp.supply_step('{INDEX}', {target})"))
        fields = ("index_nonce", "base_nonce", "graph_nonce", "folded", "from", "covered_upto",
                  "postmaster_boot_id", "daemon_id", "status")
        require(all(result.get(key) is not None for key in fields),
                f"missing supply-step witness fields {fields}")
        if prior is None or result["status"] != "ok" or int(result["covered_upto"]) == target:
            emit(gate="supply", target=target, step=result)
        expect(result["status"] == "ok", f"supply step failed: {result}")
        expect(all(int(result[key]) == physical[key] for key in fields[:4]),
               f"supply used a different graph: {result}")
        start, end = int(result["from"]), int(result["covered_upto"])
        expect(physical["folded"] <= start <= end <= target and end - start <= 4096,
               f"supply did not respect contiguous or bounded ranges: {result}")
        if prior is not None:
            expect(start == int(prior["covered_upto"]) and end > start and
                   all(result[key] == prior[key] for key in ("postmaster_boot_id", "daemon_id")),
                   f"supply failed to advance continuously on the same identity: {result}")
        if end == target:
            require(state(writer) == physical, "supply unexpectedly changed the metapage or tail")
            return result
        prior = result
    raise AssertionError(f"supply did not reach {target}: {prior}")


def rank_red(cluster, trace):
    with session(cluster, "write") as writer:
        insert_range(writer, 10001, 3, 1)
        fresh(cluster, "rank-control", [1, 10001, 10002], trace)
        writer.sql(f"DELETE FROM {TABLE} WHERE id=1")
        physical = state(writer)
        require(physical["dead"] == 0 and physical["tail"] - physical["folded"] == 3,
                f"rank test must keep a logically deleted base candidate: {physical}")
        fresh(cluster, "rank-after-delete", [10001, 10002, 10003], trace)


def cold_boundary(cluster, trace):
    with session(cluster, "write") as writer, session(cluster, "hold-prefix") as blocker:
        begin(blocker)
        insert(blocker, 9000, 0, "active")
        check_heap_visibility(writer, 9000, False)
        insert_range(writer, 10001, 4094, 1)
        for count in (4095, 4096, 4097):
            if count > 4095:
                insert(writer, 20000 + count, count)
            physical = state(writer)
            require(physical["folded"] == 0 and physical["tail"] == count,
                    f"cold boundary moved: {physical}")
            emit(gate="physical-gap", pending=count, physical=physical)
            witness = fresh(cluster, f"cold-{count}", [10001, 10002, 10003], trace)
            if trace:
                wire = count if count <= 4096 else 0
                extent(f"cold-{count}", witness, physical, 0, wire, count - wire)
        blocker.sql("ROLLBACK")


def base_filter(cluster, trace):
    with session(cluster, "write") as writer:
        fresh(cluster, "base-filter-control", [1, 2, 3], trace)
        writer.sql(f"DELETE FROM {TABLE} WHERE id <= 4100")
        insert(writer, 10001, 0.5)
        physical = state(writer)
        require(physical["n_slots"] == 4352 and physical["dead"] == 0 and physical["tail"] == 1,
                f"fixture must keep the invisible base slots in its graph: {physical}")
        witness = fresh(cluster, "base-filter-after-delete", [10001, 4101, 4102], trace)
        expect(witness.get("base_visibility_filter") is True,
               f"search did not reach its snapshot-filtered base search: {witness}")


def base_filter_hold(cluster, trace):
    with session(cluster, "write") as writer, session(cluster, "held-filter") as reader:
        insert_range(writer, 10001, 1300, 1)
        physical = state(writer)
        require(physical["n_slots"] == 4352 and physical["tail"] == 1300,
                f"base-filter lease fixture moved: {physical}")
        supply(writer, 1300, physical)
        writer.sql(f"DELETE FROM {TABLE} WHERE id <= 2100 OR id BETWEEN 10001 AND 11299")
        begin(reader)
        reader.sql("SET LOCAL cuvs.k=2000; SET LOCAL cuvs.test_resident_filter_pause_ms=6500")
        started = time.monotonic()
        witness = probe(reader, "base-filter-held-over-idle-limit", [11300, 2101, 2102], trace)
        expect(time.monotonic() - started >= 6,
               "the snapshot filter did not stay open beyond the normal five-second idle wait")
        expect(witness.get("base_visibility_filter") is True and witness["candidate_windows"] > 1,
               f"base-filter lease did not cover both streams: {witness}")
        reader.sql("ROLLBACK")


def covered_boundary(cluster, trace):
    with session(cluster, "write") as writer, session(cluster, "hold-suffix") as blocker:
        insert(writer, 10001, 1)
        insert_range(writer, 10002, 63, 3)
        supply(writer, 64, state(writer))
        begin(blocker)
        insert(blocker, 9000, 0, "active")
        check_heap_visibility(writer, 9000, False)
        insert(writer, 20001, 2)
        insert_range(writer, 21001, 4093, 1000)
        for gap in (4095, 4096, 4097):
            if gap > 4095:
                insert(writer, 30000 + gap, 6000 + gap)
            physical = state(writer)
            require(physical["folded"] == 0 and physical["tail"] == 64 + gap,
                    f"resident-plus-suffix fixture moved: {physical}")
            label = f"resident64-gap{gap}"
            witness = fresh(cluster, label, [10001, 20001, 10002], trace)
            wire = gap if gap <= 4096 else 0
            extent(label, witness, physical, 64, wire, gap - wire)
        blocker.sql("ROLLBACK")


def capacity(cluster, trace):
    with session(cluster, "write") as writer:
        insert(writer, 10001, 1)
        insert(writer, 10002, 3)
        insert(writer, 10003, 5)
        insert_range(writer, 10004, 199996, 1000)
        physical = state(writer)
        require(physical["folded"] == 0 and physical["tail"] == 199999,
                f"capacity fixture moved: {physical}")
        supply(writer, 199999, physical)
        witness = fresh(cluster, "capacity-199999", [10001, 10002, 10003], trace)
        extent("capacity-199999", witness, physical, 199999, 0, 0)

        begin(writer)
        insert(writer, 300001, 2, "active")
        physical = state(writer)
        require(physical["folded"] == 0 and physical["tail"] == 200000,
                f"capacity fixture moved: {physical}")
        witness = probe(writer, "capacity-200000", [10001, 300001, 10002], trace)
        extent("capacity-200000", witness, physical, 199999, 1, 0)
        writer.sql("COMMIT")

        insert(writer, 300002, 0.5)
        physical = state(writer)
        require(physical["folded"] == 0 and physical["tail"] == 200001,
                f"over-capacity fixture moved: {physical}")
        emit(gate="physical-capacity", pending=200001, physical=physical)
        fresh(cluster, "capacity-200001", [300002, 10001, 300001], trace,
              exact=True, reason="page_overlay_capacity")


def refilled(label, witness):
    fields = ("first_resident_candidates", "first_resident_visible", "candidate_windows")
    require(all(witness.get(key) is not None for key in fields),
            f"{label}: missing refill witness fields {fields}")
    expect(witness["first_resident_candidates"] > 0 and witness["first_resident_visible"] == 0 and
           witness["candidate_windows"] > 1,
           f"{label}: did not exhaust an invisible first resident window and refill: {witness}")


def mvcc(cluster, trace):
    with session(cluster, "write") as writer, session(cluster, "old") as old:
        for row_id, x in ((12000, 100), (12001, 101), (12002, 103), (12003, 105)):
            insert(writer, row_id, x)
        begin(old)
        parent = old.sql(f"SELECT ctid FROM {TABLE} WHERE id=12001")
        check_heap_visibility(old, 12000, True)
        insert_range(writer, 13001, 64, 1)
        physical = state(writer)
        require(physical["folded"] == 0 and physical["tail"] == 68,
                f"MVCC resident prefix moved: {physical}")
        supply(writer, 68, physical)
        writer.sql(f"""
            DELETE FROM {TABLE} WHERE id = 12000 OR id BETWEEN 13001 AND 13064;
            UPDATE {TABLE} SET note='hot' WHERE id=12001
        """)
        check_hot(writer, 12001, parent)
        check_heap_visibility(old, 13001, False)
        check_heap_visibility(old, 12000, True)
        require(old.sql(f"SELECT ctid FROM {TABLE} WHERE id=12001") == parent,
                "old snapshot did not retain the HOT parent")

        witness = probe(old, "mvcc-before-insert-and-delete", [12000, 12001, 12002], trace)
        extent("mvcc-before-insert-and-delete", witness, physical, 68, 0, 0)
        refilled("mvcc-before-insert-and-delete", witness)
        witness = fresh(cluster, "deleted-resident-refill", [12001, 12002, 12003], trace)
        extent("deleted-resident-refill", witness, physical, 68, 0, 0)
        refilled("deleted-resident-refill", witness)

        begin(writer)
        insert(writer, 30000, 0, "active")
        xid = int(writer.sql("SELECT pg_current_xact_id()"))
        physical = state(writer)
        require(physical["tail"] == 69, f"active INSERT did not append the expected position: {physical}")
        with session(cluster, "reader-during-insert") as other:
            begin(other)
            check_heap_visibility(other, 30000, False)
            require(other.sql(f"SELECT pg_visible_in_snapshot('{xid}'::xid8, pg_current_snapshot())") == "f",
                    "the writer XID was visible to the concurrent reader")
            witness = probe(other, "mvcc-other-active-writer", [12001, 12002, 12003], trace)
            extent("mvcc-other-active-writer", witness, physical, 68, 1, 0)
            other.sql("ROLLBACK")
        check_heap_visibility(writer, 30000, True)
        witness = probe(writer, "mvcc-own-active-write", [30000, 12001, 12002], trace)
        extent("mvcc-own-active-write", witness, physical, 68, 1, 0)
        writer.sql("ROLLBACK")
        old.sql("ROLLBACK")

        check_heap_visibility(writer, 30000, False)
        require(state(writer) == physical, "ROLLBACK did not preserve the physical pending position")
        insert(writer, 30001, 0.5, "after_rollback")
        physical = state(writer)
        require(physical["tail"] == 70, f"committed row did not append past the aborted position: {physical}")
        step = supply(writer, 70, physical)
        expect(int(step["from"]) == 68, "supply did not cross the aborted position")
        witness = fresh(cluster, "mvcc-after-rollback", [30001, 12001, 12002], trace)
        extent("mvcc-after-rollback", witness, physical, 70, 0, 0)

        writer.sql(f"DELETE FROM {TABLE} WHERE id <> 30001")
        witness = fresh(cluster, "mvcc-candidate-exhaustion", [30001], trace)
        extent("mvcc-candidate-exhaustion", witness, physical, 70, 0, 0)
        require("candidates_exhausted" in witness, "missing candidate exhaustion witness")
        expect(witness["candidates_exhausted"] is True, "returned fewer than k without exhaustion")


@contextmanager
def search_gate(path):
    os.mkfifo(path)

    def open_writer():
        try:
            return os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as err:
            if err.errno != errno.ENXIO:
                raise
            return None

    try:
        yield open_writer
    finally:
        path.unlink(missing_ok=True)


def compact(writer):
    writer.sql(f"SELECT pg_cuvs_compact('{INDEX}')")


def mixed_fold_ready(cluster, trace):
    """Use a resident PG module with a native daemon that lacks its READY frame."""
    log = cluster.logs / "daemon-resident.log"
    with session(cluster, "mixed-fold-write") as writer:
        insert_range(writer, 40000, 128, 1)
        parent = state(writer)
        supply(writer, 128, parent)
        poll_for(lambda: any("[page_fold]" in line and "offered=1" in line
                             for line in log.read_text().splitlines()),
                 "the older daemon did not offer a native fold")
        compact(writer)
        expect(state(writer) == parent, "the new PG worker published an unacknowledgeable fold")
        expect("[page_fold_commit]" not in log.read_text(), "the older daemon received a fold ACK")
        emit(gate="older-daemon-cannot-publish-resident-fold", physical=parent)


def short_fold_ack(cluster, trace):
    """Use a PG module that sends a short resident ACK after the legacy READY."""
    log = cluster.logs / "daemon-resident.log"
    with session(cluster, "short-ack-write") as writer:
        insert_range(writer, 40000, 128, 1)
        parent = state(writer)
        supply(writer, 128, parent)
        poll_for(lambda: any("[page_fold]" in line and "offered=1" in line
                             for line in log.read_text().splitlines()),
                 "the daemon did not offer a native fold")
        compact(writer)
        child = state(writer)
        expect(child["folded"] == 128 and child["graph_nonce"] != parent["graph_nonce"],
               "the short ACK fixture did not publish the fold")
        expect("[page_fold_commit]" in log.read_text(), "the daemon did not accept the short ACK")
        emit(gate="short-resident-ack-accepted", physical=child)


def fold(cluster, trace, daemon, phase):
    gate = cluster.data.parent / f"{phase}.fifo"
    log = cluster.logs / "daemon-resident.log"
    parent_ids = [1, 2, 3]
    child_ids = [41000, 40000, 41001]
    with session(cluster, "write") as writer, session(cluster, "old") as old:
        begin(old)
        probe(old, "parent-baseline", parent_ids, trace)
        index_oid = int(writer.sql(f"SELECT '{INDEX}'::regclass::oid"))
        old_pid = int(old.sql("SELECT pg_backend_pid()"))

        def events(name):
            return [
                dict(re.findall(r"(\w+)=([0-9A-Fa-f]+)(?:\s|$)", line))
                for line in log.read_text().splitlines()
                if f"[{name}]" in line and re.search(rf"\bindex={index_oid}\b", line)
            ]

        visible_prefix = 0
        if phase == "next":
            old.sql("ROLLBACK")
            # These rows sort behind the invisible first window, but ahead of all base rows.
            insert_range(writer, 39000, K, 200)
            parent_ids = list(range(39000, 39000 + K))
            visible_prefix = K
            begin(old)
        insert_range(writer, 40000, 128 - visible_prefix, 1)
        compact(writer)
        poll_for(lambda: any(event.get("positions") == "128" and event.get("status") == "0" and
                             event.get("offered") == "1" for event in events("page_fold")),
                 "daemon never offered the 128-row native fold")
        for row_id, x in ((41000, 0.5), (41001, 1.5), (41002, 2.5)):
            insert(writer, row_id, x)
        parent = state(writer)
        require(parent["folded"] == 0 and parent["tail"] == 131, f"wrong parent extent: {parent}")
        if phase == "receive":
            # The held pre-publication snapshot must rank rows carried by the wire supplement.
            old.sql("ROLLBACK")
            begin(old)
            parent_ids = child_ids
        check_heap_visibility(old, 40000, phase == "receive")
        check_heap_visibility(old, 41000, phase == "receive")

        descriptor = None
        try:
            started = time.monotonic()
            with search_gate(gate) as open_writer:
                request = start_probe(old, "parent-in-flight", parent_ids, trace)

                def opened():
                    nonlocal descriptor
                    descriptor = open_writer()
                    return descriptor is not None

                poll_for(opened, f"daemon search did not reach the resident {phase} FIFO")
            # The open writer still holds this request after the FIFO pathname is unlinked.
            emit(gate=f"{phase}-fifo-open", index_oid=index_oid, graph_nonce=parent["graph_nonce"])
            if phase != "score":
                require(state(writer) == parent and not events("page_fold_ack_drop") and
                        not events("page_fold_commit"), "parent was already published at FIFO entry")
                emit(gate="parent-before-publication", parent=parent, snapshot=request[0], pid=old_pid)
            compact(writer)
            child = state(writer)
            dropped = events("page_fold_ack_drop")
            require(len(dropped) == 1 and not events("page_fold_commit"),
                    f"search was not bracketed by publication and ACK: {dropped}")
            require(child["graph_nonce"] != parent["graph_nonce"] and
                    child["index_nonce"] == parent["index_nonce"] and
                    child["base_nonce"] == parent["base_nonce"] and
                    child["folded"] == 128 and child["tail"] == 131 and
                    int(dropped[0].get("graph_nonce", "0")) == child["graph_nonce"],
                    f"native publication did not produce the expected child {child}, event {dropped}")
            emit(gate="published-no-ack", child=child, event=dropped[0])
            witness = fresh(cluster, "child-before-ack", child_ids, trace)
            extent("child-before-ack", witness, child, 128, 3, 0)
            if phase != "score":
                require(not events("page_fold_commit"), "child search did not complete before ACK")
            compact(writer)
            committed = events("page_fold_commit")
            event_keys = ("db", "index", "lsn", "daemon_id", "boot_id", "index_nonce", "graph_nonce")
            require(len(committed) == 1 and
                    all(committed[0].get(key) and committed[0][key] == dropped[0].get(key)
                        for key in event_keys) and state(writer) == child,
                    f"ACK did not complete the previously published child: {committed}")
            if phase != "score":
                insert(writer, 42000, 0, "after_ack")
                advanced = state(writer)
                require(advanced == {**child, "tail": child["tail"] + 1},
                        f"expected one append after ACK while the parent was held: {advanced}")
            require(time.monotonic() - started < 25 and writer.sql(f"""
                SELECT count(*) FROM pg_stat_activity
                WHERE pid = {old_pid} AND state = 'active' AND query LIKE 'SELECT id, note, ctid%'
            """) == "1", "parent KNN was not still active within its IPC deadline at ACK")
            emit(gate=f"ack-while-{phase}-held", event=committed[0])
            if phase != "score":
                emit(gate="tail-after-ack", parent_tail=parent["tail"], child=advanced)
        finally:
            if descriptor is not None:
                os.close(descriptor)

        witness = finish_probe(old, "parent-in-flight", request, trace)
        extent("parent-in-flight", witness, parent, 128, 3, 0)
        for field, key in (("daemon_id", "daemon_id"), ("postmaster_boot_id", "boot_id")):
            expect(int(witness[field]) == int(committed[0][key]), f"pinned request lost its {field}")

        if phase != "score":
            if phase == "next":
                refilled("parent-in-flight", witness)
            check_heap_visibility(old, 42000, False)
            old.sql("ROLLBACK")
            witness = fresh(cluster, "child-after-ack", [42000, 41000, 40000], trace)
            extent("child-after-ack", witness, advanced, 128, 4, 0)
            return

        witness = probe(old, "parent-after-ack", [1, 2, 3], trace)
        extent("parent-after-ack", witness, parent, 128, 3, 0)
        old.sql("ROLLBACK")
        suffix = supply(writer, 131, child)
        expect(int(suffix["from"]) == 128, "child did not supply the remaining suffix")
        warm = fresh(cluster, "child-warm-before-restart", [41000, 40000, 41001], trace)
        extent("child-warm-before-restart", warm, child, 131, 0, 0)
        postmaster_before = writer.sql("SELECT pg_postmaster_start_time()")

    cluster.stop("immediate")
    cluster.start("restart")
    require(daemon.poll() is None, "daemon did not survive the PostgreSQL-only restart")
    with session(cluster, "restart") as writer:
        postmaster_after = writer.sql("SELECT pg_postmaster_start_time()")
        require(postmaster_after != postmaster_before and state(writer) == child,
                "new postmaster did not recover the same published index and pending tail")
        emit(gate="new-postmaster-same-index", before=postmaster_before, after=postmaster_after,
             child=child)
        cold = fresh(cluster, "child-cold-after-restart", [41000, 40000, 41001], trace)
        extent("child-cold-after-restart", cold, child, 128, 3, 0)
        expect(int(cold["postmaster_boot_id"]) != int(warm["postmaster_boot_id"]) and
               int(cold["daemon_id"]) == int(warm["daemon_id"]),
               "restart did not change only the PostgreSQL boot identity")
        suffix = supply(writer, 131, child)
        expect(int(suffix["from"]) == 128, "supply reused the previous postmaster's suffix")
        witness = fresh(cluster, "child-repopulated-after-restart", [41000, 40000, 41001], trace)
        extent("child-repopulated-after-restart", witness, child, 131, 0, 0)
        expect(all(int(witness[key]) == int(cold[key])
                   for key in ("postmaster_boot_id", "daemon_id")),
               "repopulated suffix changed daemon or PostgreSQL identity")


CASES = {
    "base-filter": base_filter,
    "base-filter-hold": base_filter_hold,
    "mixed-fold-ready": mixed_fold_ready,
    "short-fold-ack": short_fold_ack,
    "rank-red": rank_red,
    "cold-boundary": cold_boundary,
    "covered-boundary": covered_boundary,
    "capacity": capacity,
    "mvcc": mvcc,
}

FOLD_PHASES = {
    "fold": "score",
    "fold-receive": "receive",
    "fold-continuation": "next",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pg_config", type=Path)
    parser.add_argument("daemon", type=Path)
    parser.add_argument("logs", type=Path)
    parser.add_argument("--case", choices=(*CASES, *FOLD_PHASES), required=True)
    parser.add_argument("--trace", action="store_true", help="require candidate executor diagnostics")
    parser.add_argument("--pg-setting", action="append", default=[], metavar="CUVS.NAME=VALUE")
    args = parser.parse_args()
    settings = {}
    for setting in args.pg_setting:
        if not re.fullmatch(r"cuvs\.[a-z_][a-z_0-9]*=[a-zA-Z0-9_.-]+", setting):
            parser.error(f"invalid --pg-setting {setting!r}")
        name, value = setting.split("=")
        settings[name] = value

    trace = args.trace or args.case not in ("rank-red", "cold-boundary")
    logs = args.logs.resolve()
    logs.mkdir(parents=True, exist_ok=True)
    try:
        bindir = subprocess.check_output([args.pg_config, "--bindir"], text=True).strip()
        with tempfile.TemporaryDirectory(prefix="resident-", dir="/tmp") as work:
            cluster = Cluster(Path(work) / "pg", bindir, logs)
            with (cluster.data / "postgresql.conf").open("a") as config:
                for name, value in settings.items():
                    config.write(f"{name} = '{value}'\n")
            gate_dir = None
            phase = FOLD_PHASES.get(args.case)
            next_fifo = "CUVS_TEST_PAGE_OVERLAY_NEXT_FIFO"
            cluster.env.pop(next_fifo, None)
            if phase:
                cluster.env["CUVS_FAULT_PAGE_FOLD_ACK_ONCE"] = "1"
                gate_dir = cluster.data.parent
                if phase == "next":
                    cluster.env[next_fifo] = str(gate_dir / "next.fifo")
            with cluster.daemon(args.daemon.resolve(), "resident", overlay_host_bytes=128 << 20,
                                overlay_gate_dir=gate_dir) as daemon:
                cluster.start()
                try:
                    setup(cluster, settings, nearest_base=args.case == "rank-red",
                          base_rows=4352 if args.case in ("base-filter", "base-filter-hold") else BASE_ROWS)
                    if phase:
                        fold(cluster, trace, daemon, phase)
                    else:
                        CASES[args.case](cluster, trace)
                finally:
                    cluster.stop()
    except AssertionError as err:
        emit(case=args.case, status="FAIL", message=str(err))
        return 1
    except (RuntimeError, OSError, ValueError, struct.error, subprocess.SubprocessError) as err:
        emit(case=args.case, status="INVALID", message=str(err))
        return 2
    emit(case=args.case, status="PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

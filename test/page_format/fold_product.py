"""Exercise the native, manually launched page-fold product path on Linux.

Use a disposable PostgreSQL installation with the native fold extension.
The existing ACK-loss case requires pg_cuvs_server_test. The batch cases and
the original-graph case also work with the production daemon. Run the latter
on an A100 with the matching cuVS SDK, whose AUTO selects SINGLE_CTA for
512 queries at itopk_size=64.
"""

import argparse
from itertools import groupby
from pathlib import Path
import re
import select
import struct
import subprocess
import tempfile
import time

from run import ROOT, Cluster, fold_query, fold_raw_page
from fold_gate_log import publication_windows, read_records, requests_from


BATCH_INDEX = "fold_batch_cagra"
BASE_ROWS = 512
BATCH_ROWS = 64
PENDING_ROWS = 105
CACHE_BATCH_ROWS = 16
CACHE_HOST_BUDGET = 256 << 10
DAEMON_FOLD_ENV = ("CUVS_PAGE_FOLD_BATCH_ROWS", "CUVS_PAGE_FOLD_PROFILE", "CUVS_FAULT_PAGE_FOLD_ACK_ONCE")


def compact(cluster, db, index, label):
    cluster.sql(db, label, "-qAt", "-c", f"SELECT pg_cuvs_compact('{index}')")
    return (cluster.logs / f"{cluster.data.parent.name}-{label}.log").read_text()


def old_snapshot(cluster, db):
    return subprocess.Popen(
        ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", db], env=cluster.env,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )


def read_disk_page(cluster, filename, block_size, block):
    with (cluster.data / filename).open("rb") as relation:
        relation.seek(block_size * block)
        return relation.read(block_size)


def check_dead_slots_after_ready(cluster, db, index, before):
    # The daemon offered the candidate before these formerly live vectors
    # became physical dead slots. They are closest to the probe in the GPU
    # graph, including both places the old k=1 overfetch would ask for.
    cluster.sql(db, "delete-after-ready", "-c", "DELETE FROM fold_second WHERE id BETWEEN 637 AND 640")
    cluster.sql(db, "vacuum-after-ready", "-c", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) fold_second")
    pending_meta = fold_raw_page(cluster, db, 0, index)
    if pending_meta[64:88] != before[64:88]:
        raise RuntimeError("VACUUM changed the graph head ahead of fold publication")
    if "CAGRA fold published" not in compact(cluster, db, index, "publish-after-vacuum"):
        raise RuntimeError("VACUUM did not leave the ready candidate publishable")

    meta = fold_raw_page(cluster, db, 0, index)
    slots, folded, _, _, dead = struct.unpack_from("=5Q", meta, 72)
    if meta[64:72] == before[64:72] or (slots, folded, dead) != (640, 128, 68):
        raise RuntimeError(f"unexpected published dead-slot state: {(slots, folded, dead)}")
    block, = struct.unpack_from("=I", meta, 160)
    page = fold_raw_page(cluster, db, block, index)
    if any(struct.unpack_from("=3H", page, 24 + slot * 6) != (65535, 65535, 0)
           for slot in range(636, 640)):
        raise RuntimeError("the four post-READY deletes remained valid TIDs in the graph slots")

    cluster.sql(db, "search-after-vacuum-fold", "-qAt", "-c", """
        SET enable_seqscan=off;
        SET cuvs.k=1;
        SELECT id FROM fold_second ORDER BY v <-> array_fill(640::real,ARRAY[32])::vector LIMIT 1;
        SELECT pg_cuvs_last_search_k();
    """)
    output = cluster.logs / f"{cluster.data.parent.name}-search-after-vacuum-fold.log"
    if output.read_text().splitlines()[-2:] != ["636", "69"]:
        raise RuntimeError("the folded GPU search did not compensate for every physical dead slot")
    return meta


def check_vacuum_undercount(cluster, db, index, meta):
    cluster.sql(db, "delete-before-cancelled-vacuum", "-c", "DELETE FROM fold_second WHERE id BETWEEN 400 AND 636")
    search = """
        SET enable_seqscan=off;
        SET cuvs.k=1;
        SELECT coalesce((SELECT id FROM fold_second
            ORDER BY v <-> array_fill(630::real,ARRAY[32])::vector LIMIT 1)::text, 'none');
        SELECT coalesce(pg_cuvs_last_search_k()::text, 'exact');
    """

    def expect_search(label, expected):
        cluster.sql(db, label, "-qAt", "-c", search)
        result = (cluster.logs / f"{cluster.data.parent.name}-{label}.log").read_text().splitlines()[-2:]
        if result != expected:
            raise RuntimeError(f"{label}: got {result}, expected {expected}")

    expect_search("invisible-before-vacuum", ["none", "69"])
    log_offset = cluster.log.stat().st_size
    vacuum_log = cluster.logs / "fold-paused-vacuum.log"
    with vacuum_log.open("w") as output, subprocess.Popen(
        ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", db,
         "-c", "SET cuvs.test_pause_page_vacuum=on;",
         "-c", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) fold_second;"],
        env={**cluster.env, "PGAPPNAME": "fold-undercount-vacuum"},
        stdout=output, stderr=subprocess.STDOUT,
    ) as vacuum:
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and vacuum.poll() is None:
                with cluster.log.open("rb") as log:
                    log.seek(log_offset)
                    if b"page-format VACUUM paused before publishing its generation" in log.read():
                        break
                time.sleep(0.05)
            else:
                raise RuntimeError(f"VACUUM did not pause: {vacuum_log.read_text()}")
            if fold_raw_page(cluster, db, 0, index) != meta:
                raise RuntimeError("VACUUM published the dead-slot count ahead of the pause")
            block, = struct.unpack_from("=I", meta, 160)
            tid_page = fold_raw_page(cluster, db, block, index)
            if struct.unpack_from("=3H", tid_page, 24 + 629 * 6) != (65535, 65535, 0):
                raise RuntimeError("VACUUM did not persist the physical invalidation before its pause")
            expect_search("physical-undercount-paused", ["399", "exact"])
            cancelled = fold_query(cluster, db, "cancel-undercount-vacuum", """
                SELECT pg_cancel_backend(pid) FROM pg_stat_activity
                 WHERE application_name = 'fold-undercount-vacuum';
            """)
            if cancelled != "t" or vacuum.wait(timeout=10) == 0 or "canceling statement" not in vacuum_log.read_text():
                raise RuntimeError(f"VACUUM was not cancelled at the pause: {vacuum_log.read_text()}")
        finally:
            if vacuum.poll() is None:
                vacuum.kill()
    if fold_raw_page(cluster, db, 0, index) != meta:
        raise RuntimeError("cancelled VACUUM published the dead-slot count")
    expect_search("physical-undercount-cancelled", ["399", "exact"])


def check(cluster, logs, daemon):
    db = "postgres"
    cluster.sql(db, "fold-product-setup", "-c", """
        CREATE EXTENSION vector;
        CREATE EXTENSION pg_cuvs;
        CREATE EXTENSION pageinspect;
        CREATE TABLE fold_first(id integer PRIMARY KEY, v vector(32));
        CREATE TABLE fold_second(LIKE fold_first INCLUDING ALL);
        INSERT INTO fold_first
            SELECT i, array_fill(i::real, ARRAY[32])::vector FROM generate_series(1,512) i;
        INSERT INTO fold_second SELECT * FROM fold_first;
        CREATE INDEX fold_first_cagra ON fold_first USING cagra(v vector_l2_ops);
        CREATE INDEX fold_second_cagra ON fold_second USING cagra(v vector_l2_ops);
        CREATE TABLE fold_empty(id integer, v vector(32));
        CREATE INDEX fold_empty_cagra ON fold_empty USING cagra(v vector_l2_ops);
        INSERT INTO fold_empty VALUES (1, array_fill(1::real, ARRAY[32])::vector);
        CREATE ROLE fold_nonowner LOGIN;
        GRANT EXECUTE ON FUNCTION pg_cuvs_compact(regclass) TO fold_nonowner;
    """)
    indexes = ("fold_first_cagra", "fold_second_cagra")
    oids = [int(fold_query(cluster, db, f"{index}-oid", f"SELECT '{index}'::regclass::oid"))
            for index in indexes]
    idle = compact(cluster, db, indexes[0], "fold-no-work")
    if "no eligible CAGRA pending rows" not in idle:
        raise RuntimeError(f"unexpected idle fold response: {idle}")
    graphless = compact(cluster, db, "fold_empty_cagra", "fold-no-base-graph")
    if "requires an existing base GPU graph" not in graphless:
        raise RuntimeError(f"unexpected graphless fold response: {graphless}")
    cluster.sql(db, "fold-owner-check", "-U", "fold_nonowner", "-c",
                f"SELECT pg_cuvs_compact('{indexes[0]}')", expect=["must be owner"])
    cluster.sql(db, "fold-product-insert-second", "-c", """
        INSERT INTO fold_second
            SELECT i, array_fill(i::real, ARRAY[32])::vector FROM generate_series(513,640) i;
        DELETE FROM fold_second WHERE id BETWEEN 513 AND 576;
    """)
    cluster.sql(db, "fold-product-dead-batch", "-c",
                "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) fold_second")
    log_path = logs / "daemon-fold-product.log"
    with old_snapshot(cluster, db) as reader:
        try:
            reader.stdin.write("BEGIN ISOLATION LEVEL REPEATABLE READ;\nSELECT pg_current_snapshot();\n")
            reader.stdin.flush()
            if not select.select([reader.stdout], [], [], 10)[0] or ":" not in reader.stdout.readline():
                raise RuntimeError("reader did not obtain a repeatable-read snapshot")
            cluster.sql(db, "fold-product-insert-first", "-c", """
                INSERT INTO fold_first
                    SELECT i, array_fill(i::real, ARRAY[32])::vector FROM generate_series(513,640) i;
            """)
            before = [fold_raw_page(cluster, db, 0, index) for index in indexes]
            for index in reversed(indexes):
                queued = compact(cluster, db, index, f"{index}-supply")
                if "pending rows supplied" not in queued:
                    raise RuntimeError(f"{index} did not supply pending rows: {queued}")

            deadline = time.monotonic() + 60
            while time.monotonic() < deadline and daemon.poll() is None:
                log = log_path.read_text()
                if all(re.search(rf"\[page_fold\].*index={oid}\b.*positions=128 status=0 offered=1", log)
                       for oid in oids):
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("native fold candidates were not offered for both indexes")

            rr_out, rr_err = reader.communicate("""
                SET LOCAL enable_seqscan=off;
                SET LOCAL cuvs.k=1;
                SELECT id FROM fold_first ORDER BY v <-> array_fill(640::real,ARRAY[32])::vector LIMIT 1;
                SELECT pg_cuvs_last_search_k();
                ROLLBACK;
            """, timeout=15)
            (logs / "old-snapshot-search.log").write_text(rr_out + rr_err)
            rows = rr_out.splitlines()
            if reader.returncode or len(rows) != 2 or not rows[0].isdigit() or \
                    not 1 <= int(rows[0]) <= 512 or rows[1] != "1":
                raise RuntimeError(f"future pending rows hid older visible GPU results: {rr_out!r} {rr_err!r}")
        finally:
            if reader.poll() is None:
                reader.kill()

    fold_sql = f"SELECT pg_cuvs_compact('{indexes[0]}');"
    for label, setup, sqlstate in (
        ("read-only", "SET default_transaction_read_only=on;", "25006"),
        ("transaction", "BEGIN;", "25001"),
        ("subtransaction", None, "25001"),
    ):
        calls = ["-c", setup, "-c", fold_sql] if setup else [
            "-c", f"DO $$ BEGIN BEGIN PERFORM pg_cuvs_compact('{indexes[0]}'); "
                  "EXCEPTION WHEN OTHERS THEN RAISE; END; END $$;",
        ]
        cluster.sql(db, f"fold-deny-{label}", "-v", "VERBOSITY=verbose", *calls, expect=[sqlstate])
        if fold_raw_page(cluster, db, 0, indexes[0]) != before[0]:
            raise RuntimeError(f"{label} call published a candidate")

    filename, block_size = fold_query(cluster, db, "fold-relation-path", f"""
        SELECT pg_relation_filepath('{indexes[0]}'), current_setting('block_size');
    """).split("|")
    block_size = int(block_size)
    log_block, = struct.unpack_from("=I", before[0], 124)
    partial_block, = struct.unpack_from("=I", before[0], 160)
    cluster.sql(db, "checkpoint-before-fold", "-c", "CHECKPOINT")
    blocks = (0, log_block, partial_block)
    checkpoint_pages = [read_disk_page(cluster, filename, block_size, block) for block in blocks]
    if any(len(page) != block_size for page in checkpoint_pages) or checkpoint_pages[0][48:88] != before[0][48:88]:
        raise RuntimeError("checkpoint did not preserve the original metapage and both data pages")

    result = compact(cluster, db, indexes[0], "drop-ack")
    if "published with daemon acknowledgment pending" not in result:
        raise RuntimeError(f"expected a retained candidate after ACK loss: {result}")
    meta = fold_raw_page(cluster, db, 0, indexes[0])
    base, graph, slots, folded = struct.unpack_from("=4Q", meta, 56)
    if base == graph or (slots, folded) != (640, 128):
        raise RuntimeError(f"unexpected metapage transition: {(base, graph, slots, folded)}")
    if fold_raw_page(cluster, db, 0, indexes[1]) != before[1]:
        raise RuntimeError("the manual invocation modified a different index")
    fold_log = fold_raw_page(cluster, db, log_block, indexes[0])
    partial = fold_raw_page(cluster, db, partial_block, indexes[0])
    if fold_log[48:72] != meta[64:88]:
        raise RuntimeError("fold log disagrees with metapage")

    result = compact(cluster, db, indexes[0], "retry-ack")
    if "CAGRA fold published" not in result or "acknowledgment pending" in result:
        raise RuntimeError(f"retry failed to acknowledge the original transition: {result}")
    log = log_path.read_text()
    lost = re.findall(rf"\[page_fold_ack_drop\].*index={oids[0]}\b.*lsn=([0-9A-F]+)", log)
    committed = re.findall(rf"\[page_fold_commit\].*index={oids[0]}\b.*lsn=([0-9A-F]+)", log)
    if len(lost) != 1 or lost != committed:
        raise RuntimeError(f"lost delivery and retry produced different LSNs: {lost}, {committed}")
    gate_records = read_records([log_path])
    matching = [w for w in publication_windows(gate_records, list(requests_from(gate_records)))
                if w["index"] == str(oids[0])]
    if (len(matching) != 1 or matching[0]["state"] != "committed" or
            len(matching[0]["attempts"]) < 2 or matching[0]["lsn"] != committed[0]):
        raise RuntimeError(f"READY redelivery did not join the single COMMITTED event: {matching}")
    lsn = int(committed[0], 16)
    pages = [meta, fold_log, partial]
    if any(struct.unpack_from("=II", page) != (lsn >> 32, lsn & 0xFFFFFFFF) for page in pages):
        raise RuntimeError("metapage, fold log and partial TID page did not share the transition LSN")
    if [fold_raw_page(cluster, db, block, indexes[0]) for block in blocks] != pages:
        raise RuntimeError("retry rewrote a published WAL page")
    tid_offset = 24 + 512 * 6
    hi, lo, offset = struct.unpack_from("=3H", partial, tid_offset)
    first_tid = fold_query(cluster, db, "first-folded-tid", "SELECT ctid FROM fold_first WHERE id=513")
    if first_tid != f"({(hi << 16) | lo},{offset})":
        raise RuntimeError("the first partial-page slot does not point to the folded heap tuple")

    query = """
        SET enable_seqscan=off;
        SET cuvs.k=100;
        SELECT id FROM fold_first ORDER BY v <-> array_fill(513::real,ARRAY[32])::vector LIMIT 1;
        SELECT pg_cuvs_last_search_k();
    """
    if fold_query(cluster, db, "search-folded-row", query) != "100":
        raise RuntimeError("the published fold did not use daemon search")
    search_log = logs / f"{cluster.data.parent.name}-search-folded-row.log"
    if search_log.read_text().splitlines()[-2] != "513":
        raise RuntimeError("the published fold did not return the folded row")

    second_meta = check_dead_slots_after_ready(cluster, db, indexes[1], before[1])
    check_vacuum_undercount(cluster, db, indexes[1], second_meta)
    cluster.stop("immediate")
    if [read_disk_page(cluster, filename, block_size, block) for block in blocks] != checkpoint_pages:
        raise RuntimeError("transition reached relation files before redo")
    if checkpoint_pages[2][tid_offset:tid_offset + 6] != bytes(6):
        raise RuntimeError("before redo the disk partial page already contained the first folded TID")
    cluster.start("fold-restart")
    if [fold_raw_page(cluster, db, block, indexes[0]) for block in blocks] != pages:
        raise RuntimeError("WAL replay failed to recover the metapage, fold log or partial TID page")
    if fold_raw_page(cluster, db, 0, indexes[1]) != second_meta:
        raise RuntimeError("WAL replay changed the fold with post-READY dead slots")


def check_low_host_budget(cluster):
    db = "postgres"
    cluster.sql(db, "fold-low-budget-setup", "-c", """
        CREATE EXTENSION vector;
        CREATE EXTENSION pg_cuvs;
        CREATE EXTENSION pageinspect;
        CREATE TABLE fold_limited(id integer, v vector(32));
        INSERT INTO fold_limited
            SELECT i, array_fill(i::real, ARRAY[32])::vector FROM generate_series(1,512) i;
        CREATE INDEX fold_limited_cagra ON fold_limited USING cagra(v vector_l2_ops);
        INSERT INTO fold_limited VALUES (513, array_fill(513::real, ARRAY[32])::vector);
    """)
    index = "fold_limited_cagra"
    before = fold_raw_page(cluster, db, 0, index)
    output = compact(cluster, db, index, "fold-low-budget-defer")
    if "overlay host budget is full" not in output or fold_raw_page(cluster, db, 0, index) != before:
        raise RuntimeError(f"host-budget refusal changed fold state or was not reported: {output}")


def batch_fixture(cluster):
    cluster.sql("postgres", "batch-setup", "-c", f"""
        CREATE EXTENSION vector;
        CREATE EXTENSION pg_cuvs;
        CREATE EXTENSION pageinspect;
        CREATE TABLE fold_batch(id integer PRIMARY KEY, v vector(32));
        INSERT INTO fold_batch
            SELECT i, array_fill(i::real, ARRAY[32])::vector
            FROM generate_series(1,{BASE_ROWS}) i;
        CREATE INDEX {BATCH_INDEX} ON fold_batch USING cagra(v vector_l2_ops);
    """)
    oid = fold_query(cluster, "postgres", "batch-index-oid", f"SELECT '{BATCH_INDEX}'::regclass::oid")
    meta = batch_meta(cluster)
    index_nonce, base, graph, *counts = meta
    if not index_nonce or not base or base != graph or counts != [BASE_ROWS, 0, BASE_ROWS]:
        raise RuntimeError(f"expected a {BASE_ROWS}-row base graph without folds, got {meta}")
    return oid, meta


def batch_meta(cluster, index=BATCH_INDEX):
    return struct.unpack_from("=6Q", fold_raw_page(cluster, "postgres", 0, index), 48)


def batch_insert(cluster, previous, total):
    cluster.sql("postgres", f"batch-insert-{total}", "-c", f"""
        INSERT INTO fold_batch
            SELECT i, array_fill(i::real, ARRAY[32])::vector
            FROM generate_series({BASE_ROWS + previous + 1},{BASE_ROWS + total}) i;
    """)


def batch_compact(cluster, label, notice, *, index=BATCH_INDEX):
    output = compact(cluster, "postgres", index, label)
    notices = [line.removeprefix("NOTICE:").strip() for line in output.splitlines()
               if line.startswith("NOTICE:")]
    if notice not in notices:
        raise RuntimeError(f"{label}: expected {notice!r}, got {output}")


def wait_for_offer(daemon, log_path, oid, ordinal, positions):
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and daemon.poll() is None:
        offers = [r for r in read_records([log_path])["page_fold"]
                  if r["index"] == oid and r["status"] == "0" and r["offered"] == "1"]
        if len(offers) >= ordinal:
            offer = offers[ordinal - 1]
            actual = int(offer["positions"])
            if actual != positions:
                raise RuntimeError(f"offer {ordinal}: expected {positions} positions, got {actual}")
            return offer
        time.sleep(0.05)
    raise RuntimeError(f"offer {ordinal} for index {oid} was not produced: {log_path}")


def publish_batch(cluster, log_path, oid, before, positions, *, index=BATCH_INDEX):
    index_nonce, base_nonce, graph, slots, folded, base_slots = before
    label = "batch-publish" if index == BATCH_INDEX else f"{index}-publish"
    batch_compact(cluster, f"{label}-{folded + positions}", "CAGRA fold published", index=index)
    after = batch_meta(cluster, index)
    new_graph = after[2]
    expected = (index_nonce, base_nonce, new_graph, slots + positions, folded + positions, base_slots)
    if new_graph in (0, graph, base_nonce) or after != expected:
        raise RuntimeError(f"{positions}-position fold produced unexpected metapage {after}, from {before}")

    commits = [r for r in read_records([log_path])["page_fold_commit"]
               if r["index"] == oid and int(r["graph_nonce"]) == new_graph]
    if [(int(r["positions"]), int(r["folded"])) for r in commits] != [(positions, folded + positions)]:
        raise RuntimeError(f"daemon did not commit the published generation {new_graph}: {commits}")
    return after


def check_batch_policy(cluster, daemon, log_path, limit):
    oid, initial = batch_fixture(cluster)

    small = 65
    if not limit:
        # Supply an ineligible backlog first, then cross the old threshold on
        # the same graph. The later publication witnesses a working producer.
        batch_insert(cluster, 0, small)
        batch_compact(cluster, "batch-supply-small", "CAGRA pending rows supplied or already queued")
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline and daemon.poll() is None:
            starts = read_records([log_path])["page_fold_begin"]
            if any(r["index"] == oid for r in starts):
                raise RuntimeError(f"default policy started folding only {small} pending positions")
            time.sleep(0.05)
        batch_compact(cluster, "batch-still-small", "CAGRA pending rows supplied or already queued")
        if batch_meta(cluster) != initial:
            raise RuntimeError("the default policy published a sub-threshold batch")
        batch_insert(cluster, small, PENDING_ROWS)
        expected_sizes = (PENDING_ROWS,)
    else:
        batch_insert(cluster, 0, PENDING_ROWS)
        expected_sizes = (limit, PENDING_ROWS - limit)

    batch_compact(cluster, "batch-supply-all", "CAGRA pending rows supplied or already queued")
    meta = initial
    for ordinal, positions in enumerate(expected_sizes, start=1):
        wait_for_offer(daemon, log_path, oid, ordinal, positions)
        if batch_meta(cluster) != meta:
            raise RuntimeError("the daemon offer changed the metapage before pg_cuvs_compact")
        # The second offer consumes the retained suffix without adding rows or
        # issuing another supply call, even though it is below both thresholds.
        meta = publish_batch(cluster, log_path, oid, meta, positions)
    batch_compact(cluster, "batch-drained", "no eligible CAGRA pending rows to fold")
    if batch_meta(cluster) != meta:
        raise RuntimeError("compacting an empty suffix changed the graph generation")
    if not limit and any(r["index"] == oid and int(r["positions"]) == small
                         for r in read_records([log_path])["page_fold_begin"]):
        raise RuntimeError(f"default policy started folding only {small} pending positions")
    print(f"PASS native fold batch policy ({limit=}, publications={expected_sizes})")


def build_original_graph_probe(cluster):
    work = cluster.data.parent
    probe = work / "original-graph.c"
    probe.write_text(r"""
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cuvs_ipc.h"

int main(int argc, char **argv)
{
    enum { NQ = 512, DIM = 32, K = 10 };
    float queries[NQ][DIM];
    CuvsResult answers[NQ * K];
    int count = 0;
    uint32_t latency;

    if (argc != 10 && argc != 12)
        return 2;
    const int check_sample = argc == 12;
    const float sample_value = check_sample ? strtof(argv[11], NULL) : 0.0f;
    const uint64_t folded = check_sample ? strtoull(argv[10], NULL, 10) : 0;
    CuvsPageSearchRequest request = {
        .length = sizeof(request),
        .system_identifier = strtoull(argv[4], NULL, 10),
        .index_nonce = strtoull(argv[5], NULL, 10),
        .base_nonce = strtoull(argv[6], NULL, 10),
        .graph_nonce = strtoull(argv[7], NULL, 10),
        .n_slots = strtoull(argv[8], NULL, 10),
        .folded = folded,
        .tail = folded,
    };
    for (int q = 0; q < NQ; ++q)
        for (int d = 0; d < DIM; ++d)
            queries[q][d] = check_sample ? sample_value : (float) (q + 1);
    int status = cuvs_ipc_search_pages(argv[1], strtoul(argv[2], NULL, 10),
        strtoul(argv[3], NULL, 10), &request, &queries[0][0], NQ, DIM, K,
        CUVS_METRIC_L2, check_sample ? 512 : 64, 1, 0, 0, 0, NULL, 0, answers, &count, &latency);
    printf("status=%d count=%d\n", status, count);
    if (strcmp(argv[9], "retired") == 0)
        return status == CUVS_STATUS_NOT_FOUND ? 0 : 1;
    if (status != CUVS_STATUS_OK || count != NQ * K)
        return 1;
    int sample_hits = 0;
    for (int i = 0; i < count; ++i) {
        if (answers[i].tid >= request.n_slots || !isfinite(answers[i].distance))
            return 1;
        if (check_sample && answers[i].tid == request.n_slots - 1 && answers[i].distance == 0.0f)
            ++sample_hits;
        printf("%d %" PRIu64 " %a\n", i / K, answers[i].tid, (double) answers[i].distance);
    }
    if (check_sample && !sample_hits) {
        fputs("published dead slot did not match the live suffix vector\n", stderr);
        return 1;
    }
    return 0;
}
""")
    binary = work / "original-graph-probe"
    src = ROOT / "src"
    cluster.run("cc", ["-std=gnu11", "-O2", "-I", src, "-o", binary, probe,
                       src / "cuvs_ipc.c", src / "cuvs_util.c", src / "cuvs_build_corpus.c",
                       "-lm", "-lpthread", "-lrt"], "build-original-graph-probe")
    return binary


def check_original_graph(cluster, daemon, log_path):
    probe = build_original_graph_probe(cluster)
    oid, original = batch_fixture(cluster)
    db_oid, sysid = fold_query(cluster, "postgres", "original-graph-identity", """
        SELECT (SELECT oid FROM pg_database WHERE datname = current_database()), system_identifier
        FROM pg_control_system();
    """).split("|")
    keys = [cluster.socket, db_oid, oid, sysid, *original[:4]]

    def search(label, state="live"):
        cluster.run(probe, [*keys, state], label)
        path = cluster.logs / f"{cluster.data.parent.name}-{label}.log"
        return path.read_text().splitlines()[2:]

    before = search("parent-before-supply")
    batch_insert(cluster, 0, BATCH_ROWS)
    batch_compact(cluster, "parent-supply", "CAGRA pending rows supplied or already queued")
    wait_for_offer(daemon, log_path, oid, 1, BATCH_ROWS)
    if batch_meta(cluster) != original:
        raise RuntimeError("the graph changed before the original-generation comparison")

    # Both base-only IPC requests select the same physical generation and
    # omit the pending overlay. SQL cannot select this old GPU handle after
    # the worker commits the new generation.
    after = search("parent-after-offer")

    # Preserve rank and distance. Normalize only candidate order within ties.
    def with_ties_sorted(rows):
        return [sorted(tied) for _, tied in groupby(map(str.split, rows), lambda row: (row[0], row[2]))]

    if with_ties_sorted(before) != with_ties_sorted(after):
        raise RuntimeError("original graph CAGRA results changed after the daemon offered the candidate")

    publish_batch(cluster, log_path, oid, original, BATCH_ROWS)
    search("parent-after-commit", "retired")
    print("PASS native fold original generation before publication and retirement after COMMITTED")


def check_dead_head_batch(cluster, daemon, log_path):
    db = "postgres"
    probe = build_original_graph_probe(cluster)
    oid, initial = batch_fixture(cluster)
    batch_insert(cluster, 0, 2)
    cluster.sql(db, "dead-head-delete", "-c", f"DELETE FROM fold_batch WHERE id <= {BASE_ROWS + 1}")
    cluster.sql(db, "dead-head-vacuum", "-c", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) fold_batch")

    vacuumed = fold_raw_page(cluster, db, 0, BATCH_INDEX)
    slots_block, = struct.unpack_from("=I", vacuumed, 160)
    pending_block, = struct.unpack_from("=I", vacuumed, 112)
    dead_tid = struct.pack("=3H", 65535, 65535, 0)
    slots_page = fold_raw_page(cluster, db, slots_block, BATCH_INDEX)
    pending_page = fold_raw_page(cluster, db, pending_block, BATCH_INDEX)
    first_slot = 24 + BASE_ROWS * len(dead_tid)
    hi, lo, off = struct.unpack_from("=3H", pending_page, 30)
    suffix_tid = fold_query(cluster, db, "dead-head-suffix-tid",
                            f"SELECT ctid FROM fold_batch WHERE id = {BASE_ROWS + 2}")
    if (struct.unpack_from("=6Q", vacuumed, 48) != initial or
            struct.unpack_from("=Q", vacuumed, 104)[0] != BASE_ROWS or
            slots_page[24:first_slot] != dead_tid * BASE_ROWS or
            pending_page[24:30] != dead_tid or suffix_tid != f"({(hi << 16) | lo},{off})"):
        raise RuntimeError("fixture must retain only a live suffix behind the physically dead head and base")

    # Both marks are supplied together. The first offer has no live slot in
    # its batch or base, so the producer can use the retained live suffix.
    batch_compact(cluster, "dead-head-supply", "CAGRA pending rows supplied or already queued")
    wait_for_offer(daemon, log_path, oid, 1, 1)
    head = publish_batch(cluster, log_path, oid, initial, 1)
    slots_page = fold_raw_page(cluster, db, slots_block, BATCH_INDEX)
    if (struct.unpack_from("=Q", fold_raw_page(cluster, db, 0, BATCH_INDEX), 104)[0] != BASE_ROWS + 1 or
            slots_page[first_slot:first_slot + 6] != dead_tid):
        raise RuntimeError("the first published position must retain its invalid TID and increment dead_slots")

    # Query the published GPU generation before the suffix is published.
    # Search the stored graph alone so the pending vector cannot score zero
    # through the overlay in place of the newly published dead slot.
    db_oid, sysid = fold_query(cluster, db, "dead-head-identity", """
        SELECT (SELECT oid FROM pg_database WHERE datname = current_database()), system_identifier
        FROM pg_control_system();
    """).split("|")
    cluster.run(probe, [cluster.socket, db_oid, oid, sysid, *head[:4], "sample", head[4], BASE_ROWS + 2],
                "dead-head-gpu-sample")

    # The live sample is still pending and its own TID must land in slot 513.
    wait_for_offer(daemon, log_path, oid, 2, 1)
    publish_batch(cluster, log_path, oid, head, 1)
    slots_page = fold_raw_page(cluster, db, slots_block, BATCH_INDEX)
    if (struct.unpack_from("=Q", fold_raw_page(cluster, db, 0, BATCH_INDEX), 104)[0] != BASE_ROWS + 1 or
            slots_page[first_slot:first_slot + 12] != dead_tid + pending_page[30:36]):
        raise RuntimeError("the live suffix fold did not preserve both independently published TIDs")
    print("PASS native fold dead head with no live base and a retained live suffix")


def check_idle_cache_pressure(cluster, daemon, log_path):
    db = "postgres"
    first_oid, first = batch_fixture(cluster)
    second_index = "fold_pressure_cagra"
    # The first graph retains >= 544 * 64 * 4 = 139264 bytes. The second
    # supply needs 1200 * (8 + 32 * 4) = 163200 bytes plus its chunk header.
    # Each fits 256 KiB on its own; their sum cannot fit. An 8-degree second
    # graph keeps its own fold mirror small enough to fit with its overlay.
    cluster.sql(db, "cache-pressure-setup", "-c", f"""
        CREATE TABLE fold_pressure (LIKE fold_batch);
        INSERT INTO fold_pressure SELECT * FROM fold_batch;
        CREATE INDEX {second_index} ON fold_pressure USING cagra(v vector_l2_ops) WITH (graph_degree=8);
        INSERT INTO fold_pressure
            SELECT i, array_fill(i::real, ARRAY[32])::vector
            FROM generate_series({BASE_ROWS + 1},{BASE_ROWS + 1200}) i;
    """)
    second_oid = fold_query(cluster, db, "cache-pressure-oid", f"SELECT '{second_index}'::regclass::oid")
    second = batch_meta(cluster, second_index)

    batch_insert(cluster, 0, 2 * CACHE_BATCH_ROWS)
    batch_compact(cluster, "cache-prime-supply", "CAGRA pending rows supplied or already queued")
    wait_for_offer(daemon, log_path, first_oid, 1, CACHE_BATCH_ROWS)
    first = publish_batch(cluster, log_path, first_oid, first, CACHE_BATCH_ROWS)
    offer = wait_for_offer(daemon, log_path, first_oid, 2, CACHE_BATCH_ROWS)
    # Both clocks are CLOCK_MONOTONIC on Linux. Measure from the daemon's
    # completion, which starts the 30-second expiry, not from log readback.
    deadline = int(offer["mono_us"]) / 1_000_000 + 25
    publish_batch(cluster, log_path, first_oid, first, CACHE_BATCH_ROWS)
    phase = (rf"\[page_fold_phases\] gpu=0 db=\d+ index={first_oid} positions={CACHE_BATCH_ROWS} "
             r"status=0 offered=1 [^\n]*host_graph_cache_hit=([01])\n")
    hits = []
    while time.monotonic() < deadline and daemon.poll() is None:
        hits = re.findall(phase, log_path.read_text())
        if len(hits) >= 2:
            break
        time.sleep(0.05)
    if hits != ["0", "1"]:
        raise RuntimeError(f"first index did not reuse a retained host graph before pressure: {hits}")

    batch_compact(cluster, "cache-pressure-supply", "CAGRA pending rows supplied or already queued",
                  index=second_index)
    if time.monotonic() >= deadline:
        raise RuntimeError("second supply did not finish inside the cache-expiry control window")
    wait_for_offer(daemon, log_path, second_oid, 1, CACHE_BATCH_ROWS)
    publish_batch(cluster, log_path, second_oid, second, CACHE_BATCH_ROWS, index=second_index)
    print("PASS native fold reclaimed the first index's idle host graph during the second supply")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pg_config", type=Path)
    parser.add_argument("daemon", type=Path)
    parser.add_argument("logs", type=Path)
    batch_limits = {"batch-unset": None, "batch-zero": 0, "batch-64": BATCH_ROWS,
                    "original-graph": BATCH_ROWS, "batch-dead-head": 1,
                    "cache-pressure": CACHE_BATCH_ROWS}
    parser.add_argument("--case", choices=("existing", *batch_limits), default="existing")
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)
    logs = args.logs.resolve()
    bindir = subprocess.check_output([args.pg_config, "--bindir"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix="fold-product-", dir="/tmp") as work:
        if args.case != "existing":
            limit = batch_limits[args.case]
            cluster = Cluster(Path(work) / args.case, bindir, logs)
            for key in DAEMON_FOLD_ENV:
                cluster.env.pop(key, None)
            cluster.start()
            try:
                if limit is not None:
                    cluster.env["CUVS_PAGE_FOLD_BATCH_ROWS"] = str(limit)
                host_budget = 128 << 20
                if args.case == "cache-pressure":
                    cluster.env["CUVS_PAGE_FOLD_PROFILE"] = "1"
                    host_budget = CACHE_HOST_BUDGET
                with cluster.daemon(args.daemon.resolve(), args.case, overlay_host_bytes=host_budget) as daemon:
                    for key in DAEMON_FOLD_ENV:
                        cluster.env.pop(key, None)
                    log_path = logs / f"daemon-{args.case}.log"
                    if args.case == "original-graph":
                        check_original_graph(cluster, daemon, log_path)
                    elif args.case == "batch-dead-head":
                        check_dead_head_batch(cluster, daemon, log_path)
                    elif args.case == "cache-pressure":
                        check_idle_cache_pressure(cluster, daemon, log_path)
                    else:
                        check_batch_policy(cluster, daemon, log_path, limit)
            finally:
                cluster.stop()
            return

        cluster = Cluster(Path(work) / "native", bindir, logs)
        for key in DAEMON_FOLD_ENV:
            cluster.env.pop(key, None)
        cluster.env["CUVS_FAULT_PAGE_FOLD_ACK_ONCE"] = "1"
        with cluster.daemon(args.daemon.resolve(), "fold-product", overlay_host_bytes=128 << 20) as daemon:
            cluster.start()
            try:
                check(cluster, logs, daemon)
            finally:
                cluster.stop()
        limited = Cluster(Path(work) / "limited", bindir, logs)
        for key in DAEMON_FOLD_ENV:
            limited.env.pop(key, None)
        with limited.daemon(args.daemon.resolve(), "fold-low-budget", overlay_host_bytes=1):
            limited.start()
            try:
                check_low_host_budget(limited)
            finally:
                limited.stop()
    print("PASS native fold, RR, transaction guards, host budget, ACK retry and WAL replay")


if __name__ == "__main__":
    main()

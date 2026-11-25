"""Exercise the native, manually launched page-fold product path on Linux.

Use a disposable PostgreSQL installation with the opt-in extension installed,
and pg_cuvs_server_test so the first COMMITTED delivery can be dropped.
"""

import argparse
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import tempfile
import time

from run import Cluster, fold_query, fold_raw_page
from fold_gate_log import publication_windows, read_records, requests_from


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pg_config", type=Path)
    parser.add_argument("daemon", type=Path)
    parser.add_argument("logs", type=Path)
    args = parser.parse_args()
    args.logs.mkdir(parents=True, exist_ok=True)
    bindir = subprocess.check_output([args.pg_config, "--bindir"], text=True).strip()
    os.environ["CUVS_FAULT_PAGE_FOLD_ACK_ONCE"] = "1"
    with tempfile.TemporaryDirectory(prefix="fold-product-", dir="/tmp") as work:
        cluster = Cluster(Path(work) / "native", bindir, args.logs.resolve())
        with cluster.daemon(args.daemon.resolve(), "fold-product", overlay_host_bytes=128 << 20) as daemon:
            cluster.start()
            try:
                check(cluster, args.logs.resolve(), daemon)
            finally:
                cluster.stop()
        limited = Cluster(Path(work) / "limited", bindir, args.logs.resolve())
        with limited.daemon(args.daemon.resolve(), "fold-low-budget", overlay_host_bytes=1):
            limited.start()
            try:
                check_low_host_budget(limited)
            finally:
                limited.stop()
    print("PASS native fold, RR, transaction guards, host budget, ACK retry and WAL replay")


if __name__ == "__main__":
    main()

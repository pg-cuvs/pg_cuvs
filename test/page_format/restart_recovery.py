"""Disposable Linux restart regression. See restart-recovery.md."""

import argparse
from contextlib import contextmanager
import errno
import os
from pathlib import Path
import re
import select
import shutil
import struct
import subprocess
import tempfile
import time
from zlib import crc32

from checkpoint_syscalls import assert_checkpoint_syscalls
from run import Cluster, fold_query, fold_raw_page


EXPR = "(recovery_vector(v)::vector(4))"
MANIFEST = struct.Struct("=9Q8IQ2I")
MANIFEST_CRC = MANIFEST.size - 4
SAVE_CRASH_PHASES = {
    "save-crash-before-staging": "before-staging-fsync",
    "save-crash-after-staging": "after-staging-fsync",
    "save-crash-after-rename": "after-rename",
    "save-crash-rename-lost": "after-rename",
    "save-crash-after-parent": "after-parent-fsync",
}


def head(cluster):
    raw = fold_raw_page(cluster, "postgres", 0, "recovery_cagra")
    index_nonce, _, nonce, slots, folded = struct.unpack_from("=5Q", raw, 48)
    return index_nonce, nonce, slots, folded


def query(cluster, label, sql):
    return fold_query(cluster, "postgres", label, sql)


def execute(cluster, label, sql):
    cluster.sql("postgres", label, "-qAt", "-c", sql)
    return (cluster.logs / f"{cluster.data.parent.name}-{label}.log").read_text()


def search(cluster, probe, gpu, label, expected=None):
    expected = probe if expected is None else expected
    nearest = f"""
        SELECT id FROM recovery_rows
        WHERE active ORDER BY {EXPR} <-> '[{probe},1,2,3]' LIMIT 1;
    """
    oracle = query(cluster, label + "-heap", f"""
        SET ROLE recovery_owner;
        SET enable_indexscan=off;
        SET enable_bitmapscan=off;
        {nearest}
    """)
    result = execute(cluster, label, f"""
        SET ROLE recovery_owner;
        SET enable_seqscan=off;
        SET cuvs.k=1;
        {nearest}
        SELECT coalesce(pg_cuvs_last_search_k()::text, 'exact');
    """).splitlines()[-2:]
    if oracle != str(expected) or len(result) != 2 or result[0] != oracle:
        raise RuntimeError(f"{label}: expected {expected}, heap {oracle!r}, index {result!r}")
    if not result[1].isdigit() and result[1] != "exact":
        raise RuntimeError(f"{label}: invalid search route marker {result[1]!r}")
    return result[1].isdigit() == gpu


def wait_for(check, label, timeout=90):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.15)
    raise RuntimeError(label)


def fold(cluster, first, last):
    before = head(cluster)
    execute(cluster, f"insert-{first}", f"""
        SET ROLE recovery_owner;
        INSERT INTO recovery_rows
        SELECT i, format('[%s,1,2,3]', i)::vector, true
        FROM generate_series({first}, {last}) i;
    """)
    expected = last - first + 1

    def published():
        execute(cluster, f"fold-{first}", "SELECT pg_cuvs_compact('recovery_cagra');")
        return head(cluster)[2:] == (before[2] + expected, before[3] + expected)

    wait_for(published, f"fold {first}..{last} never published", 35)
    after = head(cluster)
    if before[0] != after[0] or before[1] == after[1]:
        raise RuntimeError("fold did not publish a new graph of the same incarnation")


def assertion(cluster, probe, gpu, label, expected=None):
    if not search(cluster, probe, gpu, label, expected):
        raise RuntimeError(f"{label}: unexpected search route")


def check_manifest(path, graph, expected, db, oid, *, dim=4, protected=False):
    wire = path.read_bytes()
    artifact = graph.read_bytes()
    magic, sysid, incarnation, _, xid, nonce, slots, folded, size, *tail = MANIFEST.unpack(wire)
    disk_db, disk_oid, disk_dim, metric, version, body_crc, degree, flags, boot, physical, manifest_crc = tail
    if (magic != 0x4355565350474D31 or not sysid or not xid or
            (incarnation, nonce, slots, folded) != expected or
            (disk_db, disk_oid, disk_dim, metric, version, degree) != (db, oid, dim, 0, 2, 64) or
            flags != int(protected) or bool(boot) != protected or physical > slots or
            (not protected and physical != 0) or
            size != len(artifact) or body_crc != crc32(artifact) or
            manifest_crc != crc32(wire[:-4]) or not (path.parent / "build.xid").read_text().strip()):
        raise RuntimeError(f"manifest differs from PG head or independent CRC oracle: {path}")
    return wire, artifact


def open_gate(path):
    try:
        return os.open(path, os.O_WRONLY | os.O_NONBLOCK)
    except OSError as exc:
        if exc.errno != errno.ENXIO:
            raise
        return None


def gated_search(cluster, path):
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        fd = open_gate(path)
        if fd is not None:
            try:
                assertion(cluster, 760, False, "install-paused")
                cluster.sql("postgres", "unrelated-legacy-drop", "-c",
                            "DROP INDEX recovery_legacy_cagra;", timeout=5)
            finally:
                os.write(fd, b"x")
                os.close(fd)
            return
        time.sleep(0.05)
    raise RuntimeError("proxy did not reach the unpublished install gate")


def shutdown_paused_install(cluster, daemon, gate, *, expect_timeout):
    descriptor = None

    def reached():
        nonlocal descriptor
        descriptor = open_gate(gate)
        return descriptor is not None

    wait_for(reached, "recovery did not reach the unpublished install gate", 35)
    log = cluster.logs / "daemon-recovery-restart.log"
    try:
        daemon.terminate()
        wait_for(lambda: "batch: worker thread joined" in log.read_text(),
                 "daemon did not reach its recovery drain", 10)
        if expect_timeout:
            exit_code = daemon.wait(timeout=13)
            output = log.read_text()
            if (exit_code != 1 or "shutdown timed out with 1 active owners" not in output or
                    "shutdown complete" in output):
                raise RuntimeError("daemon did not fail-stop on a held recovery owner")
            print("SIGTERM times out without recovery teardown: PASS", flush=True)
            return
        try:
            daemon.wait(timeout=1)
        except subprocess.TimeoutExpired:
            pass
        else:
            raise RuntimeError("daemon shut down before the recovery GPU owner was released")
        if "shutdown complete" in log.read_text():
            raise RuntimeError("daemon completed shutdown with a recovery GPU owner still held")
    finally:
        os.close(descriptor)
    if daemon.wait(timeout=15) != 0 or "shutdown complete" not in log.read_text():
        raise RuntimeError("daemon did not shut down cleanly after recovery released its GPU owner")
    print("SIGTERM waits for the recovery GPU owner: PASS", flush=True)


@contextmanager
def active_creator(cluster):
    with subprocess.Popen(["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", "postgres"],
                          env=cluster.env, text=True, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE) as backend:
        try:
            backend.stdin.write("BEGIN; SELECT pg_current_xact_id()::text;\n")
            backend.stdin.flush()
            if not select.select([backend.stdout], [], [], 15)[0]:
                raise RuntimeError("could not reserve an in-progress creator XID")
            yield int(backend.stdout.readline().strip())
        finally:
            if backend.poll() is None:
                backend.communicate("ROLLBACK;\n", timeout=15)


def check_reconciliation(cluster, incarnation, base, middle, current, base_wire, artifact, active_xid):
    fixture_paths = []

    def fixture(nonce, *, xid=None, wrong_system=False, bad_marker=False, extra=False):
        directory = incarnation / f"{nonce:016x}"
        directory.mkdir()
        wire = bytearray(base_wire)
        struct.pack_into("=Q", wire, 40, nonce)
        if xid is not None:
            struct.pack_into("=Q", wire, 32, xid)
        creator, = struct.unpack_from("=Q", wire, 32)
        if wrong_system:
            sysid, = struct.unpack_from("=Q", wire, 8)
            struct.pack_into("=Q", wire, 8, sysid ^ 1)
        struct.pack_into("=I", wire, MANIFEST_CRC, crc32(wire[:-4]))
        (directory / "manifest").write_bytes(wire)
        (directory / "build.xid").write_text(f"{creator + bad_marker}\n")
        (directory / "index.cagra").write_bytes(artifact)
        if extra:
            (directory / "unrecognized").write_text("retain this directory\n")
        fixture_paths.append(directory)

    # Many low nonces force a second LIST page. Also exercise upper-cursor
    # order independently of the filesystem's directory iteration order.
    foreign = [*range(100_000, 100_131), 0xFFFFFFFFFFFFFFFE]
    for nonce in reversed(foreign):
        fixture(nonce)
    keep = (200_000, 200_001, 200_002, 200_003, 200_004, middle[1], current[1])
    fixture(keep[0], xid=active_xid)
    fixture(keep[1], xid=active_xid + 1_000_000)
    fixture(keep[2], wrong_system=True)
    fixture(keep[3], bad_marker=True)
    fixture(keep[4], extra=True)
    fixture(middle[1])  # WAL nonce exists, but the stored L and F are the base's.

    log = cluster.logs / "daemon-recovery-build.log"
    expected = {(nonce, 1) for nonce in foreign} | {(base[1], 2)}

    def planned():
        rows = {(int(nonce, 16), int(action)) for nonce, action in re.findall(
            r"\[page_checkpoint_reconcile\] .*nonce=([0-9a-f]+) action=(\d+) dry_run=1", log.read_text())}
        unexpected = rows - expected
        if unexpected:
            raise RuntimeError(f"reconciliation approved unexpected checkpoints: {sorted(unexpected)}")
        return rows == expected

    wait_for(planned, "reconciliation omitted a page, an approved branch or the superseded WAL ancestor", 140)
    if any(not path.is_dir() for path in fixture_paths) or not all(
            (incarnation / f"{nonce:016x}").is_dir() for nonce in (base[1], current[1])):
        raise RuntimeError("dry-run reconciliation modified checkpoint directories")
    if head(cluster) != current:
        raise RuntimeError("reconciliation modified the authoritative PG head")


def setup(cluster):
    execute(cluster, "setup", f"""
        CREATE EXTENSION vector;
        CREATE EXTENSION pg_cuvs;
        CREATE EXTENSION pageinspect;
        CREATE ROLE recovery_owner;
        CREATE FUNCTION recovery_vector(vector) RETURNS vector
        LANGUAGE plpgsql IMMUTABLE STRICT AS $fn$
        BEGIN
            IF current_user <> 'recovery_owner' THEN
                RAISE EXCEPTION 'recovery expression executed with the wrong role';
            END IF;
            RETURN $1;
        END;
        $fn$;
        CREATE TABLE recovery_rows
            (id integer PRIMARY KEY, v vector(4), active bool, note text)
            WITH (fillfactor=80);
        INSERT INTO recovery_rows
        SELECT i, format('[%s,1,2,3]', i)::vector, true
        FROM generate_series(1,512) i;
        INSERT INTO recovery_rows VALUES (-1, '[500.25,1,2,3]', false, NULL);
        ALTER TABLE recovery_rows OWNER TO recovery_owner;
        CREATE INDEX recovery_cagra ON recovery_rows
            USING cagra ({EXPR} vector_l2_ops) WHERE active;
    """)


def recovery_daemon(cluster, binary, label, **options):
    # Match the existing page-format fixture. The fault-test daemon starts
    # with a zero overlay host budget unless the runner supplies one.
    return cluster.daemon(binary, label, overlay_host_bytes=4 * 1024**3, **options)


def base_artifact_dir(cluster):
    base = head(cluster)
    if base[2:] != (512, 0):
        raise RuntimeError("the partial index did not exclude the false-predicate input")
    db = int(query(cluster, "base-db", "SELECT oid FROM pg_database WHERE datname=current_database()"))
    oid = int(query(cluster, "base-oid", "SELECT 'recovery_cagra'::regclass::oid"))
    return cluster.indexdir / f"{db}_{oid}" / f"{base[0]:016x}" / f"{base[1]:016x}"


def check_cancelled_vacuum(cluster, binary):
    cluster.start()
    try:
        with recovery_daemon(cluster, binary, "base-vacuum-build"):
            setup(cluster)
            artifact = base_artifact_dir(cluster)
        artifact.rename(cluster.data.parent / "before-cancelled-vacuum")
        execute(cluster, "delete-before-cancel", "DELETE FROM recovery_rows WHERE id BETWEEN 2 AND 32;")
        before = fold_raw_page(cluster, "postgres", 0, "recovery_cagra")
        offset = cluster.log.stat().st_size
        with subprocess.Popen(
            ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1",
             "-c", "SET cuvs.test_pause_page_vacuum=on;",
             "-c", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) recovery_rows;"],
            env={**cluster.env, "PGAPPNAME": "recovery-cancel-vacuum"},
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        ) as vacuum:
            try:
                wait_for(lambda: b"page-format VACUUM paused before publishing its generation"
                         in cluster.log.read_bytes()[offset:], "VACUUM did not reach count publication", 15)
                assert query(cluster, "cancel-vacuum", """
                    SELECT pg_cancel_backend(pid) FROM pg_stat_activity
                    WHERE application_name = 'recovery-cancel-vacuum';
                """) == "t"
                _, errors = vacuum.communicate(timeout=15)
                if vacuum.returncode == 0 or "canceling statement" not in errors:
                    raise RuntimeError("VACUUM did not cancel at the staged page boundary")
            finally:
                if vacuum.poll() is None:
                    vacuum.terminate()
        after = fold_raw_page(cluster, "postgres", 0, "recovery_cagra")
        block, = struct.unpack_from("=I", after, 160)
        tid_page = fold_raw_page(cluster, "postgres", block, "recovery_cagra")
        if (after[80:88] != bytes(8) or after[96:112] != before[96:112] or
                struct.pack("=3H", 65535, 65535, 0) not in tid_page[24:24 + 512 * 6]):
            raise RuntimeError("fixture lacks F=0 with physical losses beyond the persisted count")
        with recovery_daemon(cluster, binary, "base-vacuum-restart"):
            log = cluster.logs / "daemon-base-vacuum-restart.log"
            wait_for(lambda: "mode=hydrate graph_rows=512 slots=512" in log.read_text(),
                     "F=0 did not rehydrate after cancelled VACUUM")
            assertion(cluster, 450, False, "physical-count-exceeds-meta")
            execute(cluster, "publish-dead-count", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) recovery_rows;")
            wait_for(lambda: search(cluster, 500.25, True, "physical-count-repaired", 500),
                     "GPU route did not resume after PG published its dead count")
    finally:
        cluster.stop()


def check_delete_visibility(cluster, binary):
    cluster.start()
    try:
        with recovery_daemon(cluster, binary, "delete-visibility-build"):
            setup(cluster)
            artifact = base_artifact_dir(cluster)
        artifact.rename(cluster.data.parent / "before-delete")
        with subprocess.Popen(["psql", "-XqAt", "-v", "ON_ERROR_STOP=1"], env=cluster.env,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True) as reader:
            try:
                reader.stdin.write("BEGIN ISOLATION LEVEL REPEATABLE READ; "
                                   "SET ROLE recovery_owner; SELECT pg_current_snapshot();\n")
                reader.stdin.flush()
                if not select.select([reader.stdout], [], [], 15)[0] or ":" not in reader.stdout.readline():
                    raise RuntimeError("the older snapshot was not acquired before DELETE")
                execute(cluster, "delete-with-old-reader", "DELETE FROM recovery_rows WHERE id=256;")
                with recovery_daemon(cluster, binary, "delete-visibility-old"):
                    wait_for(lambda: search(cluster, 500.25, True, "new-reader-before-vacuum", 500),
                             "a still-visible old row prevented recovery")
                    out, err = reader.communicate(f"""
                        SET LOCAL enable_seqscan=off;
                        SET LOCAL cuvs.k=1;
                        SELECT id FROM recovery_rows WHERE active
                        ORDER BY {EXPR} <-> '[256,1,2,3]' LIMIT 1;
                        SELECT pg_cuvs_last_search_k();
                        ROLLBACK;
                    """, timeout=15)
                    rows = out.splitlines()
                    if reader.returncode or rows != ["256", "1"]:
                        raise RuntimeError(f"old snapshot lost its deleted row: {rows!r} {err!r}")
            finally:
                if reader.poll() is None:
                    reader.terminate()
        if artifact.exists():
            artifact.rename(cluster.data.parent / "after-old-reader")
        with recovery_daemon(cluster, binary, "delete-visibility-global"):
            wait_for(lambda: search(cluster, 500.25, True, "global-dead-before-vacuum", 500),
                     "a globally dead index TID blocked rehydration before VACUUM")
            if struct.unpack_from("=Q", fold_raw_page(cluster, "postgres", 0, "recovery_cagra"), 104)[0]:
                raise RuntimeError("the fixture unexpectedly let VACUUM count the deleted row")
    finally:
        cluster.stop()


def check(cluster, binary, case):
    if case == "reconcile":
        cluster.env["CUVS_TEST_PAGE_RECONCILE_DRY_RUN"] = "1"
    cluster.start()
    try:
        with recovery_daemon(cluster, binary, "recovery-build") as daemon:
            setup(cluster)
            if case == "proxy":
                execute(cluster, "legacy-drop-setup", """
                    CREATE TABLE recovery_legacy AS SELECT v FROM recovery_rows WHERE active;
                    CREATE INDEX recovery_legacy_cagra ON recovery_legacy
                    USING cagra ((v + '[0,0,0,0]'::vector) vector_l2_ops);
                """)
            base = head(cluster)
            if base[2:] != (512, 0):
                raise RuntimeError("the partial index included its false-predicate input")
            oid = int(query(cluster, "oid", "SELECT 'recovery_cagra'::regclass::oid"))
            db = int(query(cluster, "db", "SELECT oid FROM pg_database WHERE datname=current_database()"))
            incarnation = cluster.indexdir / f"{db}_{oid}" / f"{base[0]:016x}"
            graph = incarnation / f"{base[1]:016x}" / "index.cagra"
            manifest = graph.with_name("manifest")
            base_manifest, base_artifact = check_manifest(manifest, graph, base, db, oid)
            fold(cluster, 513, 640)
            middle = head(cluster)
            fold(cluster, 641, 768)
            execute(cluster, "delete", "DELETE FROM recovery_rows WHERE id=620;")
            execute(cluster, "hot", "SET ROLE recovery_owner; "
                                    "UPDATE recovery_rows SET note='HOT candidate' WHERE id=760;")
            execute(cluster, "cleanup", "VACUUM (INDEX_CLEANUP ON) recovery_rows;")
            durable_head = head(cluster)
            assertion(cluster, 760, True, "before-restart")
            current = incarnation / f"{durable_head[1]:016x}"
            if case == "reconcile":
                with active_creator(cluster) as xid:
                    check_reconciliation(cluster, incarnation, base, middle, durable_head,
                                         base_manifest, base_artifact, xid)
                return
            if case == "normal":
                checkpoint = current / "manifest"
                log = cluster.logs / "daemon-recovery-build.log"
                wait_for(lambda: checkpoint.is_file() and
                         f"nonce={durable_head[1]:016x} slots=768" in log.read_text(),
                         "no durable post-fold checkpoint")
                check_manifest(checkpoint, current / "index.cagra", durable_head, db, oid)
            else:
                daemon.kill()
                daemon.wait(timeout=15)

        if case != "normal":
            archived = incarnation.parent / "archived-checkpoints"
            archived.mkdir()
            for child in incarnation.iterdir():
                if child.is_dir():
                    child.rename(archived / child.name)
            if case == "foreign":
                wrong = base[1] ^ (1 << 63)
                if not wrong or wrong == durable_head[1]:
                    raise RuntimeError("unexpected nonce collision in fixture")
                foreign = incarnation / f"{wrong:016x}"
                foreign.mkdir()
                wire = bytearray(base_manifest)
                struct.pack_into("=Q", wire, 40, wrong)
                struct.pack_into("=I", wire, MANIFEST_CRC, crc32(wire[:-4]))
                (foreign / "manifest").write_bytes(wire)
                (foreign / "index.cagra").write_bytes(base_artifact)
            else:
                graph.parent.mkdir()
                manifest.write_bytes(base_manifest)
                if case != "absent":
                    graph.write_bytes(base_artifact)
                if case == "corrupt":
                    changed = bytearray(base_artifact)
                    changed[-4] ^= 1
                    graph.write_bytes(changed)
                if case == "manifest":
                    manifest.write_bytes(base_manifest[:-1] + bytes([base_manifest[-1] ^ 1]))
                if case == "manifest-truncated":
                    manifest.write_bytes(base_manifest[:-3])
                if case == "manifest-fields":
                    # A valid CRC cannot bless a protection flag with no boot.
                    wire = bytearray(base_manifest)
                    struct.pack_into("=I", wire, 100, 1)
                    struct.pack_into("=I", wire, MANIFEST_CRC, crc32(wire[:-4]))
                    manifest.write_bytes(wire)
                if case == "manifest-v1":
                    # Old files lack placeholder provenance, even with an old valid CRC.
                    prefix = bytearray(base_manifest[:104])
                    struct.pack_into("=I", prefix, 88, 1)
                    struct.pack_into("=I", prefix, 100, crc32(prefix[:-4]))
                    manifest.write_bytes(prefix)

        assertion(cluster, 760, False, "daemon-absent")
        if case == "normal":
            cluster.stop()
            cluster.start("normal-pg-restart")
        gate = cluster.data.parent / "page-recovery.fifo"
        shutdown_install = case in ("shutdown-install", "shutdown-install-timeout")
        if case == "proxy" or shutdown_install:
            os.mkfifo(gate)
            cluster.env["CUVS_TEST_PAGE_RECOVERY_INSTALL_FIFO"] = str(gate)

        with recovery_daemon(cluster, binary, "recovery-restart") as daemon:
            if shutdown_install:
                shutdown_paused_install(cluster, daemon, gate, expect_timeout=case.endswith("-timeout"))
                if head(cluster) != durable_head:
                    raise RuntimeError("shutdown changed PostgreSQL's published head")
                return
            if case == "proxy":
                gated_search(cluster, gate)
                gate.unlink()  # only the first install is gated
            wait_for(lambda: search(cluster, 760, True, "after-restart"), "GPU search never resumed")
            if head(cluster) != durable_head:
                raise RuntimeError("recovery modified PostgreSQL's published head")
            assertion(cluster, 533, True, "middle-slot")
            assertion(cluster, 500.25, True, "partial-exclusion", 500)
            log = cluster.logs / "daemon-recovery-restart.log"
            expected = {"normal": "checkpoint", "proxy": "proxy"}.get(case, "hydrate")
            rows = 512 if case == "proxy" else 768
            wait_for(lambda: f"mode={expected} graph_rows={rows} slots=768" in log.read_text(),
                     f"expected {expected} recovery with {rows} physical rows", 5)
            expected_probe = 760
            if case == "proxy":
                fold(cluster, 769, 896)
                assertion(cluster, 760, True, "proxy-suffix-after-fold")
                assertion(cluster, 887, True, "new-suffix-after-fold")
                expected_probe = 887
            cluster.stop()
            cluster.start("pg-restart-with-resident-daemon")
            wait_for(lambda: search(cluster, expected_probe, True, "new-pg-life"),
                     "new PG lifetime did not resume")
            if daemon.poll() is not None:
                raise RuntimeError("daemon exited during PostgreSQL restart")
    finally:
        cluster.stop()


def check_checkpoint_crash(cluster, binary, case):
    phase = SAVE_CRASH_PHASES[case]
    syscall_trace = cluster.logs / "checkpoint-syscalls"
    gate = cluster.data.parent / "checkpoint-crash.fifo"
    os.mkfifo(gate)
    cluster.env["CUVS_TEST_PAGE_CHECKPOINT_PHASE"] = phase
    cluster.env["CUVS_TEST_PAGE_CHECKPOINT_PHASE_FIFO"] = str(gate)
    cluster.start()
    try:
        with recovery_daemon(cluster, binary, "checkpoint-crash", syscall_trace=syscall_trace) as daemon:
            setup(cluster)
            base = head(cluster)
            base_dir = base_artifact_dir(cluster)
            db = int(query(cluster, "crash-db", "SELECT oid FROM pg_database WHERE datname=current_database()"))
            oid = int(query(cluster, "crash-oid", "SELECT 'recovery_cagra'::regclass::oid"))
            fold(cluster, 513, 640)
            middle = head(cluster)
            fold(cluster, 641, 768)
            current = head(cluster)
            directory = base_dir.parent / f"{current[1]:016x}"
            log = cluster.logs / "daemon-checkpoint-crash.log"
            prefix = f"nonce={current[1]:016x} phase="
            descriptor = None

            def reached():
                nonlocal descriptor
                if f"{prefix}{phase}" not in log.read_text():
                    return False
                descriptor = open_gate(gate)
                return descriptor is not None

            wait_for(reached, f"checkpoint did not reach {phase}", 140)
            try:
                published = phase in {"after-rename", "after-parent-fsync"}
                if (directory / "manifest").exists() != published:
                    raise RuntimeError("canonical checkpoint visibility disagrees with the cut")
                staging = None
                if published:
                    check_manifest(directory / "manifest", directory / "index.cagra", current, db, oid)
                else:
                    candidates = []
                    for path in directory.parent.glob(".save-*/manifest"):
                        wire = path.read_bytes()
                        if len(wire) == MANIFEST.size and MANIFEST.unpack(wire)[5:8] == current[1:]:
                            candidates.append(path.parent)
                    if len(candidates) != 1:
                        raise RuntimeError(f"expected one staged manifest for {current}: {candidates!r}")
                    staging = candidates[0]
                    check_manifest(staging / "manifest", staging / "index.cagra", current, db, oid)
                mid_dir = base_dir.parent / f"{middle[1]:016x}"
                previous, old = (middle, mid_dir) if (mid_dir / "manifest").is_file() else (base, base_dir)
                check_manifest(old / "manifest", old / "index.cagra", previous, db, oid)
                daemon.kill()
                daemon.wait(timeout=15)
                leader = syscall_trace.with_name(f"{syscall_trace.name}.{daemon.pid}")
                wait_for(lambda: leader.is_file() and "+++ killed by SIGKILL +++" in leader.read_text(),
                         f"strace did not record death of the checkpoint daemon pid {daemon.pid}", 10)
                assert_checkpoint_syscalls(syscall_trace, directory, phase, staging)
            finally:
                os.close(descriptor)

        # Model loss of the unsynced namespace entry independently of SIGKILL.
        if case == "save-crash-rename-lost":
            directory.rename(cluster.data.parent / "unsynced-rename-not-restored")
            published = False
        cluster.env.pop("CUVS_TEST_PAGE_CHECKPOINT_PHASE")
        cluster.env.pop("CUVS_TEST_PAGE_CHECKPOINT_PHASE_FIFO")
        assertion(cluster, 760, False, "checkpoint-crash-daemon-absent")
        with recovery_daemon(cluster, binary, "checkpoint-crash-restart"):
            log = cluster.logs / "daemon-checkpoint-crash-restart.log"
            expected = "mode=checkpoint graph_rows=768" if published else f"mode=proxy graph_rows={previous[2]}"
            wait_for(lambda: expected in log.read_text(), "restart chose a cache inconsistent with the PG head")
            assertion(cluster, 760, True, "checkpoint-crash-latest-row")
            if head(cluster) != current:
                raise RuntimeError("the cache crash changed the WAL head")
    finally:
        cluster.stop()


def check_reconcile_move(cluster, binary):
    gate = cluster.data.parent / "checkpoint-prune.fifo"
    os.mkfifo(gate)
    cluster.env["CUVS_TEST_PAGE_RECONCILE_PRUNE_FIFO"] = str(gate)
    cluster.env["CUVS_TEST_PAGE_RECONCILE_DRY_RUN"] = "1"
    cluster.start()
    try:
        with recovery_daemon(cluster, binary, "reconcile-move"):
            setup(cluster)
            source = base_artifact_dir(cluster)
            current = head(cluster)
            nonce = 0xA1B2
            name = f"{nonce:016x}"
            candidate = source.parent / name
            shutil.copytree(source, candidate)
            wire = bytearray((candidate / "manifest").read_bytes())
            struct.pack_into("=Q", wire, 40, nonce)
            struct.pack_into("=I", wire, MANIFEST_CRC, crc32(wire[:-4]))
            (candidate / "manifest").write_bytes(wire)
            aliases = (source.parent / ("0" + name), source.parent / name.upper())
            for alias in aliases:
                shutil.copytree(candidate, alias)
            descriptor = None

            def listed():
                nonlocal descriptor
                descriptor = open_gate(gate)
                return descriptor is not None

            wait_for(listed, "the canonical foreign checkpoint never reached PRUNE", 90)
            moved = cluster.data.parent / "externally-moved-checkpoint"
            try:
                candidate.rename(moved)
                shutil.copytree(moved, candidate)
            finally:
                gate.rename(cluster.data.parent / "checkpoint-prune-used.fifo")
                os.write(descriptor, b"x")
                os.close(descriptor)
            log = cluster.logs / "daemon-reconcile-move.log"
            wait_for(lambda: f"nonce={nonce:016x} stale_ref=1" in log.read_text(),
                     "PRUNE accepted a stale directory identity after an external move", 10)
            if any(not path.is_dir() for path in (moved, candidate, *aliases)) or head(cluster) != current:
                raise RuntimeError("external move changed an out-of-scope directory or the PG head")
            if "dry_run=1" in log.read_text():
                raise RuntimeError("the stale approval reached the destructive decision")
    finally:
        cluster.stop()


def check_restored_placeholder_shortage(cluster):
    # Keep the same ANN query, snapshot settings and disk head. Only heap
    # visibility changes, so even the live row winning the tie leaves a short
    # result with K=8. Roll back the test's deletes before the next restart.
    nearest = f"""
        SELECT id FROM recovery_rows
        WHERE active ORDER BY {EXPR} <-> '[500.25,1,2,3]' LIMIT 8
    """
    metadata = "encode(get_raw_page('recovery_cagra',0),'hex')"
    route = """
        concat_ws('|', coalesce(pg_cuvs_last_search_k()::text, 'exact'),
                  coalesce(pg_cuvs_last_search_n_results()::text, 'exact'))
    """
    output = execute(cluster, "restored-placeholder-shortage", f"""
        BEGIN;
        SET LOCAL enable_seqscan=off;
        SET LOCAL enable_bitmapscan=off;
        SET LOCAL cuvs.k=8;
        SELECT 'meta-before|' || {metadata};
        SET ROLE recovery_owner;
        SELECT 'rows-before|' || cardinality(ARRAY({nearest}));
        SELECT 'route-before|' || {route};

        DELETE FROM recovery_rows WHERE id <> 500;
        RESET ROLE;
        SELECT 'meta-after|' || {metadata};
        SET ROLE recovery_owner;
        SELECT 'rows-after|' || ARRAY({nearest})::text;
        SELECT 'route-after|' || {route};
        SET LOCAL enable_seqscan=on;
        SET LOCAL enable_indexscan=off;
        SET LOCAL enable_indexonlyscan=off;
        SELECT 'heap|' || ARRAY({nearest})::text;
        ROLLBACK;
    """)
    labels = {"meta-before", "meta-after", "rows-before", "route-before", "rows-after", "route-after", "heap"}
    records = {}
    for line in output.splitlines():
        key, _, value = line.partition("|")
        if key in labels:
            if key in records:
                raise RuntimeError(f"duplicate SQL oracle record {key!r}")
            records[key] = value
    if records.keys() != labels:
        raise RuntimeError(f"missing SQL oracle records: {sorted(labels - records.keys())}")
    if records["meta-before"] != records["meta-after"]:
        raise RuntimeError("MVCC shortage changed the checkpoint head or overfetch metadata")
    if (records["rows-before"], records["route-before"]) != ("8", "8|8"):
        raise RuntimeError("the restored checkpoint did not supply eight GPU candidates before DELETE")
    if records["heap"] != "{500}" or records["rows-after"] != records["heap"]:
        raise RuntimeError("the restored checkpoint did not preserve the sole visible row")
    if records["route-after"] != "exact|exact":
        raise RuntimeError(f"the restored placeholder guard did not switch to exact search: {records['route-after']!r}")


def check_saved_placeholders(cluster, binary):
    cluster.start()
    try:
        with recovery_daemon(cluster, binary, "placeholder-clean"):
            setup(cluster)
            artifact = base_artifact_dir(cluster)
        artifact.rename(cluster.data.parent / "clean-placeholder-baseline")
        execute(cluster, "placeholder-delete", "DELETE FROM recovery_rows WHERE id BETWEEN 2 AND 32;")
        target = head(cluster)
        db = int(query(cluster, "placeholder-db", "SELECT oid FROM pg_database WHERE datname=current_database()"))
        oid = int(query(cluster, "placeholder-oid", "SELECT 'recovery_cagra'::regclass::oid"))
        manifest = artifact / "manifest"
        graph = artifact / "index.cagra"
        with recovery_daemon(cluster, binary, "placeholder-write"):
            log = cluster.logs / "daemon-placeholder-write.log"
            wait_for(lambda: manifest.is_file() and "[page_checkpoint]" in log.read_text(),
                     "the recovered graph did not save its boot-bound checkpoint")
            wire, _ = check_manifest(manifest, graph, target, db, oid, protected=True)
            if MANIFEST.unpack(wire)[-2] != 0:
                raise RuntimeError("VACUUM unexpectedly counted the globally dead slots as physical")
            assertion(cluster, 500.25, True, "placeholder-before-reload", 500)
            fold(cluster, 513, 640)
            target = head(cluster)
            descendant = artifact.parent / f"{target[1]:016x}"
            manifest, graph = descendant / "manifest", descendant / "index.cagra"
            wait_for(lambda: manifest.is_file() and
                     f"nonce={target[1]:016x} slots=640" in log.read_text(),
                     "propagated placeholder protection was not checkpointed after fold")
            check_manifest(manifest, graph, target, db, oid, protected=True)

        with recovery_daemon(cluster, binary, "placeholder-same-boot"):
            log = cluster.logs / "daemon-placeholder-same-boot.log"
            wait_for(lambda: "mode=checkpoint graph_rows=640 slots=640 placeholders=1" in log.read_text(),
                     "the same-boot load lost manifest search protection")
            # A placeholder may tie the live row copied into its slot. Check
            # the SQL answer regardless of which ranking branch serves it.
            if not search(cluster, 1, True, "placeholder-ties-live-row"):
                assertion(cluster, 1, False, "placeholder-ties-live-row-exact")
            assertion(cluster, 500.25, True, "placeholder-after-reload", 500)
            check_restored_placeholder_shortage(cluster)

        cluster.stop()
        cluster.start("placeholder-new-pg-life")
        with recovery_daemon(cluster, binary, "placeholder-new-boot"):
            log = cluster.logs / "daemon-placeholder-new-boot.log"
            wait_for(lambda: "mode=hydrate graph_rows=640 slots=640 placeholders=1" in log.read_text(),
                     "another PG boot accepted the boot-bound native checkpoint")
            assertion(cluster, 500.25, True, "placeholder-new-boot-heap-proof", 500)
    finally:
        cluster.stop()


def check_retire_under_budget(cluster, binary):
    # 8,192 x (128 float values + 64 graph edges) = 6 MiB resident.
    # The normal recovery claim is 63.56 MiB at the default scratch ratio.
    # 66 MiB fits the claim alone, while claim plus retired resident cannot fit.
    budget_mib = 66
    nearest = """
        SELECT id FROM recovery_rows
        ORDER BY v <-> array_fill(7000::real, ARRAY[128])::vector LIMIT 1;
    """

    def pressure_search(gpu, label):
        oracle = query(cluster, label + "-heap", "SET enable_indexscan=off; " + nearest)
        rows = execute(cluster, label,
                       "SET enable_seqscan=off; SET cuvs.k=1; " + nearest +
                       "SELECT coalesce(pg_cuvs_last_search_k()::text, 'exact');").splitlines()[-2:]
        if oracle != "7000" or len(rows) != 2 or rows[0] != oracle:
            raise RuntimeError(f"{label}: unexpected nearest row {rows!r}")
        return rows[1].isdigit() == gpu

    cluster.start()
    try:
        with recovery_daemon(cluster, binary, "retire-pressure-base", vram_mib=budget_mib, max_indexes=1):
            execute(cluster, "retire-pressure-setup", """
                CREATE EXTENSION vector;
                CREATE EXTENSION pg_cuvs;
                CREATE EXTENSION pageinspect;
                CREATE TABLE recovery_rows (id integer PRIMARY KEY, v vector(128));
                INSERT INTO recovery_rows SELECT i, array_fill(i::real, ARRAY[128])::vector
                FROM generate_series(1,8192) i;
                CREATE INDEX recovery_cagra ON recovery_rows USING cagra(v vector_l2_ops);
            """)
            db = int(query(cluster, "pressure-db", "SELECT oid FROM pg_database WHERE datname=current_database()"))
            oid = int(query(cluster, "pressure-oid", "SELECT 'recovery_cagra'::regclass::oid"))
            incarnation, nonce, _, _ = head(cluster)
            artifact = cluster.indexdir / f"{db}_{oid}" / f"{incarnation:016x}" / f"{nonce:016x}"
        artifact.rename(cluster.data.parent / "pressure-pristine-checkpoint")
        execute(cluster, "retire-delete", "DELETE FROM recovery_rows WHERE id BETWEEN 2 AND 32;")
        gate = cluster.data.parent / "pressure-pin.fifo"
        os.mkfifo(gate)
        cluster.env["CUVS_TEST_PAGE_CHECKPOINT_PIN_FIFO"] = str(gate)
        with recovery_daemon(cluster, binary, "retire-pressure", vram_mib=budget_mib, max_indexes=1) as daemon:
            log = cluster.logs / "daemon-retire-pressure.log"
            wait_for(lambda: pressure_search(True, "pressure-old-boot"), "old PG life never recovered")
            descriptor = None

            def pinned():
                nonlocal descriptor
                descriptor = open_gate(gate)
                return descriptor is not None

            wait_for(pinned, "the old graph never reached the checkpoint pin", 20)
            start = log.stat().st_size
            try:
                cluster.stop()
                cluster.start("pressure-new-pg-life")
                wait_for(lambda: f"index={oid} retired=1" in log.read_text()[start:],
                         "the new PG life did not authorize retirement before admission")
                wait_for(lambda: f"index={oid} no_capacity=1" in log.read_text()[start:],
                         "the pinned retired graph did not keep its VRAM charge", 30)
                if not pressure_search(False, "pressure-pinned-old-boot"):
                    raise RuntimeError("recovery reused the charged VRAM before the last pin released")
                if "mode=hydrate" in log.read_text()[start:]:
                    raise RuntimeError("a new GPU graph registered with insufficient logical budget")
            finally:
                gate.rename(cluster.data.parent / "pressure-pin-used.fifo")
                os.write(descriptor, b"x")
                os.close(descriptor)
            wait_for(lambda: pressure_search(True, "pressure-after-release"),
                     "the new PG life never recovered after the last reference released")
            if daemon.poll() is not None or head(cluster)[:2] != (incarnation, nonce):
                raise RuntimeError("the daemon or PostgreSQL head changed during the retirement fixture")
    finally:
        cluster.stop()


def check_root_return(cluster, binary):
    cluster.start()
    try:
        with recovery_daemon(cluster, binary, "root-return"):
            setup(cluster)
            artifact = base_artifact_dir(cluster)
            nonce = head(cluster)[1]
            original = artifact.relative_to(cluster.indexdir)
            archived = cluster.data.parent / "original-cache-root"
            cluster.indexdir.rename(archived)
            cluster.indexdir.mkdir()
            assertion(cluster, 500.25, True, "resident-with-empty-root", 500)
            log = cluster.logs / "daemon-root-return.log"
            wait_for(lambda: (artifact / "manifest").is_file() and
                     f"nonce={nonce:016x} slots=512" in log.read_text(),
                     "resident graph was not checkpointed when the cache root returned", 90)
            if not (archived / original / "manifest").is_file():
                raise RuntimeError("the root-return fixture lost the original checkpoint")
    finally:
        cluster.stop()


def check_off(cluster, binary):
    cluster.start()
    try:
        with cluster.daemon(binary, "recovery-off"):
            execute(cluster, "setup-off", """
                CREATE EXTENSION vector;
                CREATE EXTENSION pg_cuvs;
                CREATE TABLE recovery_off (id integer, v vector(4));
                INSERT INTO recovery_off
                    SELECT i, format('[%s,2,3,4]', i)::vector FROM generate_series(1,512) i;
                CREATE INDEX recovery_off_idx ON recovery_off USING cagra (v vector_l2_ops);
            """)
            if query(cluster, "workers-off", """
                SELECT count(*) FROM pg_stat_activity
                WHERE backend_type LIKE 'pg_cuvs CAGRA recovery%';
            """) != "0" or list(cluster.indexdir.rglob("manifest")):
                raise RuntimeError("recovery worker or manifest appeared in a fold-OFF build")
            result = query(cluster, "legacy-query", """
                SET enable_seqscan=off;
                SELECT id FROM recovery_off ORDER BY v <-> '[337,2,3,4]' LIMIT 1;
            """)
            if result != "337":
                raise RuntimeError(f"OFF build returned {result!r} for legacy nearest row 337")
    finally:
        cluster.stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pg_bindir", type=Path)
    parser.add_argument("daemon", type=Path)
    parser.add_argument("--case", choices=("normal", "proxy", "shutdown-install", "shutdown-install-timeout",
                                 "corrupt", "manifest", "foreign", "absent", "off",
                                 "reconcile", "cancel-vacuum", "delete-visibility", "root-return",
                                 "saved-placeholders", "retire-pressure", "reconcile-move",
                                 "manifest-fields", "manifest-truncated", "manifest-v1", *SAVE_CRASH_PHASES),
                        required=True)
    parser.add_argument("--logs", type=Path, required=True)
    args = parser.parse_args()
    args.logs.mkdir()
    with tempfile.TemporaryDirectory(prefix="pgcuvs-recovery-") as scratch:
        cluster = Cluster(Path(scratch) / "pg", args.pg_bindir.resolve(), args.logs.resolve())
        binary = args.daemon.resolve()
        if args.case == "off":
            check_off(cluster, binary)
        elif args.case == "cancel-vacuum":
            check_cancelled_vacuum(cluster, binary)
        elif args.case == "delete-visibility":
            check_delete_visibility(cluster, binary)
        elif args.case == "root-return":
            check_root_return(cluster, binary)
        elif args.case == "saved-placeholders":
            check_saved_placeholders(cluster, binary)
        elif args.case == "retire-pressure":
            check_retire_under_budget(cluster, binary)
        elif args.case == "reconcile-move":
            check_reconcile_move(cluster, binary)
        elif args.case in SAVE_CRASH_PHASES:
            check_checkpoint_crash(cluster, binary, args.case)
        else:
            check(cluster, binary, args.case)

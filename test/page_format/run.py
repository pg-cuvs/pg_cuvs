"""Run the CAGRA page-format CPU matrix in disposable PG16 clusters."""

from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time


CASES = Path(__file__).resolve().parent
ROOT = CASES.parents[1]
MUTATIONS = {
    "wal-buffer": (
        "        append_item(image, tid);",
        "        append_item(BufferGetPage(full ? nb : tb), tid);",
    ),
    "wal-lower": ("    set_lower(page, pending->count * sizeof(*items));", ""),
    "unknown": ("    meta.major = CUVS_CAGRA_PAGE_MAJOR;",
                "    meta.major = CUVS_CAGRA_PAGE_MAJOR + 1;"),
    "flags": ("    meta.major = CUVS_CAGRA_PAGE_MAJOR;",
              "    meta.major = CUVS_CAGRA_PAGE_MAJOR;\n    meta.flags = 128;"),
}


def command(args, output, *, expect=None, env=None, timeout=300):
    args = [str(arg) for arg in args]
    with output.open("w") as log:
        log.write("$ " + " ".join(args) + "\n")
        log.flush()
        run = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, env=env, timeout=timeout)
    if expect is None and run.returncode == 0:
        return
    contents = output.read_text()
    if expect is not None and run.returncode != 0 and all(line in contents for line in expect):
        return
    print(contents, file=sys.stderr)
    raise RuntimeError(f"{output.name}: exit {run.returncode}, expected {expect or 'success'}")


def build(source, flag, output, *, install=None):
    options = [f"PG_CONFIG={PG_CONFIG}", "PGCUVS_CPU_SHIM=1", f"CUVS_PAGE_FORMAT={flag}",
               "with_llvm=no", "PG_CFLAGS=-DCUVS_TEST_HOOKS",
               "EXTRA_SERVER_CFLAGS=-DCUVS_TEST_HOOKS"]
    targets = ["all", "server"] if install is None else ["install", f"DESTDIR={install}"]
    jobs = min(8, len(os.sched_getaffinity(0)))
    command(["make", "-C", source, f"-j{jobs}", *options, *targets], output, timeout=900)


def copy_source(original, target):
    target.mkdir()
    for name in ("Makefile", "pg_cuvs.control"):
        shutil.copy2(original / name, target)
    for name in ("src", "sql"):
        shutil.copytree(original / name, target / name,
                        ignore=shutil.ignore_patterns("*.o", "*.so", "*.bc"))


class Cluster:
    def __init__(self, directory, bindir, logs):
        directory.mkdir()
        self.data = directory / "data"
        self.logs = logs
        self.log = logs / f"{directory.name}-postgres.log"
        self.indexdir = directory / "indexes"
        self.indexdir.mkdir()
        self.socket = directory / "daemon.sock"
        self.env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}",
                    "LC_ALL": "C", "PGHOST": str(directory), "PGPORT": "55432",
                    "PGUSER": "pages", "PGDATABASE": "postgres"}
        for key in ("PGOPTIONS", "PGSERVICE", "PGSERVICEFILE", "PGHOSTADDR"):
            self.env.pop(key, None)
        self.run("initdb", ["-D", self.data, "-U", "pages", "--auth=trust",
                            "--no-locale", "--no-sync"], "initdb")
        with (self.data / "postgresql.conf").open("a") as config:
            config.write(f"""
listen_addresses = ''
port = 55432
unix_socket_directories = '{directory}'
shared_preload_libraries = 'pg_cuvs'
cuvs.socket_path = '{self.socket}'
cuvs.index_dir = '{self.indexdir}'
autovacuum = off
bgwriter_lru_maxpages = 0
checkpoint_timeout = '30min'
""")

    def run(self, program, args, label, **kwargs):
        command([program, *args], self.logs / f"{self.data.parent.name}-{label}.log",
                env=self.env, **kwargs)

    def start(self, label="start"):
        self.run("pg_ctl", ["-D", self.data, "-l", self.log, "-w", "start"], label)

    def stop(self, mode="fast"):
        if (self.data / "postmaster.pid").exists():
            self.run("pg_ctl", ["-D", self.data, "-m", mode, "-w", "stop"], f"stop-{mode}")

    def sql(self, db, label, *args, **kwargs):
        self.run("psql", ["-X", "-v", "ON_ERROR_STOP=1", "-d", db, *args], label, **kwargs)

    def file(self, db, case, **kwargs):
        self.sql(db, case, "-f", CASES / f"{case}.sql", **kwargs)

    @contextmanager
    def daemon(self, binary, label, *, seed_bf=False, fault=None, vram_mib=None):
        env = self.env.copy()
        for key, enabled in (
            ("CUVS_FAULT_PAGE_BF_FIXTURE", seed_bf),
            ("CUVS_FAULT_PAGE_PERSIST", fault == "persist"),
            ("CUVS_FAULT_PAGE_ATTACH", fault == "attach"),
        ):
            env.pop(key, None)
            if enabled:
                env[key] = "1"
        args = [binary, "--socket", self.socket, "--index-dir", self.indexdir, "--gpu-devices", "0"]
        if vram_mib is not None:
            args.extend(["--max-vram-mb", str(vram_mib)])
        with (self.logs / f"daemon-{label}.log").open("w") as log, subprocess.Popen(
            args, stdout=log, stderr=subprocess.STDOUT, env=env,
        ) as daemon:
            try:
                for _ in range(200):
                    if self.socket.is_socket():
                        break
                    if daemon.poll() is not None:
                        raise RuntimeError(f"CPU daemon exited: {log.name}")
                    time.sleep(0.05)
                else:
                    raise RuntimeError(f"CPU daemon did not start: {log.name}")
                yield daemon
            finally:
                if daemon.poll() is None:
                    daemon.terminate()
                    try:
                        daemon.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        daemon.kill()
                        daemon.wait()
                self.socket.unlink(missing_ok=True)


def wal(cluster, *, mutant=None):
    cluster.start()
    try:
        cluster.sql("postgres", "extensions", "-c", "CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs;")
        cluster.file("postgres", "wal_prepare")
        cluster.sql("postgres", "wal-path", "-At", "-c", """
            SELECT pg_relation_filepath('page_durable_idx'), current_setting('block_size');
        """)
        row = (cluster.logs / f"{cluster.data.parent.name}-wal-path.log").read_text().splitlines()[-1]
        filename, block_size = row.split("|")
        cluster.stop("immediate")
        if mutant is None:
            # The original pending page was checkpointed before INSERT. Verify
            # that the new TID was still absent on disk before starting redo.
            with (cluster.data / filename).open("rb") as relation:
                relation.seek(int(block_size) + 24)
                before_redo = relation.read(6)
            if before_redo != b"\0" * 6:
                raise RuntimeError(f"checkpoint page already contains a pending TID: {before_redo.hex()}")
            print("wal: pending TID absent from the relation file before redo", flush=True)
        cluster.start("after-crash")
        # These mutations change the image before WAL is written. The
        # unmutated run above independently verifies the replay boundary.
        expect = ["pending TID was lost after WAL replay"] if mutant else None
        cluster.file("postgres", "wal_verify", expect=expect)
        print(f"{mutant or 'wal'}: {'mutation detected after WAL replay' if mutant else 'PASS'}", flush=True)
    finally:
        cluster.stop()


def check_off(cluster):
    cluster.start("flag-off")
    try:
        cluster.run("createdb", ["legacy"], "createdb-legacy")
        cluster.file("legacy", "legacy")
        check_monitor_during_reindex(cluster)
        cluster.sql("alpha1", "m8-analyze", "-c", "ANALYZE page_vectors;")
        for name, statement in {
            "search": "SET enable_seqscan=off; SELECT id FROM page_vectors ORDER BY v <-> '[0,0,0]' LIMIT 2;",
            "insert": "INSERT INTO page_vectors VALUES (5, '[0,0,1]', 'legacy');",
            "vacuum": "VACUUM (INDEX_CLEANUP ON) page_vectors;",
            "convert-sql": "SELECT pg_cuvs_build_hnsw('page_vectors_idx');",
            "convert-am": """CREATE INDEX page_legacy_hnsw ON page_vectors USING pg_cuvs_hnsw
                (v vector_l2_ops) WITH (source = 'page_vectors_idx');""",
        }.items():
            cluster.sql("alpha1", f"m8-{name}", "-v", "VERBOSITY=verbose", "-c", statement,
                        expect=["0A000", "cannot open the page-format CAGRA index",
                                "HINT:", "Use a build that supports the index format"])
        print("Flag-off legacy, ANALYZE and five page-format refusals: PASS", flush=True)
    finally:
        cluster.stop()


def check_monitor_during_reindex(cluster):
    with (cluster.logs / "legacy-reindex-session.log").open("w") as err, subprocess.Popen(
        ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", "legacy"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err, text=True, env=cluster.env,
    ) as guard:
        try:
            guard.stdin.write("BEGIN; REINDEX INDEX legacy_cagra_idx;\n\\echo locked\n")
            guard.stdin.flush()
            if not select.select([guard.stdout], [], [], 20)[0] or guard.stdout.readline().strip() != "locked":
                raise RuntimeError("fixture did not acquire the REINDEX lock")
            cluster.sql("legacy", "monitor-during-reindex", "-c", """
                SET lock_timeout = '500ms';
                DO $$
                BEGIN
                    IF NOT EXISTS (SELECT FROM pg_cuvs_gpu_search_stats()) OR
                       NOT EXISTS (SELECT FROM pg_cuvs_index_status()) THEN
                        RAISE EXCEPTION 'fixture did not expose a resident legacy index';
                    END IF;
                END $$;
            """)
        finally:
            if guard.poll() is None:
                guard.stdin.close()
                guard.wait(timeout=15)


def check_page_lookup(cluster, db, label, expected_k):
    cluster.sql(db, label, "-c", f"""
        SET enable_seqscan=off;
        SET cuvs.k=6;
        DO $$
        BEGIN
            IF (SELECT id FROM g14_pages
                 ORDER BY v <-> '[1,1,1,1,1,1,1,1]' LIMIT 1) IS DISTINCT FROM 1
               OR pg_cuvs_last_search_k() IS DISTINCT FROM {expected_k} THEN
                RAISE EXCEPTION 'wrong row or execution path for the page-format index';
            END IF;
        END $$;
    """)


def check_index_directory_aliases(cluster):
    symlink = cluster.data.parent / "aliases"
    symlink.symlink_to(cluster.indexdir, target_is_directory=True)
    cluster.sql("beta1a", "index-dir-aliases", "-c", f"""
        CREATE TABLE page_dir(v vector(3));
        INSERT INTO page_dir VALUES ('[1,0,0]'), ('[2,0,0]');
        CREATE INDEX page_dir_slash_idx ON page_dir USING cagra (v vector_l2_ops)
            WITH (index_dir = '{cluster.indexdir}/');
        CREATE INDEX page_dir_symlink_idx ON page_dir USING cagra (v vector_l2_ops)
            WITH (index_dir = '{symlink}');
    """)
    cluster.sql("beta1a", "index-dir-drop-key", "-At", "-c", """
        SELECT (SELECT oid FROM pg_database WHERE datname=current_database()) ||
               '_' || 'page_dir_symlink_idx'::regclass::oid;
    """)
    key = (cluster.logs / "main-index-dir-drop-key.log").read_text().splitlines()[-1]
    marker = cluster.indexdir / key
    if not marker.is_dir():
        raise RuntimeError("DROP fixture did not have graph storage")
    cluster.sql("beta1a", "index-dir-drop", "-c", "DROP INDEX page_dir_symlink_idx;")
    if marker.exists():
        raise RuntimeError("committed DROP retained the page directory")


def check_failed_persist(cluster, daemon):
    with cluster.daemon(daemon, "failed-persist", fault="persist"):
        cluster.start()
        try:
            cluster.sql("postgres", "setup", "-c", """
                CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs;
                CREATE TABLE page_fail(v vector(3));
                INSERT INTO page_fail VALUES ('[1,0,0]'), ('[2,0,0]');
            """)
            cluster.sql("postgres", "failed-build", "-c",
                        "CREATE INDEX ON page_fail USING cagra(v vector_l2_ops);",
                        expect=["page-format CAGRA build failed (status 6)"])
            if any(path.is_dir() for path in cluster.indexdir.iterdir()):
                raise RuntimeError("failed BUILD retained a private page directory")
        finally:
            cluster.stop()


def check_missing_dataset(cluster, daemon):
    with cluster.daemon(daemon, "missing-dataset", fault="attach"):
        cluster.start()
        try:
            cluster.sql("postgres", "attach", "-c", """
                CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs; CREATE EXTENSION pageinspect;
                CREATE TABLE page_graphless(id int, v vector(3));
                INSERT INTO page_graphless VALUES (1,'[1,0,0]'), (2,'[2,0,0]');
                CREATE INDEX page_graphless_idx ON page_graphless USING cagra(v vector_l2_ops);
                SET enable_seqscan=off;
                DO $$
                BEGIN
                    IF substr(get_raw_page('page_graphless_idx',0),65,8) <> decode('0000000000000000','hex')
                       OR (SELECT id FROM page_graphless ORDER BY v <-> '[1,0,0]' LIMIT 1)
                           IS DISTINCT FROM 1 OR pg_cuvs_last_search_k() IS NOT NULL THEN
                        RAISE EXCEPTION 'missing dataset did not produce a usable exact index without a graph';
                    END IF;
                END $$;
            """)
        finally:
            cluster.stop()


def check_slots(cluster, source):
    cluster.sql("beta1a", "pageinspect", "-c", "CREATE EXTENSION pageinspect;")
    cluster.sql("beta1a", "slots-meta", "-At", "-c", """
        SELECT (SELECT oid FROM pg_database WHERE datname=current_database()),
               'g14_idx'::regclass::oid, system_identifier,
               encode(get_raw_page('g14_idx',0),'hex')
          FROM pg_control_system();
    """)
    record = (cluster.logs / "main-slots-meta.log").read_text().splitlines()[-1]
    db, oid, sysid, raw = record.split("|")
    nonces_and_slots = struct.unpack_from("=QQQQ", bytes.fromhex(raw), 48)
    binary = source / "slots-test"
    command(["cc", "-std=gnu11", "-O2", "-I", source / "src", "-o", binary,
             CASES / "beta1a_slots.c", source / "src/cuvs_ipc.c",
             source / "src/cuvs_util.c", source / "src/cuvs_build_corpus.c",
             "-lm", "-lpthread", "-lrt"], cluster.logs / "slots-build.log")
    cluster.run(binary, [cluster.socket, db, oid, sysid, *nonces_and_slots], "slots")


def old_daemon_search(cluster):
    cluster.sql("beta1a", "old-daemon-search", "-c", """
        SET enable_seqscan=off;
        DO $$
        BEGIN
            IF (SELECT id FROM g14_pages
                 ORDER BY v <-> '[1,1,1,1,1,1,1,1]' LIMIT 1) IS DISTINCT FROM 1
               OR pg_cuvs_last_search_k() IS NOT NULL THEN
                RAISE EXCEPTION 'the new extension did not use the heap with the old daemon';
            END IF;
        END $$;
    """)
    cluster.run("createdb", ["old_build"], "createdb-old-build")
    cluster.file("old_build", "old_daemon")


def legacy_search(cluster, daemon, label):
    with cluster.daemon(daemon, label):
        cluster.start(label)
        try:
            cluster.run("createdb", [label], f"createdb-{label}")
            cluster.file(label, "legacy")
        finally:
            cluster.stop()


def check_deferral(cluster, daemon, library, enabled, disabled, *, mutant=False):
    shutil.copy2(disabled, library)
    with cluster.daemon(daemon, cluster.data.parent.name):
        cluster.start("g13-off")
        try:
            cluster.file("postgres", "g13_setup")
            legacy_files = sorted(cluster.indexdir.glob("*.cagra")) + sorted(cluster.indexdir.glob("*.tids"))
            if len(legacy_files) != 8:
                raise RuntimeError("expected four legacy graphs and TID files")
            before = {path: hashlib.sha256(path.read_bytes()).digest() for path in legacy_files}
        finally:
            cluster.stop()
        shutil.copy2(enabled, library)
        cluster.start("g13-on")
        try:
            cluster.file("postgres", "g13_rollback", expect=[
                "rollback displaced the dirty legacy graph",
            ] if mutant else None)
            if mutant:
                print("G13: rollback detected the discarded resident graph", flush=True)
                return
            cluster.sql("postgres", "g13-small", "-c", f"""
                ALTER INDEX page_to_one_idx SET (index_dir = '{cluster.data.parent / 'custom'}');
                REINDEX INDEX page_to_empty_idx;
                REINDEX INDEX page_to_one_idx;
                DO $$
                BEGIN
                    IF pg_relation_size('page_to_empty_idx') = 0 OR
                       pg_relation_size('page_to_one_idx') = 0 THEN
                        RAISE EXCEPTION 'small REINDEX did not use page storage';
                    END IF;
                END $$;
            """)
            cluster.sql("postgres", "g13-unload", "-c", """
                SELECT pg_cuvs_unpin('page_to_one_idx'::regclass);
                SELECT pg_cuvs_unload('page_to_one_idx'::regclass);
            """)
            if any(hashlib.sha256(path.read_bytes()).digest() != digest
                   for path, digest in before.items()):
                raise RuntimeError("REINDEX rewrote a legacy graph or TID file")
            local = list(cluster.indexdir.glob("*/*/empty.xid"))
            if len(local) != 2 or any(not int(path.read_text()) for path in local) or (
                    cluster.data.parent / "custom").exists():
                raise RuntimeError("empty builds did not use the daemon's index directory")
            cluster.sql("postgres", "unmarked-key", "-At", "-c", """
                SELECT (SELECT oid FROM pg_database WHERE datname=current_database()) ||
                       '_' || 'page_unmarked_idx'::regclass::oid;
            """)
            record = cluster.logs / f"{cluster.data.parent.name}-unmarked-key.log"
            key = record.read_text().splitlines()[-1]
            partial = cluster.indexdir / key / "0123456789abcdff"
            partial.mkdir(parents=True)
            (partial / "empty.xid").touch()
            (partial / "build.xid.tmp").write_text("1\n")
            (partial.parent / "build.xid").write_text("1\n")
        finally:
            cluster.stop()

    label = "restart-deferred"
    with cluster.daemon(daemon, label):
        cluster.start(label)
        try:
            startup = (cluster.logs / f"daemon-{label}.log").read_text()
            if "[startup] loaded 1 index(es)" not in startup:
                raise RuntimeError("startup did not distinguish completed and partial markers")
            cluster.sql("postgres", "g13-explicit", "-c", """
                SET enable_seqscan=off;
                DO $$
                BEGIN
                    IF (SELECT count(*) FROM pg_cuvs_gpu_search_stats()
                         WHERE index_oid='page_unmarked_idx'::regclass AND resident) <> 1 THEN
                        RAISE EXCEPTION 'partial marker incorrectly suppressed legacy startup';
                    END IF;
                    IF (SELECT id FROM page_dirty ORDER BY v <-> '[0,0,0,0,0,0,0,0]' LIMIT 1)
                       IS DISTINCT FROM 0 OR pg_cuvs_last_search_k() IS NULL THEN
                        RAISE EXCEPTION 'rollback legacy index could not explicitly reload';
                    END IF;
                END $$;
            """)
        finally:
            cluster.stop()
    print("G13: rollback EXTEND row, 0/1-row markers, deferral across restart and explicit v6 reload: PASS",
          flush=True)


def check_page_eviction(cluster, daemon, library, enabled, disabled):
    shutil.copy2(disabled, library)
    with cluster.daemon(daemon, "eviction-legacy"):
        cluster.start()
        try:
            cluster.sql("postgres", "legacy", "-c", """
                CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs;
                CREATE TABLE legacy_big(v vector(8));
                INSERT INTO legacy_big SELECT array_fill(i::real, ARRAY[8])::vector
                    FROM generate_series(1,100000) AS i;
                CREATE INDEX legacy_big_idx ON legacy_big USING cagra(v vector_l2_ops);
            """)
        finally:
            cluster.stop()

    shutil.copy2(enabled, library)
    with cluster.daemon(daemon, "eviction-low-budget", vram_mib=64):
        cluster.start("low-budget")
        try:
            cluster.sql("postgres", "prepare-eviction", "-c", """
                SELECT pg_cuvs_unload('legacy_big_idx'::regclass);
                CREATE TABLE page_lru(id int, v vector(8));
                INSERT INTO page_lru SELECT i,array_fill(i::real, ARRAY[8])::vector
                    FROM generate_series(1,4096) AS i;
                CREATE INDEX page_lru_idx ON page_lru USING cagra (v vector_l2_ops);
                SET enable_seqscan=off;
                SELECT id FROM page_lru ORDER BY v <-> '[1,1,1,1,1,1,1,1]' LIMIT 1;
            """)
            cluster.sql("postgres", "orphan-graphs", "-c", "BEGIN; REINDEX INDEX page_lru_idx; ROLLBACK;" * 40)
            cluster.sql("postgres", "load-with-eviction", "-c", """
                DO $$
                DECLARE
                    graphs integer := (SELECT count(*) FROM pg_cuvs_index_status()
                        WHERE index_oid='page_lru_idx'::regclass AND kind='page_cagra');
                BEGIN
                    IF graphs < 40 THEN
                        RAISE EXCEPTION 'fixture did not retain the unsearched page graphs';
                    END IF;
                    PERFORM pg_cuvs_load('legacy_big_idx'::regclass);
                    IF (SELECT count(*) FROM pg_cuvs_index_status()
                        WHERE index_oid='page_lru_idx'::regclass AND kind='page_cagra') >= graphs OR
                       NOT EXISTS (SELECT FROM pg_cuvs_index_status()
                        WHERE index_oid='page_lru_idx'::regclass AND kind='page_cagra'
                              AND last_use IS NOT NULL) THEN
                        RAISE EXCEPTION 'legacy LOAD did not prefer unsearched page graphs for eviction';
                    END IF;
                END $$;
            """)
            if "[page-evict]" not in (cluster.logs / "daemon-eviction-low-budget.log").read_text():
                raise RuntimeError("the legacy LOAD never evicted a page graph")
        finally:
            cluster.stop()
    print("Legacy LOAD reclaimed unsearched page graphs while retaining the active graph: PASS", flush=True)


def check_unsupported_vacuum(cluster, binary, mutation):
    with cluster.daemon(binary, mutation):
        cluster.start()
        try:
            cluster.file("postgres", "alpha2_unknown")
            if "unrecognized CAGRA metapage" not in (
                    cluster.logs / f"{mutation}-alpha2_unknown.log").read_text():
                raise RuntimeError("unknown metapage fallback was not observed")
            cluster.sql("postgres", "delete", "-c", "DELETE FROM unknown_layout WHERE id=2;")
            for case in ("vacuum", "vacuum-again"):
                cluster.sql("postgres", case, "-c",
                            "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) unknown_layout;")
                log = (cluster.logs / f"{mutation}-{case}.log").read_text()
                for index in ("unknown_layout_idx", "unknown_layout_idx2"):
                    if (log.count(f'WARNING:  VACUUM skipped unrecognized or damaged CAGRA index "{index}"') != 1
                            or log.count(f"HINT:  REINDEX INDEX public.{index} ") != 1):
                        raise RuntimeError(f"{case}: expected one VACUUM warning and REINDEX hint for {index}: {log}")
            flags = 129 if mutation == "flags" else 1
            cluster.sql("postgres", "reindex-mark", "-c", f"""
                DO $$
                BEGIN
                    IF (SELECT array_agg(get_byte(get_raw_page(index, 0), 32) ORDER BY index)
                        FROM unnest(ARRAY['unknown_layout_idx', 'unknown_layout_idx2']) AS index)
                        IS DISTINCT FROM ARRAY[{flags}, {flags}] THEN
                        RAISE EXCEPTION 'VACUUM did not set only the REINDEX flag';
                    END IF;
                    IF coalesce((SELECT lp_flags FROM heap_page_items(get_raw_page('unknown_layout', 0))
                        WHERE lp = 2), 0) <> 0 THEN
                        RAISE EXCEPTION 'VACUUM did not reclaim the dead heap TID';
                    END IF;
                END $$;
            """)
            print(f"{mutation}: heap fallback, write refusal, VACUUM and REINDEX mark: PASS", flush=True)
        finally:
            cluster.stop()


def check_corrupt_vacuum(cluster, binary):
    with cluster.daemon(binary, "corrupt"):
        cluster.start()
        try:
            cluster.sql("postgres", "prepare", "-c", """
                CREATE EXTENSION vector;
                CREATE EXTENSION pg_cuvs;
                CREATE EXTENSION pageinspect;
                CREATE TABLE corrupt_layout(id int PRIMARY KEY, v vector(3));
                INSERT INTO corrupt_layout
                SELECT i, CASE WHEN i = 6900 THEN '[2,0,0]' ELSE '[1,2,3]' END::vector
                FROM generate_series(1,6900) i;
                CREATE INDEX corrupt_layout_idx ON corrupt_layout USING cagra(v vector_l2_ops);
                DELETE FROM corrupt_layout WHERE id IN (1, 1400, 2800, 4200);
            """)
            cluster.sql("postgres", "path", "-At", "-c", """
                SELECT pg_relation_filepath('corrupt_layout_idx'), current_setting('block_size');
            """)
            filename, block_size = (cluster.logs / "corrupt-path.log").read_text().splitlines()[-1].split("|")
            block_size = int(block_size)
            cluster.stop()
            relation = cluster.data / filename
            with relation.open("r+b") as file:
                before = file.read()
                bad_page = 5 * block_size
                special = struct.unpack_from("=H", before, bad_page + 16)[0]
                file.seek(bad_page + special)
                file.write(b"\xff\xff")

            cluster.start("damaged")
            cluster.sql("postgres", "cached-write", "-c",
                        "INSERT INTO corrupt_layout VALUES (8000, '[0,0,0]');",
                        "-c", "DELETE FROM corrupt_layout WHERE id=8000;",
                        "-c", "CHECKPOINT;",
                        "-c", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) corrupt_layout;",
                        "-c", """
                DO $$
                BEGIN
                    IF get_byte(get_raw_page('corrupt_layout_idx', 0), 32) <> 1 THEN
                        RAISE EXCEPTION 'corrupt TID page did not cause a REINDEX mark';
                    END IF;
                    BEGIN
                        INSERT INTO corrupt_layout VALUES (8001, '[0,0,0]');
                        RAISE EXCEPTION 'cached pending tail bypassed the REINDEX mark';
                    EXCEPTION WHEN data_corrupted THEN
                        NULL;
                    END;
                END $$;
            """)
            log = (cluster.logs / "corrupt-cached-write.log").read_text()
            if log.count('WARNING:  VACUUM skipped unrecognized or damaged CAGRA index "corrupt_layout_idx"') != 1:
                raise RuntimeError(f"corrupt VACUUM did not warn once: {log}")
            cluster.sql("postgres", "flush-wal", "-c", "SELECT pg_switch_wal();")
            cluster.stop("immediate")
            if relation.read_bytes()[32] != 0:
                raise RuntimeError("REINDEX flag reached the index file before WAL replay")
            cluster.start("redo")
            cluster.sql("postgres", "redo-flag", "-c", """
                DO $$ BEGIN
                    IF get_byte(get_raw_page('corrupt_layout_idx', 0), 32) <> 1 THEN
                        RAISE EXCEPTION 'REINDEX mark lost after WAL replay';
                    END IF;
                END $$;
            """)
            cluster.stop()
            after = relation.read_bytes()
            # The checkpoint before VACUUM captured the appended pending TID.
            # Compare all earlier base pages, where a one-pass VACUUM could
            # have committed earlier batches before it saw the bad tag.
            if after[block_size:bad_page] != before[block_size:bad_page]:
                raise RuntimeError("VACUUM modified TID pages before detecting corruption")
            with relation.open("r+b") as file:
                file.seek(bad_page + special)
                file.write(before[bad_page + special:bad_page + special + 2])
            cluster.start("restored-tag")
            cluster.sql("postgres", "fallback-rebuild", "-c", """
                SET enable_seqscan = off;
                DO $$
                BEGIN
                    IF (SELECT id FROM corrupt_layout ORDER BY v <-> '[2,0,0]' LIMIT 1)
                        IS DISTINCT FROM 6900 THEN
                        RAISE EXCEPTION 'marked index did not return an exact result';
                    END IF;
                END $$;
                REINDEX INDEX corrupt_layout_idx;
                INSERT INTO corrupt_layout VALUES (8001, '[0,0,0]');
                DO $$ BEGIN
                    IF get_byte(get_raw_page('corrupt_layout_idx', 0), 32) <> 0 THEN
                        RAISE EXCEPTION 'REINDEX retained the marker';
                    END IF;
                END $$;
            """)
            if "unrecognized CAGRA metapage" not in (
                    cluster.logs / "corrupt-fallback-rebuild.log").read_text():
                raise RuntimeError("marked known format did not use the exact heap fallback")
            print("corrupt TID page: no TID changes, cached write refusal, WAL replay and REINDEX: PASS",
                  flush=True)
        finally:
            cluster.stop()


def check_corrupt_prefix(cluster, binary):
    indexes = ("future_idx", "short_idx", "bad_magic_idx", "bad_header_idx", "bad_slot_idx")
    names = ", ".join(f"'{name}'" for name in indexes)
    with cluster.daemon(binary, "prefix"):
        cluster.start()
        try:
            cluster.sql("postgres", "prepare", "-c", """
                CREATE EXTENSION vector;
                CREATE EXTENSION pg_cuvs;
                CREATE EXTENSION pageinspect;
                CREATE TABLE broken_prefix(id int PRIMARY KEY, v vector(3));
                INSERT INTO broken_prefix VALUES (1, '[1,2,3]'), (2, '[2,0,0]');
                CREATE TABLE prefix_reclaimed AS SELECT ctid AS old_tid FROM broken_prefix WHERE id=2;
            """ + "".join(f"CREATE INDEX {name} ON broken_prefix USING cagra(v vector_l2_ops) WHERE id <= 2;"
                          for name in indexes))
            cluster.sql("postgres", "paths", "-At", "-c", f"""
                SELECT name, pg_relation_filepath(name::regclass), current_setting('block_size')
                FROM unnest(ARRAY[{names}]) AS name;
            """)
            rows = [line for line in (cluster.logs / "prefix-paths.log").read_text().splitlines()
                    if line.count("|") == 2]
            if len(rows) != len(indexes):
                raise RuntimeError("did not resolve every damaged index fixture")
            cluster.stop()
            relations = {}
            pristine_meta = {}
            for row in rows:
                name, filename, block_size = row.split("|")
                relation = cluster.data / filename
                images = bytearray(relation.read_bytes())
                pristine_meta[name] = bytes(images[:int(block_size)])
                if name in ("future_idx", "short_idx"):
                    struct.pack_into("=H", images, 12, 36 if name == "future_idx" else 35)
                    struct.pack_into("=HHI", images, 28, 777, 12, 128)
                elif name == "bad_magic_idx":
                    struct.pack_into("=I", images, 24, 0)
                else:
                    block = int(block_size) if name == "bad_slot_idx" else 0
                    struct.pack_into("=H", images, block + 16, 0)
                relation.write_bytes(images)
                relations[name] = (relation, bytes(images), int(block_size))

            cluster.start("damaged")
            cluster.sql("postgres", "vacuum", "-c",
                        "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) broken_prefix;",
                        "-c", "DELETE FROM broken_prefix WHERE id=2;",
                        "-c", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) broken_prefix;")
            log = (cluster.logs / "prefix-vacuum.log").read_text()
            for name in indexes:
                warning = f'WARNING:  VACUUM skipped unrecognized or damaged CAGRA index "{name}"'
                if log.count(warning) != 2 or log.count(f"HINT:  REINDEX INDEX public.{name} ") != 2:
                    raise RuntimeError(f"expected a warning and hint for {name} in each VACUUM: {log}")

            cluster.sql("postgres", "worker-reader", "-c", """
                CREATE FUNCTION test_cagra_reindex_required(regclass) RETURNS bool
                AS '$libdir/pg_cuvs', 'cuvs_test_cagra_reindex_required' LANGUAGE C STRICT;
                DO $$ BEGIN
                    IF NOT test_cagra_reindex_required('future_idx')
                        OR NOT test_cagra_reindex_required('bad_slot_idx')
                        OR NOT test_cagra_reindex_required('short_idx')
                        OR NOT test_cagra_reindex_required('bad_magic_idx')
                        OR NOT test_cagra_reindex_required('bad_header_idx')
                        OR test_cagra_reindex_required('broken_prefix_pkey') THEN
                        RAISE EXCEPTION 'the rebuild worker reader misclassified a metapage';
                    END IF;
                END $$;
            """)
            # A durable commit after VACUUM flushes its WAL. Restore the earlier
            # undamaged copies of block zero while the cluster is stopped, then
            # check what PostgreSQL redo applies to those copies.
            cluster.sql("postgres", "flush-marker-wal", "-c", "CREATE TABLE prefix_wal_barrier(id int);")
            cluster.stop("immediate")
            repaired_by_image = ("short_idx", "bad_magic_idx", "bad_header_idx")
            for name in repaired_by_image:
                relation, _, _ = relations[name]
                with relation.open("r+b") as file:
                    file.write(pristine_meta[name])
            cluster.start("replay-old-prefix")
            cluster.sql("postgres", "replayed-prefix", "-c", """
                DO $$ BEGIN
                    IF NOT test_cagra_reindex_required('short_idx')
                        OR NOT test_cagra_reindex_required('bad_magic_idx')
                        OR NOT test_cagra_reindex_required('bad_header_idx') THEN
                        RAISE EXCEPTION 'WAL redo did not mark the undamaged copies of block zero';
                    END IF;
                END $$;

                INSERT INTO broken_prefix VALUES (3, '[2,0,0]');
                SET enable_seqscan=off;
                DO $$ BEGIN
                    IF (SELECT ctid FROM broken_prefix WHERE id=3)
                       IS DISTINCT FROM (SELECT old_tid FROM prefix_reclaimed) THEN
                        RAISE EXCEPTION 'fixture did not reuse the vacuumed heap TID';
                    END IF;
                    IF (SELECT id FROM broken_prefix WHERE id <= 2
                        ORDER BY v <-> '[2,0,0]' LIMIT 1) IS DISTINCT FROM 1
                        OR pg_cuvs_last_search_k() IS NOT NULL THEN
                        RAISE EXCEPTION 'a repaired prefix did not force the visible heap result';
                    END IF;
                END $$;
            """)
            cluster.stop()
            for name, (relation, before, block_size) in relations.items():
                after = relation.read_bytes()
                if name in ("future_idx", "bad_slot_idx"):
                    flag = struct.unpack_from("=I", after, 32)[0]
                    expected = 129 if name == "future_idx" else 1
                    if flag != expected or after[block_size:] != before[block_size:]:
                        raise RuntimeError(f"incorrect marker or data changes for {name}")
                else:
                    lower = struct.unpack_from("=H", after, 12)[0]
                    magic, _, _, flags = struct.unpack_from("=IHHI", after, 24)
                    if (lower != 36 or magic != 0x43565047 or flags != 1
                            or after[block_size:] != before[block_size:]):
                        raise RuntimeError(f"incorrect marker-only metapage or data changes for {name}")
            print("Damaged prefix: marker-only full-image replay onto valid metadata and TID reuse: PASS",
                  flush=True)
        finally:
            cluster.stop()


def check_page_mutations(work, logs, source, bindir, lib, *, u12_only):
    changed = source / "src/cuvs_cagra_pages.c"
    original = changed.read_text()
    for mutation, (old, new) in MUTATIONS.items():
        unsupported = mutation in ("unknown", "flags")
        if u12_only and not unsupported:
            continue
        if original.count(old) != 1:
            raise RuntimeError(f"non-unique mutation anchor: {mutation}")
        changed.write_text(original.replace(old, new, 1))
        build(source, 1, logs / f"build-{mutation}.log")
        shutil.copy2(source / "pg_cuvs.so", lib)
        fixture = Cluster(work / mutation, bindir, logs)
        if unsupported:
            check_unsupported_vacuum(fixture, source / "pg_cuvs_server", mutation)
        else:
            wal(fixture, mutant=mutation)


def run(work, logs, *, u12_only=False):
    paths = {key: Path(subprocess.check_output([PG_CONFIG, "--" + key], text=True).strip())
             for key in ("bindir", "pkglibdir", "sharedir")}
    stage = work / "pg"
    # PG16 resolves its binaries, extension libraries and SQL relative to postgres.
    for path in paths.values():
        shutil.copytree(path, stage / path.relative_to("/"), symlinks=True)
    bindir = stage / paths["bindir"].relative_to("/")
    lib = stage / paths["pkglibdir"].relative_to("/") / "pg_cuvs.so"

    source_on = work / "on"
    source_off = work / "off"
    copy_source(ROOT, source_on)
    build(source_on, 1, logs / "build-on.log")
    build(source_on, 1, logs / "install.log", install=stage)
    check_corrupt_vacuum(Cluster(work / "corrupt", bindir, logs), source_on / "pg_cuvs_server")
    check_corrupt_prefix(Cluster(work / "prefix", bindir, logs), source_on / "pg_cuvs_server")
    if u12_only:
        check_page_mutations(work, logs, source_on, bindir, lib, u12_only=True)
        return
    copy_source(ROOT, source_off)
    build(source_off, 0, logs / "build-off.log")
    enabled = work / "enabled.so"
    disabled = source_off / "pg_cuvs.so"
    shutil.copy2(lib, enabled)
    legacy_source = os.environ.get("PAGE_TEST_LEGACY_SOURCE")
    if legacy_source:
        source_base = work / "base"
        copy_source(Path(legacy_source), source_base)
        build(source_base, 0, logs / "build-base.log")

    cluster = Cluster(work / "main", bindir, logs)
    with cluster.daemon(source_on / "pg_cuvs_server", "on") as daemon:
        cluster.start()
        try:
            for case in ("alpha1", "alpha2_basic", "truncate", "relcache", "beta1a"):
                cluster.run("createdb", [case], f"createdb-{case}")
                cluster.file(case, case)
                print(f"{case}: PASS", flush=True)
            check_slots(cluster, source_on)
            cluster.file("beta1a", "beta1a_rollback")
            check_index_directory_aliases(cluster)
            cluster.run("createdb", ["-T", "beta1a", "page_template"], "template-copy")
            check_page_lookup(cluster, "page_template", "template-lookup", "NULL")
            check_page_lookup(cluster, "beta1a", "original-lookup", 6)
            cluster.run(sys.executable, [CASES / "alpha1_concurrency.py", "alpha1"], "alpha1-concurrency")
            cluster.run(sys.executable, [CASES / "alpha2_concurrency.py", "alpha2_basic", cluster.log],
                        "alpha2-concurrency")
            cluster.run("createdb", ["lifecycle"], "createdb-lifecycle")
            cluster.run(sys.executable, [CASES / "alpha2_lifecycle.py", "lifecycle", cluster.data, logs],
                        "lifecycle")
            print("Dimension, pending tail, CIC, canceled VACUUM, standby and no-preload: PASS", flush=True)
            cluster.stop()
            cluster.start("before-daemon-kill")
            check_page_lookup(cluster, "beta1a", "before-daemon-kill", 6)
            daemon.kill()
            daemon.wait(timeout=10)
            check_page_lookup(cluster, "beta1a", "after-daemon-kill", "NULL")
            print("Template identity, rolled-back TRUNCATE and REINDEX, daemon SIGKILL: PASS", flush=True)
        finally:
            cluster.stop()

    if legacy_source:
        with cluster.daemon(source_base / "pg_cuvs_server", "historical"):
            cluster.start("historical")
            try:
                old_daemon_search(cluster)
            finally:
                cluster.stop()

    dead = Cluster(work / "dead", bindir, logs)
    with dead.daemon(source_on / "pg_cuvs_server", "seeded-filter", seed_bf=True):
        dead.start()
        try:
            dead.file("postgres", "beta1a_dead")
        finally:
            dead.stop()

    check_deferral(Cluster(work / "g13", bindir, logs), source_on / "pg_cuvs_server",
                   lib, enabled, disabled)
    check_failed_persist(Cluster(work / "failed", bindir, logs), source_on / "pg_cuvs_server")
    check_missing_dataset(Cluster(work / "missing", bindir, logs), source_on / "pg_cuvs_server")
    check_page_eviction(Cluster(work / "eviction", bindir, logs), source_on / "pg_cuvs_server",
                        lib, enabled, disabled)

    wal(Cluster(work / "wal", bindir, logs))
    check_page_mutations(work, logs, source_on, bindir, lib, u12_only=False)

    shutil.copy2(disabled, lib)
    with cluster.daemon(source_off / "pg_cuvs_server", "off"):
        check_off(cluster)

    # Mark the old entry absent after page BUILD. Preserve the real daemon
    # binary for the mixed-version tests and isolate the mutant in its cluster.
    server_source = source_off / "src/pg_cuvs_server.c"
    server_text = server_source.read_text()
    original = """        if (page_status == 0 && (page_build->flags & CUVS_PAGE_BUILD_DEFER_LEGACY))
            ++g_page_deferral_epoch;"""
    if server_text.count(original) != 1:
        raise RuntimeError("non-unique legacy discard mutation anchor")
    server_source.write_text(server_text.replace(original, original + """
        if (page_status == 0)
        {
            IndexEntry *legacy = find_index(cmd->db_oid, cmd->index_oid);
            if (legacy)
                legacy->valid = 0;
        }""", 1))
    daemon = source_off / "pg_cuvs_server"
    original_daemon = work / "unmutated-daemon"
    shutil.copy2(daemon, original_daemon)
    build(source_off, 0, logs / "build-discarded-legacy.log")
    check_deferral(Cluster(work / "g13-mutant", bindir, logs), daemon,
                   lib, enabled, disabled, mutant=True)
    shutil.copy2(disabled, lib)

    if legacy_source:
        legacy_search(cluster, source_base / "pg_cuvs_server", "candidate_off")
        build(source_base, 0, logs / "install-base.log", install=stage)
        old_cluster = Cluster(work / "old", bindir, logs)
        legacy_search(old_cluster, original_daemon, "old_extension")
        for log in logs.glob("daemon-*.log"):
            if "IPC protocol mismatch" in log.read_text():
                raise RuntimeError(f"protocol mismatch reported in {log.name}")
        print("G14: old daemon and new extension in both modes; old extension and new daemon: PASS",
              flush=True)
    else:
        print("G14 mixed builds skipped: set PAGE_TEST_LEGACY_SOURCE to the reference checkout",
              flush=True)


if __name__ == "__main__":
    pg_config = shutil.which(sys.argv[1])
    if pg_config is None:
        raise SystemExit(f"PostgreSQL pg_config not found: {sys.argv[1]}")
    PG_CONFIG = Path(pg_config).resolve()
    with tempfile.TemporaryDirectory(prefix="pgcuvs-pages.", dir="/tmp") as directory:
        scratch = Path(directory)
        output = Path(os.environ.get("PAGE_TEST_OUTPUT", scratch / "logs")).resolve()
        output.mkdir(parents=True, exist_ok=True)
        run(scratch, output, u12_only="--u12-only" in sys.argv[2:])

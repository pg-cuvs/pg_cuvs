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
              "    meta.major = CUVS_CAGRA_PAGE_MAJOR;\n    meta.flags = 1;"),
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
               "EXTRA_SERVER_CFLAGS=-DCUVS_TEST_HOOKS -DPG_CUVS_DEBUG=1"]
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
        self.daemon_root = str(self.indexdir)
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
        args = [binary, "--socket", self.socket, "--index-dir", self.daemon_root, "--gpu-devices", "0"]
        if vram_mib is not None:
            args.extend(["--max-vram-mb", str(vram_mib)])
        with (self.logs / f"daemon-{label}.log").open("w") as log, subprocess.Popen(
            args, stdout=log, stderr=subprocess.STDOUT, env=env, cwd=self.data.parent,
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
        CREATE INDEX page_dir_relative_idx ON page_dir USING cagra (v vector_l2_ops)
            WITH (index_dir = 'aliases');
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


def check_custom_index_directory(cluster):
    custom = cluster.data.parent / "custom"
    custom.mkdir()
    alias = cluster.data.parent / "custom-link"
    alias.symlink_to(custom, target_is_directory=True)
    db = "beta1a"
    cluster.sql(db, "custom-build", "-At", "-c", f"""
        CREATE TABLE page_custom(v vector(3));
        INSERT INTO page_custom VALUES ('[1,0,0]'), ('[2,0,0]');
        CREATE TABLE page_small(v vector(3));
        CREATE TABLE page_zero(v vector(3));
        INSERT INTO page_small VALUES ('[1,0,0]');
        CREATE INDEX page_custom_idx ON page_custom USING cagra (v vector_l2_ops)
            WITH (index_dir = '{alias}/');
        CREATE INDEX page_small_idx ON page_small USING cagra (v vector_l2_ops)
            WITH (index_dir = '{alias}/');
        CREATE INDEX page_zero_idx ON page_zero USING cagra (v vector_l2_ops)
            WITH (index_dir = '{alias}/');
        SET cuvs.index_dir = '{custom}';
        CREATE INDEX page_guc_idx ON page_custom USING cagra (v vector_l2_ops);
        SELECT d.oid || '_' || i.idx::oid
        FROM pg_database d CROSS JOIN (VALUES (1, 'page_custom_idx'::regclass),
            (2, 'page_small_idx'::regclass), (3, 'page_zero_idx'::regclass),
            (4, 'page_guc_idx'::regclass)) AS i(ord, idx)
        WHERE d.datname = current_database()
        ORDER BY i.ord;
    """)
    keys = (cluster.logs / "main-custom-build.log").read_text().splitlines()[-4:]
    markers = ("*/*/build.xid", "*/empty.xid", "*/empty.xid", "*/*/build.xid")
    for key, marker in zip(keys, markers):
        witnesses = list((custom / key).glob(marker))
        if len(witnesses) != 1 or not int(witnesses[0].read_text()):
            default = list((cluster.indexdir / key).glob(marker))
            raise RuntimeError(f"{key}: expected {marker} in custom root, default markers={default}")
        if (cluster.indexdir / key).exists():
            raise RuntimeError(f"{key}: artifacts also written into daemon default")
    print("Per-index symlink and GUC roots for BUILD and empty markers: PASS", flush=True)
    return custom, keys


def check_invalid_index_directories(cluster):
    ordinary_file = cluster.data.parent / "not-a-directory"
    ordinary_file.touch()
    wrong_owner = Path("/")
    if wrong_owner.stat().st_uid == os.geteuid():
        raise RuntimeError("wrong-owner fixture requires an unprivileged daemon")
    missing = cluster.data.parent / "missing-root"
    for label, root in (("missing", missing), ("file", ordinary_file), ("owner", wrong_owner)):
        for table in ("page_custom", "page_small"):
            cluster.sql("beta1a", f"invalid-{table}-{label}", "-c", f"""
                CREATE INDEX page_invalid_idx ON {table} USING cagra (v vector_l2_ops)
                    WITH (index_dir = '{root}');
            """, expect=["page-format CAGRA index_dir", "HINT:", "owned by the daemon"])
    prefix = missing.with_name("x" * (255 - len(str(missing.parent)) - 1))
    prefix.mkdir()
    for table in ("page_custom", "page_small"):
        cluster.sql("beta1a", f"invalid-{table}-long", "-c", f"""
            CREATE INDEX page_invalid_idx ON {table} USING cagra (v vector_l2_ops)
                WITH (index_dir = '{prefix}x');
        """, expect=["page-format CAGRA index_dir", "HINT:", "owned by the daemon"])
    if any(prefix.iterdir()):
        raise RuntimeError("a truncated index_dir received artifacts")
    print("Missing, non-directory, wrong-owner and long roots rejected for BUILD and empty markers: PASS",
          flush=True)


def check_custom_index_drop(cluster, custom, keys):
    graph = keys[0]
    # The daemon should touch only the selected root and never follow symlinks
    # below the selected root.
    other = cluster.indexdir / graph
    other.mkdir()
    guard = other / "keep"
    guard.touch()
    (custom / graph / "outside-link").symlink_to(other, target_is_directory=True)
    sidecar = custom / f"{graph}.tids"
    sidecar.touch()
    tombstone = custom / f"{graph}.tombstone"
    tombstone.touch()
    other_tombstone = cluster.indexdir / tombstone.name
    other_tombstone.touch()
    cluster.sql("beta1a", "custom-drop-rollback", "-c", """
        BEGIN;
        SAVEPOINT before_drop;
        DROP INDEX page_custom_idx, page_small_idx;
        ROLLBACK TO before_drop;
        COMMIT;
    """)
    if not sidecar.is_file() or not all((custom / key).is_dir() for key in keys):
        raise RuntimeError("rolled-back DROP removed custom artifacts")
    cluster.sql("beta1a", "custom-drop", "-c", f"""
        SET cuvs.index_dir = '{custom}';
        BEGIN;
        SAVEPOINT before_drop;
        DROP INDEX page_custom_idx, page_small_idx, page_zero_idx, page_guc_idx;
        RELEASE SAVEPOINT before_drop;
        COMMIT;
    """)
    retained = [path for path in (sidecar, tombstone, *(custom / key for key in keys))
                if path.exists()]
    if retained or not guard.is_file() or not other_tombstone.is_file():
        raise RuntimeError(f"committed DROP retained {retained}; default guard={guard.is_file()}, "
                           f"default tombstone={other_tombstone.is_file()}")
    other_tombstone.unlink()
    guard.unlink()
    other.rmdir()
    print("Custom DROP removed committed graph, empty and legacy artifacts; rollback preserved them: PASS",
          flush=True)


def check_invalid_index_drop(cluster, custom):
    cluster.sql("beta1a", "invalid-drop-setup", "-At", "-c", f"""
        CREATE INDEX page_bad_drop_idx ON page_custom USING cagra (v vector_l2_ops)
            WITH (index_dir = '{custom}');
        SELECT oid || '_' || 'page_bad_drop_idx'::regclass::oid
        FROM pg_database WHERE datname = current_database();
    """)
    key = (cluster.logs / "main-invalid-drop-setup.log").read_text().splitlines()[-1]
    witness, = (custom / key).glob("*/*/build.xid")
    other = cluster.indexdir / key
    other.mkdir()
    missing = cluster.data.parent / "missing-root"
    cluster.sql("beta1a", "invalid-drop", "-At", "-c", f"""
        ALTER INDEX page_bad_drop_idx SET (index_dir = '{missing}');
        DROP INDEX page_bad_drop_idx;
        SELECT to_regclass('page_bad_drop_idx') IS NULL;
    """)
    result = (cluster.logs / "main-invalid-drop.log").read_text()
    if ("page-format DROP-notify failed" not in result or not result.endswith("\nt\n")
            or not witness.is_file() or not other.is_dir() or missing.exists()):
        raise RuntimeError("invalid DROP root did not warn, commit and leave both roots alone")
    other.rmdir()
    print("Invalid targeted DROP warns after commit and leaves both roots intact: PASS", flush=True)


def check_index_root_spellings(cluster):
    log = (cluster.logs / "daemon-on.log").read_text()
    for requested, selected in (
        (f"{cluster.indexdir}/", cluster.daemon_root),
        (cluster.data.parent / "aliases", cluster.daemon_root),
        ("aliases", cluster.daemon_root),
        (f"{cluster.data.parent / 'custom-link'}/", cluster.data.parent / "custom"),
    ):
        if f"page-format index_dir {requested} resolved to {selected}\n" not in log:
            raise RuntimeError(f"daemon did not use {selected} for {requested}")
    print("Default aliases use daemon spelling and a distinct root uses its realpath: PASS", flush=True)


def check_failed_persist(cluster, daemon):
    custom = cluster.data.parent / "custom"
    custom.mkdir()
    with cluster.daemon(daemon, "before-failed-persist"):
        cluster.start()
        try:
            cluster.sql("postgres", "setup", "-c", f"""
                CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs;
                CREATE TABLE page_fail(v vector(3));
                INSERT INTO page_fail VALUES ('[1,0,0]'), ('[2,0,0]');
                CREATE INDEX page_keep_idx ON page_fail USING cagra(v vector_l2_ops)
                    WITH (index_dir = '{custom}');
            """)
        finally:
            cluster.stop()
    before = {path: hashlib.sha256(path.read_bytes()).digest() if path.is_file() else None
              for path in custom.rglob("*")}
    if not any(path.name == "build.xid" for path in before):
        raise RuntimeError("failed-persist fixture is missing the original graph")

    failure = ["page-format CAGRA build failed (status 6)"]
    with cluster.daemon(daemon, "failed-persist", fault="persist"):
        cluster.start("fault-on")
        try:
            cluster.sql("postgres", "failed-build", "-c", f"""
                CREATE INDEX page_fail_idx ON page_fail USING cagra(v vector_l2_ops)
                    WITH (index_dir = '{custom}');
            """, expect=failure)
            cluster.sql("postgres", "failed-reindex", "-c", "REINDEX INDEX page_keep_idx;",
                        expect=failure)
            after = {path: hashlib.sha256(path.read_bytes()).digest() if path.is_file() else None
                     for path in custom.rglob("*")}
            if before != after or any(path.is_dir() for path in cluster.indexdir.iterdir()):
                raise RuntimeError("failed BUILD retained a private directory or touched a prior incarnation")
        finally:
            cluster.stop()
    print("Failed custom-root CREATE and REINDEX clean up and preserve the earlier incarnation: PASS",
          flush=True)


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
            cluster.sql(label, f"{label}-legacy-drop-key", "-At", "-c", """
                SELECT oid || '_' || 'legacy_cagra_idx'::regclass::oid
                FROM pg_database WHERE datname = current_database();
            """)
            key = (cluster.logs / f"{cluster.data.parent.name}-{label}-legacy-drop-key.log"
                   ).read_text().splitlines()[-1]
            graph = cluster.indexdir / f"{key}.cagra"
            if not graph.is_file():
                raise RuntimeError(f"{label}: missing legacy graph before DROP")
            cluster.sql(label, f"{label}-legacy-drop", "-c", "DROP INDEX legacy_cagra_idx;")
            if graph.exists():
                raise RuntimeError(f"{label}: legacy DROP did not remove graph")
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
            cluster.sql("postgres", "g13-small", "-c", """
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
            if len(local) != 2 or any(not int(path.read_text()) for path in local):
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


def run(work, logs):
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
    for source in (source_on, source_off):
        copy_source(ROOT, source)
    build(source_on, 1, logs / "build-on.log")
    build(source_on, 1, logs / "install.log", install=stage)
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
    daemon_alias = cluster.data.parent / "daemon-root"
    daemon_alias.symlink_to(cluster.indexdir, target_is_directory=True)
    cluster.daemon_root = str(daemon_alias)
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
            custom, keys = check_custom_index_directory(cluster)
            check_invalid_index_directories(cluster)
            check_index_root_spellings(cluster)
            check_custom_index_drop(cluster, custom, keys)
            check_invalid_index_drop(cluster, custom)
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
    changed = source_on / "src/cuvs_cagra_pages.c"
    original = changed.read_text()
    for mutation, (old, new) in MUTATIONS.items():
        if original.count(old) != 1:
            raise RuntimeError(f"non-unique mutation anchor: {mutation}")
        changed.write_text(original.replace(old, new, 1))
        build(source_on, 1, logs / f"build-{mutation}.log")
        shutil.copy2(source_on / "pg_cuvs.so", lib)
        fixture = Cluster(work / mutation, bindir, logs)
        if mutation in ("unknown", "flags"):
            with fixture.daemon(source_on / "pg_cuvs_server", mutation):
                fixture.start()
                try:
                    fixture.file("postgres", "alpha2_unknown")
                    if "unrecognized CAGRA metapage" not in (
                            logs / f"{mutation}-alpha2_unknown.log").read_text():
                        raise RuntimeError("unknown metapage fallback was not observed")
                    fixture.sql("postgres", "vacuum", "-v", "VERBOSITY=verbose",
                                "-c", "VACUUM (INDEX_CLEANUP ON) unknown_layout;",
                                expect=["XX001", "invalid CAGRA index page at block 0", "HINT:",
                                        "REINDEX INDEX unknown_layout_idx"])
                    print(f"{mutation}: heap fallback and write/VACUUM refusal: PASS", flush=True)
                finally:
                    fixture.stop()
        else:
            wal(fixture, mutant=mutation)

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
        run(scratch, output)

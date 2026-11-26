"""Run the CAGRA page-format CPU matrix in disposable PG16 clusters."""

from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time


CASES = Path(__file__).resolve().parent
ROOT = CASES.parents[1]
MUTATIONS = {
    "wal-buffer": (
        "        append_item(image, tid, vector);",
        "        append_item(BufferGetPage(full ? nb : tb), tid, vector);",
    ),
    "wal-lower": ("    set_lower(page, pending->count * stride);", ""),
    "wal-vector": (
        "        memcpy(item + sizeof(*tid), vector, (Size) pending->vector_dim * sizeof(float));",
        "        memset(item + sizeof(*tid), 0, (Size) pending->vector_dim * sizeof(float));",
    ),
    "unknown": ("    meta.major = CUVS_CAGRA_PAGE_MAJOR;",
                "    meta.major = CUVS_CAGRA_PAGE_MAJOR + 1;"),
    "flags": ("    meta.flags = pending_vectors ? CUVS_CAGRA_PENDING_VECTOR : 0;",
              "    meta.flags = 1;"),
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
    def daemon(self, binary, label, *, seed_bf=False, fault=None, vram_mib=None, max_indexes=None,
               overlay_host_bytes=None, overlay_gate_dir=None):
        env = self.env.copy()
        for gate in ("score", "receive"):
            key = f"CUVS_TEST_PAGE_OVERLAY_{gate.upper()}_FIFO"
            env.pop(key, None)
            if overlay_gate_dir is not None:
                env[key] = str(overlay_gate_dir / f"{gate}.fifo")
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
        if max_indexes is not None:
            args.extend(["--max-indexes", str(max_indexes)])
        if overlay_host_bytes is not None:
            args.extend(["--page-overlay-host-bytes", str(overlay_host_bytes)])
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
            SELECT pg_relation_filepath('page_durable_idx'),
                   pg_relation_filepath('page_durable_expr_idx'),
                   current_setting('block_size');
        """)
        row = (cluster.logs / f"{cluster.data.parent.name}-wal-path.log").read_text().splitlines()[-1]
        filename, expression_filename, block_size = row.split("|")
        cluster.stop("immediate")
        if mutant is None:
            # The original pending page was checkpointed before INSERT. Verify
            # that the new TID was still absent on disk before starting redo.
            with (cluster.data / filename).open("rb") as relation:
                relation.seek(int(block_size) + 24)
                before_redo = relation.read(6)
            if before_redo != b"\0" * 6:
                raise RuntimeError(f"checkpoint page already contains a pending TID: {before_redo.hex()}")
            with (cluster.data / expression_filename).open("rb") as relation:
                relation.seek(int(block_size) + 24)
                expression_before_redo = relation.read(18)
            if expression_before_redo != b"\0" * 18:
                raise RuntimeError("checkpoint page already contains a pending expression vector: "
                                   f"{expression_before_redo.hex()}")
            print("wal: pending TID and expression vector absent before redo", flush=True)
        cluster.start("after-crash")
        # These mutations change the image before WAL is written. The
        # unmutated run above independently verifies the replay boundary.
        expect = (["pending expression vector was lost after WAL replay"]
                  if mutant == "wal-vector" else
                  ["pending TID was lost after WAL replay"] if mutant else None)
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
    bad_roots = (
        ("missing", cluster.data.parent / "missing-root"),
        ("overlong", cluster.data.parent / ("x" * 200) / ("y" * 80)),
    )
    cluster.sql("beta1a", "invalid-drop-control", "-c",
                "CREATE INDEX page_drop_control_idx ON page_custom USING cagra (v vector_l2_ops);")
    for label, bad_root in bad_roots:
        cluster.sql("beta1a", f"invalid-drop-setup-{label}", "-At", "-c", f"""
            CREATE INDEX page_bad_drop_idx ON page_custom USING cagra (v vector_l2_ops)
                WITH (index_dir = '{custom}');
            SELECT oid || '_' || 'page_bad_drop_idx'::regclass::oid
            FROM pg_database WHERE datname = current_database();
        """)
        setup = cluster.logs / f"main-invalid-drop-setup-{label}.log"
        key = setup.read_text().splitlines()[-1]
        _, oid = (int(part) for part in key.split("_"))
        witness, = (custom / key).glob("*/*/build.xid")
        other = cluster.indexdir / key
        other.mkdir()
        cluster.sql("beta1a", f"invalid-drop-{label}", "-At", "-c", f"""
            DO $$
            BEGIN
                IF NOT EXISTS (
                    SELECT FROM pg_cuvs_index_status() WHERE index_oid={oid} AND kind='page_cagra'
                ) THEN
                    RAISE EXCEPTION 'invalid DROP fixture needs a resident page graph';
                END IF;
            END $$;
            ALTER INDEX page_bad_drop_idx SET (index_dir = '{bad_root}');
            DROP INDEX page_bad_drop_idx;
        """)
        cluster.sql("beta1a", f"invalid-drop-after-{label}", "-At", "-c", f"""
            SELECT to_regclass('page_bad_drop_idx') IS NULL;
            SELECT EXISTS (
                SELECT FROM pg_cuvs_index_status()
                WHERE index_oid='page_drop_control_idx'::regclass AND kind='page_cagra'
            );
            SELECT count(*) FROM pg_cuvs_index_status()
            WHERE index_oid={oid} AND kind='page_cagra';
        """)
        result = (cluster.logs / f"main-invalid-drop-{label}.log").read_text()
        after = (cluster.logs / f"main-invalid-drop-after-{label}.log").read_text().splitlines()
        if ("page-format DROP-notify failed" not in result or "(status 17)" not in result
                or after[-3:-1] != ["t", "t"]
                or not witness.is_file() or not other.is_dir() or bad_root.exists()):
            raise RuntimeError(f"{label}: invalid DROP root did not warn, commit and leave both roots alone")
        if after[-1] != "0":
            raise RuntimeError(f"{label}: invalid DROP root retained a resident page graph")
        other.rmdir()
    print("Invalid targeted DROP warns, clears the resident graph and leaves both roots intact: PASS",
          flush=True)


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


def page_slot_keys(cluster, index):
    label = f"{index}-slots-meta"
    cluster.sql("beta1a", label, "-At", "-c", f"""
        SELECT (SELECT oid FROM pg_database WHERE datname=current_database()),
               '{index}'::regclass::oid, system_identifier,
               encode(get_raw_page('{index}',0),'hex')
          FROM pg_control_system();
    """)
    record = (cluster.logs / f"{cluster.data.parent.name}-{label}.log").read_text().splitlines()[-1]
    db, oid, sysid, raw = record.split("|")
    return db, oid, sysid, *struct.unpack_from("=QQQQ", bytes.fromhex(raw), 48)


FOLD_PREPARE_SQL = """
    CREATE FUNCTION pg_temp.prepare_fold(regclass,integer,integer) RETURNS text
    AS '$libdir/pg_cuvs','cuvs_test_fold_prepare' LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;
"""

FOLD_PUBLISH_SQL = """
    CREATE FUNCTION pg_temp.publish_fold(regclass,integer,boolean) RETURNS text
    AS '$libdir/pg_cuvs','cuvs_test_fold_publish' LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;
"""

FOLD_LAUNCH_SQL = """
    CREATE FUNCTION pg_temp.cuvs_test_launch_fold_worker() RETURNS void
    AS '$libdir/pg_cuvs','cuvs_test_launch_fold_worker' LANGUAGE C VOLATILE PARALLEL UNSAFE;
"""


def fold_query(cluster, db, label, sql):
    cluster.sql(db, label, "-qAt", "-c", sql)
    return (cluster.logs / f"{cluster.data.parent.name}-{label}.log").read_text().splitlines()[-1]


def check_fold_prepare_fcl_release(cluster, db):
    for mode, expected in ((0, "prepared "), (2, "discard")):
        with subprocess.Popen(["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", db],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              env=cluster.env, text=True) as backend:
            try:
                backend.stdin.write(FOLD_PREPARE_SQL + "BEGIN;\n" +
                    f"SELECT pg_temp.prepare_fold('fold_prepare_idx',3,{mode});\n")
                backend.stdin.flush()
                if not select.select([backend.stdout], [], [], 10)[0]:
                    raise RuntimeError(f"fold preparation mode {mode} did not return")
                first = backend.stdout.readline().strip()
                if not first.startswith(expected):
                    raise RuntimeError(f"fold preparation mode {mode} returned {first!r}")
                second = fold_query(cluster, db, f"fold-fcl-release-{mode}", FOLD_PREPARE_SQL +
                    "SELECT pg_temp.prepare_fold('fold_prepare_idx',3,0);")
                if not second.startswith("prepared "):
                    raise RuntimeError(f"fold preparation mode {mode} retained the FCL: {second}")
            finally:
                backend.stdin.close()
                try:
                    backend.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    backend.kill()
                    backend.wait(timeout=5)


def setup_fold_publish(cluster, db):
    cluster.sql(db, "fold-publish-setup", "-c", """
        CREATE EXTENSION vector;
        CREATE EXTENSION pg_cuvs;
        CREATE EXTENSION pageinspect;
        CREATE TABLE fold_pub(id integer PRIMARY KEY, v vector(3));
        INSERT INTO fold_pub SELECT i, ARRAY[i::real,0,0]::vector FROM generate_series(1,1356) i;
        CREATE INDEX fold_pub_idx ON fold_pub USING cagra(v vector_l2_ops);
        INSERT INTO fold_pub VALUES (2001,'[1,0,0]'), (2002,'[2,0,0]'),
                                    (2003,'[3,0,0]'), (2004,'[4,0,0]');
        DELETE FROM fold_pub WHERE id=2002;
    """, "-c", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) fold_pub;")


def fold_raw_page(cluster, db, block, index="fold_pub_idx"):
    return bytes.fromhex(fold_query(cluster, db, f"{index}-block-{block}",
        f"SELECT encode(get_raw_page('{index}',{block}),'hex');"))


def check_fold_published_pages(cluster, db, before, transition_lsn):
    meta = fold_raw_page(cluster, db, 0)
    first = fold_raw_page(cluster, db, 1)
    second = fold_raw_page(cluster, db, 2)
    log_block, = struct.unpack_from("=I", meta, 124)
    log = fold_raw_page(cluster, db, log_block)
    n_slots, folded, base_slots, _, dead_slots = struct.unpack_from("=5Q", meta, 72)
    if meta[48:64] != before[48:64] or meta[64:72] == before[64:72]:
        raise RuntimeError("fold modified its base identity or retained the old graph identity")
    if (n_slots, folded, base_slots, dead_slots) != (1359, 3, 1356, 1):
        raise RuntimeError(f"invalid post-fold counts: {(n_slots, folded, base_slots, dead_slots)}")
    if log[48:72] != meta[64:88] or struct.unpack_from("=I", log, 8180)[0] != 2:
        raise RuntimeError("fold log and metapage disagree")
    if [struct.unpack_from("=H", page, 8172)[0] for page in (first, second)] != [0, 1]:
        raise RuntimeError("partial and staged TID page dead counts disagree")
    if transition_lsn is not None:
        page_lsns = [struct.unpack_from("=II", page) for page in (meta, first, log)]
        if page_lsns != [transition_lsn] * 3:
            raise RuntimeError(f"metadata, partial TID and fold log used different WAL: {page_lsns}")

    rows = fold_query(cluster, db, "fold-publish-live", """
        SELECT string_agg(ctid::text, ' ' ORDER BY id) FROM fold_pub WHERE id IN (2001,2003,2004);
    """).split()
    slots = []
    for page, offset in ((first, 1356), (second, 0), (second, 1)):
        hi, low, item = struct.unpack_from("=3H", page, 24 + offset * 6)
        slots.append(f"({(hi << 16) | low},{item})" if item else None)
    if slots != [rows[0], None, rows[1]]:
        raise RuntimeError(f"fold changed slot order or lost a dead position: {slots}")
    remaining = fold_query(cluster, db, "fold-publish-suffix", FOLD_PREPARE_SQL +
        "SELECT pg_temp.prepare_fold('fold_pub_idx',1,0);")
    if remaining != f"prepared {rows[2]}":
        raise RuntimeError(f"fold changed the pending suffix: {remaining}")
    return meta, first, second, log


def check_fold_publish(cluster, db):
    setup_fold_publish(cluster, db)
    before = fold_raw_page(cluster, db, 0)
    cluster.sql(db, "fold-publish-worker-gate", "-qAt", "-c", FOLD_PUBLISH_SQL +
        "SELECT pg_temp.publish_fold('fold_pub_idx',3,true);",
        expect=["CAGRA fold publication requires a standalone background worker operation"])
    if fold_raw_page(cluster, db, 0) != before:
        raise RuntimeError("a user SQL call published the fold")
    result = fold_query(cluster, db, "fold-publish", FOLD_PUBLISH_SQL + """
        BEGIN;
        SELECT pg_temp.publish_fold('fold_pub_idx',3,false);
        ROLLBACK;
    """)
    status, lsn = result.split()
    if status != "published":
        raise RuntimeError(f"synthetic fold failed: {result}")
    transition_lsn = tuple(int(word, 16) for word in lsn.split("/"))
    pages = check_fold_published_pages(cluster, db, before, transition_lsn)
    cluster.stop(mode="immediate")
    cluster.start(label="fold-publish-restart")
    if check_fold_published_pages(cluster, db, before, transition_lsn) != pages:
        raise RuntimeError("fold publication changed during immediate restart")
    cluster.sql(db, "fold-publish-vacuum", "-c", "DELETE FROM fold_pub WHERE id=2003;",
        "-c", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) fold_pub;")
    if struct.unpack_from("=Q", fold_raw_page(cluster, db, 0), 104)[0] != 2:
        raise RuntimeError("VACUUM did not invalidate the folded TID")
    print("Fold publication, flush, immediate restart and VACUUM: PASS", flush=True)


def check_fold_extent_reuse(cluster, db):
    cluster.sql(db, "fold-extent-setup", "-c", """
        CREATE TABLE fold_extent(id integer PRIMARY KEY, v vector(3));
        INSERT INTO fold_extent SELECT i, ARRAY[i::real,0,0]::vector FROM generate_series(1,86847) i;
        CREATE INDEX fold_extent_idx ON fold_extent USING cagra(v vector_l2_ops);
        INSERT INTO fold_extent
        SELECT i, ARRAY[i::real,0,0]::vector FROM generate_series(100001,101359) i;
    """)
    size_sql = "SELECT pg_relation_size('fold_extent_idx') / current_setting('block_size')::int;"
    before_blocks = int(fold_query(cluster, db, "fold-extent-before", size_sql))
    before_meta = fold_raw_page(cluster, db, 0, "fold_extent_idx")
    cluster.sql(db, "fold-extent-interrupt", "-c", FOLD_PUBLISH_SQL + """
        SET cuvs.test_abort_staged_fold = on;
        SELECT pg_temp.publish_fold('fold_extent_idx',2,false);
    """, expect=["stopped the fold after staging its TID pages"])
    staged_blocks = int(fold_query(cluster, db, "fold-extent-staged", size_sql))
    if staged_blocks != before_blocks + 64:
        raise RuntimeError("interrupted fold did not reserve one complete extent")
    if fold_raw_page(cluster, db, 0, "fold_extent_idx") != before_meta:
        raise RuntimeError("interrupted extent reservation published a new directory")
    staged_last = fold_raw_page(cluster, db, staged_blocks - 1, "fold_extent_idx")

    cluster.sql(db, "fold-extent-later-appends", "-c", """
        INSERT INTO fold_extent
        SELECT i, ARRAY[i::real,0,0]::vector FROM generate_series(200001,201500) i;
    """, "-c", "CHECKPOINT;")
    before_retry = int(fold_query(cluster, db, "fold-extent-before-retry", size_sql))
    if before_retry <= staged_blocks:
        raise RuntimeError("the pending list did not extend beyond the orphaned extent")
    cluster.stop(mode="immediate")
    cluster.start(label="fold-extent-restart")
    for count in (2, 1357):
        result = fold_query(cluster, db, f"fold-extent-publish-{count}", FOLD_PUBLISH_SQL +
            f"SELECT pg_temp.publish_fold('fold_extent_idx',{count},false);")
        if not result.startswith("published "):
            raise RuntimeError(f"fold of {count} positions failed: {result}")
    meta = fold_raw_page(cluster, db, 0, "fold_extent_idx")
    extent_count, = struct.unpack_from("=I", meta, 132)
    second_extent, = struct.unpack_from("=I", meta, 164)
    retry_blocks = int(fold_query(cluster, db, "fold-extent-after-retry", size_sql))
    if retry_blocks != before_retry or second_extent != before_blocks:
        raise RuntimeError("retry did not reclaim the orphaned extent after restart")
    next_page = fold_raw_page(cluster, db, second_extent + 1, "fold_extent_idx")
    if extent_count != 2 or struct.unpack_from("=Q", next_page, 8184)[0] != 88205:
        raise RuntimeError("the second fold did not use the next page of the existing extent")
    last_page = fold_raw_page(cluster, db, second_extent + 63, "fold_extent_idx")
    if struct.unpack_from("=II", last_page) <= struct.unpack_from("=II", staged_last):
        raise RuntimeError("reclaiming the extent did not WAL-log its final block")
    cluster.sql(db, "fold-extent-vacuum", "-c", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) fold_extent;")
    remaining = fold_query(cluster, db, "fold-extent-pending-suffix", FOLD_PREPARE_SQL + """
        SELECT pg_temp.prepare_fold('fold_extent_idx',1500,0) =
               (SELECT 'prepared ' || string_agg(ctid::text, ' ' ORDER BY id)
                FROM fold_extent WHERE id >= 200001);
    """)
    if remaining != "t":
        raise RuntimeError("extent reuse changed the pending TIDs appended after the interruption")

    published = fold_raw_page(cluster, db, second_extent, "fold_extent_idx")
    cluster.sql(db, "fold-extent-next-pending", "-c", """
        INSERT INTO fold_extent
        SELECT i, ARRAY[i::real,0,0]::vector FROM generate_series(300001,383991) i;
    """)
    before_third = int(fold_query(cluster, db, "fold-extent-before-third", size_sql))
    result = fold_query(cluster, db, "fold-extent-publish-third", FOLD_PUBLISH_SQL + """
        SELECT pg_temp.publish_fold('fold_extent_idx',85491,false);
    """)
    if not result.startswith("published "):
        raise RuntimeError(f"fold crossing the third extent failed: {result}")
    meta = fold_raw_page(cluster, db, 0, "fold_extent_idx")
    extent_count, = struct.unpack_from("=I", meta, 132)
    second_after, third_extent = struct.unpack_from("=2I", meta, 164)
    if (extent_count, second_after, third_extent) != (3, second_extent, before_third):
        raise RuntimeError("a published extent was reclaimed")
    if fold_raw_page(cluster, db, second_extent, "fold_extent_idx") != published:
        raise RuntimeError("crossing into the third extent rewrote published TIDs")

    reclaim_from, = struct.unpack_from("=I", meta, 136)
    if reclaim_from != third_extent:
        raise RuntimeError("a fresh allocation did not advance the reclaim scan")
    cluster.stop(mode="immediate")
    cluster.start(label="fold-reclaim-watermark-restart")
    if fold_raw_page(cluster, db, 0, "fold_extent_idx") != meta:
        raise RuntimeError("WAL replay changed the reclaim boundary or extent directory")
    third_first = fold_raw_page(cluster, db, third_extent, "fold_extent_idx")
    cluster.sql(db, "fold-extent-fourth-pending", "-c", """
        INSERT INTO fold_extent
        SELECT i, ARRAY[i::real,0,0]::vector FROM generate_series(400001,486848) i;
    """)
    before_fourth = int(fold_query(cluster, db, "fold-extent-before-fourth", size_sql))
    result = fold_query(cluster, db, "fold-extent-publish-fourth", FOLD_PUBLISH_SQL + """
        SELECT pg_temp.publish_fold('fold_extent_idx',86848,false);
    """)
    if not result.startswith("published "):
        raise RuntimeError(f"fold crossing the fourth extent failed: {result}")
    meta = fold_raw_page(cluster, db, 0, "fold_extent_idx")
    extent_count, = struct.unpack_from("=I", meta, 132)
    third_after, fourth_extent = struct.unpack_from("=2I", meta, 168)
    if (extent_count, third_after, fourth_extent) != (4, third_extent, before_fourth):
        raise RuntimeError("a scanned published extent was reclaimed")
    old_tid = third_first[24:30]
    if fold_raw_page(cluster, db, third_extent, "fold_extent_idx")[24:30] != old_tid:
        raise RuntimeError("crossing into the fourth extent overwrote a published TID")
    print("Fold extent rollover and reuse: PASS", flush=True)


def check_fold_worker_wire(cluster, source):
    db = "postgres"
    work = cluster.data.parent
    event_wire = struct.Struct("=IIIIQQQQQQQQQ")
    query_wire = struct.Struct("=IIQQ")

    def read_frame(peer, size):
        frame = bytearray()
        while len(frame) < size:
            chunk = peer.recv(size - len(frame))
            if not chunk:
                raise EOFError("incomplete fold frame")
            frame.extend(chunk)
        return frame

    probe = work / "frame.c"
    probe.write_text(r"""
        #include <stddef.h>
        #include <stdio.h>
        #include "cuvs_ipc.h"
        int main(void) {
            printf("%zu %zu %zu %u %u %d %d %d\n", sizeof(CuvsCmdFrame),
                   sizeof(CuvsReplyHeader), offsetof(CuvsReplyHeader, status),
                   CUVS_PROTO_MAGIC, CUVS_PROTO_VERSION, CUVS_OP_FOLD_READY,
                   CUVS_OP_FOLD_COMMITTED, CUVS_OP_FOLD_DISCARD);
            return 0;
        }
    """)
    subprocess.run(["cc", "-I", str(source / "src"), probe, "-o", work / "frame"], check=True)
    cmd_size, reply_size, status_offset, magic, version, ready, committed, discard = map(
        int, subprocess.check_output([work / "frame"], text=True).split())

    # PostgreSQL logs registration even for a launcher with no database connection.
    with (cluster.data / "postgresql.conf").open("a") as config:
        config.write("log_min_messages = debug1\n")
    with cluster.daemon(source / "pg_cuvs_server", "setup", overlay_host_bytes=4 * 1024**3):
        cluster.start()
        try:
            setup_fold_publish(cluster, db)
            value = fold_query(cluster, db, "index-identity", """
                SELECT (SELECT oid FROM pg_database WHERE datname=current_database()),
                       'fold_pub_idx'::regclass::oid, system_identifier
                  FROM pg_control_system();
            """)
            db_oid, index_oid, sysid = map(int, value.split("|"))
            old_meta = fold_raw_page(cluster, db, 0)
        finally:
            cluster.stop()

    index_nonce, base_nonce, parent_nonce, parent_l, parent_f = struct.unpack_from("=5Q", old_meta, 48)
    nonce, stale_nonce = [value for value in (1, 2, 3) if value != parent_nonce][:2]
    old_log_block, = struct.unpack_from("=I", old_meta, 124)

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(str(cluster.socket))
        listener.listen(16)
        listener.settimeout(10)

        @contextmanager
        def request(label, op, target_index):
            try:
                peer, _ = listener.accept()
            except TimeoutError as error:
                raise RuntimeError(f"{label}: no IPC request for operation {op}") from error
            with peer:
                peer.settimeout(3)
                header = struct.unpack_from("=IIIII", read_frame(peer, cmd_size))
                if header != (magic, version, op, db_oid, target_index):
                    raise RuntimeError(f"{label}: unexpected IPC header {header}")
                yield peer

        def reply(peer, body=b""):
            header = bytearray(reply_size)
            struct.pack_into("=I", header, status_offset, 0)
            peer.sendall(header + body)

        def check_workers(label, expected):
            state = fold_query(cluster, db, f"{label}-workers", """
                SELECT count(*), count(*) FILTER (
                    WHERE datname=current_database() AND backend_type='pg_cuvs fold worker')
                  FROM pg_stat_activity
                 WHERE backend_type LIKE 'pg_cuvs fold %' OR backend_type LIKE 'pg_cuvs page %';
            """)
            if state != f"{expected}|{expected}":
                raise RuntimeError(f"{label}: expected {expected} fold worker(s), got {state}")

        def check_idle(label):
            check_workers(label, 0)
            if select.select([listener], [], [], 0)[0]:
                raise RuntimeError(f"{label}: unexpected IPC connection outside a fold step")

        def launch(label, graph_nonce, result_op=None, *, lose_ack=False):
            check_idle(f"{label}-before")
            output = cluster.logs / f"{work.name}-{label}-launch.log"
            with output.open("w") as log, subprocess.Popen(
                ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", db, "-c", FOLD_LAUNCH_SQL,
                 "-c", "SELECT pg_temp.cuvs_test_launch_fold_worker();"],
                env=cluster.env, stdout=log, stderr=subprocess.STDOUT,
            ) as launcher:
                try:
                    with request(label, ready, 0) as peer:
                        length, reserved, current_sysid, boot = query_wire.unpack(
                            read_frame(peer, query_wire.size))
                        if (length, reserved, current_sysid) != (query_wire.size, 0, sysid) or not boot:
                            raise RuntimeError(f"{label}: invalid READY query")
                        check_workers(f"{label}-ready", 1)
                        event = event_wire.pack(event_wire.size, db_oid, index_oid, 3, sysid, boot,
                                                0xD00D, index_nonce, base_nonce, parent_nonce,
                                                graph_nonce, parent_l, parent_f)
                        reply(peer, event)
                    lsn = 0
                    if result_op is not None:
                        with request(label, result_op, index_oid) as peer:
                            result = read_frame(peer, event_wire.size + 8)
                            lsn, = struct.unpack_from("=Q", result, event_wire.size)
                            if result[:event_wire.size] != event or bool(lsn) != (result_op == committed):
                                raise RuntimeError(f"{label}: unexpected fold result or LSN {lsn:#x}")
                            # Closing after reading COMMITTED simulates a lost ACK.
                            if not lose_ack:
                                reply(peer)
                    if launcher.wait(timeout=10) != 0:
                        raise RuntimeError(f"{label}: SQL launcher failed\n{output.read_text()}")
                finally:
                    if launcher.poll() is None:
                        launcher.kill()
            check_idle(f"{label}-after")
            return event, lsn

        def fence_pid():
            return fold_query(cluster, db, "fold-fence-pid", """
                SELECT coalesce(max(a.pid),0)
                  FROM pg_stat_activity a JOIN pg_locks l ON a.pid=l.pid
                 WHERE a.datname=current_database() AND a.application_name='fold-worker-fence'
                   AND l.database=a.datid AND l.relation='fold_pub_idx'::regclass AND l.page=0
                   AND l.locktype='page' AND l.mode='ExclusiveLock' AND l.granted;
            """)

        cluster.start()
        try:
            check_idle("postmaster-start")
            registrations = [line for line in cluster.log.read_text().splitlines()
                             if 'registering background worker "pg_cuvs fold ' in line
                             or 'registering background worker "pg_cuvs page ' in line]
            if registrations:
                raise RuntimeError(f"automatic fold worker registration: {registrations}")
            old_log = fold_raw_page(cluster, db, old_log_block)
            cluster.sql(db, "make-base-dead", "-c", "DELETE FROM fold_pub WHERE id=42;")
            vacuum_log = cluster.logs / f"{work.name}-vacuum-fence.log"
            with vacuum_log.open("w") as log, subprocess.Popen(
                ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", db,
                 "-c", "SET cuvs.test_pause_page_vacuum=on",
                 "-c", "VACUUM (INDEX_CLEANUP ON, TRUNCATE OFF) fold_pub"],
                env={**cluster.env, "PGAPPNAME": "fold-worker-fence"},
                stdout=log, stderr=subprocess.STDOUT,
            ) as vacuum:
                try:
                    for _ in range(200):
                        if "page-format VACUUM paused before publishing its generation" in cluster.log.read_text():
                            break
                        if vacuum.poll() is not None:
                            raise RuntimeError(f"VACUUM exited before the FCL pause: {vacuum_log.read_text()}")
                        time.sleep(0.05)
                    else:
                        raise RuntimeError("VACUUM did not reach the FCL pause")
                    pid = int(fence_pid())
                    if not pid:
                        raise RuntimeError("paused VACUUM does not hold the FCL")
                    deferred, _ = launch("fold-deferred", nonce)
                    if int(fence_pid()) != pid:
                        raise RuntimeError("VACUUM released the FCL during the deferred attempt")
                    if (fold_raw_page(cluster, db, 0), fold_raw_page(cluster, db, old_log_block)) != (old_meta, old_log):
                        raise RuntimeError("fold changed metadata or its log while VACUUM held the FCL")
                    if fold_query(cluster, db, "release-fence", f"SELECT pg_cancel_backend({pid});") != "t":
                        raise RuntimeError("could not cancel the fenced VACUUM")
                    if vacuum.wait(timeout=10) == 0 or "canceling statement due to user request" not in vacuum_log.read_text():
                        raise RuntimeError(f"expected VACUUM cancellation: {vacuum_log.read_text()}")
                finally:
                    if vacuum.poll() is None:
                        vacuum.kill()

            published, lsn = launch("fold-publish-lost-ack", nonce, committed, lose_ack=True)
            meta = fold_raw_page(cluster, db, 0)
            if struct.unpack_from("=4Q", meta, 64) != (nonce, parent_l + 3, parent_f + 3, parent_l):
                raise RuntimeError("fold did not publish the offered generation")
            log_block, = struct.unpack_from("=I", meta, 124)
            fold_log = fold_raw_page(cluster, db, log_block)
            hi, lo = struct.unpack_from("=II", fold_log)
            fold_lsn = (hi << 32) | lo
            if struct.unpack_from("=I", fold_log, 8180)[0] != 2 or lsn != fold_lsn:
                raise RuntimeError("COMMITTED LSN did not match the published fold log")

            replayed, replay_lsn = launch("fold-replay", nonce, committed)
            if deferred != published or published != replayed or lsn != replay_lsn:
                raise RuntimeError("READY redelivery or COMMITTED replay changed the event or LSN")
            if (fold_raw_page(cluster, db, 0), fold_raw_page(cluster, db, log_block)) != (meta, fold_log):
                raise RuntimeError("replayed COMMITTED changed the published metapage or fold log")

            launch("fold-discard-stale-parent", stale_nonce, discard)
            if (fold_raw_page(cluster, db, 0), fold_raw_page(cluster, db, log_block)) != (meta, fold_log):
                raise RuntimeError("stale parent changed the published metapage or fold log")
            check_idle("fold-complete")
        finally:
            cluster.stop()
    print("Fold worker: FCL deferred, READY redelivered, lost ACK replayed at the same LSN, "
          "stale parent discarded, no automatic or residual worker: PASS", flush=True)


def check_fold_worker_ddl(cluster):
    db = "fold_target"

    @contextmanager
    def session(role):
        output = cluster.logs / f"{cluster.data.parent.name}-{role}-session.log"
        with output.open("w") as log, subprocess.Popen(
            ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-U", role, "-d", db],
            env=cluster.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True,
        ) as holder:
            try:
                holder.stdin.write("SELECT pg_backend_pid();\n")
                holder.stdin.flush()
                if not select.select([holder.stdout], [], [], 10)[0]:
                    raise RuntimeError(f"{role}: database session did not start")
                pid = holder.stdout.readline().strip()
                if not pid.isdecimal():
                    raise RuntimeError(f"{role}: failed to get a backend PID: {output.read_text()}")
                yield int(pid)
            finally:
                holder.stdin.close()
                try:
                    holder.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    holder.kill()
                    holder.wait(timeout=5)

    def state(label, pid):
        value = fold_query(cluster, "postgres", label, f"""
            SELECT coalesce((SELECT oid FROM pg_database WHERE datname='{db}'),0),
                   count(*) FROM pg_stat_activity
             WHERE pid={pid} AND datname='{db}' AND backend_type='client backend';
        """)
        return tuple(map(int, value.split("|")))

    cluster.start()
    try:
        cluster.sql("postgres", "fold-ddl-roles", "-c", """
            CREATE ROLE fold_owner LOGIN;
            CREATE ROLE fold_other LOGIN;
        """)
        create_db = f"CREATE DATABASE {db} OWNER fold_owner;"
        force_drop = f"DROP DATABASE {db} WITH (FORCE);"
        cluster.sql("postgres", "fold-ddl-create", "-c", create_db)
        with session("fold_owner") as pid:
            before = state("fold-ddl-owner-before", pid)
            if before[0] == 0 or before[1] != 1:
                raise RuntimeError(f"missing owner session or database: {before}")
            for label, sql in (("drop", f"DROP DATABASE {db};"), ("force", force_drop)):
                cluster.sql("postgres", f"fold-ddl-nonowner-{label}", "-U", "fold_other",
                            "-v", "VERBOSITY=verbose", "-c", sql,
                            expect=["42501", "must be owner of database"])
                if state(f"fold-ddl-nonowner-{label}-after", pid) != before:
                    raise RuntimeError(f"non-owner {label} changed the database or owner session")

            cluster.sql("postgres", "fold-ddl-owner-force", "-U", "fold_owner", "-c", force_drop)
            if state("fold-ddl-owner-force-after", pid) != (0, 0):
                raise RuntimeError("owner FORCE left the database or owner session")

        cluster.sql("postgres", "fold-ddl-recreate", "-c", create_db)
        with session("fold_other") as pid:
            before = state("fold-ddl-other-before", pid)
            if before[0] == 0 or before[1] != 1:
                raise RuntimeError(f"missing other-role session or database: {before}")
            cluster.sql("postgres", "fold-ddl-owner-force-other", "-U", "fold_owner",
                        "-v", "VERBOSITY=verbose", "-c", force_drop,
                        expect=["42501", "permission denied to terminate process"])
            if state("fold-ddl-owner-force-other-after", pid) != before:
                raise RuntimeError("denied owner FORCE changed the database or other-role session")
    finally:
        cluster.stop()
    print("Database DDL: non-owner denied, owner FORCE drops with own session, "
          "other-role session preserved on FORCE denial: PASS", flush=True)


def check_slots(cluster, source):
    cluster.sql("beta1a", "pageinspect", "-c", "CREATE EXTENSION pageinspect;")
    binary = source / "slots-test"
    command(["cc", "-std=gnu11", "-O2", "-I", source / "src", "-o", binary,
             CASES / "beta1a_slots.c", source / "src/cuvs_ipc.c",
             source / "src/cuvs_util.c", source / "src/cuvs_build_corpus.c",
             "-lm", "-lpthread", "-lrt"], cluster.logs / "slots-build.log")
    cluster.run(binary, [cluster.socket, *page_slot_keys(cluster, "g14_idx")], "slots")
    cluster.run(binary, [cluster.socket, *page_slot_keys(cluster, "page_ip_idx"), "ip"], "slots-ip")


def check_overlay_budget(cluster, source):
    gate_dir = cluster.data.parent
    score_gate = gate_dir / "score.fifo"
    os.mkfifo(score_gate)
    with cluster.daemon(source / "pg_cuvs_server", "overlay-budget", overlay_host_bytes=128,
                        overlay_gate_dir=gate_dir):
        cluster.start()
        try:
            cluster.run("createdb", ["beta1a"], "createdb-beta1a")
            cluster.file("beta1a", "beta1a")
            cluster.sql("beta1a", "pageinspect", "-c", "CREATE EXTENSION pageinspect;")
            cluster.run(source / "slots-test", [cluster.socket, *page_slot_keys(cluster, "g14_idx"),
                        "budget", *page_slot_keys(cluster, "page_ip_idx"), score_gate,
                        gate_dir / "receive.fifo"], "slots-budget", timeout=100)
        finally:
            cluster.stop()
    print("Daemon host budget, boot ABA, pinned overlay and in-flight receive: PASS", flush=True)


def check_sql_overlay_budget(cluster, source):
    with cluster.daemon(source / "pg_cuvs_server", "sql-overlay-budget", overlay_host_bytes=1):
        cluster.start()
        try:
            cluster.file("postgres", "sql_overlay_budget")
        finally:
            cluster.stop()
    print("SQL heap fallback on host-budget exhaustion and continued writes: PASS", flush=True)


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
                CREATE TABLE legacy_small(v vector(8));
                INSERT INTO legacy_small SELECT v FROM legacy_big LIMIT 2;
                CREATE INDEX legacy_small_idx ON legacy_small USING cagra(v vector_l2_ops);
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
                CREATE TEMP TABLE previous_graphs AS
                    SELECT warmup_state FROM pg_cuvs_index_status()
                      WHERE index_oid='page_lru_idx'::regclass AND kind='page_cagra';
                REINDEX INDEX page_lru_idx;
                DO $$
                DECLARE
                    graphs integer := (SELECT count(*) FROM pg_cuvs_index_status()
                        WHERE index_oid='page_lru_idx'::regclass AND kind='page_cagra');
                    newest text;
                BEGIN
                    IF graphs < 40 THEN
                        RAISE EXCEPTION 'fixture did not retain the unsearched page graphs';
                    END IF;
                    SELECT warmup_state INTO STRICT newest FROM pg_cuvs_index_status()
                        WHERE index_oid='page_lru_idx'::regclass AND kind='page_cagra'
                              AND warmup_state NOT IN (SELECT warmup_state FROM previous_graphs);
                    PERFORM pg_cuvs_load('legacy_big_idx'::regclass);
                    IF (SELECT count(*) FROM pg_cuvs_index_status()
                        WHERE index_oid='page_lru_idx'::regclass AND kind='page_cagra') >= graphs OR
                       NOT EXISTS (SELECT FROM pg_cuvs_index_status()
                        WHERE index_oid='page_lru_idx'::regclass AND kind='page_cagra'
                              AND warmup_state=newest AND last_use IS NULL) THEN
                        RAISE EXCEPTION 'legacy LOAD did not prefer superseded page graphs for eviction';
                    END IF;
                END $$;
            """)
            if "[page-evict]" not in (cluster.logs / "daemon-eviction-low-budget.log").read_text():
                raise RuntimeError("the legacy LOAD never evicted a page graph")
        finally:
            cluster.stop()
    print("Legacy LOAD reclaimed superseded page graphs before the new unsearched graph: PASS", flush=True)

    with cluster.daemon(daemon, "eviction-legacy-slots", max_indexes=1):
        cluster.start("legacy-slots")
        try:
            cluster.sql("postgres", "legacy-slot-pressure", "-c", """
                SELECT pg_cuvs_unload('legacy_big_idx'::regclass);
                SELECT pg_cuvs_unload('legacy_small_idx'::regclass);
                CREATE TABLE page_slot(v vector(8));
                INSERT INTO page_slot SELECT v FROM legacy_big LIMIT 32;
                CREATE INDEX page_slot_idx ON page_slot USING cagra(v vector_l2_ops);
                SELECT pg_cuvs_load('legacy_big_idx'::regclass);
                DO $$
                BEGIN
                    IF NOT EXISTS (SELECT FROM pg_cuvs_index_status()
                                   WHERE index_oid='page_slot_idx'::regclass AND kind='page_cagra') THEN
                        RAISE EXCEPTION 'fixture has no resident page graph';
                    END IF;
                    PERFORM pg_cuvs_load('legacy_small_idx'::regclass);
                    IF NOT EXISTS (SELECT FROM pg_cuvs_gpu_search_stats()
                                   WHERE index_oid='legacy_small_idx'::regclass AND resident) THEN
                        RAISE EXCEPTION 'legacy LOAD did not replace the existing resident';
                    END IF;
                    IF NOT EXISTS (SELECT FROM pg_cuvs_index_status()
                                   WHERE index_oid='page_slot_idx'::regclass AND kind='page_cagra') THEN
                        RAISE EXCEPTION 'legacy slot pressure evicted a page graph';
                    END IF;

                    PERFORM pg_cuvs_pin('legacy_small_idx'::regclass);
                    BEGIN
                        PERFORM pg_cuvs_load('legacy_big_idx'::regclass);
                    EXCEPTION WHEN internal_error THEN
                        NULL; -- the single legacy registry slot is pinned
                    END;
                    IF NOT EXISTS (SELECT FROM pg_cuvs_index_status()
                                   WHERE index_oid='page_slot_idx'::regclass AND kind='page_cagra') THEN
                        RAISE EXCEPTION 'full pinned legacy registry evicted a page graph';
                    END IF;
                END $$;
            """)
        finally:
            cluster.stop()
    print("Legacy slot pressure leaves separately registered page graphs resident: PASS", flush=True)


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
    with cluster.daemon(source_on / "pg_cuvs_server", "on", overlay_host_bytes=4 * 1024**3) as daemon:
        cluster.start()
        try:
            for case in ("alpha1", "alpha2_basic", "truncate", "relcache", "beta1a", "page_bf",
                         "beta1b_expression", "beta3a_fold_prepare"):
                cluster.run("createdb", [case], f"createdb-{case}")
                cluster.file(case, case)
                print(f"{case}: PASS", flush=True)
            check_fold_prepare_fcl_release(cluster, "beta3a_fold_prepare")
            cluster.run("createdb", ["beta3a_fold_publish"], "createdb-beta3a-fold-publish")
            check_fold_publish(cluster, "beta3a_fold_publish")
            check_fold_extent_reuse(cluster, "beta3a_fold_publish")
            check_slots(cluster, source_on)
            cluster.file("postgres", "pending_overlay_scan")
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

    check_overlay_budget(Cluster(work / "budget", bindir, logs), source_on)
    check_sql_overlay_budget(Cluster(work / "sql-budget", bindir, logs), source_on)
    check_fold_worker_wire(Cluster(work / "fold-worker", bindir, logs), source_on)
    check_fold_worker_ddl(Cluster(work / "fold-worker-ddl", bindir, logs))

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
            dead.run("createdb", ["u13"], "createdb-u13")
            dead.sql("u13", "u13-extensions", "-c",
                     "CREATE EXTENSION vector; CREATE EXTENSION pg_cuvs; CREATE EXTENSION pageinspect;")
            dead.file("u13", "u13")
            dead.run(sys.executable, [CASES / "u13-concurrent.py", "u13", "--observe-page-hits"],
                     "u13-concurrent")
            print("Page SQL snapshot, HOT, filter and SSI cases: PASS", flush=True)
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


def run_slots_only(work, logs):
    """Exercise the overlay IPC fixture with one private build and cluster."""
    paths = {key: Path(subprocess.check_output([PG_CONFIG, "--" + key], text=True).strip())
             for key in ("bindir", "pkglibdir", "sharedir")}
    stage = work / "pg"
    for path in paths.values():
        shutil.copytree(path, stage / path.relative_to("/"), symlinks=True)
    bindir = stage / paths["bindir"].relative_to("/")
    source = work / "on"
    copy_source(ROOT, source)
    build(source, 1, logs / "build-on.log")
    build(source, 1, logs / "install.log", install=stage)
    cluster = Cluster(work / "main", bindir, logs)
    with cluster.daemon(source / "pg_cuvs_server", "on", overlay_host_bytes=4 * 1024**3):
        cluster.start()
        try:
            cluster.run("createdb", ["beta1a"], "createdb-beta1a")
            cluster.file("beta1a", "beta1a")
            check_slots(cluster, source)
            cluster.file("postgres", "pending_overlay_scan")
            cluster.run("createdb", ["beta3a_fold_prepare"], "createdb-beta3a-fold-prepare")
            cluster.file("beta3a_fold_prepare", "beta3a_fold_prepare")
            check_fold_prepare_fcl_release(cluster, "beta3a_fold_prepare")
            cluster.run("createdb", ["beta3a_fold_publish"], "createdb-beta3a-fold-publish")
            check_fold_publish(cluster, "beta3a_fold_publish")
        finally:
            cluster.stop()
    check_overlay_budget(Cluster(work / "budget", bindir, logs), source)
    check_sql_overlay_budget(Cluster(work / "sql-budget", bindir, logs), source)
    check_fold_worker_wire(Cluster(work / "fold-worker", bindir, logs), source)
    check_fold_worker_ddl(Cluster(work / "fold-worker-ddl", bindir, logs))
    print("Overlay IPC slots: PASS", flush=True)


if __name__ == "__main__":
    pg_config = shutil.which(sys.argv[1])
    if pg_config is None:
        raise SystemExit(f"PostgreSQL pg_config not found: {sys.argv[1]}")
    PG_CONFIG = Path(pg_config).resolve()
    with tempfile.TemporaryDirectory(prefix="pgcuvs-pages.", dir="/tmp") as directory:
        scratch = Path(directory)
        output = Path(os.environ.get("PAGE_TEST_OUTPUT", scratch / "logs")).resolve()
        output.mkdir(parents=True, exist_ok=True)
        if os.environ.get("PAGE_TEST_SLOTS_ONLY") == "1":
            run_slots_only(scratch, output)
        else:
            run(scratch, output)

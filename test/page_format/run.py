"""Run the CAGRA page-format CPU matrix in disposable PG16 clusters."""

from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
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
               "with_llvm=no", "PG_CFLAGS=-DCUVS_TEST_HOOKS"]
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
    def daemon(self, binary, label):
        with (self.logs / f"daemon-{label}.log").open("w") as log, subprocess.Popen(
            [binary, "--socket", self.socket, "--index-dir", self.indexdir, "--gpu-devices", "0"],
            stdout=log, stderr=subprocess.STDOUT, env=self.env,
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
                yield
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
            if len(legacy_files) != 6:
                raise RuntimeError("expected three legacy graphs and TID files")
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
            if any(hashlib.sha256(path.read_bytes()).digest() != digest
                   for path, digest in before.items()):
                raise RuntimeError("REINDEX rewrote a legacy graph or TID file")
            local = list(cluster.indexdir.glob("*/*/empty.xid"))
            redirected = list(cluster.indexdir.glob("*/*/legacy.xid"))
            custom = list((cluster.data.parent / "custom").glob("*/*/empty.xid"))
            if any(len(paths) != 1 or not int(paths[0].read_text()) for paths in (local, redirected, custom)):
                raise RuntimeError("missing marked xid for an empty build or its directory override")
        finally:
            cluster.stop()

    label = "restart-deferred"
    with cluster.daemon(daemon, label):
        cluster.start(label)
        try:
            startup = (cluster.logs / f"daemon-{label}.log").read_text()
            if "[startup] loaded 0 index(es)" not in startup:
                raise RuntimeError("startup loaded an index marked for legacy deferral")
            cluster.sql("postgres", "g13-explicit", "-c", """
                SET enable_seqscan=off;
                DO $$
                BEGIN
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
    with cluster.daemon(source_on / "pg_cuvs_server", "on"):
        cluster.start()
        try:
            for case in ("alpha1", "alpha2_basic", "truncate", "relcache", "beta1a"):
                cluster.run("createdb", [case], f"createdb-{case}")
                cluster.file(case, case)
                print(f"{case}: PASS", flush=True)
            check_slots(cluster, source_on)
            cluster.run(sys.executable, [CASES / "alpha1_concurrency.py", "alpha1"], "alpha1-concurrency")
            cluster.run(sys.executable, [CASES / "alpha2_concurrency.py", "alpha2_basic", cluster.log],
                        "alpha2-concurrency")
            cluster.run("createdb", ["lifecycle"], "createdb-lifecycle")
            cluster.run(sys.executable, [CASES / "alpha2_lifecycle.py", "lifecycle", cluster.data, logs],
                        "lifecycle")
            print("Dimension, pending tail, CIC, canceled VACUUM, standby and no-preload: PASS", flush=True)
        finally:
            cluster.stop()

    if legacy_source:
        with cluster.daemon(source_base / "pg_cuvs_server", "historical"):
            cluster.start("historical")
            try:
                old_daemon_search(cluster)
            finally:
                cluster.stop()

    check_deferral(Cluster(work / "g13", bindir, logs), source_on / "pg_cuvs_server",
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

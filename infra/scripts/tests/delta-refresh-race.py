#!/usr/bin/env python3
"""Run against a disposable PostgreSQL cluster and a CUVS_TEST_HOOKS daemon."""

import argparse
import concurrent.futures
import fcntl
import os
from pathlib import Path
import shlex
import stat
import struct
import subprocess
import tempfile
import time
import uuid


HEADER = struct.Struct("<IIqIIII")
RECORD_BYTES = struct.calcsize("<Q4f")


class Fixture:
    def __init__(self, workdir, index_dir, mode):
        self.workdir = workdir
        self.index_dir = index_dir
        self.schema = "delta_race_" + uuid.uuid4().hex[:12]
        self.batch_wait = 500 if mode == "microbatch" else 0
        self.shards = 2 if mode == "sharded" else 1

    def sql(self, statement, *, timeout=8):
        return subprocess.run(
            ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-c", statement],
            check=True, capture_output=True, text=True, timeout=timeout,
        ).stdout.strip()

    def setup(self):
        self.sql(
            "CREATE EXTENSION IF NOT EXISTS vector; "
            "CREATE EXTENSION IF NOT EXISTS pg_cuvs; "
            f"CREATE SCHEMA {self.schema}; "
            f"SET search_path TO {self.schema}, public; "
            f"SET cuvs.shard_count = {self.shards}; "
            "CREATE TABLE d (id int, v vector(4)); "
            "INSERT INTO d SELECT g, ARRAY[g,1,0,0]::vector FROM generate_series(1,200) g; "
            "CREATE INDEX d_i ON d USING cagra (v vector_l2_ops); "
            "SELECT pg_cuvs_set_vram_budget(vram_bytes) "
            "FROM pg_stat_gpu_search WHERE index_oid = 'd_i'::regclass; "
            "INSERT INTO d VALUES (901, '[900,1,0,0]'), (902, '[950,1,0,0]'), (903, '[990,1,0,0]'); "
            "SELECT pg_cuvs_set_vram_budget(0); "
            "SET cuvs.shard_count = 1; "
            "CREATE TABLE other AS SELECT * FROM d WHERE id <= 200; "
            "CREATE INDEX other_i ON other USING cagra (v vector_l2_ops); ",
            timeout=120,
        )
        basename = self.sql(
            f"SELECT oid || '_' || '{self.schema}.d_i'::regclass::oid "
            "FROM pg_database WHERE datname = current_database()"
        )
        self.delta = self.index_dir / (basename + ".delta")
        self.tids = self.index_dir / (basename + ".tids")
        image = self.delta.read_bytes()
        assert len(image) == HEADER.size + 3 * RECORD_BYTES
        self.header = HEADER.unpack_from(image)
        assert self.header[2:4] == (3, 4)
        self.records = [
            image[HEADER.size + i * RECORD_BYTES:HEADER.size + (i + 1) * RECORD_BYTES]
            for i in range(3)
        ]

    def header_for(self, count):
        return HEADER.pack(*self.header[:2], count, *self.header[3:])

    def image(self, *ids):
        return self.header_for(len(ids)) + b"".join(self.records[i - 901] for i in ids)

    def replace(self, *ids, keep_mtime=False):
        stamp = self.delta.stat()
        next_image = self.delta.with_suffix(".test-next")
        next_image.write_bytes(self.image(*ids))
        if keep_mtime:
            os.utime(next_image, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        next_image.replace(self.delta)


    def write_append(self, handle, row):
        count = HEADER.unpack(handle.read(HEADER.size))[2]
        handle.seek(0, os.SEEK_END)
        handle.write(self.records[row - 901])
        handle.seek(0)
        handle.write(self.header_for(count + 1))
        handle.flush()
        os.fsync(handle.fileno())

    def append(self, row):
        with self.delta.open("r+b") as handle:
            fcntl.flock(handle, fcntl.LOCK_EX)
            self.write_append(handle, row)

    def search(self, q, *, table="d", delta_mode="gpu", search_mode="cagra", wait=None, timeout=15):
        if wait is None:
            wait = self.batch_wait
        settings = (
            "SET enable_seqscan = off; "
            f"SET cuvs.delta_search = '{delta_mode}'; "
            f"SET cuvs.search_mode = '{search_mode}'; "
            f"SET cuvs.cagra_batch_wait_us = {wait}; "
        )
        query = f"SELECT id FROM {self.schema}.{table} ORDER BY v <-> '[{q},1,0,0]'::vector LIMIT 1"
        result = subprocess.run(
            ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1", "-v", "VERBOSITY=verbose",
             "-c", settings, "-c", f"EXPLAIN (COSTS OFF) {query}", "-c", query],
            capture_output=True, text=True, timeout=timeout,
        )
        index = "d_i" if table == "d" else "other_i"
        assert f"Index Scan using {index} on " in result.stdout, (result.stdout, result.stderr)
        return result

    def nearest(self, q, **kwargs):
        result = self.search(q, **kwargs)
        result.check_returncode()
        return int(result.stdout.strip().splitlines()[-1])

    def stats(self):
        return self.sql(
            "SELECT search_count || ' ' || delta_merged_count || ' ' || cagra_batch_count "
            f"FROM pg_stat_gpu_search WHERE index_oid = '{self.schema}.d_i'::regclass"
        )

    def race(self, phase, query, *actions):
        arm = self.workdir / (phase + ".arm")
        reached = self.workdir / (phase + ".reached")
        arm.touch(exist_ok=False)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            search = pool.submit(query)
            try:
                for i, action in enumerate(actions):
                    deadline = time.monotonic() + 5
                    while not reached.exists():
                        if search.done():
                            raise AssertionError(f"search returned {search.result()} before {phase} barrier")
                        if time.monotonic() >= deadline:
                            raise TimeoutError(f"daemon did not reach {phase} barrier")
                        time.sleep(0.005)
                    assert self.nearest(100, table="other", wait=0, timeout=2) == 100
                    assert reached.exists() and not search.done()
                    action()
                    if i + 1 < len(actions):
                        arm.touch(exist_ok=False)
                    reached.unlink(missing_ok=True)
            finally:
                reached.unlink(missing_ok=True)
                arm.unlink(missing_ok=True)
            return search.result()

    def writer_lock(self):
        self.replace(901)
        assert self.nearest(900) == 901
        self.append(902)
        with self.delta.open("r+b") as writer:
            fcntl.flock(writer, fcntl.LOCK_EX)

            def publish():
                read_arm = self.workdir / "read.arm"
                read_reached = self.workdir / "read.reached"
                read_arm.touch(exist_ok=False)
                (self.workdir / "wait-file-lock.reached").unlink()
                try:
                    assert self.nearest(100, table="other", wait=0, timeout=2) == 100
                    deadline = time.monotonic() + 0.1
                    while time.monotonic() < deadline:
                        assert not read_reached.exists(), "reader ignored LOCK_EX"
                        time.sleep(0.005)
                    read_arm.unlink()
                    self.write_append(writer, 903)
                finally:
                    read_arm.unlink(missing_ok=True)
                    read_reached.unlink(missing_ok=True)
                    fcntl.flock(writer, fcntl.LOCK_UN)

            # All three heap rows committed during setup, before this query's
            # snapshot. The daemon must read the completed append after LOCK_SH.
            assert self.race("wait-file-lock", lambda: self.nearest(990), publish) == 903

    def metadata_append(self, *, rewrite_boundary=False):
        self.replace(901)
        self.delta.chmod(0o644)
        assert self.nearest(900) == 901
        state_sql = (
            "SELECT delta_rows, delta_vram_bytes, delta_merged_count "
            f"FROM pg_stat_gpu_search WHERE index_oid = '{self.schema}.d_i'::regclass"
        )
        cached, indexed_bytes, merged = map(int, self.sql(state_sql).split("|"))
        assert (cached, indexed_bytes) == (1, RECORD_BYTES)

        with self.delta.open("r+b") as writer:
            fd = writer.fileno()
            fcntl.flock(fd, fcntl.LOCK_EX)
            before = os.fstat(fd)
            deadline = time.monotonic() + 2
            while True:
                os.fchmod(fd, 0o644)
                changed = os.fstat(fd)
                if changed.st_ctime_ns != before.st_ctime_ns:
                    break
                assert time.monotonic() < deadline, "fchmod did not advance ctime"
                time.sleep(0.005)
            assert (changed.st_ino, changed.st_size, changed.st_mtime_ns) == (
                before.st_ino, HEADER.size + RECORD_BYTES, before.st_mtime_ns
            )

            def publish():
                try:
                    if rewrite_boundary:
                        writer.seek(HEADER.size)
                        writer.write(self.records[2])
                        writer.seek(0)
                    self.write_append(writer, 902)
                finally:
                    fcntl.flock(fd, fcntl.LOCK_UN)

            # The wait barrier follows the daemon's initial stat. Until then
            # the writer has changed only metadata, with no second row on disk.
            query = 990 if rewrite_boundary else 950
            expected_id = 903 if rewrite_boundary else 902
            assert self.race("wait-file-lock", lambda: self.nearest(query), publish) == expected_id

        expected_bytes = 2 * RECORD_BYTES if rewrite_boundary else indexed_bytes
        state = tuple(map(int, self.sql(state_sql).split("|")))
        expected = (2, expected_bytes, merged + 1)
        assert state == expected, ("delta rows, indexed bytes, merged searches", state, expected)
        print(f"delta cache (rows, indexed bytes, merged searches): {state}", flush=True)

    def competing_publish(self):
        self.replace(901, 902)

        def publish():
            self.replace(901, 902, 903)
            assert self.nearest(990, wait=0, timeout=2) == 903

        assert self.race("read", lambda: self.nearest(990), publish) == 903

    def delta_replace(self):
        self.replace(901, 902)
        assert self.race("read", lambda: self.nearest(990),
                         lambda: self.replace(901, 903, keep_mtime=True)) == 903

    def delta_truncate(self):
        self.replace(901, 902, 903)

        def truncate_in_place():
            # A non-cooperating file rewrite, while the reader holds LOCK_SH.
            inode = self.delta.stat().st_ino
            self.delta.write_bytes(self.image(901, 902))
            assert self.delta.stat().st_ino == inode

        assert self.race("read", lambda: self.nearest(990), truncate_in_place) == 902

    def late_header(self):
        self.replace(901, 902)
        merged_before = self.stats().split()[1]

        def incomplete_replacement():
            next_image = self.delta.with_suffix(".test-next")
            next_image.write_bytes(self.image(901, 902) + self.records[2])
            next_image.replace(self.delta)

        assert self.race("read", lambda: self.nearest(990), incomplete_replacement) == 200
        assert self.stats().split()[1] == merged_before
        with self.delta.open("r+b") as handle:
            fcntl.flock(handle, fcntl.LOCK_EX)
            handle.write(self.header_for(3))
        assert self.nearest(990) == 903

    def unreadable_delta(self):
        self.replace(901, 902)
        permissions = stat.S_IMODE(self.delta.stat().st_mode)
        merged_before = self.stats().split()[1]

        def deny_read():
            self.delta.chmod(0)
            assert not os.access(self.delta, os.R_OK)

        try:
            assert self.race("request", lambda: self.nearest(950), deny_read) == 200
            assert self.stats().split()[1] == merged_before
        finally:
            self.delta.chmod(permissions)
        assert self.nearest(950) == 902

    def generation_change(self, *, warm):
        self.replace(901, 902)
        if warm:
            assert self.nearest(950) == 902
        tids_image = self.tids.read_bytes()
        wrong_crc = struct.unpack_from("<I", tids_image, 24)[0] ^ 1

        def change_generation():
            replacement = self.tids.with_suffix(".test-next")
            changed = bytearray(tids_image)
            struct.pack_into("<I", changed, 24, wrong_crc)
            replacement.write_bytes(changed)
            replacement.replace(self.tids)

        phase = "request" if warm else "read"
        try:
            merged_before = self.stats().split()[1]
            assert self.race(phase, lambda: self.nearest(950), change_generation) == 200
            assert self.stats().split()[1] == merged_before
        finally:
            self.tids.write_bytes(tids_image)
        assert self.nearest(950) == 902

    def unload(self):
        assert self.sql(f"SELECT pg_cuvs_unload('{self.schema}.d_i')").startswith("unloaded")

    def reload(self):
        self.unload()
        self.sql(f"SELECT pg_cuvs_load('{self.schema}.d_i')")

    def reload_read(self):
        self.replace(901, 902)
        assert self.race("read", lambda: self.nearest(950), self.reload) == 902

    def reload_score(self):
        self.replace(901)
        assert self.nearest(900) == 901
        self.append(902)
        assert self.race("score", lambda: self.nearest(950), self.reload) == 902
        assert self.stats().split() == ["0", "0", "0"]

    def auto_fallback(self):
        self.reload()
        self.replace(901, 902, 903)
        self.sql("SELECT pg_cuvs_set_vram_budget(1)")
        try:
            assert self.nearest(990, delta_mode="auto") == 903
            searches, merges, batches = map(int, self.stats().split())
            assert searches == 1 and merges == 0
            assert batches == bool(self.batch_wait)
        finally:
            self.sql("SELECT pg_cuvs_set_vram_budget(0)")

    @staticmethod
    def expect_sqlstate(result, code):
        assert result.returncode != 0 and f":  {code}:" in result.stderr, (
            result.stdout, result.stderr
        )

    def repeated_replacement(self):
        self.replace(901, 902, 903)
        action = lambda: self.replace(901, 902, 903)
        result = self.race("read", lambda: self.search(990), action, action, action)
        self.expect_sqlstate(result, "40001")
        assert self.nearest(990) == 903

    def unload_without_files(self):
        self.replace(901, 902)
        parked_tids = self.tids.with_suffix(".test-parked")

        def unload():
            self.unload()
            self.tids.rename(parked_tids)

        try:
            result = self.race("read", lambda: self.search(950), unload)
            self.expect_sqlstate(result, "40001")
        finally:
            if parked_tids.exists():
                parked_tids.replace(self.tids)
        assert self.nearest(950) == 902

    def mark_stale(self):
        self.sql(
            "SET cuvs.max_delta_rows = 0; "
            f"INSERT INTO {self.schema}.d VALUES (904, '[1000,1,0,0]')"
        )

    def stale_after_read(self):
        self.replace(901, 902)
        result = self.race("read", lambda: self.search(950), self.mark_stale)
        self.expect_sqlstate(result, "55000")
        assert "is stale" in result.stderr

    def shard_stale_cpu(self):
        self.replace(901, 902)
        assert self.race("shard-fanout",
                         lambda: self.nearest(150, delta_mode="cpu"),
                         self.mark_stale) == 150

    def shard_stale_bf(self):
        self.replace(901, 902)
        index = f"'{self.schema}.d_i'::regclass"
        assert self.sql(
            f"SELECT count(*) FROM pg_stat_gpu_shards WHERE index_oid = {index} AND resident"
        ) == "2"
        db_oid, index_oid = self.delta.stem.split("_")
        socket = self.sql("SHOW cuvs.socket_path")
        expected_ctid = self.sql(f"SELECT ctid FROM {self.schema}.d WHERE id = 150")
        assert expected_ctid
        root = Path(__file__).resolve().parents[3]

        with tempfile.TemporaryDirectory(prefix="delta-shard-bf-") as temp:
            client = Path(temp) / "search-client"
            subprocess.run(
                [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall",
                 "-Wextra", "-Werror", "-I", "src", "-o", str(client),
                 "infra/scripts/tests/shard-bf-search-client.c", "src/cuvs_ipc.c",
                 "src/cuvs_util.c", "src/cuvs_build_corpus.c", "-lpthread", "-lm", "-lrt"],
                cwd=root, check=True, timeout=120,
            )

            def search_bf():
                result = subprocess.run(
                    [str(client), socket, db_oid, index_oid],
                    check=True, capture_output=True, text=True, timeout=15,
                )
                status, count, merged, ctid, distance = result.stdout.strip().split()
                return status, int(count), int(merged), ctid, float(distance)

            expected = ("ok", 1, 0, expected_ctid, 0.0)
            control = search_bf()
            assert control == expected, ("quiescent shard BF", control, expected)
            assert self.sql(
                f"SELECT search_mode FROM pg_stat_gpu_search WHERE index_oid = {index}"
            ) == "brute_force"
            raced = self.race("shard-fanout", search_bf, self.mark_stale)
            assert raced == expected, ("in-flight shard BF after MARK_STALE", raced, expected)
            after = search_bf()
            assert after[:3] == ("stale", 0, 0), ("new shard BF after MARK_STALE", after)
            print(f"shard BF replies: control={control!r}, in-flight={raced!r}, new={after!r}",
                  flush=True)

    def cases(self):
        return {
            "writer-lock": self.writer_lock,
            "metadata-append": self.metadata_append,
            "metadata-append-boundary": lambda: self.metadata_append(rewrite_boundary=True),
            "competing-publish": self.competing_publish,
            "delta-replace": self.delta_replace,
            "delta-truncate": self.delta_truncate,
            "late-header": self.late_header,
            "unreadable-delta": self.unreadable_delta,
            "tids-during-read": lambda: self.generation_change(warm=False),
            "tids-warm": lambda: self.generation_change(warm=True),
            "reload-read": self.reload_read,
            "reload-score": self.reload_score,
            "auto-fallback": self.auto_fallback,
            "repeated-replacement": self.repeated_replacement,
            "unload-without-files": self.unload_without_files,
            "stale-after-read": self.stale_after_read,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--barrier-dir", type=Path, required=True)
    parser.add_argument("--index-dir", type=Path, required=True)
    parser.add_argument("--mode", choices=("immediate", "sharded", "microbatch"), required=True)
    parser.add_argument("--case", action="append", help="run a named case, repeatable")
    args = parser.parse_args()
    if os.environ.get("PGCUVS_DISPOSABLE_CLUSTER") != "1":
        parser.error("set PGCUVS_DISPOSABLE_CLUSTER=1 for the isolated test cluster")
    fixture = Fixture(args.barrier_dir, args.index_dir, args.mode)
    cases = fixture.cases()
    selected = args.case or list(cases)
    if args.mode == "sharded":
        cases.update({
            "shard-stale-cpu": fixture.shard_stale_cpu,
            "shard-stale-bf": fixture.shard_stale_bf,
        })
    if any(name not in cases for name in selected):
        parser.error("available cases: " + ", ".join(cases))
    try:
        fixture.setup()
        for name in selected:
            cases[name]()
            print(f"{args.mode}/{name}: passed", flush=True)
    finally:
        fixture.sql(f"DROP SCHEMA IF EXISTS {fixture.schema} CASCADE")


if __name__ == "__main__":
    main()

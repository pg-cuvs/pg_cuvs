#!/usr/bin/env python3
"""Run against a disposable PostgreSQL cluster and a CUVS_TEST_HOOKS daemon."""

import argparse
import concurrent.futures
import os
from pathlib import Path
import struct
import subprocess
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

    def append(self, row, *, publish_header=True):
        with self.delta.open("r+b") as handle:
            count = HEADER.unpack(handle.read(HEADER.size))[2]
            handle.seek(0, os.SEEK_END)
            handle.write(self.records[row - 901])
            if publish_header:
                handle.seek(0)
                handle.write(self.header_for(count + 1))

    def nearest(self, q, *, table="d", delta_mode="gpu", wait=None, timeout=8):
        if wait is None:
            wait = self.batch_wait
        return int(self.sql(
            "SET enable_seqscan = off; "
            f"SET cuvs.delta_search = '{delta_mode}'; "
            f"SET cuvs.cagra_batch_wait_us = {wait}; "
            f"SELECT id FROM {self.schema}.{table} ORDER BY v <-> '[{q},1,0,0]'::vector LIMIT 1",
            timeout=timeout,
        ))

    def stats(self):
        return self.sql(
            "SELECT search_count || ' ' || delta_merged_count || ' ' || cagra_batch_count "
            f"FROM pg_stat_gpu_search WHERE index_oid = '{self.schema}.d_i'::regclass"
        )

    def race(self, phase, query, action):
        arm = self.workdir / (phase + ".arm")
        reached = self.workdir / (phase + ".reached")
        arm.touch(exist_ok=False)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            search = pool.submit(query)
            try:
                deadline = time.monotonic() + 5
                while not reached.exists():
                    if search.done():
                        raise AssertionError(f"search returned {search.result()} before {phase} barrier")
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"daemon did not reach {phase} barrier")
                    time.sleep(0.005)
                # A different index must finish while this search is still parked.
                assert self.nearest(100, table="other", wait=0, timeout=2) == 100
                assert reached.exists() and not search.done()
                action()
            finally:
                reached.unlink(missing_ok=True)
                arm.unlink(missing_ok=True)
            return search.result()

    def run(self):
        self.replace(901)
        assert self.nearest(900) == 901
        self.append(902)

        def competing_publish():
            self.append(903)
            assert self.nearest(990, wait=0, timeout=2) == 903

        assert self.race("read", lambda: self.nearest(990), competing_publish) == 903

        self.replace(901, 902)
        assert self.race("read", lambda: self.nearest(990),
                         lambda: self.replace(901, 903, keep_mtime=True)) == 903

        self.replace(901, 902, 903)
        assert self.race("read", lambda: self.nearest(990),
                         lambda: self.delta.write_bytes(self.image(901, 902))) == 902

        self.replace(901, 902)
        merged_before = self.stats().split()[1]
        assert self.race("read", lambda: self.nearest(990),
                         lambda: self.append(903, publish_header=False)) == 200
        assert self.stats().split()[1] == merged_before
        with self.delta.open("r+b") as handle:
            handle.write(self.header_for(3))
        assert self.nearest(990) == 903

        tids_image = self.tids.read_bytes()
        wrong_crc = struct.unpack_from("<I", tids_image, 24)[0] ^ 1

        def change_generation():
            with self.tids.open("r+b") as handle:
                handle.seek(24)
                handle.write(struct.pack("<I", wrong_crc))

        self.replace(901, 902)
        try:
            merged_before = self.stats().split()[1]
            assert self.race("read", lambda: self.nearest(950), change_generation) == 200
            assert self.stats().split()[1] == merged_before
        finally:
            self.tids.write_bytes(tids_image)
        assert self.nearest(950) == 902

        def reload():
            assert self.sql(f"SELECT pg_cuvs_unload('{self.schema}.d_i')").startswith("unloaded")
            self.sql(f"SELECT pg_cuvs_load('{self.schema}.d_i')")

        self.replace(901, 902)
        assert self.race("read", lambda: self.nearest(950), reload) == 902
        self.replace(901)
        assert self.nearest(900) == 901
        self.append(902)
        assert self.race("score", lambda: self.nearest(950), reload) == 902
        assert self.stats().split() == ["0", "0", "0"]

        self.replace(901, 902, 903)
        self.sql("SELECT pg_cuvs_set_vram_budget(1)")
        try:
            assert self.nearest(990, delta_mode="auto") == 903
            searches, merges, batches = map(int, self.stats().split())
            assert searches == 1 and merges == 0
            assert batches == bool(self.batch_wait)
        finally:
            self.sql("SELECT pg_cuvs_set_vram_budget(0)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--barrier-dir", type=Path, required=True)
    parser.add_argument("--index-dir", type=Path, required=True)
    parser.add_argument("--mode", choices=("immediate", "sharded", "microbatch"), required=True)
    args = parser.parse_args()
    if os.environ.get("PGCUVS_DISPOSABLE_CLUSTER") != "1":
        parser.error("set PGCUVS_DISPOSABLE_CLUSTER=1 for the isolated test cluster")
    fixture = Fixture(args.barrier_dir, args.index_dir, args.mode)
    try:
        fixture.setup()
        fixture.run()
    finally:
        fixture.sql(f"DROP SCHEMA IF EXISTS {fixture.schema} CASCADE")
    print(f"{args.mode}: file races, unlocked tail, reload and auto-mode merge passed")


if __name__ == "__main__":
    main()

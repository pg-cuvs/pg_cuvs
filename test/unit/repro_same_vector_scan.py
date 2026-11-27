#!/usr/bin/env python3
"""Try to reproduce a backend SIGBUS the independent review saw once (REVIEW-INDEP-COVER-af8d48e1 R6-7a): a table of 100,000 rows that all
hold the same vector, a ledger of about 91,640 keys (an index filled by inserts, never recertified), 100 more inserts, then
EXPLAIN (ANALYZE) of ordered scans. Runs the same configuration a number of times, with parallel statements; reports every server process that died by a signal. Prints JSON.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5851)
    ap.add_argument('--rows', type=int, default=100000)
    ap.add_argument('--queries', type=int, default=300)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    res = {}
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'sb', out, args.port)
        c.extra_opts = '-c autovacuum=off -c max_parallel_maintenance_workers=0 -c shared_buffers=256MB -c max_wal_size=2GB'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE tc(id int, v vector(96))')
            c.scalar('CREATE INDEX tc_i ON tc USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar(f"INSERT INTO tc SELECT g, (SELECT array_agg(0.5::real) FROM generate_series(1, 96) d WHERE g > 0)::vector FROM generate_series(1, {args.rows}) g")
            c.scalar("INSERT INTO tc SELECT 1000000 + g, (SELECT array_agg(0.5::real) FROM generate_series(1, 96) d WHERE g > 0)::vector FROM generate_series(1, 100) g")
            st = json.loads(c.scalar("SELECT pg_cuvs_hnsw_cover_status('tc_i')::text"))
            res['status'] = {k: st.get(k) for k in ('class', 'ledger_keys', 'guarantee_in_force')}
            q = c.scalar("SELECT v::text FROM tc WHERE id = 5")
            bad = []

            def run(k):
                for i in range(args.queries // 4):
                    rc, out_q = c.q(f"SET enable_seqscan = off; EXPLAIN (ANALYZE, BUFFERS) SELECT id FROM tc ORDER BY v <-> '{q}' LIMIT 10")
                    if rc != 0:
                        bad.append((k, i, out_q[-300:]))
            threads = [threading.Thread(target=run, args=(k,)) for k in range(4)]
            for th in threads:
                th.start()
            for th in threads:
                th.join()
            res['queries'] = args.queries
            res['failed_queries'] = bad[:5]
            log = ''.join(f.read_text(errors='replace') for f in sorted(c.data.glob('log-*')))
            res['server_log_signals'] = re.findall(r'.*terminated by signal.*', log)[:5]
            res['server_log_bus'] = len(re.findall(r'Bus error|SIGBUS', log))
        finally:
            c.drop()
    finally:
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    (out / 'result.json').write_text(json.dumps(res, indent=2) + '\n')
    print(json.dumps(res, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())

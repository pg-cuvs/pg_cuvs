#!/usr/bin/env python3
"""Does a recertification ever get the exclusive cover lock while writers insert without a pause? (review 2f6789ed R7-3)

W sessions each run a server-side loop of single-row inserts into a CERTIFIED index (the cover lock is taken and released per row);
the main thread calls pg_cuvs_cover_recertify('hi') K times and counts the answers. Prints JSON: per answer, how long the call took.
"""
import argparse
import json
from pathlib import Path
import shutil
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5881)
    ap.add_argument('--writers', type=int, default=8)
    ap.add_argument('--calls', type=int, default=12)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    res = {'writers': args.writers, 'answers': []}
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'st', out, args.port)
        c.extra_opts = '-c autovacuum=off -c max_connections=100'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE h(id int, v vector(8))')
            c.scalar('CREATE INDEX hi ON h USING pg_cuvs_hnsw (v vector_l2_ops)')
            stop = threading.Event()
            sessions = [dg.Session(c) for _ in range(args.writers)]

            def writer(k):
                n = 0
                while not stop.is_set():
                    n += 1
                    sessions[k].run(f"DO $$ BEGIN FOR i IN 1..300 LOOP INSERT INTO h VALUES ({k} * 100000000 + {n} * 1000 + i, ARRAY[i, {k}, {n} % 7, 1, 2, 3, 4, 5]::real[]::vector); END LOOP; END $$")
            threads = [threading.Thread(target=writer, args=(k,)) for k in range(args.writers)]
            for th in threads:
                th.start()
            time.sleep(2)
            for _ in range(args.calls):
                t0 = time.time()
                rc, r = c.q("SELECT pg_cuvs_cover_recertify('hi')::text")
                res['answers'].append({'result': (json.loads(r).get('result') if r.startswith('{') else 'text:' + r[:200]) if rc == 0 else 'error:' + r[:200], 'secs': round(time.time() - t0, 2)})
            stop.set()
            for th in threads:
                th.join()
            for s in sessions:
                s.close()
            res['done'] = sum(1 for a in res['answers'] if a['result'] == 'done')
            res['deferred'] = sum(1 for a in res['answers'] if a['result'] == 'deferred')
        finally:
            c.drop()
    finally:
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    (out / 'result.json').write_text(json.dumps(res, indent=2) + '\n')
    print(json.dumps({k: v for k, v in res.items() if k != 'answers'}), json.dumps(res['answers']))
    return 0


if __name__ == '__main__':
    sys.exit(main())

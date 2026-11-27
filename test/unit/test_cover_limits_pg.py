#!/usr/bin/env python3
"""The memory-limit failure of a recertification through the SQL and VACUUM paths (CPU only; needs the gate build).

The gate build lets a test set the RAM limit of the walk to a number of bytes (CUVS_RECERT_GATE_RAM_LIMIT, no spill), smaller than any
maintenance_work_mem allows. Checked:

  M1 pg_cuvs_cover_recertify answers "failed" (over the memory limits) and says how much the walk needs;
  M2 the status function describes the failure (cause over_memory_limit, the limit it had) and the pending text says what to do;
  M3 a VACUUM at the same size and limit skips the walk (DEBUG1 line), and the index stays LOST;
  M4 with the limit gone (server restarted without it) the recertification works and the failure is forgotten.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_downgrade_pg as dg  # noqa: E402
import test_cover_vacuum_pg as tv  # noqa: E402

check = dg.check


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--new-lib', type=Path, required=True)
    ap.add_argument('--old-lib', type=Path, required=True)
    ap.add_argument('--port', type=int, default=5871)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = out / 'pgroot'
    plant_bin = out / 'cover_plant'
    r = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(repo / 'src'), str(repo / 'test/unit/cover_plant.c'), '-o', str(plant_bin)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert r.returncode == 0, r.stdout
    limit = 8400
    os.environ['CUVS_RECERT_GATE_RAM_LIMIT'] = str(limit)
    try:
        o = dg.Overlay(root, args.new_lib.resolve(), args.old_lib.resolve(), repo / 'sql', repo / 'pg_cuvs.control')
        c = dg.Cluster(o, 'lim', out, args.port)
        c.extra_opts = '-c autovacuum=off'
        try:
            o.install('new')
            c.start()
            c.scalar('CREATE EXTENSION vector')
            c.scalar('CREATE EXTENSION pg_cuvs')
            c.scalar('CREATE TABLE t(id int, v vector(8))')
            c.scalar(tv.clustered('t', 1500))
            tv.hack_index(c, 't', 'ti', 'WITH (m = 8, ef_construction = 16)')
            tv.plant(c, plant_bin, 'ti', 'uncertified')
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ti')::text")
            j = json.loads(res) if rc == 0 else {}
            check('M1 the recertification answers failed (over the memory limits) and says what the walk needs',
                  j.get('result') == 'failed' and 'memory limits' in j.get('detail', ''), res)
            st = tv.cover_status(c, 'ti')
            lf = st.get('last_recertification_failure', {})
            check('M2 the status function describes the failure: cause, the limit it had, what it needed', lf.get('cause') == 'over_memory_limit'
                  and lf.get('limit_bytes') == limit and lf.get('needed_bytes', 0) > limit, str(st))
            check('M2 and the pending text says what to do', 'gave up' in st.get('pending', '') or 'autovacuum' in st.get('pending', ''), str(st))
            s = dg.Session(c)
            s.run('SET client_min_messages = debug1')
            res = s.run('VACUUM t')
            s.close()
            st = tv.cover_status(c, 'ti')
            check('M3 a VACUUM at the same size and limit skips the walk, and the index stays LOST', 'by vacuum: skipped' in res and st.get('class') == 'lost', f'{res[-300:]} {st.get("class")}')
            del os.environ['CUVS_RECERT_GATE_RAM_LIMIT']
            c.restart()
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('ti')::text")
            st = tv.cover_status(c, 'ti')
            check('M4 with the limit gone the recertification works, in force, and the failure is forgotten',
                  rc == 0 and '"result": "done"' in res and st.get('guarantee_in_force') is True and 'last_recertification_failure' not in st, f'{res} {st}')

            # M5 (review 32af2a8c R8-3): a CERTIFIED index whose recertification fails says so in the pending text
            c.scalar('CREATE TABLE t2(id int, v vector(8))')
            c.scalar('CREATE INDEX t2i ON t2 USING pg_cuvs_hnsw (v vector_l2_ops)')
            c.scalar(tv.clustered('t2', 1500))
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('t2i')::text")		# no limit now: certified with nothing since
            check('M5 the index is certified without keys since', rc == 0 and tv.cover_status(c, 't2i').get('recertify_recommended') is False, res)
            os.environ['CUVS_RECERT_GATE_RAM_LIMIT'] = str(limit)
            c.restart()
            rc, res = c.q("SELECT pg_cuvs_cover_recertify('t2i')::text")
            st = tv.cover_status(c, 't2i')
            check('M5 a certified index whose recertification failed: still certified, the failure described, and a pending text says what happens next',
                  '"result": "failed"' in res and st.get('class') == 'certified' and 'last_recertification_failure' in st and 'last recertification' in st.get('pending', ''), f'{res} {st}')
        finally:
            c.drop()
    finally:
        os.environ.pop('CUVS_RECERT_GATE_RAM_LIMIT', None)
        (out / 'transcript.txt').write_text('\n'.join(dg.TRANSCRIPT) + '\n')
        shutil.rmtree(root, ignore_errors=True)
        for d in out.glob('data-*'):
            shutil.rmtree(d, ignore_errors=True)
    result = {'failures': dg.FAILURES, 'verdict': 'FAIL' if dg.FAILURES else 'PASS'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if dg.FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())

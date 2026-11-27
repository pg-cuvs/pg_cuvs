#!/usr/bin/env python3
"""RED controls for test_downgrade_pg.py: each mutant damages one line of the 0.9.3 SQL scripts, and the
runner must fail for it. The unmutated scripts must pass, otherwise a failing mutant proves nothing."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

DOWN = 'pg_cuvs--0.9.3--0.9.2.sql'
UP = 'pg_cuvs--0.9.2--0.9.3.sql'
BASE = 'pg_cuvs--0.9.3.sql'
NEW_SYM = "'pg_cuvs_hnsw_handler_c1p'"
OLD_SYM = "'pg_cuvs_hnsw_handler'"

# name -> (file, exact text, replacement); the text occurs exactly once in the file
MUTANTS = {
    'downgrade-skips-invalidation': (DOWN, 'SELECT pg_cuvs_cover_invalidate_all();', 'SELECT 1;'),
    'downgrade-keeps-new-symbol': (DOWN, "AS '$libdir/pg_cuvs', 'pg_cuvs_hnsw_handler'\nLANGUAGE C;", "AS '$libdir/pg_cuvs', 'pg_cuvs_hnsw_handler_c1p'\nLANGUAGE C;"),
    'downgrade-skips-handler-revert': (DOWN, 'CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal)', 'CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler_unused(internal)'),
    'downgrade-leaves-function': (DOWN, 'DROP FUNCTION pg_cuvs_cover_invalidate_all();', 'SELECT 1;'),
    'downgrade-leaves-status-function': (DOWN, 'DROP FUNCTION pg_cuvs_hnsw_cover_status(regclass);', 'SELECT 1;'),
    'upgrade-keeps-old-symbol': (UP, "AS '$libdir/pg_cuvs', 'pg_cuvs_hnsw_handler_c1p'", "AS '$libdir/pg_cuvs', 'pg_cuvs_hnsw_handler'"),
    'base-keeps-old-symbol': (BASE, "AS '$libdir/pg_cuvs', 'pg_cuvs_hnsw_handler_c1p'", "AS '$libdir/pg_cuvs', 'pg_cuvs_hnsw_handler'"),
    'invalidation-open-to-public': (BASE, 'REVOKE ALL ON FUNCTION pg_cuvs_cover_invalidate_all() FROM PUBLIC;', 'SELECT 1;'),
}


def run(runner, sql_dir, out, passthrough):
    return subprocess.run([sys.executable, str(runner), '--output', str(out), '--sql-dir', str(sql_dir), *passthrough],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('runner_args', nargs=argparse.REMAINDER, help='after --: --new-lib ... --old-lib ... [--port N]')
    args = ap.parse_args()
    passthrough = [a for a in args.runner_args if a != '--']
    repo = Path(__file__).resolve().parents[2]
    runner = repo / 'test/unit/test_downgrade_pg.py'
    args.output.mkdir(parents=True, exist_ok=False)
    result = {'mutants': [], 'verdict': 'PASS'}
    base = run(runner, repo / 'sql', args.output / 'unmutated', passthrough)
    if base.returncode != 0:
        result.update(verdict='FAIL', error='the unmutated scripts do not pass: ' + base.stderr[-400:])
    else:
        for name, (fname, old, new) in MUTANTS.items():
            with tempfile.TemporaryDirectory() as work:
                sql = Path(work) / 'sql'
                shutil.copytree(repo / 'sql', sql)
                text = (sql / fname).read_text()
                if text.count(old) != 1:
                    result['verdict'] = 'FAIL'
                    result['mutants'].append({'name': name, 'status': 'NOT-APPLICABLE', 'count': text.count(old)})
                    continue
                (sql / fname).write_text(text.replace(old, new, 1))
                r = run(runner, sql, args.output / name, passthrough)
                status = 'KILLED' if r.returncode != 0 else 'SURVIVED'
                if status == 'SURVIVED':
                    result['verdict'] = 'FAIL'
                failed = sorted(set(l.split(' ', 1)[1] for l in r.stderr.splitlines() if l.startswith('FAIL ')))
                result['mutants'].append({'name': name, 'status': status, 'failed_checks': failed[:4]})
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['verdict'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())

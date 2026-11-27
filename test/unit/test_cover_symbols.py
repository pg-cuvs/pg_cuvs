#!/usr/bin/env python3
"""Source check for the handler symbol versioning of decision "V+D" (no PostgreSQL needed).

  * SQL up to 0.9.2 (base scripts and upgrade scripts) names the OLD C symbol pg_cuvs_hnsw_handler.
  * The 0.9.3 base script and the 0.9.2 -> 0.9.3 script name the NEW symbol pg_cuvs_hnsw_handler_c1p.
  * The 0.9.3 -> 0.9.2 script invalidates first, then names the old symbol, then drops the function.
  * hnsw_export.c exports both symbols, and the old one asks for the routine without cover wrappers.
  * hnsw_store_connectivity (which resets pd_upper and so would drop a cover descriptor) is called only from the one
    finishing function hnsw_stage_metapage_tail, and every metapage-building path calls that function.
  * With --library, the shared object exports both handlers and the invalidation function.
The checker is run against damaged copies of its own inputs first, so a check that cannot fail is caught.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

OLD, NEW = 'pg_cuvs_hnsw_handler', 'pg_cuvs_hnsw_handler_c1p'
FUNC = re.compile(r"FUNCTION\s+pg_cuvs_hnsw_handler\s*\(internal\)\s*RETURNS\s+index_am_handler\s*AS\s*'\$libdir/pg_cuvs',\s*'(\w+)'", re.S)


def handler_symbols(text):
    return FUNC.findall(text)


def check(files, c_source):
    """files: {name: text}. Returns a list of problems (empty = fine)."""
    bad = []
    for name, text in files.items():
        syms = handler_symbols(text)
        m = re.fullmatch(r'pg_cuvs--([\d.]+)(?:--([\d.]+))?\.sql', name)
        if not m:
            continue
        frm, to = m.group(1), m.group(2)
        if to is None:
            want = NEW if frm == '0.9.3' else OLD
        elif (frm, to) == ('0.9.2', '0.9.3'):
            want = NEW
        elif (frm, to) == ('0.9.3', '0.9.2'):
            want = OLD
        else:
            want = OLD
        for s in syms:
            if s != want:
                bad.append(f'{name}: handler names {s}, expected {want}')
        if (frm, to) in (('0.9.2', '0.9.3'), ('0.9.3', '0.9.2')) and not syms:
            bad.append(f'{name}: no handler definition')
        if to is None and frm == '0.9.3' and not syms:
            bad.append(f'{name}: no handler definition')
    down = files.get('pg_cuvs--0.9.3--0.9.2.sql', '')
    i, j, k = (down.find(x) for x in ('SELECT pg_cuvs_cover_invalidate_all();', "'" + OLD + "'", 'DROP FUNCTION pg_cuvs_cover_invalidate_all();'))
    if not (0 <= i < j < k):
        bad.append('downgrade script: expected invalidate call, then old symbol, then DROP FUNCTION, in that order')
    for name in ('pg_cuvs--0.9.3.sql', 'pg_cuvs--0.9.2--0.9.3.sql'):
        t = files.get(name, '')
        if 'REVOKE ALL ON FUNCTION pg_cuvs_cover_invalidate_all() FROM PUBLIC;' not in t:
            bad.append(f'{name}: the invalidation function is not revoked from PUBLIC')
        if "'pg_cuvs_cover_invalidate_all'" not in t:
            bad.append(f'{name}: the invalidation function is not defined')
    for sym, flag in ((OLD, 'false'), (NEW, 'true')):
        if f'PG_FUNCTION_INFO_V1({sym});' not in c_source:
            bad.append(f'hnsw_export.c: {sym} is not declared')
        pat = re.compile(r'PG_FUNCTION_INFO_V1\(%s\);\s*Datum\s*%s\(PG_FUNCTION_ARGS\)\s*\{\s*PG_RETURN_POINTER\(cuvs_hnsw_handler_routine\(%s\)\);' % (sym, sym, flag))
        if not pat.search(c_source):
            bad.append(f'hnsw_export.c: {sym} must return cuvs_hnsw_handler_routine({flag})')
    bad += check_single_finisher(c_source)
    return bad


def check_single_finisher(c_source):
    bad = []
    stage = re.search(r"\nhnsw_stage_metapage_tail\(.*?\n}\n", c_source, re.S)
    if stage is None:
        return ['hnsw_export.c: hnsw_stage_metapage_tail is missing']
    outside = c_source[:stage.start()] + c_source[stage.end():]
    calls = [m.start() for m in re.finditer(r"hnsw_store_connectivity\s*\(", outside)]
    definition = len(re.findall(r"\nhnsw_store_connectivity\(Page", outside))
    if len(calls) != definition:
        bad.append('hnsw_export.c: hnsw_store_connectivity is called outside hnsw_stage_metapage_tail')
    users = len(re.findall(r"hnsw_stage_metapage_tail\s*\(", outside))
    if users < 5:
        bad.append(f'hnsw_export.c: {users} callers of hnsw_stage_metapage_tail, expected the four build paths and cuvs_cover_buildempty')
    return bad


def check_library(path):
    out = subprocess.run(['nm', '-D', '--defined-only', str(path)], stdout=subprocess.PIPE, text=True, check=True).stdout
    return [f'{path}: does not export {s}' for s in (OLD, NEW, 'pg_cuvs_cover_invalidate_all')
            if not re.search(r'\sT\s%s$' % s, out, re.M)]


def self_test(files, c_source):
    """Every damage below must be reported; the damage list is the proof that the checks can fail."""
    def edit(name, old, new):
        f = dict(files)
        assert f[name].count(old) >= 1, (name, old)
        f[name] = f[name].replace(old, new)
        return f, c_source
    cases = {
        'base 0.9.3 on the old symbol': edit('pg_cuvs--0.9.3.sql', "'" + NEW + "'", "'" + OLD + "'"),
        'upgrade on the old symbol': edit('pg_cuvs--0.9.2--0.9.3.sql', "'" + NEW + "'", "'" + OLD + "'"),
        'base 0.9.2 on the new symbol': edit('pg_cuvs--0.9.2.sql', "'" + OLD + "'", "'" + NEW + "'"),
        'downgrade on the new symbol': edit('pg_cuvs--0.9.3--0.9.2.sql', "'" + OLD + "'", "'" + NEW + "'"),
        'downgrade without invalidation': edit('pg_cuvs--0.9.3--0.9.2.sql', 'SELECT pg_cuvs_cover_invalidate_all();', 'SELECT 1;'),
        'downgrade invalidates after the handler': (
            {**files, 'pg_cuvs--0.9.3--0.9.2.sql': "CREATE OR REPLACE FUNCTION pg_cuvs_hnsw_handler(internal) RETURNS index_am_handler AS '$libdir/pg_cuvs', '" + OLD + "' LANGUAGE C;\nSELECT pg_cuvs_cover_invalidate_all();\nDROP FUNCTION pg_cuvs_cover_invalidate_all();\n"}, c_source),
        'invalidation open to PUBLIC': edit('pg_cuvs--0.9.3.sql', 'REVOKE ALL ON FUNCTION pg_cuvs_cover_invalidate_all() FROM PUBLIC;', ''),
        'old symbol asks for wrappers': (files, c_source.replace('cuvs_hnsw_handler_routine(false)', 'cuvs_hnsw_handler_routine(true)', 1)),
        'a build path calls the old finisher': (files, c_source.replace('hnsw_stage_metapage_tail(rel, staged, connectivity, false);', 'hnsw_store_connectivity(staged, connectivity);', 1)),
        'a build path loses the finisher': (files, c_source.replace('hnsw_stage_metapage_tail(indexRel,metapage,&c,true);', '', 1)),
        'new symbol missing': (files, c_source.replace('PG_FUNCTION_INFO_V1(%s);' % NEW, '', 1)),
    }
    return [name for name, (f, c) in cases.items() if not check(f, c)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--library', type=Path, help='a built pg_cuvs.so to inspect with nm')
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    files = {p.name: p.read_text() for p in (repo / 'sql').glob('pg_cuvs--*.sql')}
    c_source = (repo / 'src/hnsw_export.c').read_text()
    problems = check(files, c_source)
    survivors = self_test(files, c_source)
    if survivors:
        problems.append('self-test: damage not reported: ' + ', '.join(survivors))
    if args.library:
        problems += check_library(args.library)
    for p in problems:
        print('FAIL', p, file=sys.stderr)
    print('test_cover_symbols:', 'FAILED' if problems else 'all checks passed')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())

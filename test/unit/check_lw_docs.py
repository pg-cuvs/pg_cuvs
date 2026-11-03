"""CK-27 (a) for the I4 knobs: docs/reference.md flag rows vs src/cuvs_lw_policy.h.

Usage: python3 -I test/unit/check_lw_docs.py [REPO_ROOT]

Checks each of the four I4 flags has exactly one row in the daemon flag table,
that its default and range text equal the macros, that the margin row states no
number (it points at SHOW cuvs.rmm_pool_status, so the LW-8 one-line commit
does not touch the docs), and that the provisional margin value appears nowhere
in the reference. CK-27 (b)(c) (startup line, pool status) are in
test_lw_startup.c.
"""
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
DOC = (ROOT / 'docs' / 'reference.md').read_text()
HDR = (ROOT / 'src' / 'cuvs_lw_policy.h').read_text()


def macro(name):
    m = re.search(rf'^#define {name}\s+(\S+)', HDR, re.M)
    if not m:
        raise SystemExit(f'FAIL macro {name} missing')
    return m.group(1)


def row(flag):
    rows = [l for l in DOC.splitlines() if l.startswith(f'| `{flag} ')]
    if len(rows) != 1:
        raise SystemExit(f'FAIL {flag}: {len(rows)} doc rows')
    # The value cell may contain an escaped pipe (error\|managed); rejoin it.
    joined = rows[0].replace('\\|', '\x00')
    cells = [c.strip().replace('\x00', '|') for c in joined.strip('|').split('|')]
    return cells


failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


policy = {'CUVS_LW_OVERFLOW_MANAGED': 'managed', 'CUVS_LW_OVERFLOW_ERROR': 'error'}
want_policy = policy.get(macro('CUVS_CAGRA_BUILD_OVERFLOW_DEFAULT'), '?')
want_overcommit = 'on' if macro('CUVS_MANAGED_OVERCOMMIT_DEFAULT') == '1' else 'off'
margin = macro('CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT')
margin_min = macro('CUVS_MANAGED_LW_MARGIN_PERMILLE_MIN').rstrip('u')
margin_max = macro('CUVS_MANAGED_LW_MARGIN_PERMILLE_MAX').rstrip('u')
oom_default = macro('CUVS_OOM_SCORE_ADJ_DEFAULT')
oom_max = macro('CUVS_OOM_SCORE_ADJ_MAX')

c = row('--cagra-build-overflow')
check(c[2] == f'`{want_policy}`', f'--cagra-build-overflow default {c[2]!r} != `{want_policy}`')
check('error|managed' in c[0], '--cagra-build-overflow values')
c = row('--managed-overcommit')
check(c[2] == f'`{want_overcommit}`', f'--managed-overcommit default {c[2]!r} != `{want_overcommit}`')
c = row('--managed-lw-margin-permille')
check(not re.search(r'\d', c[2]), f'margin default cell states a number: {c[2]!r}')
check('lw_margin_permille' in c[2] and 'SHOW cuvs.rmm_pool_status' in c[2],
      'margin default cell must point at SHOW cuvs.rmm_pool_status lw_margin_permille')
check(f'{margin_min}–{margin_max}' in c[3], f'margin range != {margin_min}–{margin_max}')
c = row('--oom-score-adj')
check(c[2] == f'`{oom_default}`', f'--oom-score-adj default {c[2]!r} != `{oom_default}`')
check(f'0–{oom_max}' in c[3], f'--oom-score-adj range != 0–{oom_max}')
for c in (row('--cagra-build-overflow'), row('--managed-overcommit'),
          row('--managed-lw-margin-permille'), row('--oom-score-adj')):
    check('restart' in c[1], f'{c[0]}: layer/restart cell {c[1]!r}')
check(not re.search(rf'\b{margin}\b', DOC), f'provisional margin {margin} appears in the reference')

if failures:
    for f in failures:
        print('FAIL', f)
    sys.exit(1)
print('PASS lw docs: four I4 flag rows match cuvs_lw_policy.h defaults and ranges; '
      'margin row has no number')

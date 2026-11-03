"""I3-C behavioral mutants of the real headers against test_hnsw_auto_overflow.c.

Each mutant injects one defect the brief names (default reverted to error,
silent transition, decision without statistics refusing, missing disk check,
...) into a COPY of the headers; the unit test must fail with an assertion.
CPU only; no GPU/SDK/PG/daemon.
"""
from pathlib import Path
import argparse
import json
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HEADERS = ['cuvs_hnsw_policy.h', 'cuvs_hnsw_build_common.h', 'cuvs_route_preflight.h',
           'cuvs_ipc.h', 'cuvs_admission_contract.h']  # copied together: quoted includes resolve next to the includer

MUTANTS = [
    ('default_reverted_to_error', 'cuvs_hnsw_policy.h',
     '#define CUVS_HNSW_AUTO_OVERFLOW_DEFAULT CUVS_HNSW_OVERFLOW_ACE',
     '#define CUVS_HNSW_AUTO_OVERFLOW_DEFAULT CUVS_HNSW_OVERFLOW_ERROR'),
    ('overflow_branch_ignores_knob', 'cuvs_hnsw_build_common.h',
     '        && overflow == CUVS_HNSW_OVERFLOW_ACE;', ';'),
    ('explicit_ivf_pq_overflows', 'cuvs_hnsw_build_common.h',
     'int overflow_to_ace = !tiny && exceeded && algo == CUVS_HNSW_ALGO_AUTO',
     'int overflow_to_ace = !tiny && exceeded && algo != CUVS_HNSW_ALGO_ACE'),
    ('overflow_not_recorded', 'cuvs_hnsw_build_common.h',
     'out->overflowed = (uint32_t)overflow_to_ace;', 'out->overflowed = 0;'),
    ('receipt_accepts_unrecorded_auto_ace', 'cuvs_hnsw_policy.h',
     '        && (!!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_AUTO_OVERFLOW)\n            == (p->requested_algo == CUVS_HNSW_ALGO_AUTO && p->route == CUVS_HNSW_ROUTE_ACE))\n',
     ''),
    ('transition_notice_skipped', 'cuvs_route_preflight.h',
     '| (transition_sent ? 0 : CUVS_HNSW_NOTICE_TRANSITION);', ';'),
    ('completion_notice_skipped', 'cuvs_route_preflight.h',
     '        return CUVS_HNSW_NOTICE_AUTO_COMPLETION\n', '        return 0\n'),
    ('stats_zero_refuses_instead_of_fallback', 'cuvs_route_preflight.h',
     '    if (!(reltuples >= 1.0) || !isfinite(reltuples)) return CUVS_HNSW_PRESPOOL_FALLBACK;\n', ''),
    ('fallback_not_recorded_in_receipt', 'cuvs_route_preflight.h',
     '    if (action != CUVS_HNSW_PRESPOOL_FALLBACK) return CUVS_HNSW_ROUTE_FLAG_PRESPOOL;',
     '    return CUVS_HNSW_ROUTE_FLAG_PRESPOOL;'),
    # I3CD-B1: a failed pre-scan forecast with statistics recorded as "no statistics"
    ('preflight_failure_recorded_as_no_statistics', 'cuvs_route_preflight.h',
     '        | (cuvs_prespool_rows(reltuples) ? 0 : CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS);',
     '        | CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS;'),
    ('route_decided_ignores_cause', 'cuvs_hnsw_policy.h',
     '            ? "after_scan_no_statistics" : "after_scan_preflight_unavailable";',
     '            ? "after_scan_no_statistics" : "after_scan_no_statistics";'),
    ('no_statistics_without_after_scan_valid', 'cuvs_hnsw_policy.h',
     '        && (!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_NO_STATISTICS)\n            || (p->route_flags & CUVS_HNSW_ROUTE_FLAG_AFTER_SCAN))\n', ''),
    ('prespool_overflow_not_announced', 'cuvs_route_preflight.h',
     '        ? CUVS_HNSW_PRESPOOL_NOTICE_OVERFLOW : CUVS_HNSW_PRESPOOL_PROCEED;',
     '        ? CUVS_HNSW_PRESPOOL_PROCEED : CUVS_HNSW_PRESPOOL_PROCEED;'),
    ('disk_precheck_missing', 'cuvs_hnsw_build_common.h',
     '    f->disk_ok = f->status == CUVS_STATUS_OK && disk_regular && disk_avail >= f->disk_need;',
     '    f->disk_ok = f->status == CUVS_STATUS_OK;'),
    ('disk_formula_changed', 'cuvs_hnsw_build_common.h',
     '    return raw * 4 + n * (degree * 8ULL + 64);', '    return raw * 2 + n * (degree * 8ULL + 64);'),
    ('cagra_ar0_refuses_without_stats', 'cuvs_route_preflight.h',
     '    if (!n || !dim || !max_limit || !n_devices) return 0;',
     '    if (!n) n = UINT32_MAX;\n    if (!dim || !max_limit || !n_devices) return 0;'),
    ('cagra_ar0_ignores_auto_sharding', 'cuvs_route_preflight.h',
     '    if (!(shard_count == 1 || (shard_count == 0 && n_devices == 1))) return 0;\n', ''),
]


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main(output):
    out = Path(output).resolve(); out.mkdir(exist_ok=False)
    originals = {n: (ROOT / 'src' / n).read_text() for n in HEADERS}
    rows = []
    for name, header, old, new in MUTANTS:
        case = out / name; case.mkdir()
        assert originals[header].count(old) == 1, ('MUTANT_ANCHOR', name, originals[header].count(old))
        for n, body in originals.items():
            (case / n).write_text(body.replace(old, new, 1) if n == header else body)
        argv = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-I', str(case), '-I', 'src', '-o',
                str(case / 'check'), 'test/unit/test_hnsw_auto_overflow.c', '-lpthread', '-lm']
        c = subprocess.run(argv, cwd=ROOT, capture_output=True, timeout=60)
        if c.returncode != 0:
            rows.append(dict(name=name, status='FAIL_COMPILE_NOT_GUARD_DETECTION', stderr=c.stderr.decode()[-400:]))
            continue
        r = subprocess.run([str(case / 'check')], cwd=ROOT, capture_output=True, timeout=60, preexec_fn=no_core)
        err = r.stderr.decode()
        caught = r.returncode != 0 and 'Assertion failed' in err or 'assertion' in err.lower()
        rows.append(dict(name=name, status='PASS_MUTATION_DETECTED' if caught else 'FAIL_MUTATION_SURVIVED',
                         exit=r.returncode, assertion=err.strip().splitlines()[-1][:200] if err.strip() else ''))
    unchanged = all((ROOT / 'src' / n).read_text() == v for n, v in originals.items())
    status = 'PASS' if unchanged and all(r['status'] == 'PASS_MUTATION_DETECTED' for r in rows) else 'FAIL'
    value = dict(status=status, count=len(rows), production_sources_unchanged=unchanged, results=rows,
                 scope='CPU header mutants vs test_hnsw_auto_overflow.c; GPU/SDK/PG not run')
    (out / 'RESULT.json').write_text(json.dumps(value, indent=2) + '\n')
    print(json.dumps(value))
    assert status == 'PASS'


if __name__ == '__main__':
    a = argparse.ArgumentParser(); a.add_argument('output'); main(a.parse_args().output)

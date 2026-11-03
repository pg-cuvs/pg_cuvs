"""I3-E / C8 mutation runner: every planted defect must turn an I3-E test RED.

Usage: python3 -I test/unit/test_i3e_mutations.py OUT_DIR   (OUT_DIR must not exist)

Tests: 'obs' = test/unit/test_i3e_observation.c built against the CPU shim
(Linux: memfd, shm), 'sql' = test/unit/check_i3e_sql.py. The unmutated tree
must PASS both first. A mutation that only breaks the build counts as
BUILD-BROKEN, not RED. Defect classes (task brief): field omission, unit
error, stale value; plus counting, filtering and bound defects.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get('CC', 'cc')

SERVER = 'src/pg_cuvs_server.c'
BACKEND = 'src/pg_cuvs.c'
IPC_C = 'src/cuvs_ipc.c'
TEXT_C = 'src/cuvs_status_text.c'
UP = 'sql/pg_cuvs--0.8.2--0.9.0.sql'
INST = 'sql/pg_cuvs--0.9.0.sql'
DOCS = 'docs/reference.md'


def both(old, new):
    return [(UP, old, new), (INST, old, new)]


# name: ([(file, old, new), ...], tests expected to turn RED)
MUTATIONS = {
    # --- rejection counters ---
    'reject_not_counted': ([(SERVER,
        '        observe_reject(dev, reason);   /* I3-E: one count per emitted rejection */',
        '        (void)observe_reject;')], {'obs'}),
    'reject_counts_oom_att': ([(SERVER,
        '(int)reason > (int)CUVS_ADMISSION_AR7)\n        return;',
        '(int)reason > (int)CUVS_ADMISSION_ATT)\n        return;\n    if ((int)reason > (int)CUVS_ADMISSION_AR7) { g_last_reject_reason1[dev < 0 ? 0 : dev] = (int)reason + 1; return; }')],
        {'obs'}),
    'no_gpu_charged_to_gpu0': ([(SERVER,
        '        g_rejects_no_gpu[reason]++;\n        return;',
        '        g_rejects[0][reason]++;\n        return;')], {'obs'}),
    'last_reject_not_updated': ([(SERVER,
        '    g_last_reject_reason1[dev] = (int)reason + 1;\n', '')], {'obs'}),
    'reject_time_not_updated': ([(SERVER,
        '    g_last_reject_at[dev] = time(NULL);\n', '')], {'obs'}),
    # stale value: a read resets the counters
    'read_resets_rejects': ([(SERVER,
        '        tb_printf(b, "\\t%llu", (unsigned long long)g_rejects[dev][r]);',
        '        { tb_printf(b, "\\t%llu", (unsigned long long)g_rejects[dev][r]); g_rejects[dev][r] = 0; }')],
        {'obs'}),
    # --- memory_status columns ---
    # field omission: builds_active dropped from the row
    'mem_field_omitted': ([(SERVER,
        '    tb_printf(b, "\\t%zu\\t%zu\\t%lld\\t%lld\\t%zu\\t%llu\\t%d",',
        '    tb_printf(b, "\\t%zu\\t%zu\\t%lld\\t%lld\\t%zu\\t%llu",'),
        (SERVER, '              (unsigned long long)g_lw_pending[dev],\n              g_pending_build_count[dev]);',
         '              (unsigned long long)g_lw_pending[dev]);')], {'obs'}),
    # unit error: lw_pending in MiB
    'mem_lw_pending_mib': ([(SERVER,
        '              (unsigned long long)g_lw_pending[dev],\n              g_pending_build_count[dev]);',
        '              (unsigned long long)(g_lw_pending[dev] >> 20),\n              g_pending_build_count[dev]);')],
        {'obs'}),
    'mem_budget_quota_swapped': ([(SERVER,
        '              g_max_vram_per_gpu[dev], g_rmm_pool_limits[dev],',
        '              g_rmm_pool_limits[dev], g_max_vram_per_gpu[dev],')], {'obs'}),
    'mem_pool_without_quota': ([(SERVER,
        '    int pool_ok = g_rmm_pool_limits[dev] > 0 && cuvs_rmm_pool_stats(dev, &ps) == 0;',
        '    int pool_ok = cuvs_rmm_pool_stats(dev, &ps) == 0;')], {'obs'}),
    'mem_pending_rmm_is_logical': ([(SERVER,
        '              g_pending_rmm_build_vram[dev],\n              (unsigned long long)g_lw_pending[dev],',
        '              g_pending_build_vram[dev],\n              (unsigned long long)g_lw_pending[dev],')],
        {'obs'}),
    'kind_i64n_never_null': ([(TEXT_C,
        '            if (v < -1)\n                return -1;\n            *isnull = v == -1;',
        '            if (v < -1)\n                return -1;\n            *isnull = 0;')], {'obs'}),
    # --- build_status rows ---
    'single_not_registered': ([(SERVER,
        '    lw_pending_reserve(lw_ub, target_gpu);\n    int build_slot = build_status_register(&al, lw_policy, lw_ub);',
        '    lw_pending_reserve(lw_ub, target_gpu);\n    int build_slot = -1;')], {'obs'}),
    'multi_not_registered': ([(SERVER,
        '        int build_slot = build_status_register(&al, lw_policy, lw_ub);',
        '        int build_slot = -1;')], {'obs'}),
    # stale value: the row outlives its build
    'single_not_unregistered': ([(SERVER,
        '    lw_pending_release(lw_ub, target_gpu);\n    build_status_unregister(build_slot);   /* I3-E */',
        '    lw_pending_release(lw_ub, target_gpu);\n    (void)build_slot;   /* I3-E */')], {'obs'}),
    'multi_not_unregistered': ([(SERVER,
        '        lw_pending_release(lw_ub, target_gpu);\n        build_status_unregister(build_slot);   /* I3-E */',
        '        lw_pending_release(lw_ub, target_gpu);\n        (void)build_slot;   /* I3-E */')], {'obs'}),
    'fill_ignores_used_flag': ([(SERVER,
        '        if (g_builds_used[slot])\n            status->builds[status->n_builds++] = g_builds[slot];',
        '        status->builds[status->n_builds++] = g_builds[slot];')], {'obs'}),
    'started_at_monotonic': ([(SERVER,
        '        clock_gettime(CLOCK_REALTIME, &ts);\n        r->gpu = a->gpu;',
        '        clock_gettime(CLOCK_MONOTONIC, &ts);\n        r->gpu = a->gpu;')], {'obs'}),
    # unit error: scratch reported as N
    'scratch_is_n': ([(SERVER,
        '        r->scratch_bytes = a->needed > a->final ? a->needed - a->final : 0;',
        '        r->scratch_bytes = a->needed;')], {'obs'}),
    'lw_ub_not_from_build': ([(SERVER,
        '        r->lw_ub_bytes = lw_ub;', '        r->lw_ub_bytes = 0; (void)lw_ub;')], {'obs'}),
    'unlisted_not_counted': ([(SERVER,
        '    g_builds_unlisted++;\n    return -1;', '    return -1;')], {'obs'}),
    'client_nbuilds_unbounded': ([(IPC_C,
        ' ||\n             out->n_builds > CUVS_MAX_ACTIVE_BUILDS))', '))')], {'obs'}),
    'dataset_attached_reported_true': ([(SERVER,
        '                 "\\t-1\\n",', '                 "\\t1\\n",')], {'obs'}),
    # --- SQL / backend / docs ---
    'sql_blocks_differ': ([(UP,
        '    managed_spill_bytes_max bigint\n)\nAS \'MODULE_PATHNAME\', \'pg_cuvs_memory_status\'',
        '    managed_spill_bytes_max integer\n)\nAS \'MODULE_PATHNAME\', \'pg_cuvs_memory_status\'')], {'sql'}),
    'sql_column_omitted': (both(
        '    rejects_ar7             bigint,\n', ''), {'sql'}),
    'sql_build_status_revoked': (both(
        "COMMENT ON FUNCTION pg_cuvs_build_status() IS",
        "REVOKE ALL ON FUNCTION pg_cuvs_build_status() FROM PUBLIC;\nCOMMENT ON FUNCTION pg_cuvs_build_status() IS"),
        {'sql'}),
    'sql_memory_grant_dropped': (both(
        'GRANT EXECUTE ON FUNCTION pg_cuvs_memory_status()    TO pg_monitor;\n\nCREATE FUNCTION pg_cuvs_build_status()',
        '\nCREATE FUNCTION pg_cuvs_build_status()'), {'sql'}),
    'backend_build_status_unfiltered': ([(BACKEND,
        '        if (!cuvs_status_row_visible(is_priv, r->db_oid, (uint32_t) MyDatabaseId, owns))\n            continue;',
        '        (void) is_priv; (void) owns;')], {'sql'}),
    'backend_build_status_value_omitted': ([(BACKEND,
        '        values[c++] = CStringGetTextDatum(r->path);\n', '')], {'sql'}),
    # field omission in the docs: a column the C9 table text never names
    'docs_column_missing': ([(DOCS,
        '`managed_spill_builds` and `managed_spill_bytes_max`', '`managed_spill_builds`')],
        {'sql'}),
}

TESTS = ('obs', 'sql')


def run(argv, cwd):
    try:
        q = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=180)
    except subprocess.TimeoutExpired:
        return 124, 'timeout'
    return q.returncode, q.stdout + q.stderr


OBS_SRCS = ['test/unit/test_i3e_observation.c', 'src/cuvs_ipc.c', 'src/cuvs_util.c',
            'src/cuvs_status_text.c', 'src/cuvs_objstore.c', 'src/cuvs_build_corpus.c',
            'src/cuvs_wrapper_shim_cpu.c', 'src/cuvs_cache_policy.c', 'src/cuvs_peak_meter.c']


def build_and_run(tree):
    out = {}
    rc, text = run([CC, '-std=gnu11', '-O1', '-DPGCUVS_CPU_SHIM', '-ffunction-sections',
                    '-fdata-sections', '-I', 'src'] + OBS_SRCS +
                   ['-Wl,--gc-sections', '-lm', '-lpthread', '-lrt', '-lcurl', '-lssl',
                    '-lcrypto', '-o', 'bin-obs'], tree)
    if rc != 0:
        out['obs'] = ('BUILD-BROKEN', text.strip().splitlines()[-1:] or [''])
    else:
        rc, text = run(['./bin-obs'], tree)
        lines = [l for l in text.splitlines() if l.startswith(('FAIL', 'PASS', 'test_i3e'))]
        out['obs'] = ('PASS' if rc == 0 else 'RED (run)', lines[:1] or text.strip().splitlines()[-1:])
    rc, text = run([sys.executable, '-I', 'test/unit/check_i3e_sql.py', '.'], tree)
    lines = [l for l in text.splitlines() if l.startswith(('FAIL', 'check_i3e'))]
    out['sql'] = ('PASS' if rc == 0 else 'RED (run)', lines[:1] or [''])
    return out


def copy_tree(dest):
    dest.mkdir(parents=True)
    for d in ('src', 'sql'):
        shutil.copytree(ROOT / d, dest / d)
    shutil.copytree(ROOT / 'test' / 'unit', dest / 'test' / 'unit')
    (dest / 'docs').mkdir()
    shutil.copy2(ROOT / 'docs' / 'reference.md', dest / 'docs' / 'reference.md')


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    out = Path(argv[1]).resolve()
    out.mkdir(parents=False, exist_ok=False)
    base = out / 'baseline'
    copy_tree(base)
    res = build_and_run(base)
    for name, (status, tail) in res.items():
        print(f'BASE  {name}: {status} {tail[0] if tail else ""}')
    if any(s != 'PASS' for s, _ in res.values()):
        print('baseline not green')
        return 1
    red = 0
    for name, (edits, expect) in MUTATIONS.items():
        tree = out / name
        copy_tree(tree)
        for target, old, new in edits:
            path = tree / target
            text = path.read_text()
            if text.count(old) != 1:
                print(f'STALE {name}: pattern count {text.count(old)} in {target}')
                return 1
            path.write_text(text.replace(old, new))
        res = build_and_run(tree)
        reds = {t for t, (s, _) in res.items() if s == 'RED (run)'}
        broken = {t for t, (s, _) in res.items() if s == 'BUILD-BROKEN'}
        ok = expect <= reds and not broken
        red += ok
        detail = '; '.join(f'{t}={s} {tail[0] if tail else ""}'.strip() for t, (s, tail) in res.items()
                           if t in reds or t in broken)
        print(f'{"RED  " if ok else "GREEN"} {name}: {detail or "all tests passed"}')
        shutil.rmtree(tree)
    print(f'mutations: {red}/{len(MUTATIONS)} RED')
    return 0 if red == len(MUTATIONS) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))

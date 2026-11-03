"""I4 / ADR-088 mutation runner: every planted defect must turn an I4 test RED.

Usage: python3 -I test/unit/test_lw_mutations.py OUT_DIR   (OUT_DIR must not exist)

Builds the unmutated tree first (all three tests must PASS), then one copy per
mutation. The daemon fixture needs Linux (memfd, /proc, shm); run this inside
the CPU CI container as a non-root user so the EACCES branch is real.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get('CC', 'cc')
CXX = os.environ.get('CXX', 'c++')
LINUX = sys.platform.startswith('linux')

POLICY = 'src/cuvs_lw_policy.h'
GUARD = 'src/cuvs_lw_guard.hpp'
SERVER = 'src/pg_cuvs_server.c'
UTIL = 'src/cuvs_util.c'
DOCS = 'docs/reference.md'

# name: (file, [(old, new), ...], tests expected to turn RED)
MUTATIONS = {
    # brief: AR6 missing with overcommit off
    'ar6_missing_overcommit_off': (POLICY, [(
        'if (r.spill_est > 0) r.reason = CUVS_ADMISSION_AR6;',
        'if (r.spill_est > 0) (void)0;')], {'policy', 'daemon'}),
    # brief: host guard ignored
    'host_guard_ignored': (POLICY, [(
        '} else if (r.spill_est > in->host_guard) {', '} else if (0) {')], {'policy', 'daemon'}),
    'ar7_boundary_ge': (POLICY, [(
        '} else if (r.spill_est > in->host_guard) {',
        '} else if (r.spill_est >= in->host_guard) {')], {'policy', 'daemon'}),
    'pool_slack_ignored': (POLICY, [(
        'growth = in->n_m > pool_slack ? in->n_m - pool_slack : 0;',
        'growth = in->n_m; (void)pool_slack;')], {'policy'}),
    'shape_uses_dim_not_ld': (POLICY, [(
        'uint64_t ld = ((uint64_t)dim + 3u) / 4u * 4u;', 'uint64_t ld = dim;')], {'policy'}),
    'shape_prune_term_dropped': (POLICY, [(
        'return sort_term > prune_term ? sort_term : prune_term;',
        '(void)prune_term; return sort_term;')], {'policy'}),
    'lw_ub_floor_not_ceil': (POLICY, [(
        'return scaled / 1000u + (scaled % 1000u != 0);', 'return scaled / 1000u;')], {'policy'}),
    # review C-5 / CK-11 (l): LW-8 margin rule
    'margin_rule_no_lower_bound': (POLICY, [(
        '    unsigned __int128 best = CUVS_MANAGED_LW_MARGIN_PERMILLE_MIN;',
        '    unsigned __int128 best = 0;')], {'policy'}),
    'margin_rule_floor_not_ceil': (POLICY, [(
        'q = ((unsigned __int128)lw_peak[i] * 1100u + shape[i] - 1) / shape[i];',
        'q = ((unsigned __int128)lw_peak[i] * 1100u) / shape[i];')], {'policy'}),
    'margin_rule_no_safety_factor': (POLICY, [(
        'q = ((unsigned __int128)lw_peak[i] * 1100u + shape[i] - 1) / shape[i];',
        'q = ((unsigned __int128)lw_peak[i] * 1000u + shape[i] - 1) / shape[i];')], {'policy'}),
    'lw_checked_grid_widened': (POLICY, [(
        'g >= 128 && g <= 256;', 'g >= 64 && g <= 256;')], {'policy'}),
    'margin_range_removed': (POLICY, [(
        'if (cuvs_lw_parse_uint(s, CUVS_MANAGED_LW_MARGIN_PERMILLE_MIN,\n'
        '                           CUVS_MANAGED_LW_MARGIN_PERMILLE_MAX, &v) != 0)',
        'if (cuvs_lw_parse_uint(s, 0, 4000000000ul, &v) != 0)')], {'policy', 'daemon'}),
    'oom_range_removed': (POLICY, [(
        'cuvs_lw_parse_uint(s, 0, CUVS_OOM_SCORE_ADJ_MAX, &v)',
        'cuvs_lw_parse_uint(s, 0, 100000, &v)')], {'policy', 'daemon'}),
    # brief: oom_score_adj lowering attempt
    'oom_lowers_current': (POLICY, [(
        'if (current >= setting) return CUVS_OOM_ADJ_KEEP;',
        'if (current == setting) return CUVS_OOM_ADJ_KEEP;')], {'policy', 'daemon'}),
    'oom_zero_writes': (SERVER, [
        ('    if (g_oom_score_adj != 0)\n    {\n        FILE *f;', '    if (1)\n    {\n        FILE *f;'),
        ('            if (action == CUVS_OOM_ADJ_WRITE)\n',
         '            if (action == CUVS_OOM_ADJ_WRITE || g_oom_score_adj == 0)\n')], {'daemon'}),
    'oom_failure_exits': (SERVER, [(
        '    LOG_INFO("pg_cuvs_server: %s (--oom-score-adj %d)\\n"',
        '    if (err) exit(1);\n    LOG_INFO("pg_cuvs_server: %s (--oom-score-adj %d)\\n"')], {'daemon'}),
    # brief: completion NOTICE missing
    'notice_missing_pure': (POLICY, [(
        "    if (!spill) { if (len) buf[0] = '\\0'; return 0; }",
        "    if (!spill || 1) { if (len) buf[0] = '\\0'; return 0; }")], {'policy', 'daemon'}),
    'notice_missing_daemon': (SERVER, [(
        '    if (!cuvs_lw_spill_notice(spill, st->lw_peak, st->free_at_peak, notice, notice_len))',
        '    if (1 || !cuvs_lw_spill_notice(spill, st->lw_peak, st->free_at_peak, notice, notice_len))')],
        {'daemon'}),
    'spill_boundary_le': (POLICY, [(
        'lw_peak > 0 && free_at_peak < lw_peak;', 'lw_peak > 0 && free_at_peak <= lw_peak;')],
        {'policy', 'daemon'}),
    'spill_counter_missing': (SERVER, [(
        '        g_managed_spill_builds[device_id]++;\n', '')], {'daemon'}),
    # CK-28
    'guard_no_restore_on_exception': (GUARD, [(
        '~CuvsLwWorkspaceGuard() { if (ops_) ops_->restore(); }',
        '~CuvsLwWorkspaceGuard() { if (ops_ && !std::uncaught_exceptions()) ops_->restore(); }')],
        {'guard'}),
    'guard_installs_under_error': (GUARD, [(
        '        if (policy != CUVS_LW_OVERFLOW_MANAGED)\n            return;\n',
        '        if (policy == 0xFFFFFFFFu)\n            return;\n')], {'guard'}),
    'guard_restores_before_build': (GUARD, [(
        '        ops.install();   /* may throw: then nothing was installed */\n        ops_ = &ops;',
        '        ops.install();\n        ops.restore();\n        ops_ = &ops;')], {'guard'}),
    'counter_peak_not_cumulative': (GUARD, [(
        'uint64_t now = live.fetch_add(bytes, std::memory_order_relaxed) + bytes;',
        'live.fetch_add(bytes, std::memory_order_relaxed); uint64_t now = bytes;')], {'guard'}),
    'sharded_passes_policy': (SERVER, [(
        '                                            cmd->build_algo, dev,\n'
        '                                            CUVS_LW_OVERFLOW_ERROR, NULL);',
        '                                            cmd->build_algo, dev,\n'
        '                                            g_cagra_build_overflow, NULL);')], {'daemon'}),
    'build_drops_policy': (SERVER, [(
        '    uint32_t lw_policy = g_cagra_build_overflow;\n    uint64_t lw_ub = lw_ub_for_build(cmd->n_vecs',
        '    uint32_t lw_policy = CUVS_LW_OVERFLOW_ERROR;\n    uint64_t lw_ub = lw_ub_for_build(cmd->n_vecs')],
        {'daemon'}),
    'build_multi_drops_policy': (SERVER, [(
        '        uint32_t lw_policy = g_cagra_build_overflow;\n        uint64_t lw_ub = lw_ub_for_build(total',
        '        uint32_t lw_policy = CUVS_LW_OVERFLOW_ERROR;\n        uint64_t lw_ub = lw_ub_for_build(total')],
        {'daemon'}),
    # CK-3b
    'lw_pending_not_subtracted': (SERVER, [(
        '    ws->lw_pending = g_lw_pending[device_id];', '    ws->lw_pending = 0;')], {'daemon'}),
    'lw_pending_not_reserved': (SERVER, [(
        '    g_lw_pending[device_id] = cuvs_lw_sat_add(g_lw_pending[device_id], lw_ub);',
        '    (void)lw_ub;')], {'daemon'}),
    'lw_pending_not_released': (SERVER, [(
        '    lw_pending_release(lw_ub, target_gpu);\n    (void)lw_build_finish("build",',
        '    (void)lw_build_finish("build",')], {'daemon'}),
    'error_policy_adds_pending': (SERVER, [(
        '    if (g_cagra_build_overflow != CUVS_LW_OVERFLOW_MANAGED)\n        return 0;\n'
        '    return cuvs_lw_ub_bytes(',
        '    return cuvs_lw_ub_bytes(')], {'daemon'}),
    'startup_accepts_bad_value': (SERVER, [(
        '            if (lw_flag < 0)\n                return 1;', '            (void)lw_flag;')],
        {'daemon'}),
    # review B-2: production host guard and startup application (X1, X10, X11)
    'host_guard_unbounded': (SERVER, [(
        '    char error[160] = "";\n    return hnsw_host_headroom(UINT64_MAX, error, sizeof(error));',
        '    return UINT64_MAX;')], {'daemon'}),
    'oom_apply_call_deleted': (SERVER, [(
        '    apply_oom_score_adj("/proc/self/oom_score_adj");\n', '')], {'startup'}),
    'oom_apply_wrong_path': (SERVER, [(
        '    apply_oom_score_adj("/proc/self/oom_score_adj");\n',
        '    apply_oom_score_adj("/proc/self/oom_adj");\n')], {'startup'}),
    # review C-6: cgroup v1 and read failure give a zero guard
    'cgroup_v1_accepted': (SERVER, [(
        '"HNSW capacity: cgroup v1 memory controller is unsupported; use verified cgroup v2");\n'
        '                            fclose(f); return 0;',
        '"HNSW capacity: cgroup v1 memory controller is unsupported; use verified cgroup v2");\n'
        '                            break;')], {'daemon'}),
    'cgroup_current_unreadable_ignored': (SERVER, [(
        '                        if (!m) { fclose(f); return 0; }',
        '                        if (!m) break;')], {'daemon'}),
    # review C-9 subset: pool status values
    'pool_status_pending_zero': (SERVER, [(
        '        d->lw_pending_bytes = g_lw_pending[dev];', '        d->lw_pending_bytes = 0;')],
        {'daemon'}),
    'pool_status_spill_max_zero': (SERVER, [(
        '        d->managed_spill_bytes_max = g_managed_spill_bytes_max[dev];',
        '        d->managed_spill_bytes_max = 0;')], {'daemon'}),
    'startup_line_drops_margin': (SERVER, [(
        '"lw_margin_permille=%u\\n", cuvs_lw_overflow_name(g_cagra_build_overflow),',
        '"lw_margin=%u\\n", cuvs_lw_overflow_name(g_cagra_build_overflow),')], {'startup'}),
    # CK-27 (a): docs drift from the macros
    'docs_default_policy_drift': (DOCS, [(
        "| `--cagra-build-overflow error\\|managed` | — (daemon-only, restart) | `managed` |",
        "| `--cagra-build-overflow error\\|managed` | — (daemon-only, restart) | `error` |")],
        {'docs'}),
    'docs_margin_number': (DOCS, [(
        '| see `SHOW cuvs.rmm_pool_status` (`lw_margin_permille`) |',
        '| `1250` (see `SHOW cuvs.rmm_pool_status`, `lw_margin_permille`) |')], {'docs'}),
    'macro_oom_default_drift': (POLICY, [(
        '#define CUVS_OOM_SCORE_ADJ_DEFAULT              500',
        '#define CUVS_OOM_SCORE_ADJ_DEFAULT              400')], {'policy', 'docs'}),
    # re-review C-12 (X15) and C-13 (X17)
    'guard_pointer_default_unbounded': (SERVER, [(
        'static uint64_t (*g_lw_host_guard_fn)(void) = lw_host_guard_bytes;',
        'static uint64_t lw_guard_inf(void) { return UINT64_MAX; }\n'
        'static uint64_t (*g_lw_host_guard_fn)(void) = lw_guard_inf;')], {'daemon'}),
    'margin_literal_suffix': (POLICY, [(
        '#define CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT 1250\n',
        '#define CUVS_MANAGED_LW_MARGIN_PERMILLE_DEFAULT 1250u\n')], {'docs'}),
    # hook body (I4-owned, conch 2026-11-03 16:0x)
    'hook_inert_after_i4': (UTIL, [(
        '    return cuvs_lw_admit(&lw).reason;', '    (void)lw;\n    return CUVS_ADMISSION_NONE;')],
        {'policy', 'daemon'}),
}


def run(argv, cwd):
    try:
        q = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return 124, 'FAIL timeout after 120 s'
    return q.returncode, q.stdout + q.stderr


def build_and_run(tree):
    """Returns {test: (status, tail)} with status PASS, RED (run) or RED (build)."""
    out = {}
    jobs = {
        'policy': [CC, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', 'src', '-o', 'bin-policy',
                   'test/unit/test_lw_policy.c', 'src/cuvs_util.c'],
        'guard': [CXX, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', 'src', '-o',
                  'bin-guard', 'test/unit/test_lw_guard.cpp'],
    }
    jobs['docs'] = [sys.executable, '-I', 'test/unit/check_lw_docs.py', str(tree)]
    if LINUX:
        jobs['server'] = [CC, '-std=gnu11', '-O1', '-DPGCUVS_CPU_SHIM', '-D_POSIX_C_SOURCE=200809L',
                          '-I', 'src', 'src/pg_cuvs_server.c', 'src/cuvs_ipc.c', 'src/cuvs_util.c',
                          'src/cuvs_objstore.c', 'src/cuvs_build_corpus.c',
                          'src/cuvs_wrapper_shim_cpu.c', '-lm', '-lpthread', '-lrt', '-lcurl',
                          '-lssl', '-lcrypto', '-o', 'bin-server']
        jobs['startup'] = [CC, '-std=gnu11', '-Wall', '-Wextra', '-I', 'src', '-o', 'bin-startup',
                           'test/unit/test_lw_startup.c', 'src/cuvs_ipc.c', 'src/cuvs_util.c',
                           'src/cuvs_build_corpus.c', '-lm', '-lpthread', '-lrt']
        jobs['daemon'] = [CC, '-std=gnu11', '-O1', '-DPGCUVS_CPU_SHIM', '-ffunction-sections',
                          '-fdata-sections', '-I', 'src', '-o', 'bin-daemon',
                          'test/unit/test_lw_daemon.c', 'src/cuvs_ipc.c', 'src/cuvs_util.c',
                          'src/cuvs_objstore.c', 'src/cuvs_build_corpus.c',
                          'src/cuvs_wrapper_shim_cpu.c', '-Wl,--gc-sections', '-lm', '-lpthread',
                          '-lrt', '-lcurl', '-lssl', '-lcrypto']
    for name, argv in jobs.items():
        if name == 'docs':   # a check, not a build
            rc, text = run(argv, tree)
            lines = [l for l in text.splitlines() if l.startswith(('FAIL', 'PASS'))]
            out[name] = ('PASS' if rc == 0 else 'RED (run)', lines[:1] or [''])
            continue
        rc, text = run(argv, tree)
        if name == 'server':  # helper binary for the startup test
            if rc != 0:
                out[name] = ('BUILD-BROKEN', text.strip().splitlines()[-1:] or [''])
            continue
        if rc != 0:
            # A planted defect must be caught by an assertion, not by -Werror.
            out[name] = ('BUILD-BROKEN', text.strip().splitlines()[-1:] or [''])
            continue
        argv_run = ['./bin-' + name] + (['./bin-server'] if name == 'startup' else [])
        rc, text = run(argv_run, tree)
        lines = [l for l in text.splitlines() if l.startswith(('FAIL', 'PASS', 'test_lw'))]
        out[name] = ('PASS' if rc == 0 else 'RED (run)', lines[:1] or text.strip().splitlines()[-1:])
    return out


def copy_tree(dest):
    dest.mkdir(parents=True)
    shutil.copytree(ROOT / 'src', dest / 'src')
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
    for name, (target, edits, expect) in MUTATIONS.items():
        tree = out / name
        copy_tree(tree)
        path = tree / target
        text = path.read_text()
        for old, new in edits:
            if text.count(old) != 1:
                print(f'STALE {name}: pattern count {text.count(old)} in {target}')
                return 1
            text = text.replace(old, new)
        path.write_text(text)
        res = build_and_run(tree)
        expect_here = {t for t in expect if t in res}
        reds = {t for t, (s, _) in res.items() if s == 'RED (run)'}
        broken = {t for t, (s, _) in res.items() if s == 'BUILD-BROKEN'}
        ok = bool(reds) and expect_here <= reds and not broken
        red += ok
        detail = '; '.join(f'{t}={s} {tail[0] if tail else ""}'.strip() for t, (s, tail) in res.items()
                           if t in reds or t in broken)
        print(f'{"RED  " if ok else "GREEN"} {name}: {detail or "all tests passed"}')
    print(f'mutations: {red}/{len(MUTATIONS)} RED')
    return 0 if red == len(MUTATIONS) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))

"""ADR-089 AR8 mutation runner: every planted defect must turn an AR8 test RED.

Usage: python3 -I test/unit/test_ar8_mutations.py OUT_DIR   (OUT_DIR must not exist)

Builds the unmutated tree first (every test must PASS), then one copy per
mutation. 'daemon' (test_ar8_daemon.c, the real ensure_vram on the CPU shim)
needs Linux; elsewhere it is skipped and a mutation that only it can catch is
reported SKIP, never RED. A mutation that only breaks the build is
BUILD-BROKEN, not RED.
"""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get('CC', 'cc')
LINUX = sys.platform.startswith('linux')

CONTRACT = 'src/cuvs_admission_contract.h'
POLICY = 'src/cuvs_lw_policy.h'
SERVER = 'src/pg_cuvs_server.c'
UTIL = 'src/cuvs_util.c'

AR8_IF = ('        if (cuvs_admission_pool_refuses(quota, line->pool_pending, line->pool_used,\n'
          '                                        needed, line->pool_headroom))\n'
          '        {\n'
          '            g_last_admit_reason = CUVS_ADMISSION_AR8;\n'
          '            goto admission_rejected;\n'
          '        }\n')
MAKE_ROOM = ('        g_last_admit_reason = pick_reason(&pk, phys_short > budget_short);\n'
             '        goto admission_rejected;\n'
             '    }\n')
AR8_MOVED = ('    if (line && line->pool_valid &&\n'
             '        cuvs_admission_pool_refuses(quota, line->pool_pending, line->pool_used, needed, line->pool_headroom))\n'
             '    {\n'
             '        g_last_admit_reason = CUVS_ADMISSION_AR8;\n'
             '        goto admission_rejected;\n'
             '    }\n')

# name: ([(file, old, new), ...], tests expected to turn RED)
MUTATIONS = {
    # (1) AR8 check removed
    'ar8_check_removed_daemon': ([(SERVER, AR8_IF, '')], {'daemon'}),
    'ar8_check_removed_rule': ([(CONTRACT,
        '    if (quota == 0)\n        return 0;\n    return cuvs_admission_sat_add(',
        '    (void)quota; (void)pending; (void)pool_used; (void)needed; (void)headroom;\n'
        '    return 0;\n    return cuvs_admission_sat_add(')], {'contract', 'daemon'}),
    # (2) headroom dropped: permille ignored
    'headroom_dropped_rule': ([(CONTRACT,
        '    if (permille == 0)\n        return 0;\n    if (permille > CUVS_RMM_BUILD_HEADROOM_PERMILLE_MAX ||',
        '    (void)permille;\n    return 0;\n    if (permille > CUVS_RMM_BUILD_HEADROOM_PERMILLE_MAX ||')],
        {'contract', 'daemon'}),
    'headroom_knob_ignored_daemon': ([(SERVER,
        '        line->pool_headroom = cuvs_admission_build_headroom(needed,\n'
        '                                                            g_rmm_build_headroom_permille);',
        '        line->pool_headroom = cuvs_admission_build_headroom(needed, 0);')],
        {'daemon'}),
    'headroom_floor_dropped': ([(CONTRACT,
        '    return h > CUVS_RMM_BUILD_HEADROOM_FLOOR_BYTES ? h\n'
        '                                                   : CUVS_RMM_BUILD_HEADROOM_FLOOR_BYTES;',
        '    return h;')], {'contract'}),
    'headroom_floor_not_ceil': ([(CONTRACT,
        '        ((needed % 1000u) * permille + 999u) / 1000u);\n    return h >',
        '        ((needed % 1000u) * permille) / 1000u);\n    return h >')], {'contract'}),
    # design 40c33a3b: headroom is on N_m, not on N_m + lw_ub
    'headroom_on_n_plus_lw_ub': ([(SERVER,
        '        line->pool_headroom = cuvs_admission_build_headroom(needed,\n',
        '        line->pool_headroom = cuvs_admission_build_headroom(\n'
        '            cuvs_admission_sat_add(needed, line->workspace.lw_ub),\n')], {'daemon'}),
    # design 40c33a3b: lw_ub of this build is not a pool term
    'lw_ub_readded_to_pool_rule': ([(SERVER,
        '                                        needed, line->pool_headroom))',
        '                                        cuvs_admission_sat_add(needed, line->workspace.lw_ub),\n'
        '                                        line->pool_headroom))')], {'daemon'}),
    'pool_used_dropped': ([(SERVER,
        '        if (cuvs_admission_pool_refuses(quota, line->pool_pending, line->pool_used,',
        '        if (cuvs_admission_pool_refuses(quota, line->pool_pending, 0,')], {'daemon'}),
    'pool_used_dropped_rule': ([(CONTRACT,
        'cuvs_admission_sat_add(cuvs_admission_sat_add(pending, pool_used), needed),',
        'cuvs_admission_sat_add(cuvs_admission_sat_add(pending, 0 * pool_used), needed),')],
        {'contract'}),
    'lw_pending_dropped': ([(SERVER,
        '        line->pool_pending = cuvs_admission_sat_add(pending, g_lw_pending[device_id]);',
        '        line->pool_pending = pending;')], {'daemon'}),
    'build_pending_dropped': ([(SERVER,
        '        line->pool_pending = cuvs_admission_sat_add(pending, g_lw_pending[device_id]);',
        '        line->pool_pending = g_lw_pending[device_id];')], {'daemon'}),
    'boundary_ge': ([(CONTRACT,
        '               headroom) > quota;',
        '               headroom) >= quota;')], {'contract', 'daemon'}),
    # (4) AR8 placed after make_room (evicts before refusing)
    'ar8_after_make_room': ([(SERVER, AR8_IF, ''),
                             (SERVER, MAKE_ROOM, MAKE_ROOM + AR8_MOVED)], {'daemon'}),
    # AR8 placed after AR6/AR7 (AR6 would win)
    'ar8_after_workspace': ([(SERVER, AR8_IF, ''),
                             (SERVER, '        if (wr != CUVS_ADMISSION_NONE)\n'
                                      '        {\n'
                                      '            g_last_admit_reason = wr;\n'
                                      '            goto admission_rejected;\n'
                                      '        }\n',
                              '        if (wr != CUVS_ADMISSION_NONE)\n'
                              '        {\n'
                              '            g_last_admit_reason = wr;\n'
                              '            goto admission_rejected;\n'
                              '        }\n' + AR8_IF)], {'daemon'}),
    # (5) AR8 reason code / text changed to AR6's
    'ar8_reported_as_ar6': ([(SERVER,
        '            g_last_admit_reason = CUVS_ADMISSION_AR8;',
        '            g_last_admit_reason = CUVS_ADMISSION_AR6;')], {'daemon'}),
    'ar8_spelled_ar6': ([(UTIL,
        '"AR6", "AR7", "AR8", "OOM", "ATT"', '"AR6", "AR7", "AR6", "OOM", "ATT"')],
        {'wire'}),
    'ar8_sqlstate_53400': ([(UTIL,
        '    if (reason == CUVS_ADMISSION_AR6)\n        return CUVS_ADMISSION_SQLSTATE_WORKSPACE;',
        '    if (reason == CUVS_ADMISSION_AR6 || reason == CUVS_ADMISSION_AR8)\n'
        '        return CUVS_ADMISSION_SQLSTATE_WORKSPACE;')], {'wire', 'message'}),
    'ar8_errmsg_is_ar6': ([(UTIL,
        '            cuvs_sb_add(&m, "pg_cuvs: this CAGRA build does not fit the GPU memory pool "\n'
        '                            "with headroom; CREATE INDEX aborted");',
        '            cuvs_sb_add(&m, "pg_cuvs: large_workspace would not fit in GPU memory and "\n'
        '                            "managed overcommit is disabled; CREATE INDEX aborted");')],
        {'message'}),
    'ar8_hint_offers_unpin': ([(UTIL,
        '"--rmm-build-headroom-permille.");',
        '"--rmm-build-headroom-permille. Or run pg_cuvs_unpin.");')],
        {'message'}),
    'ar8_hint_drops_resident': ([(UTIL,
        '"raise --rmm-pool-max-percent. Pool used includes resident "\n'
        '                            "indexes (for REINDEX, the index\'s own current copy); unloading "\n'
        '                            "other indexes frees it; an operator can also lower "\n'
        '                            "--rmm-build-headroom-permille.");',
        '"raise --rmm-pool-max-percent.");')],
        {'message'}),
    'ar8_startup_rule_stale': ([(SERVER,
        '    "AR8 refuses pending + pool_used + N + headroom > pool quota; pending = in-flight "',
        '    "AR8 refuses pending + N + lw_ub + headroom > pool quota; pending = in-flight "')],
        {'daemon'}),
    'ar8_detail_drops_pending': ([(UTIL,
        '            cuvs_sb_add(&d, " MiB (pending "); cuvs_sb_mib(&d, f->p);\n'
        '            cuvs_sb_add(&d, " MiB, pool used "); cuvs_sb_mib(&d, f->o);\n'
        '            cuvs_sb_add(&d, " MiB)");',
        '            (void)0;')], {'message'}),
    'ar8_err_out_drops_headroom': ([(SERVER,
        '    f->present = CUVS_ADM_F_N | CUVS_ADM_F_Q | CUVS_ADM_F_P | CUVS_ADM_F_D | CUVS_ADM_F_O;',
        '    f->present = CUVS_ADM_F_N | CUVS_ADM_F_Q | CUVS_ADM_F_P | CUVS_ADM_F_O;')], {'daemon'}),
    'wire_d_key_dropped': ([(UTIL,
        '    if (ok == 0 && (pm & CUVS_ADM_F_D))\n',
        '    if (ok == 0 && 0)\n')], {'wire'}),
    # (6) AR0..AR7 text changed
    'ar0_msg_changed': ([(UTIL, '"the configured limit); CREATE INDEX aborted",',
                          '"the configured limit); CREATE INDEX aborted!",')], {'message'}),
    'ar1_msg_changed': ([(UTIL, '"INDEX holds GPU memory; CREATE INDEX aborted");',
                          '"INDEX holds GPU memory; CREATE INDEX aborted.");')], {'message'}),
    'ar3_msg_changed': ([(UTIL, '"pg_cuvs: GPU memory is insufficient for %s; CREATE INDEX "',
                          '"pg_cuvs: GPU memory is too small for %s; CREATE INDEX "')], {'message'}),
    'ar5_msg_changed': ([(UTIL, '"the build eviction scope does not allow; CREATE INDEX aborted",',
                          '"the build eviction scope forbids; CREATE INDEX aborted",')], {'message'}),
    'ar6_hint_changed': ([(UTIL, '"--managed-overcommit on; or build smaller tables. "',
                           '"--managed-overcommit on; or build a smaller table. "')], {'message'}),
    'ar7_msg_changed': ([(UTIL, '"pg_cuvs: large_workspace would spill beyond the host memory "',
                          '"pg_cuvs: large_workspace would spill past the host memory "')], {'message'}),
    'ar7_hint_changed': ([(UTIL, '"or review --managed-lw-margin-permille.");',
                           '"or raise --managed-lw-margin-permille.");')], {'message'}),
    'ar7_spelling_changed': ([(UTIL, '"AR6", "AR7", "AR8"', '"AR6", "AR-7", "AR8"')], {'wire'}),
    # knob and [admit] line
    'knob_range_widened': ([(POLICY,
        'cuvs_lw_parse_uint(s, 0, CUVS_RMM_BUILD_HEADROOM_PERMILLE_MAX, &v)',
        'cuvs_lw_parse_uint(s, 0, 4000000000ul, &v)')], {'daemon'}),
    'knob_default_not_150': ([(CONTRACT,
        '#define CUVS_RMM_BUILD_HEADROOM_PERMILLE_DEFAULT 150u',
        '#define CUVS_RMM_BUILD_HEADROOM_PERMILLE_DEFAULT 100u')], {'contract', 'daemon'}),
    'admit_fields_swapped': ([(SERVER,
        '"pool_need=%llu headroom=%llu pool_quota=%zu pending=%llu\\n",',
        '"headroom=%llu pool_need=%llu pool_quota=%zu pending=%llu\\n",')], {'daemon'}),
    'wire_o_key_dropped': ([(UTIL,
        '    if (ok == 0 && (pm & CUVS_ADM_F_O))\n',
        '    if (ok == 0 && 0)\n')], {'wire'}),
    'logs_judge_ar8_ge': ([('test/unit/check_i3_logs.py',
        "+ n('headroom') > n('pool_quota'))",
        "+ n('headroom') >= n('pool_quota'))")], {'logs'}),
}


def run(argv, cwd):
    try:
        q = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return 124, 'FAIL timeout after 120 s'
    text = q.stdout + q.stderr
    with (Path(cwd) / 'COMMANDS.jsonl').open('a') as log:
        log.write(json.dumps({'argv': argv, 'exit': q.returncode, 'output': text}) + '\n')
    return q.returncode, text


def build_and_run(tree):
    """Returns {test: (status, tail)}: PASS, RED (run) or BUILD-BROKEN."""
    out = {}
    jobs = {
        'contract': [CC, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', 'src', '-o', 'bin-contract',
                     'test/unit/test_admission_contract.c', 'src/cuvs_util.c'],
        'wire': [CC, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', 'src', '-o', 'bin-wire',
                 'test/unit/test_admission_wire.c', 'src/cuvs_util.c'],
        'message': [CC, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', 'src', '-o', 'bin-message',
                    'test/unit/test_admission_message.c', 'src/cuvs_util.c'],
    }
    if LINUX:
        jobs['daemon'] = [CC, '-std=gnu11', '-O1', '-DPGCUVS_CPU_SHIM', '-ffunction-sections',
                          '-fdata-sections', '-I', 'src', '-o', 'bin-daemon',
                          'test/unit/test_ar8_daemon.c', 'src/cuvs_ipc.c', 'src/cuvs_util.c',
                          'src/cuvs_objstore.c', 'src/cuvs_build_corpus.c',
                          'src/cuvs_wrapper_shim_cpu.c', 'src/cuvs_cache_policy.c',
                          'src/cuvs_peak_meter.c', '-Wl,--gc-sections', '-lm', '-lpthread',
                          '-lrt', '-lcurl', '-lssl', '-lcrypto']
    for name, argv in jobs.items():
        rc, text = run(argv, tree)
        if rc != 0:
            out[name] = ('BUILD-BROKEN', text.strip().splitlines()[-1:] or [''])
            continue
        rc, text = run(['./bin-' + name], tree)
        fails = [l for l in text.splitlines() if l.startswith('FAIL')]
        lines = [l for l in text.splitlines() if l.startswith(('FAIL', 'PASS', 'test_'))]
        tail = [' | '.join(fails[:3])] if fails else lines[:1] or text.strip().splitlines()[-1:]
        out[name] = ('PASS' if rc == 0 else 'RED (run)', tail)
    rc, text = run([sys.executable, '-I', '-B', 'test/unit/test_i3_logs.py'], tree)
    lines = [l for l in text.splitlines() if l.startswith(('FAIL', 'OK', 'Ran'))]
    out['logs'] = ('PASS' if rc == 0 else 'RED (run)', lines[:1] or [''])
    return out


def copy_tree(dest):
    dest.mkdir(parents=True)
    shutil.copytree(ROOT / 'src', dest / 'src',
                    ignore=shutil.ignore_patterns('*.o', '*.bc', '*.so', '__pycache__'))
    shutil.copytree(ROOT / 'test' / 'unit', dest / 'test' / 'unit',
                    ignore=shutil.ignore_patterns('__pycache__'))


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
    red = skipped = 0
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
        expect_here = {t for t in expect if t in res}
        if not expect_here:
            skipped += 1
            print(f'SKIP  {name}: needs {sorted(expect)} (Linux only)')
            continue
        reds = {t for t, (s, _) in res.items() if s == 'RED (run)'}
        broken = {t for t, (s, _) in res.items() if s == 'BUILD-BROKEN'}
        ok = expect_here <= reds and not broken
        red += ok
        detail = '; '.join(f'{t}={s} {tail[0] if tail else ""}'.strip()
                           for t, (s, tail) in res.items() if t in reds or t in broken)
        print(f'{"RED  " if ok else "LIVE "} {name}: {detail or "all tests passed"}')
    ran = len(MUTATIONS) - skipped
    print(f'mutations: {red}/{ran} RED' + (f', {skipped} skipped (no Linux)' if skipped else ''))
    return 0 if red == ran and not skipped else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))

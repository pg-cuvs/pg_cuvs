"""Mutations of the UC-5 ACE quality rule, knob parser, partition-builder readback, and the op-23
client's readback binding. Each mutated copy must turn its test RED; the unmutated copy must be GREEN.
CPU-only: builds test_hnsw_ace_quality.c and test_hnsw_build_ipc.c against a scratch copy of src/."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CC = os.environ.get('CC', 'cc')
RT = os.environ.get('TEST_RT_LIB', '-lrt' if sys.platform.startswith('linux') else '').split()

H = 'cuvs_hnsw_policy.h'
MUTATIONS = {
    # auto must derive q from the linked SDK's NN-descent default, not a literal A.
    'auto_literal_240': (H, 'return cuvs_hnsw_scaled_quality(q * CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR, ef, 4, 65535);',
                         'return cuvs_hnsw_scaled_quality(240, ef, 4, 65535);'),
    'auto_sdk_ace_default_120': (H, 'return cuvs_hnsw_scaled_quality(q * CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR, ef, 4, 65535);',
                                 'return cuvs_hnsw_scaled_quality(120, ef, 4, 65535);'),
    'auto_iteration_offset_dropped': (H, '            ? sdk_nn_iterations - CUVS_HNSW_SDK_ACE_NN_ITERATION_OFFSET : 0;',
                                      '            ? sdk_nn_iterations : 0;'),
    'auto_ignores_ef_construction': (H, 'return cuvs_hnsw_scaled_quality(q * CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR, ef, 4, 65535);',
                                     'return q * CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR;'),
    'manual_scaled_by_ef_construction': (H, '    return setting * CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR;\n}',
                                         '    return cuvs_hnsw_scaled_quality(setting * 16, ef, 4, 65535);\n}'),
    'parser_accepts_4096': (H, '    if (q > CUVS_HNSW_ACE_QUALITY_MAX) return -1;\n', ''),
    'readback_reports_receipt_width': (H, '    return 3u * (degree / 2u);', '    return 2u * degree;'),
    'readback_ignores_1e6_ledger': (H, 'if (n_rows < CUVS_HNSW_SDK_ACE_NN_MAX_ROWS)', 'if (1)'),
    'readback_valid_unbound_from_quality': (H, '        && r->quality == p->ace_ef / CUVS_HNSW_SDK_ACE_QUALITY_DIVISOR\n', ''),
    'reader_accepts_short_tail': (H, '    if (available < sizeof(*out)) return CUVS_HNSW_POLICY_CORRUPT;\n    memcpy(out, data, sizeof(*out));',
                                  '    memcpy(out, data, sizeof(*out));'),
    'client_skips_ace_readback': ('cuvs_ipc.c', '                    ? !cuvs_hnsw_ace_readback_valid(&wire.ace, &wire.policy)\n',
                                  '                    ? 0\n'),
}


def build_and_run(src, work, test):
    exe = work / test
    if test == 'ace_quality':
        argv = [CC, '-std=gnu11', '-Wall', '-Wextra', '-I', str(src), '-o', str(exe),
                str(ROOT / 'test/unit/test_hnsw_ace_quality.c')]
    else:
        argv = [CC, '-std=gnu11', '-Wall', '-Wextra', '-I', str(src), '-o', str(exe),
                str(ROOT / 'test/unit/test_hnsw_build_ipc.c'), str(src / 'cuvs_ipc.c'),
                str(src / 'cuvs_util.c'), str(src / 'cuvs_build_corpus.c'), '-lpthread', '-lm'] + RT
    b = subprocess.run(argv, capture_output=True, timeout=120)
    if b.returncode:
        return 'BUILD-FAIL', b.stderr.decode(errors='replace')[-400:]
    r = subprocess.run([str(exe)], capture_output=True, timeout=60)
    return ('GREEN' if r.returncode == 0 else 'RED'), (r.stdout + r.stderr).decode(errors='replace')[-300:]


def main():
    results = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for name in ['unmutated'] + list(MUTATIONS):
            src = tmp / name / 'src'
            shutil.copytree(ROOT / 'src', src)
            test = 'ace_quality'
            if name != 'unmutated':
                f, old, new = MUTATIONS[name]
                body = (src / f).read_text()
                assert body.count(old) == 1, (name, body.count(old))
                (src / f).write_text(body.replace(old, new, 1))
                test = 'build_ipc' if f == 'cuvs_ipc.c' else 'ace_quality'
            if name == 'unmutated':
                states = [build_and_run(src, src.parent, t) for t in ('ace_quality', 'build_ipc')]
                ok = all(s == 'GREEN' for s, _ in states)
                results.append((name, ok, ' / '.join(s for s, _ in states), states[0][1].strip()))
            else:
                state, tail = build_and_run(src, src.parent, test)
                results.append((name, state == 'RED', state, tail.strip().splitlines()[-1] if tail.strip() else ''))
    for name, ok, state, tail in results:
        print('%-38s %-6s %s  %s' % (name, 'OK' if ok else 'WRONG', state, tail[:160]))
    bad = [n for n, ok, _, _ in results if not ok]
    if bad:
        print('mutation check failed: ' + ', '.join(bad))
        return 1
    print('PASS unmutated GREEN, %d mutations RED' % len(MUTATIONS))
    return 0


if __name__ == '__main__':
    sys.exit(main())

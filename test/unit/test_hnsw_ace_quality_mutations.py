"""Mutations of the UC-5 ACE quality rule, knob parser, partition-builder readback, the op-23 client's
readback binding, and the metapage write/read of the readback (review B2). Each mutated copy must turn its
test RED; the unmutated copy must be GREEN. CPU-only: test_hnsw_ace_quality.c and test_hnsw_build_ipc.c are
built against a scratch copy of src/; metapage mutations run `make test-hnsw-converter` in a scratch copy of
the tree (needs pg_config)."""
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
    # The ACE effective partition count is optional: the requested-only mark, its origin and its reach into the readback.
    'requested_mark_accepted_off_ace_route': (H, '        && (!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED)\n            || p->route == CUVS_HNSW_ROUTE_ACE)\n', ''),
    'requested_mark_unknown_to_reader': (H, '#define CUVS_HNSW_ROUTE_FLAGS_KNOWN 0x7fu', '#define CUVS_HNSW_ROUTE_FLAGS_KNOWN 0x3fu'),
    'requested_mark_claimed_off_ace_route': (H, '    return p && p->route == CUVS_HNSW_ROUTE_ACE\n        && (p->route_flags & CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED) != 0;',
                                             '    return p && (p->route_flags & CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED) != 0;'),
    'origin_ignores_requested_mark': (H, '    return cuvs_hnsw_ace_partitions_unverified(p)\n        ? "requested_effective_not_guaranteed" : "same_SDK_stable_explicit_plan";',
                                      '    return "same_SDK_stable_explicit_plan";'),
    'iterations_guarantee_ignores_mark': (H, '    return !(cuvs_hnsw_ace_partitions_unverified(p)\n             && p->n_rows', '    return !(0\n             && p->n_rows'),
    'iterations_guarantee_drops_1e6_bound': (H, '             && p->n_rows >= CUVS_HNSW_SDK_ACE_NN_MAX_ROWS && r->local_iterations != 0);',
                                             '             && r->local_iterations != 0);'),
    'iterations_guarantee_marks_zero': (H, '             && p->n_rows >= CUVS_HNSW_SDK_ACE_NN_MAX_ROWS && r->local_iterations != 0);',
                                       '             && p->n_rows >= CUVS_HNSW_SDK_ACE_NN_MAX_ROWS);'),
    'reader_accepts_short_tail': (H, '    if (available < sizeof(*out)) return CUVS_HNSW_POLICY_CORRUPT;\n    memcpy(out, data, sizeof(*out));',
                                  '    memcpy(out, data, sizeof(*out));'),
    'client_skips_ace_readback': ('cuvs_ipc.c', '                    ? !cuvs_hnsw_ace_readback_valid(&wire.ace, &wire.policy)\n',
                                  '                    ? 0\n'),
    # Review B2 (d0a51e7): the reviewer's three survivors, verbatim.
    'meta_pd_lower_not_bumped': ('hnsw_export.c', '        ((PageHeader)staged)->pd_lower += sizeof(*ace);\n', ''),
    'meta_read_wrong_offset': ('hnsw_export.c', '    Size at = SizeOfPageHeaderData + sizeof(PgvHnswMeta) + sizeof(*policy);',
                               '    Size at = SizeOfPageHeaderData + sizeof(PgvHnswMeta);'),
    'client_skips_policy_validation': ('cuvs_ipc.c', '            if (!cuvs_hnsw_policy_valid(&wire.policy)\n',
                                       '            if (0 && !cuvs_hnsw_policy_valid(&wire.policy)\n'),
    'client_accepts_readback_on_resident': ('cuvs_ipc.c', '                    : wire.ace.magic != 0)', '                    : 0)'),
}


# These fixtures also assert setup prerequisites. Require the failure emitted by
# the intended check rather than counting an abort anywhere in the executable.
EXPECTED_FAILURES = {
    'client_skips_ace_readback': 'FAIL IPC fixture 17 accepted an invalid reply',
    'client_skips_policy_validation': 'FAIL IPC fixture 4 accepted an invalid reply',
    'client_accepts_readback_on_resident': 'FAIL IPC fixture 18 accepted an invalid reply',
    'meta_pd_lower_not_bumped': 'FAIL ACE metapage extent omits the readback',
    'meta_read_wrong_offset': 'FAIL ACE metapage readback could not be decoded',
}


def test_result(run, mutant=None):
    output = (run.stdout + run.stderr).strip()
    if run.returncode == 0:
        return 'GREEN', output
    expected = EXPECTED_FAILURES.get(mutant)
    failures = [line for line in output.splitlines() if line.startswith('FAIL ')]
    if mutant is not None and run.returncode == 1 and (expected in failures if expected else failures):
        return 'RED', output
    return 'UNEXPECTED-FAIL', output or f'exit {run.returncode}'


def run_converter(tree, mutant=None):
    built = subprocess.run(['make', '-s', 'test-hnsw-converter-build'],
                           cwd=tree, capture_output=True, text=True, timeout=600)
    if built.returncode:
        return 'BUILD-FAIL', built.stderr.strip()[-400:]
    run = subprocess.run(['./test-hnsw-converter'], cwd=tree, capture_output=True,
                         text=True, timeout=60)
    return test_result(run, mutant)


def copy_tree(dst):
    shutil.copytree(ROOT, dst, ignore=shutil.ignore_patterns('.git', '.ccache', '*.o', '*.so', 'test-*', 'pg_cuvs_server', '*.bc'))


def build_and_run(src, work, test, mutant=None):
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
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    return test_result(r, mutant)


def main():
    results = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for name in ['unmutated'] + list(MUTATIONS):
            tree = tmp / name
            copy_tree(tree)
            src = tree / 'src'
            test = 'ace_quality'
            if name != 'unmutated':
                f, old, new = MUTATIONS[name]
                body = (src / f).read_text()
                assert body.count(old) == 1, (name, body.count(old))
                (src / f).write_text(body.replace(old, new, 1))
                test = {'cuvs_ipc.c': 'build_ipc', 'hnsw_export.c': 'converter'}.get(f, 'ace_quality')
            if name == 'unmutated':
                states = [build_and_run(src, src.parent, t) for t in ('ace_quality', 'build_ipc')] + [run_converter(tree)]
                ok = all(s == 'GREEN' for s, _ in states)
                results.append((name, ok, ' / '.join(s for s, _ in states), states[0][1].strip()))
            else:
                if test == 'converter':
                    state, tail = run_converter(tree, name)
                else:
                    state, tail = build_and_run(src, src.parent, test, name)
                results.append((name, state == 'RED', state, tail.strip().splitlines()[-1] if tail.strip() else ''))
    for name, ok, state, tail in results:
        print('%-38s %-6s %s  %s' % (name, 'OK' if ok else 'WRONG', state, tail[:160]))
    bad = [n for n, ok, _, _ in results if not ok]
    # A mutation that produced no result is a failure, never a silent pass.
    bad += sorted(set(['unmutated'] + list(MUTATIONS)) - {n for n, _, _, _ in results})
    if bad:
        print('mutation check failed: ' + ', '.join(bad))
        return 1
    print('PASS unmutated GREEN, %d mutations RED' % (len(results) - 1))
    return 0


if __name__ == '__main__':
    sys.exit(main())

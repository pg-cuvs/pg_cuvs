"""Mutations of the real admission estimate header and hook definition; each must turn the unit test RED.

Usage: python3 -I test/unit/test_admission_contract_mutations.py <new-output-dir>
No GPU, daemon or PostgreSQL is needed.
"""
from pathlib import Path
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HEADER = 'cuvs_admission_contract.h'
TEST = 'test/unit/test_admission_contract.c'

MUTATIONS = {
    'ld_alignment_ignored': (
        'ld = 4u * (((uint64_t)dim + 3u) / 4u);', 'ld = (uint64_t)dim;'),
    'scratch_rounds_down': (
        '* scratch_rho_permille + 999u) / 1000u);',
        '* scratch_rho_permille) / 1000u);'),
    'scratch_term_dropped': (
        'needed = cuvs_admission_sat_add(needed, scratch);', ''),
    'corpus_h2d_dropped': (
        'needed = cuvs_admission_sat_add(final_bytes, corpus_h2d);',
        'needed = final_bytes;'),
    'margin_dropped': (
        'return cuvs_admission_sat_add(needed, margin);', 'return needed;'),
    'scratch_scales_h2d': (
        'cuvs_admission_sat_mul(final_bytes / 1000u, scratch_rho_permille),',
        'cuvs_admission_sat_mul((final_bytes + corpus_h2d) / 1000u, scratch_rho_permille),'),
    'mul_wraps': (
        'if (a != 0 && b > CUVS_ADMISSION_BYTES_SATURATED / a)\n'
        '        return CUVS_ADMISSION_BYTES_SATURATED;\n    return a * b;',
        'return a * b;'),
    'add_wraps': (
        'if (a >= CUVS_ADMISSION_BYTES_SATURATED ||\n'
        '        b >= CUVS_ADMISSION_BYTES_SATURATED - a)\n'
        '        return CUVS_ADMISSION_BYTES_SATURATED;\n    return a + b;',
        'return a + b;'),
    'invalid_shape_admits': (
        'if (n < 0 || dim <= 0)\n        return CUVS_ADMISSION_BYTES_SATURATED;',
        'if (n < 0 || dim <= 0)\n        return 0;'),
    'rho_bound_removed': (
        'if (scratch_rho_permille > CUVS_ADMISSION_RHO_PERMILLE_MAX ||\n'
        '        final_bytes >= CUVS_ADMISSION_BYTES_SATURATED)',
        'if (final_bytes >= CUVS_ADMISSION_BYTES_SATURATED)'),
    'zero_degree_not_default': (
        'gd = graph_degree > 0 ?', 'gd = graph_degree >= 0 ?'),
    'hook_emits_ar6_before_i4': (
        '    (void) in;\n    return CUVS_ADMISSION_NONE;',
        '    (void) in;\n    return CUVS_ADMISSION_AR6;'),
}
UTIL_MUTATIONS = {'hook_emits_ar6_before_i4'}


def run(argv):
    q = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=60)
    return q.returncode, q.stdout + q.stderr


def main(output):
    out = Path(output).resolve()
    out.mkdir(parents=True, exist_ok=False)
    originals = {HEADER: (ROOT / 'src' / HEADER).read_text(),
                 'cuvs_util.c': (ROOT / 'src' / 'cuvs_util.c').read_text()}
    results, ok = [], True
    for name, (old, new) in MUTATIONS.items():
        target = 'cuvs_util.c' if name in UTIL_MUTATIONS else HEADER
        case = out / name
        case.mkdir()
        for fname, body in originals.items():
            if fname == target:
                assert body.count(old) == 1, (name, body.count(old))
                body = body.replace(old, new, 1)
            (case / fname).write_text(body)
        binary = str(case / 'check')
        rc, log = run(['cc', '-std=c11', '-Wall', '-Wextra', '-I', str(case), '-I', 'src',
                       '-o', binary, TEST, str(case / 'cuvs_util.c')])
        stage = 'compile'
        if rc == 0:
            stage = 'run'
            rc, log = run([binary])
        red = rc != 0
        ok &= red
        (case / 'log.txt').write_text(log)
        first = next((l for l in log.splitlines() if 'FAIL' in l or 'error' in l), '')
        results.append(dict(mutation=name, stage=stage, exit=rc, red=red, first=first))
        print(('RED   ' if red else 'GREEN ') + name + ' (' + stage + ') ' + first)
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print('mutations: %d/%d RED' % (sum(r['red'] for r in results), len(results)))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))

"""Targeted guard mutations of the real headers/IPC consumer; no GPU or PG."""
from pathlib import Path
import argparse
import hashlib
import json
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HEADERS = ['cuvs_hnsw_policy.h', 'cuvs_hnsw_build_common.h', 'cuvs_ipc.h']


def replace_once(body, old, new):
    assert body.count(old) == 1, (old, body.count(old))
    return body.replace(old, new, 1)


def mutate(name, files):
    p = files['cuvs_hnsw_policy.h']; c = files['cuvs_hnsw_build_common.h']
    if name == 'whitelist_range_opens_NN':
        p = replace_once(p, 'return algo == CUVS_HNSW_ALGO_AUTO || algo == CUVS_HNSW_ALGO_IVF_PQ\n        || algo == CUVS_HNSW_ALGO_ACE;', 'return algo <= CUVS_HNSW_ALGO_ACE;')
    elif name == 'resident_budget_guard_removed':
        c = replace_once(c, 'if (!tiny && algo != CUVS_HNSW_ALGO_ACE && exceeded && !overflow_to_ace)', 'if (0 && !tiny && algo != CUVS_HNSW_ALGO_ACE && exceeded && !overflow_to_ace)')
    elif name == 'ACE_not_forced_when_resident_fits':
        c = replace_once(c, 'if (algo == CUVS_HNSW_ALGO_ACE || overflow_to_ace)', 'if ((algo == CUVS_HNSW_ALGO_ACE && exceeded) || overflow_to_ace)')
    elif name == 'legacy_reinterpreted_as_current':
        p = replace_once(p, 'p->version == CUVS_HNSW_POLICY_LEGACY_VERSION && p->bytes', 'p->version == CUVS_HNSW_POLICY_VERSION && p->bytes')
    elif name == 'reader_truncation_guard_removed':
        p = replace_once(p, ' || available < sizeof(*out)', '')
    elif name == 'tiny_wrongly_gets_resident_status13':
        c = replace_once(c, '"tiny exact route estimate exceeds host/GPU budget");\n        return CUVS_STATUS_BUILD_FAILED;', '"tiny exact route estimate exceeds host/GPU budget");\n        return CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED;')
    elif name == 'sampling_ignores_GPU_budget':
        c = replace_once(c, '(host < gpu ? host : gpu) / 64', 'host / 64')
    elif name == 'writer_route_contract_removed':
        a = p.index('static inline int cuvs_hnsw_policy_valid(')
        b = p.index('/* Historical v1', a)
        fn = p[a:b]; k = fn.index('\n        && ((p->requested_algo')
        p = p[:a] + fn[:k] + ';\n}\n\n' + p[b:]
    elif name == 'reply_request_binding_removed':
        files['cuvs_ipc.c'] = replace_once(files['cuvs_ipc.c'], '                || wire.policy.requested_algo != build_algo\n', '')
    elif name == 'prefix_version_guard_after_tail':
        c = replace_once(c, 'if (!cuvs_hnsw_file_version_supported(request->version))', 'if (0 && !cuvs_hnsw_file_version_supported(request->version))')
    else:
        raise AssertionError(name)
    files['cuvs_hnsw_policy.h'] = p; files['cuvs_hnsw_build_common.h'] = c


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def command(argv, folder, label):
    q = subprocess.run(argv, cwd=ROOT, capture_output=True, timeout=30, preexec_fn=no_core)
    (folder / (label + '.stdout')).write_bytes(q.stdout)
    (folder / (label + '.stderr')).write_bytes(q.stderr)
    return dict(argv=argv, cwd=str(ROOT), exit=q.returncode,
                stdout_sha256=hashlib.sha256(q.stdout).hexdigest(),
                stderr_sha256=hashlib.sha256(q.stderr).hexdigest())


def main(output):
    out = Path(output).resolve(); out.mkdir(exist_ok=False)
    originals = {n: (ROOT / 'src' / n).read_text() for n in HEADERS + ['cuvs_ipc.c']}
    names = ['whitelist_range_opens_NN', 'resident_budget_guard_removed',
             'ACE_not_forced_when_resident_fits', 'legacy_reinterpreted_as_current',
             'reader_truncation_guard_removed', 'tiny_wrongly_gets_resident_status13',
             'sampling_ignores_GPU_budget', 'writer_route_contract_removed',
             'reply_request_binding_removed', 'prefix_version_guard_after_tail']
    results = []
    for name in names:
        case = out / name; case.mkdir(); files = dict(originals); mutate(name, files)
        for n, body in files.items():
            (case / n).write_text(body)
        ipc = name in ('reply_request_binding_removed', 'prefix_version_guard_after_tail')
        test = 'test_hnsw_build_ipc.c' if ipc else 'test_hnsw_explicit_route.c'
        argv = ['/usr/bin/cc', '-std=gnu11', '-Wall', '-Wextra', '-I', str(case), '-I', 'src',
                '-o', str(case / 'check'), 'test/unit/' + test]
        if ipc:
            argv += [str(case / 'cuvs_ipc.c'), 'src/cuvs_util.c', 'src/cuvs_build_corpus.c', '-lm']
        argv += ['-lpthread']
        compiled = command(argv, case, 'compile')
        if compiled['exit'] != 0:
            row = dict(name=name, status='FAIL_COMPILE_NOT_GUARD_DETECTION', compile=compiled)
        else:
            ran = command([str(case / 'check')], case, 'run')
            error = (case / 'run.stderr').read_text()
            caught = ran['exit'] != 0 and ('Assertion failed' in error or 'assertion' in error.lower())
            row = dict(name=name, status='PASS_MUTATION_DETECTED' if caught else 'FAIL_MUTATION_SURVIVED',
                       compile=compiled, run=ran,
                       changes={n: dict(before_sha256=hashlib.sha256(originals[n].encode()).hexdigest(),
                                        after_sha256=hashlib.sha256(v.encode()).hexdigest())
                                for n, v in files.items() if v != originals[n]})
        results.append(row)
        (case / 'RESULT.json').write_text(json.dumps(row, indent=2) + '\n')
    unchanged = all((ROOT / 'src' / n).read_text() == v for n, v in originals.items())
    value = dict(status='PASS' if unchanged and all(r['status'] == 'PASS_MUTATION_DETECTED' for r in results) else 'FAIL',
                 count=len(results), results=results, production_sources_unchanged=unchanged,
                 scope='CPU actual-header/IPC mutants; real GPU/SDK/PG/daemon not run')
    (out / 'RESULT.json').write_text(json.dumps(value, indent=2) + '\n')
    print(json.dumps(dict(status=value['status'], count=len(results), output=str(out / 'RESULT.json'))))
    assert value['status'] == 'PASS'


if __name__ == '__main__':
    a = argparse.ArgumentParser(); a.add_argument('output'); args = a.parse_args(); main(args.output)

"""I3-D mutants: header behaviour (vs test_res_pool.cpp) and wrapper/daemon wiring
(source conformance, since CUDA cannot be compiled on CPU). Each must be RED."""
from pathlib import Path
import argparse
import json
import re
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HEADERS = ['cuvs_res_pool.hpp', 'cuvs_wrapper.h', 'cuvs_ipc.h', 'cuvs_hnsw_policy.h']

BEHAVIOUR = [
    ('stream_off_by_default', 'cuvs_wrapper.h',
     '#define CUVS_RMM_DEDICATED_STREAM_DEFAULT 1', '#define CUVS_RMM_DEDICATED_STREAM_DEFAULT 0'),
    ('stream_never_created', 'cuvs_res_pool.hpp',
     '        if (dedicated) slot->stream = Traits::make_stream();', '        (void)dedicated;'),
    ('res_not_built_on_stream', 'cuvs_res_pool.hpp',
     '        slot->res = Traits::make_res(slot->stream.get());', '        slot->res = Traits::make_res(nullptr);'),
    ('handles_discarded_on_release', 'cuvs_res_pool.hpp',
     '        free_.push_back(std::move(slot));\n    }\n\n    /* Teardown', '        slot.reset();\n    }\n\n    /* Teardown'),
    ('stream_destroyed_before_res', 'cuvs_res_pool.hpp',
     '    std::unique_ptr<Stream> stream; /* null: --rmm-dedicated-stream off */\n    std::unique_ptr<Res> res;       /* declared after stream: destroyed first */',
     '    std::unique_ptr<Res> res;\n    std::unique_ptr<Stream> stream;'),
    ('mode_switch_after_use', 'cuvs_res_pool.hpp',
     '        if (acquired_ && on != dedicated_) return -1;\n', ''),
]


def wiring(w, s, sh):
    traits = w[w.index('struct RaftResTraits'):w.index('using DevicePool')]
    assert 'std::make_unique<Res>(stream->view())' in traits, 'RES_ON_STREAM_VIEW'
    assert 'std::make_unique<Stream>()' in traits, 'OWNS_RMM_CUDA_STREAM'
    assert ': std::make_unique<Res>()' in traits, 'OFF_IS_PER_THREAD_DEFAULT'
    assert 'return g_device_pools[device_id].acquire();' in w, 'ACQUIRE_FROM_POOL'
    assert '~PooledRes() { if (r && !poisoned) release_res(dev, std::move(r)); }' in w, 'POISON_DESTROYS'
    assert 'raft::device_resources &get() { return *r->res; }' in w, 'GET_SLOT_RES'
    main = s[s.index('\nmain(int argc'):]
    a = main.index('cuvs_rmm_set_dedicated_stream(g_rmm_dedicated_stream)')
    assert a < main.index('cuvs_rmm_pool_init(') and a < main.index('write_hw_profile();'), 'MODE_BEFORE_FIRST_GPU_USE'
    assert re.search(r'static int g_rmm_dedicated_stream = CUVS_RMM_DEDICATED_STREAM_DEFAULT;', s), 'DAEMON_DEFAULT'
    assert 'int cuvs_rmm_set_dedicated_stream(int on)' in sh, 'SHIM_STUB'


SOURCE = [
    ('traits_ignore_stream', 'w', 'stream ? std::make_unique<Res>(stream->view()) : std::make_unique<Res>()', 'std::make_unique<Res>()'),
    ('poisoned_slot_pooled', 'w', '~PooledRes() { if (r && !poisoned) release_res(dev, std::move(r)); }', '~PooledRes() { if (r) release_res(dev, std::move(r)); }'),
    ('mode_set_after_pool_init', 's', '    /* Before the pool, probes, loads and warmup acquire any RAFT resource. */\n    if (cuvs_rmm_set_dedicated_stream(g_rmm_dedicated_stream) != 0)',
     '    if (0)'),
    ('daemon_default_off', 's', 'static int g_rmm_dedicated_stream = CUVS_RMM_DEDICATED_STREAM_DEFAULT;', 'static int g_rmm_dedicated_stream = 0;'),
]


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main(output):
    out = Path(output).resolve(); out.mkdir(exist_ok=False)
    orig = {n: (ROOT / 'src' / n).read_text() for n in HEADERS}
    rows = []
    for name, header, old, new in BEHAVIOUR:
        case = out / name; case.mkdir()
        assert orig[header].count(old) == 1, ('ANCHOR', name)
        for n, body in orig.items():
            (case / n).write_text(body.replace(old, new, 1) if n == header else body)
        c = subprocess.run(['c++', '-std=c++17', '-I', str(case), '-I', 'src', '-o', str(case / 't'),
                            'test/unit/test_res_pool.cpp', '-lpthread'], cwd=ROOT, capture_output=True)
        if c.returncode:
            rows.append(dict(name=name, status='FAIL_COMPILE', err=c.stderr.decode()[-300:])); continue
        r = subprocess.run([str(case / 't')], cwd=ROOT, capture_output=True, preexec_fn=no_core, timeout=60)
        err = r.stderr.decode()
        rows.append(dict(name=name, status='PASS_MUTATION_DETECTED' if r.returncode and 'Assertion' in err else 'FAIL_MUTATION_SURVIVED',
                         assertion=err.strip().splitlines()[-1][:160] if err.strip() else ''))
    src = dict(w=(ROOT / 'src/cuvs_wrapper.cu').read_text(), s=(ROOT / 'src/pg_cuvs_server.c').read_text(),
               sh=(ROOT / 'src/cuvs_wrapper_shim_cpu.c').read_text())
    wiring(src['w'], src['s'], src['sh'])
    for name, key, old, new in SOURCE:
        assert src[key].count(old) == 1, ('ANCHOR', name)
        m = dict(src); m[key] = src[key].replace(old, new, 1)
        try:
            wiring(m['w'], m['s'], m['sh'])
            rows.append(dict(name=name, status='FAIL_MUTATION_SURVIVED'))
        except (AssertionError, ValueError) as e:
            rows.append(dict(name=name, status='PASS_MUTATION_DETECTED', assertion=str(e)))
    status = 'PASS' if all(r['status'] == 'PASS_MUTATION_DETECTED' for r in rows) else 'FAIL'
    value = dict(status=status, count=len(rows), results=rows, wiring='PASS_SOURCE_CONFORMANCE_NOT_CUDA')
    (out / 'RESULT.json').write_text(json.dumps(value, indent=2) + '\n')
    print(json.dumps(value))
    assert status == 'PASS'


if __name__ == '__main__':
    a = argparse.ArgumentParser(); a.add_argument('output'); main(a.parse_args().output)

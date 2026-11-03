"""Bounded source conformance, not CUDA compilation or physical execution."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASE = '42f8a43daa43451ae3e38d28e729886a74c39a66'


def compact(s):
    return re.sub(r'\s+', '', s)


def function(s, signature):
    # Keep offsets while ignoring braces inside comments and normal literals.
    masked = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), s, flags=re.S)
    a = s.index(signature); b = masked.index('{', a); depth = 1; i = b + 1
    while depth:
        if masked[i] == '{': depth += 1
        elif masked[i] == '}': depth -= 1
        i += 1
    return s[a:i]


def check(wrapper, backend, server, baseline):
    prepare = function(wrapper, 'static int prepare_hnsw_settings(')
    execute = function(wrapper, 'static int execute_hnsw_file_build(')
    public = function(wrapper, 'cuvs_hnsw_build_file(int vectors_fd')
    old = function(baseline, 'cuvs_hnsw_build_file(int vectors_fd')
    ordered = ['cuvs_hnsw_scaled_quality(', 'cuvs_hnsw_capacity(',
               'sample_rows = cuvs_hnsw_sample_rows(',
               'cagra::graph_build_params::ivf_pq_params(shape.extents(),',
               'pq_params.build_params.n_lists =', 'if (sample_rows < 256)',
               'pq_params.build_params.kmeans_trainset_fraction =',
               'pq_params.search_params.max_internal_batch_size = 4096;',
               'cuvs_hnsw_training_rows(', 'cuvs_hnsw_training_reserve(']
    positions = [prepare.index(x) for x in ordered]
    assert positions == sorted(positions), 'B_PREPARE_ORDER'
    # Same graph/build/hierarchy/serialization statements moved verbatim.
    a = old.index('        PooledRes resources(device_id);')
    b = old.index('\n    }\n    catch (const std::exception &e)', a)
    native_before = old[a:b]
    native_after = execute[execute.index('    PooledRes resources(device_id);'):execute.rfind('\n}')]
    assert compact(native_before) == compact(native_after), 'NATIVE_EXECUTION_BODY_CHANGED'
    resolve = public.index('rc = cuvs_hnsw_resolve_route(')
    mapping = public.index('files.mapping = mmap(')
    dispatch = public.index('return execute_hnsw_file_build(')
    assert resolve < mapping < dispatch, 'ROUTE_BEFORE_MAPPING_AND_EXECUTE'
    assert 'if (rc != CUVS_STATUS_OK) return rc;' in public[resolve:mapping], 'RESOLVER_ERROR_MUST_STOP'
    assert 'receipt.route = prepared.route.route;' in public, 'RECEIPT_ROUTE_SEAM'
    # NO_SILENT_AUTO_ACE was replaced by the I3-C invariant (UC-5 sub-decision 4):
    # check_hnsw_auto_overflow_source.py ONE_OVERFLOW_* / *_NOTICE / RECEIPT_*.
    fn = function(backend, 'cuvs_build_hnsw_from_heap(Relation heapRel')
    assert 'if (rc == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED)' in fn, 'RESIDENT_ONLY_HINT_GUARD'
    assert 'explicit build_algo=ace selected ACE-disk' in fn, 'EXPLICIT_NOTICE'
    handler = function(server, 'handle_build_hnsw_file(int client_fd')
    assert handler.index('cuvs_hnsw_receive_file_request(') < handler.index('cuvs_hnsw_wait_for_idle('), 'DAEMON_CONSUMER_BEFORE_ADMISSION'
    return dict(status='PASS_SOURCE_CONFORMANCE_NOT_CUDA',
                B_order_positions=positions,
                original_native_tokens_sha256=hashlib.sha256(compact(native_before).encode()).hexdigest(),
                new_native_tokens_sha256=hashlib.sha256(compact(native_after).encode()).hexdigest(),
                actual_NVCC_SDK_GPU_PG='NOT_RUN')


def main(output):
    out = Path(output).resolve(); out.mkdir(exist_ok=False)
    w = (ROOT/'src/cuvs_wrapper.cu').read_text(); b = (ROOT/'src/pg_cuvs.c').read_text()
    s = (ROOT/'src/pg_cuvs_server.c').read_text()
    old = subprocess.check_output(['git', 'show', BASE+':src/cuvs_wrapper.cu'], cwd=ROOT, text=True)
    result = check(w, b, s, old)
    # Valid source deltas that must no longer conform to the preserved contract.
    prep = function(w, 'static int prepare_hnsw_settings(')
    assignment = re.search(r'    sample_rows = cuvs_hnsw_sample_rows\(.*?;', prep, re.S)[0]
    changed = prep.replace(assignment+'\n', '', 1).replace('    auto &pq_params = *prepared.pq_params;',
                 '    auto &pq_params = *prepared.pq_params;\n'+assignment)
    altered_order = w.replace(prep, changed, 1)
    public = function(w, 'cuvs_hnsw_build_file(int vectors_fd')
    a = public.index('rc = cuvs_hnsw_resolve_route(')
    altered_guard = public[:a] + public[a:].replace('if (rc != CUVS_STATUS_OK) return rc;',
                         'if (false && rc != CUVS_STATUS_OK) return rc;', 1)
    mutants = [dict(name='factory_before_sample_assignment', w=altered_order, b=b, s=s),
               dict(name='resolver_error_fallthrough', w=w.replace(public, altered_guard, 1), b=b, s=s),
               dict(name='attach_dataset_semantics_changed', w=w.replace('cp.attach_dataset_on_build = false;', 'cp.attach_dataset_on_build = true;'), b=b, s=s),
               dict(name='ACE_hint_on_generic_build_failure', w=w, b=b.replace('if (rc == CUVS_STATUS_HNSW_RESIDENT_BUDGET_EXCEEDED)', 'if (rc == CUVS_STATUS_BUILD_FAILED)'), s=s)]
    rows=[]
    for m in mutants:
        try: check(m['w'], m['b'], m['s'], old)
        except (AssertionError, ValueError) as error: rows.append(dict(name=m['name'],status='PASS_SOURCE_COUNTER_DETECTED',reason=str(error)))
        else: raise AssertionError('SOURCE_COUNTER_SURVIVED:'+m['name'])
    result.update(source_counters=rows, count=len(rows), baseline=BASE,
                  wrapper_sha256=hashlib.sha256(w.encode()).hexdigest())
    (out/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    p=argparse.ArgumentParser();p.add_argument('output');a=p.parse_args();main(a.output)

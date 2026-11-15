"""Bounded source conformance, not CUDA compilation or physical execution."""
from pathlib import Path
import argparse
import hashlib
import json
import re

ROOT = Path(__file__).resolve().parents[2]
BASE = '42f8a43daa43451ae3e38d28e729886a74c39a66'
# cuvs_hnsw_build_file() of src/cuvs_wrapper.cu at BASE, kept in the tree so the
# check does not need the git history of that commit (CI checks out depth 1).
BASELINE = ROOT / 'test/unit/fixtures/hnsw_build_file.42f8a43d.cu.txt'
# Approved changes to the moved native body, each (statement it must directly
# follow, statement, reason). Each must appear exactly once at its anchor and is
# removed before the token comparison; any other change still fails
# NATIVE_EXECUTION_BODY_CHANGED.
APPROVED_NATIVE_DELTAS = [
    ('if (cuvs_hnsw_owner_cancelled(owner_fd)) return CUVS_STATUS_CANCELED;',
     'resources.fence_check();',
     'fence review B1 (8b64728): a RAFT host free whose request-stream wait failed '
     'fails the build before the serialized index is returned'),
]

# Approved explicit nn_descent additions to the moved native body, each
# (first line, the text that must follow the block, SHA-256 of the block's
# whitespace-free tokens, reason). Each block must appear exactly once, end
# exactly at its follower, and match its pinned tokens; it is removed before
# the token comparison. Any edit inside a block changes its hash and fails.
APPROVED_NN_BLOCKS = [
    ('        else if (prepared.route.route == CUVS_HNSW_ROUTE_NN_DESCENT)\n',
     '        else\n        {\n            cp.graph_build_params = *prepared.pq_params;',
     '3232a7e86a9a6292d739e9aae686f9e14c8c26dbfa75c352737a40d678cf7c2d',
     'explicit build_algo=nn_descent: SDK NN-descent parameters with the '
     'replacement guard, readback fill, and the resident upper-level budget protocol'),
    ('            if (!tiny && prepared.route.route == CUVS_HNSW_ROUTE_NN_DESCENT\n',
     '            if (!tiny) return cagra::build(res, cp, data);',
     '46258d7cb9dd3667ddccb9fd9d50f8387ccfdbc6519bbfc267c082f80d0f3a39',
     'only the NN-descent L2 graph input is centered; serializers keep `data`'),
    ('        if (prepared.route.route == CUVS_HNSW_ROUTE_NN_DESCENT\n'
     '            && !cuvs_hnsw_nn_readback_valid(',
     '        if (params->policy) *params->policy = receipt;',
     '27000e91e34527b9ed1a7cd5a789e3746944d2bc4f1a813874675a1ec9111511',
     'an NN receipt must match the consumed parameters before it is published'),
    ('        if (params->nn_readback && prepared.route.route == CUVS_HNSW_ROUTE_NN_DESCENT)\n',
     '        *out_fd = fd;',
     '61a91fbe699fa913bafdd4f4458ab33bf0983b5fd5e936f9e660e4492641afbc',
     'the NN readback reaches the caller only on the NN route'),
]
# Approved one-for-one replacements in the moved body: (before, after, reason).
APPROVED_NATIVE_REPLACEMENTS = [
    ('cp.graph_build_params = pq_params;', 'cp.graph_build_params = *prepared.pq_params;',
     'the IVF-PQ parameters are owned by the prepared settings (NN routes have none)'),
    ('disk ? "ACE-disk" : tiny ? "tiny-GPU" : "host-IVFPQ"', 'cuvs_hnsw_route_name(prepared.route.route)',
     'route names come from one table that includes nn-descent'),
]


def strip_nn_blocks(body):
    for first, follower, digest, _ in APPROVED_NN_BLOCKS:
        assert body.count(first) == 1, 'APPROVED_NN_BLOCK_COUNT:' + first.strip()
        start = body.index(first)
        end = body.find(follower, start)
        assert end > start, 'APPROVED_NN_BLOCK_FOLLOWER:' + first.strip()
        block = body[start:end]
        actual = hashlib.sha256(compact(block).encode()).hexdigest()
        assert actual == digest, 'APPROVED_NN_BLOCK_CHANGED:' + first.strip() + ':' + actual
        body = body[:start] + body[end:]
    for before, after, _ in APPROVED_NATIVE_REPLACEMENTS:
        assert body.count(after) == 1, 'APPROVED_REPLACEMENT_COUNT:' + after
        body = body.replace(after, before, 1)
    return body


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
    native_moved = strip_nn_blocks(native_after)
    for anchor, statement, _ in APPROVED_NATIVE_DELTAS:
        assert native_moved.count(statement) == 1, 'APPROVED_NATIVE_DELTA_COUNT:' + statement
        head, tail = native_moved.split(statement)
        assert compact(head).endswith(compact(anchor)), 'APPROVED_NATIVE_DELTA_ANCHOR:' + statement
        native_moved = head + tail
    assert compact(native_before) == compact(native_moved), 'NATIVE_EXECUTION_BODY_CHANGED'
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
                approved_native_deltas=[dict(after=a, statement=t, reason=r) for a, t, r in APPROVED_NATIVE_DELTAS],
                approved_nn_blocks=[dict(first=f.strip(), sha256=d, reason=r) for f, _, d, r in APPROVED_NN_BLOCKS],
                approved_native_replacements=[dict(before=b, after=a, reason=r) for b, a, r in APPROVED_NATIVE_REPLACEMENTS],
                actual_NVCC_SDK_GPU_PG='NOT_RUN')


def main(output):
    out = Path(output).resolve(); out.mkdir(exist_ok=False)
    w = (ROOT/'src/cuvs_wrapper.cu').read_text(); b = (ROOT/'src/pg_cuvs.c').read_text()
    s = (ROOT/'src/pg_cuvs_server.c').read_text()
    old = BASELINE.read_text()
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
    # The approved fence delta stays exactly one statement at its anchor.
    execute = function(w, 'static int execute_hnsw_file_build(')
    fence = '        resources.fence_check();\n'
    assert execute.count(fence) == 1, 'MUTANT_ANCHOR:fence_check'
    for name, changed in (
            ('fence_check_removed', execute.replace(fence, '', 1)),
            ('fence_check_duplicated', execute.replace(fence, fence + fence, 1)),
            ('fence_check_moved_before_cancel_check', execute.replace(fence, '', 1).replace(
                '        if (cuvs_hnsw_owner_cancelled(owner_fd)) return CUVS_STATUS_CANCELED;\n        if (tiny)',
                fence + '        if (cuvs_hnsw_owner_cancelled(owner_fd)) return CUVS_STATUS_CANCELED;\n        if (tiny)', 1)),
            ('native_statement_added_beside_fence', execute.replace(fence, fence + '        resources.sync_stream();\n', 1))):
        assert changed != execute, ('MUTANT_ANCHOR', name)
        mutants.append(dict(name=name, w=w.replace(execute, changed, 1), b=b, s=s))
    for name, old_text, new_text in (
            ('nn_readback_copy_dropped',
             '            *params->nn_readback = prepared.nn_readback;\n', '            ;\n'),
            ('nn_receipt_validation_dropped',
             '            && !cuvs_hnsw_nn_readback_valid(&prepared.nn_readback, &receipt))',
             '            && false)'),
            ('centering_on_every_route',
             '            if (!tiny && prepared.route.route == CUVS_HNSW_ROUTE_NN_DESCENT\n                && cuvs_hnsw_nn_centers(params->metric))',
             '            if (!tiny && cuvs_hnsw_nn_centers(params->metric))'),
            ('nn_sdk_replacement_guard_dropped',
             'if (selected.graph_degree != cp.intermediate_graph_degree || selected.metric != cp.metric)',
             'if (false)'),
            ('nn_statement_added_after_readback_copy',
             '            *params->nn_readback = prepared.nn_readback;\n',
             '            *params->nn_readback = prepared.nn_readback;\n        resources.sync_stream();\n')):
        assert execute.count(old_text) == 1, ('MUTANT_ANCHOR', name)
        mutants.append(dict(name=name, w=w.replace(execute, execute.replace(old_text, new_text, 1), 1), b=b, s=s))
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

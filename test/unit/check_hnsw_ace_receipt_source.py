"""Source-structure invariant for the optional ACE effective partition count.

With the SDK source planner (CUVS_HNSW_HAS_SDK_PLAN 1) the ACE receipt records the
effective count, computed twice and refused if unstable. Without it (0) an ACE
build is not refused: the requested count goes to the SDK and the receipt is
marked requested-only (CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED), build_info
says so, and CREATE INDEX leaves a NOTICE. The patched-DSO markers stay required
in both builds. CPU source conformance only: it does not compile CUDA or run
PG/GPU. Every check is paired with a source mutant that must be detected.
"""
from pathlib import Path
import argparse
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_hnsw_explicit_source import function  # noqa: E402

FILES = {
    'policy': 'src/cuvs_hnsw_policy.h',
    'plan': 'src/cuvs_hnsw_sdk_plan.cuh',
    'wrapper': 'src/cuvs_wrapper.cu',
    'export': 'src/hnsw_export.c',
    'backend': 'src/pg_cuvs.c',
    'server': 'src/pg_cuvs_server.c',
    'makefile': 'Makefile',
}
FLAG = 'CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED'


def strip_comments(s):
    # Drop comments but keep string/char literals.
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/',
                  lambda m: m[0] if m[0][0] in '"\'' else '', s, flags=re.S)


def compact(s):
    return re.sub(r'\s+', '', s)


def at(body, needle, label):
    # A missing anchor is a rule violation with a name, not an IndexError.
    assert needle in body, label
    return body.index(needle)


def check(src):
    code = {k: strip_comments(v) for k, v in src.items()}

    # --- policy header: one new bit of the existing route_flags word ---
    policy = code['policy']
    assert re.search(r'#define\s+' + FLAG + r'\s+0x40u', policy), 'FLAG_VALUE'
    assert re.search(r'#define\s+CUVS_HNSW_ROUTE_FLAGS_KNOWN\s+0x7fu', policy), 'FLAG_KNOWN_TO_READER'
    valid = function(policy, 'static inline int cuvs_hnsw_policy_valid(')
    assert compact('(!(p->route_flags & ' + FLAG + ')|| p->route == CUVS_HNSW_ROUTE_ACE)') in compact(valid), \
        'FLAG_ACE_ROUTE_ONLY'
    assert compact('(p->route == CUVS_HNSW_ROUTE_ACE ? p->partitions > 0 : p->partitions == 0)') in compact(valid), \
        'PARTITIONS_STILL_REQUIRED'
    unverified = function(policy, 'static inline int cuvs_hnsw_ace_partitions_unverified(')
    assert 'p->route == CUVS_HNSW_ROUTE_ACE' in unverified and FLAG in unverified, 'UNVERIFIED_READS_FLAG'
    origin = function(policy, 'static inline const char *cuvs_hnsw_ace_partitions_origin(')
    assert 'cuvs_hnsw_ace_partitions_unverified(p)' in origin, 'ORIGIN_READS_FLAG'
    for text in ('"not_applicable"', '"requested_effective_not_guaranteed"', '"same_SDK_stable_explicit_plan"'):
        assert text in origin, 'ORIGIN_TEXT:' + text
    iterations = function(policy, 'static inline int cuvs_hnsw_ace_iterations_guaranteed(')
    assert 'cuvs_hnsw_ace_partitions_unverified(p)' in iterations \
        and 'p->n_rows >= CUVS_HNSW_SDK_ACE_NN_MAX_ROWS' in iterations \
        and 'r->local_iterations != 0' in iterations, 'ITERATIONS_GUARANTEE_RULE'

    # --- plan header: both builds exist ---
    plan = code['plan']
    assert '#define CUVS_HNSW_HAS_SDK_PLAN 1' in plan and '#define CUVS_HNSW_HAS_SDK_PLAN 0' in plan, 'BOTH_PLAN_VARIANTS'

    # --- wrapper: the ACE branch of execute_hnsw_file_build ---
    execute = function(code['wrapper'], 'static int execute_hnsw_file_build(')
    start = at(execute, '#if CUVS_HNSW_HAS_SDK_PLAN', 'PLANNER_BRANCH_PRESENT')
    other = at(execute, '#else', 'NO_PLANNER_BRANCH_PRESENT')
    end = at(execute, '#endif', 'PLANNER_BRANCH_CLOSED')
    planned, unplanned = execute[start:other], execute[other:end]
    head = execute[:start]
    assert 'ace.npartitions = partitions;' in head, 'REQUESTED_COUNT_REACHES_SDK'
    # planner present: exactly today's behaviour, not marked
    assert planned.count('cuvs_hnsw_sdk_ace_plan(') == 2, 'PLANNER_TWICE'
    assert 'if (effective != rechecked)' in planned and 'ACE SDK partition plan is not stable' in planned, 'PLANNER_STABILITY_REFUSAL'
    assert 'ace.npartitions = effective;' in planned and 'receipt.partitions = (uint32_t)effective;' in planned, 'PLANNER_EFFECTIVE_RECORDED'
    assert FLAG not in planned, 'PLANNER_BUILD_NOT_MARKED'
    # planner absent: no refusal, marked requested-only without clearing other bits
    assert 'throw' not in unplanned and 'return' not in unplanned, 'NO_PLANNER_NO_REFUSAL'
    assert compact('receipt.route_flags |= ' + FLAG + ';') in compact(unplanned), 'NO_PLANNER_MARKED_REQUESTED'
    assert 'receipt.partitions' not in unplanned and 'ace.npartitions' not in unplanned, 'NO_PLANNER_COUNT_UNTOUCHED'
    assert 'ACE metadata requires matching pinned SDK source planner' not in code['wrapper'], 'OLD_REFUSAL_TEXT_GONE'

    # --- the patched-DSO markers still gate every ACE build, ahead of the execute call ---
    public = function(code['wrapper'], 'cuvs_hnsw_build_file(int vectors_fd')
    assert '#if CUVS_HNSW_HAS_SDK_PLAN' not in public, 'NO_PLANNER_GATE_BEFORE_MARKERS'
    block = public[at(public, 'if (disk)', 'ACE_BLOCK'):at(public, 'receipt.magic = CUVS_HNSW_POLICY_MAGIC;', 'RECEIPT_AFTER_ACE_BLOCK')]
    for marker in ('pgcuvs_hnsw_disk_copy_patch_id', 'pgcuvs_hnsw_memory_guard_patch_id'):
        assert block.count('hnsw_sdk_marker_verified("' + marker + '"') == 1, 'ACE_MARKER_REQUIRED:' + marker
    assert 'ACE requires verified SDK partition/reorder/serializer capacity guard' in block \
        and 'return CUVS_STATUS_BUILD_FAILED;' in block, 'ACE_MARKER_REFUSAL'
    assert public.index('if (disk)') < at(public, 'return execute_hnsw_file_build(', 'EXECUTE_CALL'), 'MARKERS_BEFORE_EXECUTE'

    # --- where a requested-only receipt gets its count: the route resolver's candidate, in both ACE writers ---
    assert 'const auto partitions = prepared.route.partitions;' in execute, 'EXECUTE_PARTITIONS_FROM_ROUTE'
    assert 'const auto partitions = prepared.route.partitions;' in public, 'PUBLIC_PARTITIONS_FROM_ROUTE'
    assert public.count('receipt.partitions = partitions;') == 1, 'RECEIPT_COUNT_IS_REQUESTED'
    assert public.index('receipt.partitions = partitions;') > public.index('receipt.magic = CUVS_HNSW_POLICY_MAGIC;'), 'RECEIPT_COUNT_IN_RECEIPT_BLOCK'
    # graph-failure fallback: the second ACE writer, with its own marker gate (the old throw is gone)
    fb_start = at(public, 'const bool sdk_ok =', 'FALLBACK_MARKER_GATE')
    fb = public[fb_start:]
    fb = fb[:at(fb, 'return execute_hnsw_file_build(', 'FALLBACK_EXECUTE')]
    for marker in ('pgcuvs_hnsw_disk_copy_patch_id', 'pgcuvs_hnsw_memory_guard_patch_id'):
        assert fb.count('hnsw_sdk_marker_verified("' + marker + '"') == 1, 'FALLBACK_MARKER_REQUIRED:' + marker
    assert compact('if (!sdk_ok ||') in compact(fb) and 'throw;' in fb[fb.index('if (!sdk_ok ||'):], 'FALLBACK_MARKER_REFUSES'
    assert 'receipt.partitions = ace.partitions;' in fb, 'FALLBACK_RECEIPT_COUNT_IS_REQUESTED'
    assert 'prepared.route = ace;' in fb, 'FALLBACK_ROUTE_RECORDED'

    # --- the receipt reaches the backend as the wrapper wrote it: the daemon hands the wire reply to the
    # wrapper and never edits a field of it (a cleared mark would read as an effective count) ---
    handler = function(code['server'], 'handle_build_hnsw_file(int client_fd')
    assert 'params.policy = &wire.policy;' in handler, 'DAEMON_RECEIPT_IS_THE_WIRE_REPLY'
    assert not re.search(r'wire\.policy\.\w+\s*(?:[-+|&^*/%]|<<|>>)?=(?!=)', handler), 'DAEMON_NEVER_EDITS_RECEIPT_FIELDS'
    assert not re.search(r'(?:memset|memcpy)\s*\(\s*&wire\.policy|wire\.policy\s*=(?!=)', handler), 'DAEMON_NEVER_REPLACES_RECEIPT'
    assert 'if (params->policy) *params->policy = receipt;' in execute, 'WRAPPER_COPIES_WHOLE_RECEIPT'

    # --- Makefile: the tests that guard the planner switch run in test-unit, and the note names the variable ---
    mk = code['makefile']
    unit = mk[at(mk, 'test-unit:', 'TEST_UNIT_RULE'):]
    unit = unit[:unit.index('\n\t')]
    for target in ('test-hnsw-ace-origin-json', 'test-hnsw-ace-planner-branch', 'test-hnsw-sdk-plan-gate',
                   'test-hnsw-gpu-link-lines', 'test-hnsw-ace-quality'):
        assert re.search(r'(?<![\w-])' + target + r'(?![\w-])', unit), 'TEST_UNIT_RUNS:' + target
    note = mk[at(mk, 'CUVS_SDK_PLAN_NOTE = @echo', 'NOTE_DEFINED'):]
    note = note[:note.index('\n')]
    assert 'CUVS_SDK_SOURCE_INCLUDE is not set' in note and 'set CUVS_SDK_SOURCE_INCLUDE to' in note \
        and 'guarantee is OFF' in note, 'NOTE_NAMES_THE_VARIABLE'

    # --- build_info: origin and exactness come from the receipt, not from a constant ---
    info = function(code['export'], 'pg_cuvs_hnsw_build_info(PG_FUNCTION_ARGS)')
    assert 'cuvs_hnsw_ace_partitions_origin(&policy)' in info, 'BUILD_INFO_ORIGIN_FROM_RECEIPT'
    assert 'same_SDK_stable_explicit_plan' not in info, 'BUILD_INFO_NO_FIXED_ORIGIN'
    guard = at(info, 'if (!cuvs_hnsw_ace_iterations_guaranteed(&policy, &ace))', 'BUILD_INFO_ITERATIONS_GUARD')
    assert '"ace_local_nn_descent_iterations\\":\\"not_guaranteed\\""' in info[guard:], 'BUILD_INFO_ITERATIONS_NOT_GUARANTEED'
    assert guard < at(info, 'else if (ace.local_iterations)', 'BUILD_INFO_ITERATIONS_EXACT_BRANCH'), 'BUILD_INFO_ITERATIONS_GUARD_FIRST'

    # --- CREATE INDEX NOTICE: after the build returns, says it is off and how to turn it on ---
    backend = src['backend']
    build = function(code['backend'], 'cuvs_build_hnsw_from_heap(Relation heapRel')
    pos = at(build, 'if (cuvs_hnsw_ace_partitions_unverified(&out->policy))', 'NOTICE_GATED_ON_RECEIPT')
    notice = build[pos:at(build, 'out->tids_fd = dup(', 'TIDS_HANDOFF')]
    assert at(build, 'cuvs_hnsw_build_notices(', 'BUILD_NOTICES') < pos, 'NOTICE_AFTER_BUILD'
    assert 'ereport(NOTICE' in notice, 'NOTICE_IS_NOTICE'
    assert 'the effective count is not guaranteed' in notice, 'NOTICE_SAYS_GUARANTEE_OFF'
    assert 'built without the cuVS SDK source planner' in notice, 'NOTICE_SAYS_WHY'
    assert 'errhint(' in notice and 'CUVS_SDK_SOURCE_INCLUDE' in notice, 'NOTICE_SAYS_HOW'
    assert 'neighbors/detail/cagra/cagra_build.cuh' in notice and '-fopenmp' in notice \
        and 'planner source check' in notice, 'NOTICE_SAYS_HOW_COMPLETELY'
    assert re.search(r'errmsg\("[^"]*%u[^"]*",\s*out->policy\.partitions\)', notice), 'NOTICE_PRINTS_RECEIPT_COUNT'
    assert backend.count('the effective count is not guaranteed') == 1, 'NOTICE_ONCE'
    return 'PASS_SOURCE_INVARIANT_NOT_CUDA'


OLD_THROW = 'throw std::runtime_error("ACE metadata requires matching pinned SDK source planner; set CUVS_SDK_SOURCE_INCLUDE");'
MARK = '            receipt.route_flags |= ' + FLAG + ';\n'
MUTANTS = [
    ('planner_absent_refuses_again', 'wrapper', MARK, '            ' + OLD_THROW + '\n'),
    ('planner_absent_not_marked', 'wrapper', MARK, ''),
    ('planner_absent_mark_overwrites_other_flags', 'wrapper',
     MARK, '            receipt.route_flags = ' + FLAG + ';\n'),
    ('planner_present_also_marked', 'wrapper',
     '            receipt.partitions = (uint32_t)effective;\n',
     '            receipt.partitions = (uint32_t)effective;\n            receipt.route_flags |= ' + FLAG + ';\n'),
    ('planner_present_stability_check_dropped', 'wrapper',
     '            if (effective != rechecked)', '            if (false && effective != rechecked)'),
    ('planner_present_effective_not_recorded', 'wrapper',
     '            receipt.partitions = (uint32_t)effective;\n', ''),
    ('planner_absent_count_overwritten', 'wrapper',
     MARK, MARK + '            receipt.partitions = 0;\n'),
    ('requested_count_not_passed_to_sdk', 'wrapper',
     '            ace.npartitions = partitions;\n', ''),
    ('ace_disk_copy_marker_dropped', 'wrapper',
     '            if (!hnsw_sdk_marker_verified("pgcuvs_hnsw_disk_copy_patch_id",\n'
     '                    "pgcuvs-hnsw-disk-copy-v1@60b19a0f62ba0bd716de349382828e43be6e2daa")\n'
     '                || !hnsw_sdk_marker_verified("pgcuvs_hnsw_memory_guard_patch_id",',
     '            if (!hnsw_sdk_marker_verified("pgcuvs_hnsw_memory_guard_patch_id",'),
    ('ace_marker_check_behind_planner_gate', 'wrapper',
     '            if (std::strlen(workdir) > 255 || std::strstr(workdir, "//"))\n',
     '#if CUVS_HNSW_HAS_SDK_PLAN\n            if (std::strlen(workdir) > 255 || std::strstr(workdir, "//"))\n'),
    ('build_info_fixed_origin', 'export',
     '        cuvs_hnsw_ace_partitions_origin(&policy),\n',
     '        policy.route == CUVS_HNSW_ROUTE_ACE ? "same_SDK_stable_explicit_plan" : "not_applicable",\n'),
    ('build_info_iterations_always_exact', 'export',
     '        if (!cuvs_hnsw_ace_iterations_guaranteed(&policy, &ace))',
     '        if (0 && !cuvs_hnsw_ace_iterations_guaranteed(&policy, &ace))'),
    ('notice_dropped', 'backend',
     '            if (cuvs_hnsw_ace_partitions_unverified(&out->policy))',
     '            if (0 && cuvs_hnsw_ace_partitions_unverified(&out->policy))'),
    ('notice_without_how_to_enable', 'backend',
     'errhint("Build pg_cuvs_server with CUVS_SDK_SOURCE_INCLUDE set to the cpp/src of the patched SDK source: neighbors/detail/cagra/cagra_build.cuh must be under it (the make build stops in its planner source check otherwise) and the wrapper must be compiled with OpenMP (-fopenmp), without which the planner is left out.")',
     'errhint("Build pg_cuvs_server with CUVS_SDK_SOURCE_INCLUDE.")'),
    ('notice_prints_wrong_field', 'backend',
     'is the requested count; the effective count is not guaranteed", out->policy.partitions)',
     'is the requested count; the effective count is not guaranteed", out->policy.n_lists)'),
    ('receipt_count_not_the_request', 'wrapper',
     '        receipt.partitions = partitions;\n', '        receipt.partitions = 2;\n'),
    ('fallback_receipt_count_not_the_request', 'wrapper',
     '            receipt.partitions = ace.partitions;\n', '            receipt.partitions = 2;\n'),
    ('fallback_memory_guard_marker_dropped', 'wrapper',
     '                && hnsw_sdk_marker_verified("pgcuvs_hnsw_memory_guard_patch_id",\n'
     '                    "pgcuvs-hnsw-memory-guard-v2@60b19a0f62ba0bd716de349382828e43be6e2daa");',
     '                ;'),
    ('fallback_marker_gate_unused', 'wrapper',
     '            if (!sdk_ok || std::strlen(workdir) > 255', '            if (std::strlen(workdir) > 255'),
    ('daemon_clears_requested_mark', 'server',
     '    reply.status = cuvs_hnsw_build_file(vectors_fd, owner_fd, &params, workdir,\n        device, &result_fd, reply.error, sizeof(reply.error));\n',
     '    reply.status = cuvs_hnsw_build_file(vectors_fd, owner_fd, &params, workdir,\n        device, &result_fd, reply.error, sizeof(reply.error));\n    wire.policy.route_flags &= ~CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED;\n'),
    ('daemon_replaces_receipt', 'server',
     '    params.policy = &wire.policy;', '    params.policy = &wire.policy; memset(&wire.policy, 0, sizeof(wire.policy));'),
    ('wrapper_copies_partial_receipt', 'wrapper',
     'if (params->policy) *params->policy = receipt;', 'if (params->policy) params->policy->partitions = receipt.partitions;'),
    ('test_unit_drops_plan_gate', 'makefile',
     'test-hnsw-ace-planner-branch test-hnsw-sdk-plan-gate test-hnsw-gpu-link-lines \\', 'test-hnsw-ace-planner-branch test-hnsw-gpu-link-lines \\'),
    ('note_misnames_the_variable', 'makefile',
     'set CUVS_SDK_SOURCE_INCLUDE to the patched SDK cpp/src to turn it on', 'set CUVS_SDK_SOURCE_INC to the patched SDK cpp/src to turn it on'),
    ('notice_without_reason', 'backend',
     'This pg_cuvs_server was built without the cuVS SDK source planner, so the receipt records',
     'The receipt records'),
    ('policy_flag_unknown_to_reader', 'policy',
     '#define CUVS_HNSW_ROUTE_FLAGS_KNOWN 0x7fu', '#define CUVS_HNSW_ROUTE_FLAGS_KNOWN 0x3fu'),
    ('policy_flag_accepted_off_ace_route', 'policy',
     '        && (!(p->route_flags & CUVS_HNSW_ROUTE_FLAG_ACE_PARTITIONS_REQUESTED)\n            || p->route == CUVS_HNSW_ROUTE_ACE)\n', ''),
    ('policy_origin_ignores_flag', 'policy',
     '    return cuvs_hnsw_ace_partitions_unverified(p)\n        ? "requested_effective_not_guaranteed" : "same_SDK_stable_explicit_plan";',
     '    return "same_SDK_stable_explicit_plan";'),
    ('policy_iterations_guarantee_ignores_flag', 'policy',
     '    return !(cuvs_hnsw_ace_partitions_unverified(p)\n             && p->n_rows',
     '    return !(0 && p->n_rows'),
]


def main(output):
    out = Path(output).resolve(); out.mkdir(exist_ok=False)
    src = {k: (ROOT / v).read_text() for k, v in FILES.items()}
    result = dict(status=check(src), mutants=[], count=0)
    rows = result['mutants']
    for name, key, old, new in MUTANTS:
        assert src[key].count(old) == 1, ('MUTANT_ANCHOR', name, src[key].count(old))
        m = dict(src); m[key] = src[key].replace(old, new, 1)
        try:
            check(m)
        except (AssertionError, ValueError) as e:
            rows.append(dict(name=name, status='PASS_SOURCE_MUTANT_DETECTED', reason=str(e)))
        else:
            raise AssertionError('SOURCE_MUTANT_SURVIVED:' + name)
    result['count'] = len(rows)
    (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    a = argparse.ArgumentParser(); a.add_argument('output'); main(a.parse_args().output)
